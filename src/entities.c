#include "entities.h"
#include "world.h"
#include "mesh.h"
#include "physics.h"
#include "inventory.h"
#include "fluids.h"
#include "state.h"
#include "net.h"
#include "prof.h"
#include "raymath.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ============================== shared meshes ==============================
static Mesh sand_cube, tnt_cube;
static Mesh drop_cube[32];          // per block id, lazily built, scale 0.3
static bool drop_cube_ready[32];
static Texture2D items_tex;         // for flat item drops (billboards)
static int items_rows = 4;

static Mesh *cube_for(int id) {
  if (id < 0 || id >= 32) id = B_STONE;
  if (!drop_cube_ready[id]) { drop_cube[id] = build_block_cube_mesh((uint8_t)id); drop_cube_ready[id] = true; }
  return &drop_cube[id];
}

Texture2D entities_items_tex(void);   // shared with ui.c
Texture2D entities_items_tex(void) { return items_tex; }

static void pigs_init(void);
static void mobs_init(void);

void entities_init(void) {
  sand_cube = build_block_cube_mesh(B_SAND);
  tnt_cube  = build_block_cube_mesh(B_TNT);
  if (FileExists("assets/items.png")) {
    items_tex = LoadTexture("assets/items.png");
    SetTextureFilter(items_tex, TEXTURE_FILTER_POINT);
    items_rows = items_tex.height / 64 > 0 ? items_tex.height / 64 : 4;
  }
  pigs_init();
  mobs_init();
}

// ============================== falling sand ==============================
// game.js:689-723
typedef struct FallingBlock { int x, z, land_y; double y; float vy; bool used; } FallingBlock;
#define MAX_FALLING 256
static FallingBlock falling[MAX_FALLING];

typedef struct FallTarget { int x, z, y; bool used; } FallTarget;
static FallTarget fall_targets[MAX_FALLING];

static FallTarget *target_for(int x, int z) {
  for (int i = 0; i < MAX_FALLING; i++)
    if (fall_targets[i].used && fall_targets[i].x == x && fall_targets[i].z == z) return &fall_targets[i];
  return NULL;
}

void check_fall(int x, int y, int z) {
  if (get_voxel(x, y, z) != B_SAND) return;
  if (get_voxel(x, y - 1, z) != B_AIR) return;
  int land_y;
  FallTarget *t = target_for(x, z);
  if (t) land_y = t->y;
  else { land_y = y; while (land_y > 0 && get_voxel(x, land_y - 1, z) == B_AIR) land_y--; }
  if (!t) { for (int i = 0; i < MAX_FALLING; i++) if (!fall_targets[i].used) { t = &fall_targets[i]; t->used = true; t->x = x; t->z = z; break; } }
  if (t) t->y = land_y + 1;
  set_voxel(x, y, z, B_AIR); remesh_around(x, z); touch_water(x, y, z);
  for (int i = 0; i < MAX_FALLING; i++)
    if (!falling[i].used) { falling[i] = (FallingBlock){ x, z, land_y, (double)y, 0, true }; break; }
  check_fall(x, y + 1, z);           // the sand above follows
}

void update_falling(float dt) {
  for (int i = 0; i < MAX_FALLING; i++) {
    FallingBlock *f = &falling[i];
    if (!f->used) continue;
    f->vy += P_GRAVITY * dt; f->y += f->vy * dt;
    if (f->y <= f->land_y) {
      f->used = false;
      set_voxel(f->x, f->land_y, f->z, B_SAND); remesh_around(f->x, f->z); touch_water(f->x, f->land_y, f->z);
      bool more = false;
      for (int k = 0; k < MAX_FALLING; k++)
        if (falling[k].used && falling[k].x == f->x && falling[k].z == f->z) { more = true; break; }
      if (!more) { FallTarget *t = target_for(f->x, f->z); if (t) t->used = false; }
    }
  }
}

// ============================== TNT ==============================
// game.js:777-834
typedef struct PrimedTNT { int x, z; double y; float vy, fuse; bool used; } PrimedTNT;
#define MAX_TNT 64
static PrimedTNT primed[MAX_TNT];

void prime_tnt(int x, int y, int z, float fuse) {
  set_voxel(x, y, z, B_AIR); remesh_around(x, z); touch_water(x, y, z);
  for (int i = 0; i < MAX_TNT; i++)
    if (!primed[i].used) { primed[i] = (PrimedTNT){ x, z, (double)y, 0, fuse, true }; break; }
}

// explode (game.js:787): R=4 sphere, batched dirty-chunk remesh, chain TNT.
// net_emit: broadcast the crater (BOOM event + per-block edits for the server
// log); chain: prime nested TNT. explode_remote replays with both off — the
// authority's follow-up BOOMs cover chained explosions.
static void explode_impl(int cx, int cy, int cz, bool net_emit, bool chain) {
  double t0 = GetTime();
  int cleared = 0;
  const int R = 4;
  enum { MAXD = 128 };
  int dirty[MAXD][2]; int nd = 0;
  if (net_emit) net_send_boom(cx, cy, cz);
  for (int dx = -R; dx <= R; dx++)
    for (int dy = -R; dy <= R; dy++)
      for (int dz = -R; dz <= R; dz++) {
        if (dx * dx + dy * dy + dz * dz > R * R) continue;
        int x = cx + dx, y = cy + dy, z = cz + dz;
        if (y <= 0) continue;
        uint8_t b = get_voxel(x, y, z);
        if (b == B_AIR) continue;
        if (b == B_TNT) {
          if (chain) prime_tnt(x, y, z, 0.1f + (float)GetRandomValue(0, 30) / 100.0f);
          continue;
        }
        set_voxel(x, y, z, B_AIR); touch_water(x, y, z); cleared++;
        if (net_emit) net_send_edit(x, y, z);
        int ccx = chunk_of(x), ccz = chunk_of(z);
        bool seen = false;
        for (int k = 0; k < nd; k++) if (dirty[k][0] == ccx && dirty[k][1] == ccz) { seen = true; break; }
        if (!seen && nd < MAXD) { dirty[nd][0] = ccx; dirty[nd][1] = ccz; nd++; }
      }
  // deferred + deduped: the queue rebuilds a few chunks per frame instead of
  // a synchronous 3x3-per-dirty-chunk remesh (which double-built overlapping
  // neighbors and spiked the frame 13-38 ms)
  for (int k = 0; k < nd; k++)
    for (int da = -1; da <= 1; da++)
      for (int db = -1; db <= 1; db++) {
        Chunk *c = chunk_get(dirty[k][0] + da, dirty[k][1] + db);
        if (c && c->meshed) mesh_enqueue(dirty[k][0] + da, dirty[k][1] + db);
      }
  double pd = sqrt((player.x - (cx + 0.5)) * (player.x - (cx + 0.5))
                 + (player.y + 0.9 - (cy + 0.5)) * (player.y + 0.9 - (cy + 0.5))
                 + (player.z - (cz + 0.5)) * (player.z - (cz + 0.5)));
  if (pd < R + 2) damage((int)floor((R + 2 - pd) / (R + 2) * 16 + 0.5));
  if (prof_on()) {
    double ms = (GetTime() - t0) * 1000.0;
    prof_note(PH_EXPLODE, ms);
    printf("PROF explode_at(%d,%d,%d): %.2f ms, %d blocks, %d dirty chunks\n", cx, cy, cz, ms, cleared, nd);
  }
}

void explode_at(int cx, int cy, int cz) { explode_impl(cx, cy, cz, true, true); }
void explode_remote(int x, int y, int z) { explode_impl(x, y, z, false, false); }

void update_primed(float dt) {
  for (int i = 0; i < MAX_TNT; i++) {
    PrimedTNT *p = &primed[i];
    if (!p->used) continue;
    // falls like sand: gravity, stops on solid ground
    p->vy += P_GRAVITY * dt;
    double ny = p->y + p->vy * dt;
    int cy = (int)floor(ny);
    if (p->vy < 0 && !passable(get_voxel(p->x, cy, p->z))) { ny = cy + 1; p->vy = 0; }
    p->y = ny;
    p->fuse -= dt;
    if (p->fuse <= 0) {
      p->used = false;
      explode_at(p->x, (int)floor(p->y + 0.5), p->z);
    }
  }
}

// ============================== dropped items ==============================
// game.js:725-775
typedef struct Drop { int id, count; double x, y, z; float vy, age, delay; bool used; } Drop;
#define MAX_DROPS 256
static Drop drops[MAX_DROPS];

void spawn_drop(int id, int count, double x, double y, double z, float delay) {
  for (int i = 0; i < MAX_DROPS; i++)
    if (!drops[i].used) { drops[i] = (Drop){ id, count, x, y, z, 1.5f, 0, delay, true }; return; }
}

void drop_held(void) {
  Stack *s = &slots[sel_slot];
  if (!s->id) return;
  float cp = cosf(player.pitch);
  double dx = -sinf(player.yaw) * cp, dz = -cosf(player.yaw) * cp;
  spawn_drop(s->id, 1, player.x + dx * 0.8, player.y + 1.0, player.z + dz * 0.8, 1.5f);
  s->count--; if (s->count == 0) s->id = 0;
}

void update_drops(float dt) {
  for (int i = 0; i < MAX_DROPS; i++) {
    Drop *d = &drops[i];
    if (!d->used) continue;
    d->age += dt;
    d->vy += P_GRAVITY * dt;
    double ny = d->y + d->vy * dt;
    int cy = (int)floor(ny);
    if (d->vy < 0 && !passable(get_voxel((int)floor(d->x), cy, (int)floor(d->z)))) { ny = cy + 1; d->vy = 0; }
    d->y = ny < 0 ? 0 : ny;
    if (d->age > d->delay) {
      double dx = d->x - player.x, dz = d->z - player.z, dy = d->y - player.y;
      if (dx * dx + dz * dz < 1.6 && dy > -1.5 && dy < 2.2) {
        add_item(d->id, d->count);
        d->used = false;
      }
    }
  }
}

// ============================== pigs ==============================
// game.js:1466-1621 — boxel model with the Minecraft box-unwrap UVs.
typedef struct Pig { double x, y, z; float vy, heading, wander; int health; bool used; } Pig;
#define MAX_PIGS 32
static Pig pigs[MAX_PIGS];

static Texture2D pig_tex;
static Material pig_mat;
static bool pig_textured = false;

// One box part: size in texture pixels (w,h,d), UV unwrap origin (ox,oy),
// local offset and yaw-flip flag (head/snout face +z like the JS model).
typedef struct PigPart { Mesh mesh; Vector3 offset; bool rot_body; bool flip; } PigPart;
static PigPart pig_parts[7];
static int n_pig_parts = 0;

#define PIG_TW 64.0f
#define PIG_TH 32.0f

// Build a w×h×d (pixels) box mesh with MC unwrap at (ox,oy). Sizes in blocks = px/16.
static Mesh pig_box(int w, int h, int d, int ox, int oy) {
  float W = w / 16.0f, H = h / 16.0f, D = d / 16.0f;
  // UV regions per face (game.js setBoxUV); {x0,y0,x1,y1} in pixels, y from top
  float R[6][4] = {
    { (float)ox,             (float)(oy + d),  (float)(ox + d),          (float)(oy + d + h) },   // east  +x
    { (float)(ox + d + w),   (float)(oy + d),  (float)(ox + 2 * d + w),  (float)(oy + d + h) },   // west  -x
    { (float)(ox + d),       (float)oy,        (float)(ox + d + w),      (float)(oy + d) },       // up    +y
    { (float)(ox + d + w),   (float)oy,        (float)(ox + d + 2 * w),  (float)(oy + d) },       // down  -y
    { (float)(ox + 2 * d + w), (float)(oy + d), (float)(ox + 2 * d + 2 * w), (float)(oy + d + h) }, // south +z
    { (float)(ox + d),       (float)(oy + d),  (float)(ox + d + w),      (float)(oy + d + h) },   // north -z
  };
  // Corners per face: TL, TR, BL, BR when looking at the face from outside.
  float x0 = -W / 2, x1 = W / 2, y0 = -H / 2, y1 = H / 2, z0 = -D / 2, z1 = D / 2;
  float FC[6][4][3] = {
    { {x1,y1,z1},{x1,y1,z0},{x1,y0,z1},{x1,y0,z0} },   // +x
    { {x0,y1,z0},{x0,y1,z1},{x0,y0,z0},{x0,y0,z1} },   // -x
    { {x0,y1,z0},{x1,y1,z0},{x0,y1,z1},{x1,y1,z1} },   // +y
    { {x0,y0,z1},{x1,y0,z1},{x0,y0,z0},{x1,y0,z0} },   // -y
    { {x0,y1,z1},{x1,y1,z1},{x0,y0,z1},{x1,y0,z1} },   // +z
    { {x1,y1,z0},{x0,y1,z0},{x1,y0,z0},{x0,y0,z0} },   // -z
  };
  float FN[6][3] = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };
  Mesh m = {0};
  m.vertexCount = 24; m.triangleCount = 12;
  m.vertices = malloc(sizeof(float) * 72);
  m.normals = malloc(sizeof(float) * 72);
  m.texcoords = malloc(sizeof(float) * 48);
  m.indices = malloc(sizeof(unsigned short) * 36);
  for (int f = 0; f < 6; f++) {
    float u0 = R[f][0] / PIG_TW, v0 = R[f][1] / PIG_TH, u1 = R[f][2] / PIG_TW, v1 = R[f][3] / PIG_TH;
    float uv[4][2] = { {u0,v0},{u1,v0},{u0,v1},{u1,v1} };   // TL,TR,BL,BR (v grows down = texture rows)
    for (int k = 0; k < 4; k++) {
      int vi = f * 4 + k;
      memcpy(&m.vertices[vi * 3], FC[f][k], sizeof(float) * 3);
      memcpy(&m.normals[vi * 3], FN[f], sizeof(float) * 3);
      m.texcoords[vi * 2] = uv[k][0]; m.texcoords[vi * 2 + 1] = uv[k][1];
    }
    int b = f * 4;
    unsigned short *ix = &m.indices[f * 6];
    ix[0] = b; ix[1] = b + 2; ix[2] = b + 1; ix[3] = b + 1; ix[4] = b + 2; ix[5] = b + 3;
  }
  UploadMesh(&m, false);
  return m;
}

static void pig_add_part(int w, int h, int d, int ox, int oy, float px, float py, float pz, bool rot_body, bool flip) {
  pig_parts[n_pig_parts++] = (PigPart){ pig_box(w, h, d, ox, oy), (Vector3){ px, py, pz }, rot_body, flip };
}

static void pigs_init(void) {
  if (FileExists("assets/pig.png")) {
    pig_tex = LoadTexture("assets/pig.png");
    SetTextureFilter(pig_tex, TEXTURE_FILTER_POINT);
    pig_textured = true;
  }
  pig_mat = LoadMaterialDefault();
  if (pig_textured) SetMaterialTexture(&pig_mat, MATERIAL_MAP_DIFFUSE, pig_tex);
  else pig_mat.maps[MATERIAL_MAP_DIFFUSE].color = (Color){ 240, 160, 176, 255 };
  // model parts (game.js makePigMesh)
  pig_add_part(10, 16, 8, 28, 8, 0, 0.44f, -0.05f, true, false);   // body (rotated -90° about x)
  pig_add_part(8, 8, 8, 0, 0,   0, 0.56f, 0.5f,  false, true);     // head
  pig_add_part(4, 3, 1, 16, 16, 0, 0.5f,  0.78f, false, true);     // snout
  pig_add_part(4, 6, 4, 0, 16, -0.15f, 0.1875f,  0.3f, false, false);
  pig_add_part(4, 6, 4, 0, 16,  0.15f, 0.1875f,  0.3f, false, false);
  pig_add_part(4, 6, 4, 0, 16, -0.15f, 0.1875f, -0.3f, false, false);
  pig_add_part(4, 6, 4, 0, 16,  0.15f, 0.1875f, -0.3f, false, false);
}

static bool pig_collides(double px, double py, double pz) {
  const double half = 0.35, h = 0.8;
  for (int x = (int)floor(px - half); x <= (int)floor(px + half); x++)
    for (int y = (int)floor(py); y <= (int)floor(py + h); y++)
      for (int z = (int)floor(pz - half); z <= (int)floor(pz + half); z++)
        if (!passable(get_voxel(x, y, z))) return true;
  return false;
}

static bool pig_wet(double px, double py, double pz) {
  const double half = 0.35;
  for (int x = (int)floor(px - half); x <= (int)floor(px + half); x++)
    for (int z = (int)floor(pz - half); z <= (int)floor(pz + half); z++)
      if (water_at(x, (int)floor(py), z) > 0 || water_at(x, (int)floor(py + 0.5), z) > 0) return true;
  return false;
}

#include "gen.h"
void spawn_pigs(int n) {
  for (int k = 0; k < n; k++) {
    int x = 0, z = 0; bool ok = false;
    for (int tries = 0; tries < 30 && !ok; tries++) {
      x = (int)floor(player.x) + (GetRandomValue(0, 1) ? 1 : -1) * (6 + GetRandomValue(0, 19));
      z = (int)floor(player.z) + (GetRandomValue(0, 1) ? 1 : -1) * (6 + GetRandomValue(0, 19));
      if (terrain_height(x, z) > SEA_Y) ok = true;
    }
    if (!ok) continue;
    for (int i = 0; i < MAX_PIGS; i++)
      if (!pigs[i].used) {
        pigs[i] = (Pig){ x + 0.5, terrain_height(x, z) + 1, z + 0.5, 0,
                         (float)GetRandomValue(0, 628) / 100.0f, 0, 6, true };
        break;
      }
  }
}

int pig_count(void) { int c = 0; for (int i = 0; i < MAX_PIGS; i++) if (pigs[i].used) c++; return c; }

static void kill_pig(Pig *p, bool drop) {
  p->used = false;
  if (drop) spawn_drop(I_PORK, 1 + GetRandomValue(0, 1), p->x, p->y, p->z, 0.5f);
}

static void update_pig(Pig *p, float dt) {
  bool in_w = pig_wet(p->x, p->y, p->z);
  p->wander -= dt;
  if (in_w) {
    // swim up and toward the highest nearby land
    int best = -9999;
    const float heads[4] = { 0, (float)M_PI, (float)M_PI / 2, (float)-M_PI / 2 };
    const int dirs[4][2] = { {0,1},{0,-1},{1,0},{-1,0} };
    for (int i = 0; i < 4; i++) {
      int th = terrain_height((int)floor(p->x) + dirs[i][0] * 4, (int)floor(p->z) + dirs[i][1] * 4);
      if (th > best) { best = th; p->heading = heads[i]; }
    }
    p->wander = 1;
  } else if (p->wander <= 0) {
    p->heading = (float)GetRandomValue(0, 628) / 100.0f;
    p->wander = 2 + (float)GetRandomValue(0, 300) / 100.0f;
  }
  const float speed = 1.3f;
  double mx = sinf(p->heading) * speed, mz = cosf(p->heading) * speed;

  if (in_w) p->vy = 1.6f; else p->vy += P_GRAVITY * dt;

  double nx = p->x + mx * dt;
  if (!pig_collides(nx, p->y, p->z) && (in_w || !pig_wet(nx, p->y, p->z))) p->x = nx; else p->wander = 0;
  double nz = p->z + mz * dt;
  if (!pig_collides(p->x, p->y, nz) && (in_w || !pig_wet(p->x, p->y, nz))) p->z = nz; else p->wander = 0;
  double ny = p->y + p->vy * dt;
  if (!pig_collides(p->x, ny, p->z)) p->y = ny; else p->vy = 0;

  if (p->y < -10) kill_pig(p, false);
}

void update_pigs(float dt) {
  for (int i = 0; i < MAX_PIGS; i++) if (pigs[i].used) update_pig(&pigs[i], dt);
}

bool attack_pig(void) {
  float cp = cosf(player.pitch);
  double dx = -sinf(player.yaw) * cp, dy = sinf(player.pitch), dz = -cosf(player.yaw) * cp;
  double ox = player.x, oy = player.y + P_EYE, oz = player.z;
  Pig *best = NULL; double best_dist = 4;
  for (int i = 0; i < MAX_PIGS; i++) {
    Pig *p = &pigs[i];
    if (!p->used) continue;
    double vx = p->x - ox, vy = (p->y + 0.5) - oy, vz = p->z - oz;
    double dist = sqrt(vx * vx + vy * vy + vz * vz);
    if (dist > 4) continue;
    double dot = vx * dx + vy * dy + vz * dz;
    if (dot <= 0) continue;
    double px = vx - dx * dot, py = vy - dy * dot, pz = vz - dz * dot;
    double perp = sqrt(px * px + py * py + pz * pz);
    if (perp < 0.6 && dist < best_dist) { best = p; best_dist = dist; }
  }
  if (!best) return false;
  Stack *held = &slots[sel_slot];
  best->health -= (held->id == I_SWORD) ? 4 : 1;
  best->x += dx * 0.4; best->z += dz * 0.4; best->vy = 4;
  if (best->health <= 0) kill_pig(best, true);
  return true;
}

// ============================== hostile mobs ==============================
// Humanoid boxels on the pig pipeline: classic 64x32 MC skin layout
// (head 8^3 @0,0; body 8x12x4 @16,16; arm 4x12x4 @40,16; leg 4x12x4 @0,16).
// Textures come from tools/make_mobs.mjs. Faces live on the -z unwrap
// region, so the body base rotates heading+PI to point them along movement.
typedef struct Mob {
  double x, y, z;
  double tx, ty, tz;                 // mirror mode: lerp target from the wire
  float vy, heading, wander, atk_cd, burn, fuse;   // fuse: creeper hiss (<0 idle)
  int health, type;
  uint8_t netflags;                  // mirror mode: bit0 = fuse flash
  bool used;
} Mob;
#define MAX_MOBS 32
static Mob mobs[MAX_MOBS];
static bool authority = true;        // offline / elected master: we simulate
static Material mob_mats[3];      // MOB_ZOMBIE, MOB_SKELETON, MOB_CREEPER

enum { HP_HEAD, HP_BODY, HP_ARM_L, HP_ARM_R, HP_LEG_L, HP_LEG_R, HP_N };
static Mesh hum_mesh[HP_N];
static Mesh creeper_leg;          // creeper reuses head+body meshes, short legs, no arms

typedef struct Arrow { double x, y, z; float vx, vy, vz, age; bool used; } Arrow;
#define MAX_ARROWS 64
static Arrow arrows[MAX_ARROWS];

static void mobs_init(void) {
  const char *tex[3] = { "assets/zombie.png", "assets/skeleton.png", "assets/creeper.png" };
  const Color fallback[3] = { { 70, 140, 70, 255 }, { 200, 200, 195, 255 }, { 70, 180, 70, 255 } };
  for (int t = 0; t < 3; t++) {
    mob_mats[t] = LoadMaterialDefault();
    if (FileExists(tex[t])) {
      Texture2D tx = LoadTexture(tex[t]);
      SetTextureFilter(tx, TEXTURE_FILTER_POINT);
      SetMaterialTexture(&mob_mats[t], MATERIAL_MAP_DIFFUSE, tx);
    } else {
      mob_mats[t].maps[MATERIAL_MAP_DIFFUSE].color = fallback[t];
    }
  }
  hum_mesh[HP_HEAD]  = pig_box(8, 8, 8, 0, 0);
  hum_mesh[HP_BODY]  = pig_box(8, 12, 4, 16, 16);
  hum_mesh[HP_ARM_L] = pig_box(4, 12, 4, 40, 16);
  hum_mesh[HP_ARM_R] = pig_box(4, 12, 4, 40, 16);
  hum_mesh[HP_LEG_L] = pig_box(4, 12, 4, 0, 16);
  hum_mesh[HP_LEG_R] = pig_box(4, 12, 4, 0, 16);
  creeper_leg = pig_box(4, 6, 4, 0, 16);
}

static bool mob_collides(double px, double py, double pz) {
  const double half = 0.3, h = 1.85;
  for (int x = (int)floor(px - half); x <= (int)floor(px + half); x++)
    for (int y = (int)floor(py); y <= (int)floor(py + h); y++)
      for (int z = (int)floor(pz - half); z <= (int)floor(pz + half); z++)
        if (!passable(get_voxel(x, y, z))) return true;
  return false;
}

// night window of the shared clock (0.25 noon, 0.75 midnight)
static bool is_night(void) { return day_time > 0.55 && day_time < 0.95; }

void spawn_mobs(int type, int n) {
  if (!authority) return;            // mirrors get their mobs from the wire
  for (int k = 0; k < n; k++) {
    int x = 0, z = 0; bool ok = false;
    for (int tries = 0; tries < 30 && !ok; tries++) {
      x = (int)floor(player.x) + (GetRandomValue(0, 1) ? 1 : -1) * (10 + GetRandomValue(0, 14));
      z = (int)floor(player.z) + (GetRandomValue(0, 1) ? 1 : -1) * (10 + GetRandomValue(0, 14));
      if (terrain_height(x, z) > SEA_Y) ok = true;
    }
    if (!ok) continue;
    for (int i = 0; i < MAX_MOBS; i++)
      if (!mobs[i].used) {
        mobs[i] = (Mob){ x + 0.5, terrain_height(x, z) + 1, z + 0.5, 0,
                         (float)GetRandomValue(0, 628) / 100.0f, 0, 0, 0, -1, 20, type, true };
        break;
      }
  }
}

int mob_count(void) { int c = 0; for (int i = 0; i < MAX_MOBS; i++) if (mobs[i].used) c++; return c; }

// nearest of: our own player + every remote player (the authority simulates
// for the whole lobby, so mobs must not tunnel-vision its local player)
static void nearest_player(const Mob *m, double *tx, double *ty, double *tz) {
  *tx = player.x; *ty = player.y; *tz = player.z;
  double best = (player.x - m->x) * (player.x - m->x) + (player.z - m->z) * (player.z - m->z);
  for (int i = 0; i < MAX_REMOTE; i++) {
    if (!remote_players[i].used) continue;
    double dx = remote_players[i].x - m->x, dz = remote_players[i].z - m->z;
    double d = dx * dx + dz * dz;
    if (d < best) { best = d; *tx = remote_players[i].x; *ty = remote_players[i].y; *tz = remote_players[i].z; }
  }
}

static void shoot_arrow(Mob *m, double px, double py, double pz) {
  double sx = m->x, sy = m->y + 1.4, sz = m->z;
  double dx = px - sx, dy = (py + 0.9) - sy, dz = pz - sz;
  double d = sqrt(dx * dx + dy * dy + dz * dz);
  if (d < 0.01) return;
  const float speed = 16.0f;
  for (int i = 0; i < MAX_ARROWS; i++)
    if (!arrows[i].used) {
      arrows[i] = (Arrow){ sx, sy, sz,
        (float)(dx / d) * speed, (float)(dy / d) * speed + 2.0f, (float)(dz / d) * speed, 0, true };
      return;
    }
}

static void update_mob(Mob *m, float dt) {
  m->atk_cd -= dt;
  double px, py, pz;
  nearest_player(m, &px, &py, &pz);
  double dx = px - m->x, dz = pz - m->z;
  double dist = sqrt(dx * dx + dz * dz);

  // daylight burns hostiles away (random short fuse so they don't pop at
  // once) — creepers are day-proof like the original
  if (!is_night() && m->type != MOB_CREEPER) {
    if (m->burn <= 0) m->burn = 1.0f + (float)GetRandomValue(0, 400) / 100.0f;
    m->burn -= dt;
    if (m->burn <= 0) { m->used = false; return; }
  }

  bool in_w = pig_wet(m->x, m->y, m->z);
  float move = 0;                      // -1 back, 0 hold, +1 forward
  if (m->type == MOB_CREEPER) {
    double dy = py - m->y;
    bool near = dist < 2.5 && dy > -2 && dy < 2;
    if (m->fuse >= 0) {                // hissing: stand still, count down
      m->fuse -= dt;
      if (dist > 5) m->fuse = -1;      // player escaped: disarm
      else if (m->fuse <= 0) {
        m->used = false;
        explode_at((int)floor(m->x), (int)floor(m->y + 0.5), (int)floor(m->z));
        return;
      }
    } else if (near) {
      m->fuse = 1.5f;                  // sssss...
    } else if (dist < 16 && dist > 0.1) {
      m->heading = atan2f((float)dx, (float)dz);
      move = 1;
    } else {
      m->wander -= dt;
      if (m->wander <= 0) {
        m->heading = (float)GetRandomValue(0, 628) / 100.0f;
        m->wander = 2 + (float)GetRandomValue(0, 300) / 100.0f;
      }
      move = 0.6f;
    }
  } else if (m->type == MOB_ZOMBIE) {
    if (dist < 16 && dist > 0.1) {
      m->heading = atan2f((float)dx, (float)dz);
      move = 1;
      // melee damage is NOT applied here: every client (authority included)
      // checks synced zombie positions against its OWN player in local_hazards
    } else {
      m->wander -= dt;
      if (m->wander <= 0) {
        m->heading = (float)GetRandomValue(0, 628) / 100.0f;
        m->wander = 2 + (float)GetRandomValue(0, 300) / 100.0f;
      }
      move = 0.6f;
    }
  } else {                             // skeleton: kite and shoot
    if (dist < 14 && dist > 0.1) {
      m->heading = atan2f((float)dx, (float)dz);
      if (dist > 11) move = 1;
      else if (dist < 7) move = -1;
      if (m->atk_cd <= 0 && dist > 3) { shoot_arrow(m, px, py, pz); m->atk_cd = 2.5f; }
    } else {
      m->wander -= dt;
      if (m->wander <= 0) {
        m->heading = (float)GetRandomValue(0, 628) / 100.0f;
        m->wander = 2 + (float)GetRandomValue(0, 300) / 100.0f;
      }
      move = 0.5f;
    }
  }

  const float speed = (m->type == MOB_SKELETON ? 1.5f : 1.9f) * move;
  double mx = sinf(m->heading) * speed, mz = cosf(m->heading) * speed;
  if (in_w) m->vy = 1.6f; else m->vy += P_GRAVITY * dt;

  bool blocked = false;
  double nx = m->x + mx * dt;
  if (!mob_collides(nx, m->y, m->z)) m->x = nx; else blocked = true;
  double nz = m->z + mz * dt;
  if (!mob_collides(m->x, m->y, nz)) m->z = nz; else blocked = true;
  bool on_ground = mob_collides(m->x, m->y - 0.05, m->z);
  if (blocked && on_ground && !in_w) m->vy = 8.0f;    // hop the 1-block step
  double ny = m->y + m->vy * dt;
  if (!mob_collides(m->x, ny, m->z)) m->y = ny; else m->vy = 0;

  if (m->y < -10) m->used = false;
}

static void update_arrows(float dt) {   // physics only; damage in local_hazards
  for (int i = 0; i < MAX_ARROWS; i++) {
    Arrow *a = &arrows[i];
    if (!a->used) continue;
    a->age += dt;
    a->vy += -14.0f * dt;
    a->x += a->vx * dt; a->y += a->vy * dt; a->z += a->vz * dt;
    if (!passable(get_voxel((int)floor(a->x), (int)floor(a->y), (int)floor(a->z)))) { a->used = false; continue; }
    if (a->age > 4.0f) a->used = false;
  }
}

// Runs on EVERY client against its own player: zombie contact + arrow hits.
// The authority moves the mobs; each player owns their health.
static float melee_cd[MAX_MOBS];
static float arrow_cd[MAX_ARROWS];
static void local_hazards(float dt) {
  for (int i = 0; i < MAX_MOBS; i++) {
    melee_cd[i] -= dt;
    Mob *m = &mobs[i];
    if (!m->used || m->type != MOB_ZOMBIE || melee_cd[i] > 0) continue;
    double dx = player.x - m->x, dz = player.z - m->z, dy = player.y - m->y;
    if (dx * dx + dz * dz < 1.5 * 1.5 && dy > -1.5 && dy < 1.5) { damage(3); melee_cd[i] = 1.2f; }
  }
  for (int i = 0; i < MAX_ARROWS; i++) {
    arrow_cd[i] -= dt;
    if (!arrows[i].used || arrow_cd[i] > 0) continue;
    double px = player.x - arrows[i].x, py = (player.y + 0.9) - arrows[i].y, pz = player.z - arrows[i].z;
    if (px * px + py * py + pz * pz < 0.6 * 0.6) {
      damage(3);
      arrow_cd[i] = 1.0f;              // snapshots may re-import this slot
      if (authority) arrows[i].used = false;
    }
  }
}

void entities_set_authority(bool is_master) { authority = is_master; }

int mobs_export(PMob *out, PArrow *aout, int *na) {
  int n = 0;
  for (int i = 0; i < MAX_MOBS && n < P_MOBS_MAX; i++) {
    Mob *m = &mobs[i];
    if (!m->used) continue;
    out[n++] = (PMob){ (uint8_t)i, (uint8_t)m->type,
                       (uint8_t)((m->type == MOB_CREEPER && m->fuse >= 0) ? 1 : 0),
                       (uint8_t)((m->heading < 0 ? m->heading + 2 * (float)M_PI : m->heading) / (2 * (float)M_PI) * 255.0f),
                       (float)m->x, (float)m->y, (float)m->z };
  }
  *na = 0;
  for (int i = 0; i < MAX_ARROWS && *na < P_ARROWS_MAX; i++)
    if (arrows[i].used) aout[(*na)++] = (PArrow){ (float)arrows[i].x, (float)arrows[i].y, (float)arrows[i].z };
  return n;
}

void mobs_import(const PMob *in, int n, const PArrow *ain, int na) {
  if (authority) return;
  bool seen[MAX_MOBS] = { false };
  for (int i = 0; i < n; i++) {
    int s = in[i].slot;
    if (s < 0 || s >= MAX_MOBS) continue;
    Mob *m = &mobs[s];
    if (!m->used) {                    // newly appeared: snap, don't glide in
      memset(m, 0, sizeof *m);
      m->used = true;
      m->x = in[i].x; m->y = in[i].y; m->z = in[i].z;
    }
    m->type = in[i].type;
    m->netflags = in[i].flags;
    m->heading = in[i].yaw256 / 255.0f * 2 * (float)M_PI;
    m->tx = in[i].x; m->ty = in[i].y; m->tz = in[i].z;
    seen[s] = true;
  }
  for (int s = 0; s < MAX_MOBS; s++) if (!seen[s]) mobs[s].used = false;
  for (int i = 0; i < MAX_ARROWS; i++) {
    arrows[i].used = i < na;
    if (i < na) { arrows[i].x = ain[i].x; arrows[i].y = ain[i].y; arrows[i].z = ain[i].z; }
  }
}

void mob_apply_hit(int slot, int dmg, float kx, float kz) {
  if (slot < 0 || slot >= MAX_MOBS || !mobs[slot].used) return;
  mobs[slot].health -= dmg;
  mobs[slot].x += kx * 0.4; mobs[slot].z += kz * 0.4; mobs[slot].vy = 4;
  if (mobs[slot].health <= 0) mobs[slot].used = false;
}

void update_mobs(float dt) {
  if (authority) {
    // night spawner: keep some pressure on, capped
    if (is_night() && mob_count() < 10 && GetRandomValue(0, 1000) < (int)(dt * 250)) {
      int roll = GetRandomValue(0, 5);
      spawn_mobs(roll < 3 ? MOB_ZOMBIE : roll < 5 ? MOB_SKELETON : MOB_CREEPER, 1);
    }
    for (int i = 0; i < MAX_MOBS; i++) if (mobs[i].used) update_mob(&mobs[i], dt);
    update_arrows(dt);
  } else {
    // mirror mode: glide toward the last 10 Hz snapshot
    float u = dt * 12.0f; if (u > 1) u = 1;
    for (int i = 0; i < MAX_MOBS; i++) {
      Mob *m = &mobs[i];
      if (!m->used) continue;
      m->x += (m->tx - m->x) * u;
      m->y += (m->ty - m->y) * u;
      m->z += (m->tz - m->z) * u;
    }
  }
  local_hazards(dt);
}

// shared look-cone hit test (same math as the pig attack)
static bool hit_cone(double cx, double cy, double cz, double *out_dist,
                     double dx, double dy, double dz) {
  double ox = player.x, oy = player.y + P_EYE, oz = player.z;
  double vx = cx - ox, vy = cy - oy, vz = cz - oz;
  double dist = sqrt(vx * vx + vy * vy + vz * vz);
  if (dist > 4) return false;
  double dot = vx * dx + vy * dy + vz * dz;
  if (dot <= 0) return false;
  double px = vx - dx * dot, py = vy - dy * dot, pz = vz - dz * dot;
  if (sqrt(px * px + py * py + pz * pz) >= 0.7) return false;
  *out_dist = dist;
  return true;
}

bool attack_creature(void) {
  float cp = cosf(player.pitch);
  double dx = -sinf(player.yaw) * cp, dy = sinf(player.pitch), dz = -cosf(player.yaw) * cp;
  Mob *bm = NULL; double best = 4, d;
  for (int i = 0; i < MAX_MOBS; i++)
    if (mobs[i].used && hit_cone(mobs[i].x, mobs[i].y + 1.0, mobs[i].z, &d, dx, dy, dz) && d < best) { bm = &mobs[i]; best = d; }
  if (bm) {
    Stack *held = &slots[sel_slot];
    int dmg = (held->id == I_SWORD) ? 4 : 1;
    if (authority) {
      mob_apply_hit((int)(bm - mobs), dmg, (float)dx, (float)dz);
    } else {
      net_send_mob_hit((uint8_t)(bm - mobs), (uint8_t)dmg, (float)dx, (float)dz);
    }
    return true;
  }
  return attack_pig();
}

// ============================== drawing ==============================
static float drop_spin = 0;

void draw_entities(Camera3D cam) {
  drop_spin += GetFrameTime() * 1.5f;
  Material *cube_mat = block_cube_material();

  for (int i = 0; i < MAX_FALLING; i++) {
    if (!falling[i].used) continue;
    Matrix t = MatrixTranslate(falling[i].x + 0.5f, (float)falling[i].y + 0.5f, falling[i].z + 0.5f);
    DrawMesh(sand_cube, *cube_mat, t);
  }

  for (int i = 0; i < MAX_TNT; i++) {
    if (!primed[i].used) continue;
    PrimedTNT *p = &primed[i];
    Matrix t = MatrixTranslate(p->x + 0.5f, (float)p->y + 0.5f, p->z + 0.5f);
    DrawMesh(tnt_cube, *cube_mat, t);
    // white flash shell blinking with the fuse
    float op = 0.2f + 0.4f * fabsf(sinf(p->fuse * 12.0f));
    DrawCube((Vector3){ p->x + 0.5f, (float)p->y + 0.5f, p->z + 0.5f }, 1.04f, 1.04f, 1.04f,
             (Color){ 255, 255, 255, (uint8_t)(op * 255) });
  }

  for (int i = 0; i < MAX_DROPS; i++) {
    Drop *d = &drops[i];
    if (!d->used) continue;
    if (is_block(d->id) && d->id != B_TORCH) {
      Matrix t = MatrixMultiply(MatrixMultiply(MatrixScale(0.3f, 0.3f, 0.3f), MatrixRotateY(drop_spin)),
                                MatrixTranslate((float)d->x, (float)d->y + 0.2f, (float)d->z));
      DrawMesh(*cube_for(d->id), *cube_mat, t);
    } else if (items_tex.id != 0 && item_tile(d->id) >= 0) {
      int tile = item_tile(d->id), col = tile % 4, row = tile / 4;
      Rectangle src = { col * 64.0f, row * 64.0f, 64, 64 };
      DrawBillboardRec(cam, items_tex, src, (Vector3){ (float)d->x, (float)d->y + 0.4f, (float)d->z },
                       (Vector2){ 0.5f, 0.5f }, WHITE);
    } else {
      DrawCube((Vector3){ (float)d->x, (float)d->y + 0.3f, (float)d->z }, 0.25f, 0.25f, 0.25f, (Color){ 204, 170, 136, 255 });
    }
  }

  for (int i = 0; i < MAX_PIGS; i++) {
    Pig *p = &pigs[i];
    if (!p->used) continue;
    Matrix base = MatrixMultiply(MatrixRotateY(p->heading), MatrixTranslate((float)p->x, (float)p->y, (float)p->z));
    for (int k = 0; k < n_pig_parts; k++) {
      PigPart *pp = &pig_parts[k];
      Matrix local = MatrixTranslate(pp->offset.x, pp->offset.y, pp->offset.z);
      if (pp->rot_body) local = MatrixMultiply(MatrixRotateX(-(float)M_PI / 2), local);
      if (pp->flip) local = MatrixMultiply(MatrixRotateY((float)M_PI), local);
      DrawMesh(pp->mesh, pig_mat, MatrixMultiply(local, base));
    }
  }

  // hostile mobs: faces sit on the -z unwrap region -> base spins heading+PI
  static const Vector3 hum_off[HP_N] = {
    { 0, 1.75f, 0 }, { 0, 1.125f, 0 },
    { -0.375f, 1.125f, 0 }, { 0.375f, 1.125f, 0 },
    { -0.125f, 0.375f, 0 }, { 0.125f, 0.375f, 0 },
  };
  for (int i = 0; i < MAX_MOBS; i++) {
    Mob *m = &mobs[i];
    if (!m->used) continue;
    Matrix base = MatrixMultiply(MatrixRotateY(m->heading + (float)M_PI),
                                 MatrixTranslate((float)m->x, (float)m->y, (float)m->z));
    if (m->type == MOB_CREEPER) {
      DrawMesh(hum_mesh[HP_HEAD], mob_mats[m->type],
               MatrixMultiply(MatrixTranslate(0, 1.375f, 0), base));
      DrawMesh(hum_mesh[HP_BODY], mob_mats[m->type],
               MatrixMultiply(MatrixTranslate(0, 0.75f, 0), base));
      for (int lx = -1; lx <= 1; lx += 2)
        for (int lz = -1; lz <= 1; lz += 2)
          DrawMesh(creeper_leg, mob_mats[m->type],
                   MatrixMultiply(MatrixTranslate(lx * 0.125f, 0.1875f, lz * 0.25f), base));
      if (m->fuse >= 0 || (m->netflags & 1)) {   // hissing (mirrors: wire flag)
        float op = 0.25f + 0.45f * fabsf(sinf((m->fuse >= 0 ? m->fuse : (float)GetTime()) * 14.0f));
        DrawCube((Vector3){ (float)m->x, (float)m->y + 0.9f, (float)m->z }, 0.9f, 1.9f, 0.9f,
                 (Color){ 255, 255, 255, (uint8_t)(op * 255) });
      }
      continue;
    }
    for (int k = 0; k < HP_N; k++) {
      Matrix local;
      if (m->type == MOB_ZOMBIE && (k == HP_ARM_L || k == HP_ARM_R)) {
        // iconic zombie arms: rotated forward 90° about the shoulder
        local = MatrixMultiply(MatrixMultiply(MatrixTranslate(0, -0.375f, 0), MatrixRotateX(-(float)M_PI / 2)),
                               MatrixTranslate(hum_off[k].x, 1.5f, 0));
      } else {
        local = MatrixTranslate(hum_off[k].x, hum_off[k].y, hum_off[k].z);
      }
      DrawMesh(hum_mesh[k], mob_mats[m->type], MatrixMultiply(local, base));
    }
  }

  for (int i = 0; i < MAX_ARROWS; i++) {
    if (!arrows[i].used) continue;
    DrawCube((Vector3){ (float)arrows[i].x, (float)arrows[i].y, (float)arrows[i].z },
             0.12f, 0.12f, 0.12f, (Color){ 120, 96, 64, 255 });
  }
}

void draw_world_held(Camera3D cam, int id, Vector3 pos, float yaw) {
  if (id <= 0) return;
  if (is_block(id) && id != B_TORCH) {
    Matrix t = MatrixMultiply(MatrixMultiply(MatrixScale(0.25f, 0.25f, 0.25f), MatrixRotateY(yaw)),
                              MatrixTranslate(pos.x, pos.y, pos.z));
    DrawMesh(*cube_for(id), *block_cube_material(), t);
  } else if (items_tex.id != 0 && item_tile(id) >= 0) {
    int tile = item_tile(id), col = tile % 4, row = tile / 4;
    Rectangle src = { col * 64.0f, row * 64.0f, 64, 64 };
    DrawBillboardRec(cam, items_tex, src, pos, (Vector2){ 0.4f, 0.4f }, WHITE);
  }
}

