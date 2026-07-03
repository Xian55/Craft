#include "mesh.h"
#include "light.h"
#include "prof.h"
#include "rlgl.h"
#include "raymath.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

// --- Atlas layout (game.js:215-225) ---
#define TILE 64
#define ATLAS_COLS 4
static int ATLAS_ROWS = 5;
static float ATLAS_W = ATLAS_COLS * TILE, ATLAS_H = 5 * TILE;

enum {
  T_GRASS_TOP = 0, T_GRASS_SIDE = 1, T_DIRT = 2, T_STONE = 3,
  T_WOOD_TOP = 4, T_WOOD_SIDE = 5, T_LEAVES = 6, T_SAND = 7,
  T_PLANKS = 8, T_COBBLE = 9, T_STONE_BRICK = 10, T_BRICK = 11,
  T_WATER = 12, T_TORCH = 13, T_TNT_SIDE = 14, T_TNT_TOP = 15,
  T_TNT_BOTTOM = 16, T_CHEST = 17,
};

// face: 0=top 1=bottom 2=side (JS uses strings)
enum { F_TOP, F_BOTTOM, F_SIDE };

static int tile_for(uint8_t block, int face) {
  switch (block) {
    case B_GRASS:  return face == F_TOP ? T_GRASS_TOP : face == F_BOTTOM ? T_DIRT : T_GRASS_SIDE;
    case B_DIRT:   return T_DIRT;
    case B_STONE:  return T_STONE;
    case B_WOOD:   return face == F_SIDE ? T_WOOD_SIDE : T_WOOD_TOP;
    case B_LEAVES: return T_LEAVES;
    case B_SAND:   return T_SAND;
    case B_PLANKS: return T_PLANKS;
    case B_COBBLE: return T_COBBLE;
    case B_STONE_BRICK: return T_STONE_BRICK;
    case B_BRICK:  return T_BRICK;
    case B_TORCH:  return T_TORCH;
    case B_CHEST:  return T_CHEST;
    case B_TNT:    return face == F_TOP ? T_TNT_TOP : face == F_BOTTOM ? T_TNT_BOTTOM : T_TNT_SIDE;
    default:       return T_STONE;
  }
}

// FACES table verbatim from game.js:362-369. corners: x,y,z,u,v
typedef struct { int name; int dir[3]; int corners[4][5]; } FaceDef;
static const FaceDef FACES[6] = {
  { F_SIDE,   {-1, 0, 0}, {{0,1,0,0,1},{0,0,0,0,0},{0,1,1,1,1},{0,0,1,1,0}} },
  { F_SIDE,   { 1, 0, 0}, {{1,1,1,0,1},{1,0,1,0,0},{1,1,0,1,1},{1,0,0,1,0}} },
  { F_BOTTOM, { 0,-1, 0}, {{1,0,1,1,0},{0,0,1,0,0},{1,0,0,1,1},{0,0,0,0,1}} },
  { F_TOP,    { 0, 1, 0}, {{0,1,1,1,1},{1,1,1,0,1},{0,1,0,1,0},{1,1,0,0,0}} },
  { F_SIDE,   { 0, 0,-1}, {{1,0,0,0,0},{0,0,0,1,0},{1,1,0,0,1},{0,1,0,1,1}} },
  { F_SIDE,   { 0, 0, 1}, {{0,0,1,0,0},{1,0,1,1,0},{0,1,1,0,1},{1,1,1,1,1}} },
};

// --- Growable mesh builder ---
typedef struct {
  float *pos, *nrm, *uv; uint8_t *col; uint16_t *idx;
  int verts, tris, cap_v, cap_i;
  float vscale;   // fluid textures are vertical animation strips: v *= 1/frames
} Builder;

static void b_reserve(Builder *b, int add_v, int add_i) {
  if (b->verts + add_v > b->cap_v) {
    b->cap_v = b->cap_v ? b->cap_v * 2 : 4096;
    if (b->cap_v < b->verts + add_v) b->cap_v = b->verts + add_v;
    b->pos = realloc(b->pos, b->cap_v * 3 * sizeof(float));
    b->nrm = realloc(b->nrm, b->cap_v * 3 * sizeof(float));
    b->uv  = realloc(b->uv,  b->cap_v * 2 * sizeof(float));
    b->col = realloc(b->col, b->cap_v * 4);
  }
  if (b->tris * 3 + add_i > b->cap_i) {
    b->cap_i = b->cap_i ? b->cap_i * 2 : 8192;
    if (b->cap_i < b->tris * 3 + add_i) b->cap_i = b->tris * 3 + add_i;
    b->idx = realloc(b->idx, b->cap_i * sizeof(uint16_t));
  }
}
static void b_free(Builder *b) { free(b->pos); free(b->nrm); free(b->uv); free(b->col); free(b->idx); memset(b, 0, sizeof(*b)); }

// raylib Mesh.indices are uint16 — 65532 is the last full-quad-aligned count.
#define MAX_VERTS 65532

static void b_vertex(Builder *b, float x, float y, float z,
                     float nx, float ny, float nz, float u, float v,
                     uint8_t r, uint8_t g, uint8_t bl, uint8_t a) {
  int i = b->verts;
  b->pos[i*3] = x; b->pos[i*3+1] = y; b->pos[i*3+2] = z;
  b->nrm[i*3] = nx; b->nrm[i*3+1] = ny; b->nrm[i*3+2] = nz;
  b->uv[i*2] = u; b->uv[i*2+1] = v;
  b->col[i*4] = r; b->col[i*4+1] = g; b->col[i*4+2] = bl; b->col[i*4+3] = a;
  b->verts++;
}
static void b_quad_indices(Builder *b, int start) {
  uint16_t *p = b->idx + b->tris * 3;
  p[0] = start; p[1] = start + 1; p[2] = start + 2;
  p[3] = start + 2; p[4] = start + 1; p[5] = start + 3;
  b->tris += 2;
}

// Two-channel bake (game.js vertex colors): r = sky light, g = block light
// (torch/lava). The solid shader computes max(g, r * uDaylight) so nights
// darken the sky channel but torches keep glowing.
static uint8_t to_u8(float v) { if (v > 1.0f) v = 1.0f; if (v < 0.0f) v = 0.0f; return (uint8_t)(v * 255.0f + 0.5f); }

// pushFace (game.js:395): one block face, 4 corners with per-corner shade.
static void push_face(Builder *b, int x, int y, int z, const FaceDef *f, int tile,
                      float shades[4][2]) {
  if (b->verts + 4 > MAX_VERTS) return;   // spill guard (real chunks stay far below)
  b_reserve(b, 4, 6);
  int col = tile % ATLAS_COLS, row = tile / ATLAS_COLS;
  int start = b->verts;
  for (int k = 0; k < 4; k++) {
    const int *c = f->corners[k];
    // three.js UV origin is bottom-left; raylib/OpenGL textures load with row 0
    // at top and raylib does not flip -> v_gl = 1 - v_three.
    float u = (col + c[3]) * TILE / ATLAS_W;
    float v = (row + 1 - c[4]) * TILE / ATLAS_H;
    b_vertex(b, (float)(x + c[0]), (float)(y + c[1]), (float)(z + c[2]),
             (float)f->dir[0], (float)f->dir[1], (float)f->dir[2], u, v,
             to_u8(shades[k][0]), to_u8(shades[k][1]), 0, 255);
  }
  b_quad_indices(b, start);
}

// addTorch (game.js:521): thin stick, no bottom face, fixed sub-tile UVs.
static void add_torch(Builder *b, int x, int y, int z) {
  float sky = powf(0.8f, 15.0f - (float)get_light_sky(x, y, z));
  if (sky < 0.06f) sky = 0.06f;
  float blk = powf(0.8f, 15.0f - (float)get_light_blk(x, y, z));
  uint8_t cr = to_u8(sky), cg = to_u8(blk);
  int c0 = T_TORCH % ATLAS_COLS, r0 = T_TORCH / ATLAS_COLS;
  const float W[2] = { 0.4375f, 0.5625f }, H = 0.625f;
  for (int fi = 0; fi < 6; fi++) {
    const FaceDef *f = &FACES[fi];
    if (f->name == F_BOTTOM) continue;
    int top = f->name == F_TOP;
    float uMin = 0.4375f, uMax = 0.5625f, vMin = top ? 0.5f : 0.0f, vMax = 0.625f;
    if (b->verts + 4 > MAX_VERTS) return;
    b_reserve(b, 4, 6);
    int start = b->verts;
    for (int k = 0; k < 4; k++) {
      const int *c = f->corners[k];
      float ul = uMin + c[3] * (uMax - uMin), vl = vMin + c[4] * (vMax - vMin);
      float u = (c0 + ul) * TILE / ATLAS_W;
      float v = (r0 + 1 - vl) * TILE / ATLAS_H;
      b_vertex(b, x + W[c[0]], y + c[1] * H, z + W[c[2]],
               (float)f->dir[0], (float)f->dir[1], (float)f->dir[2], u, v, cr, cg, 0, 255);
    }
    b_quad_indices(b, start);
  }
}

// --- Materials / textures ---
static Texture2D atlas_tex;
static Material mat_solid, mat_wstill, mat_wflow, mat_lstill, mat_lflow;
static Shader solid_shader;
static int u_daylight = -1;
static float daylight_now = 1.0f;
static bool inited = false;

// Solid-world shader: vertex color r = sky, g = block light.
// final light = max(g, r * uDaylight); alpha-test 0.5 like the JS material.
#ifdef __EMSCRIPTEN__
#define GLSL_V "#version 100\n"
#define GLSL_F "#version 100\nprecision mediump float;\n"
#define IN_V   "attribute"
#define OUT_V  "varying"
#define IN_F   "varying"
#define TEX2D  "texture2D"
#define FRAGCOLOR "gl_FragColor"
#define FRAG_DECL ""
#else
#define GLSL_V "#version 330\n"
#define GLSL_F "#version 330\n"
#define IN_V   "in"
#define OUT_V  "out"
#define IN_F   "in"
#define TEX2D  "texture"
#define FRAGCOLOR "finalColor"
#define FRAG_DECL "out vec4 finalColor;\n"
#endif

static const char *SOLID_VS =
  GLSL_V
  IN_V " vec3 vertexPosition;\n"
  IN_V " vec2 vertexTexCoord;\n"
  IN_V " vec4 vertexColor;\n"
  OUT_V " vec2 fragTexCoord;\n"
  OUT_V " vec4 fragColor;\n"
  "uniform mat4 mvp;\n"
  "void main() {\n"
  "  fragTexCoord = vertexTexCoord;\n"
  "  fragColor = vertexColor;\n"
  "  gl_Position = mvp * vec4(vertexPosition, 1.0);\n"
  "}\n";

static const char *SOLID_FS =
  GLSL_F
  IN_F " vec2 fragTexCoord;\n"
  IN_F " vec4 fragColor;\n"
  "uniform sampler2D texture0;\n"
  "uniform vec4 colDiffuse;\n"
  "uniform float uDaylight;\n"
  FRAG_DECL
  "void main() {\n"
  "  vec4 texel = " TEX2D "(texture0, fragTexCoord);\n"
  "  if (texel.a < 0.5) discard;\n"
  "  float light = max(fragColor.g, fragColor.r * uDaylight);\n"
  "  " FRAGCOLOR " = vec4(texel.rgb * light, 1.0) * colDiffuse;\n"
  "}\n";

void mesh_set_daylight(float b) {
  daylight_now = b;
  if (u_daylight >= 0) SetShaderValue(solid_shader, u_daylight, &daylight_now, SHADER_UNIFORM_FLOAT);
  // water darkens with the day; lava glows unchanged (game.js:2343)
  Color wc = { (uint8_t)(63 * b), (uint8_t)(121 * b), (uint8_t)(208 * b), 204 };
  mat_wstill.maps[MATERIAL_MAP_DIFFUSE].color = wc;
  mat_wflow.maps[MATERIAL_MAP_DIFFUSE].color = wc;
}

static Texture2D load_or_checker(const char *path, Color fallback) {
  Texture2D t = {0};
  if (FileExists(path)) t = LoadTexture(path);
  if (t.id == 0) {
    Image img = GenImageColor(16, 16, fallback);
    t = LoadTextureFromImage(img);
    UnloadImage(img);
  }
  SetTextureFilter(t, TEXTURE_FILTER_POINT);
  return t;
}

void mesh_init(void) {
  atlas_tex = load_or_checker("assets/atlas.png", MAGENTA);
  ATLAS_ROWS = atlas_tex.height / TILE > 0 ? atlas_tex.height / TILE : 5;
  ATLAS_H = (float)(ATLAS_ROWS * TILE);
  ATLAS_W = (float)atlas_tex.width > 0 ? (float)atlas_tex.width : ATLAS_COLS * TILE;

  mat_solid = LoadMaterialDefault();
  SetMaterialTexture(&mat_solid, MATERIAL_MAP_DIFFUSE, atlas_tex);
  solid_shader = LoadShaderFromMemory(SOLID_VS, SOLID_FS);
  if (solid_shader.id != 0) {
    mat_solid.shader = solid_shader;
    u_daylight = GetShaderLocation(solid_shader, "uDaylight");
    float one = 1.0f;
    SetShaderValue(solid_shader, u_daylight, &one, SHADER_UNIFORM_FLOAT);
  }

  // Water tint 0x3f79d0 at opacity 0.8, lava fullbright (game.js:2214-2222).
  Texture2D wstill = load_or_checker("assets/water.png",     (Color){ 63, 121, 208, 255 });
  Texture2D wflow  = load_or_checker("assets/waterflow.png", (Color){ 63, 121, 208, 255 });
  Texture2D lstill = load_or_checker("assets/lava.png",      (Color){ 207, 84, 16, 255 });
  Texture2D lflow  = load_or_checker("assets/lavaflow.png",  (Color){ 207, 84, 16, 255 });
  mat_wstill = LoadMaterialDefault();
  SetMaterialTexture(&mat_wstill, MATERIAL_MAP_DIFFUSE, wstill);
  mat_wstill.maps[MATERIAL_MAP_DIFFUSE].color = (Color){ 63, 121, 208, 204 };
  mat_wflow = LoadMaterialDefault();
  SetMaterialTexture(&mat_wflow, MATERIAL_MAP_DIFFUSE, wflow);
  mat_wflow.maps[MATERIAL_MAP_DIFFUSE].color = (Color){ 63, 121, 208, 204 };
  mat_lstill = LoadMaterialDefault();
  SetMaterialTexture(&mat_lstill, MATERIAL_MAP_DIFFUSE, lstill);
  mat_lstill.maps[MATERIAL_MAP_DIFFUSE].color = (Color){ 255, 255, 255, 230 };
  mat_lflow = LoadMaterialDefault();
  SetMaterialTexture(&mat_lflow, MATERIAL_MAP_DIFFUSE, lflow);
  mat_lflow.maps[MATERIAL_MAP_DIFFUSE].color = (Color){ 255, 255, 255, 230 };

  inited = true;
}

// Upload a Builder into a raylib Mesh (takes ownership of the arrays).
static bool upload(Builder *b, Mesh *out) {
  if (b->tris == 0) { b_free(b); return false; }
  Mesh m = {0};
  m.vertexCount = b->verts;
  m.triangleCount = b->tris;
  m.vertices  = b->pos;
  m.normals   = b->nrm;
  m.texcoords = b->uv;
  m.colors    = b->col;
  m.indices   = b->idx;
  UploadMesh(&m, false);
  memset(b, 0, sizeof(*b));   // raylib owns the arrays now (freed by UnloadMesh)
  *out = m;
  return true;
}

// buildChunkSolid (game.js:547)
static void build_chunk_solid(int cx, int cz) {
  Chunk *c = ensure_chunk(cx, cz);
  compute_light(c);           // fresh light for current blocks (JS does this too)
  Builder b = {0};
  int x0 = cx * CHUNK, z0 = cz * CHUNK;
  for (int x = x0; x < x0 + CHUNK; x++)
    for (int z = z0; z < z0 + CHUNK; z++)
      for (int y = 0; y < WORLD_H; y++) {
        uint8_t block = c->blocks[LI(x - x0, y, z - z0)];
        if (block == B_AIR) continue;
        if (block == B_TORCH) { add_torch(&b, x, y, z); continue; }
        for (int fi = 0; fi < 6; fi++) {
          const FaceDef *f = &FACES[fi];
          if (occludes(get_voxel(x + f->dir[0], y + f->dir[1], z + f->dir[2]))) continue;
          int dx = f->dir[0], dy = f->dir[1];
          float dir_f = dy > 0 ? 1.0f : dy < 0 ? 0.45f : dx != 0 ? 0.6f : 0.8f;
          int n_axis = dx != 0 ? 0 : dy != 0 ? 1 : 2;
          int tang[2], ti = 0;
          for (int a = 0; a < 3; a++) if (a != n_axis) tang[ti++] = a;
          float shades[4][2];
          for (int k = 0; k < 4; k++)
            corner_shade(x, y, z, f->dir, f->corners[k], tang, dir_f, shades[k]);
          push_face(&b, x, y, z, f, tile_for(block, f->name), shades);
        }
      }
  if (c->has_solid) { UnloadMesh(c->mesh_solid); c->has_solid = false; }
  c->has_solid = upload(&b, &c->mesh_solid);
}

// --- Fluids (game.js:575-656) ---
typedef int (*AmtFn)(int, int, int);

static double fluid_height(AmtFn amt, int x, int y, int z) {
  if (amt(x, y, z) == 0) return 0;
  if (amt(x, y + 1, z) > 0) return 1;
  return amt(x, y, z) / 8.0;
}

static double corner_height(AmtFn amt, int x, int y, int z, int dx, int dz) {
  double total = 0; int count = 0;
  const int o[4][2] = {{0,0},{dx,0},{0,dz},{dx,dz}};
  for (int i = 0; i < 4; i++) {
    int ox = o[i][0], oz = o[i][1];
    if (amt(x + ox, y, z + oz) > 0 && amt(x + ox, y + 1, z + oz) > 0) return 1;
    double h = fluid_height(amt, x + ox, y, z + oz);
    if (h > 0) { total += h; count++; }
  }
  return count > 0 ? total / count : 0;
}

// pushQuad (game.js:615): verts are {x,y,z,u,v}, shade into vertex color.
static void push_quad(Builder *b, const float v[4][5], const float n[3], float shade) {
  if (b->verts + 4 > MAX_VERTS) return;
  b_reserve(b, 4, 6);
  uint8_t g = (uint8_t)((shade > 1 ? 1 : shade) * 255.0f + 0.5f);
  float vs = b->vscale > 0 ? b->vscale : 1.0f;
  int start = b->verts;
  for (int k = 0; k < 4; k++)
    b_vertex(b, v[k][0], v[k][1], v[k][2], n[0], n[1], n[2], v[k][3], v[k][4] * vs, g, g, g, 255);
  b_quad_indices(b, start);
}

static void emit_fluid(Builder *b, AmtFn amt, int x, int y, int z, float shade) {
  float h00 = (float)corner_height(amt, x, y, z, -1, -1);
  float h10 = (float)corner_height(amt, x, y, z,  1, -1);
  float h01 = (float)corner_height(amt, x, y, z, -1,  1);
  float h11 = (float)corner_height(amt, x, y, z,  1,  1);
  float X = (float)x, Y = (float)y, Z = (float)z;
  if (amt(x, y + 1, z) == 0) {
    float v[4][5] = {{X,Y+h00,Z,0,0},{X+1,Y+h10,Z,1,0},{X,Y+h01,Z+1,0,1},{X+1,Y+h11,Z+1,1,1}};
    push_quad(b, v, (float[3]){0,1,0}, shade);
  }
  #define DRAW_SIDE(dx,dz) (get_voxel(x+(dx), y, z+(dz)) == B_AIR && amt(x+(dx), y, z+(dz)) == 0)
  if (DRAW_SIDE(1,0)) {
    float v[4][5] = {{X+1,Y+h10,Z,0,1},{X+1,Y,Z,0,0},{X+1,Y+h11,Z+1,1,1},{X+1,Y,Z+1,1,0}};
    push_quad(b, v, (float[3]){1,0,0}, shade);
  }
  if (DRAW_SIDE(-1,0)) {
    float v[4][5] = {{X,Y+h00,Z,0,1},{X,Y,Z,0,0},{X,Y+h01,Z+1,1,1},{X,Y,Z+1,1,0}};
    push_quad(b, v, (float[3]){-1,0,0}, shade);
  }
  if (DRAW_SIDE(0,-1)) {
    float v[4][5] = {{X,Y+h00,Z,0,1},{X,Y,Z,0,0},{X+1,Y+h10,Z,1,1},{X+1,Y,Z,1,0}};
    push_quad(b, v, (float[3]){0,0,-1}, shade);
  }
  if (DRAW_SIDE(0,1)) {
    float v[4][5] = {{X,Y+h01,Z+1,0,1},{X,Y,Z+1,0,0},{X+1,Y+h11,Z+1,1,1},{X+1,Y,Z+1,1,0}};
    push_quad(b, v, (float[3]){0,0,1}, shade);
  }
  #undef DRAW_SIDE
  if (get_voxel(x, y - 1, z) == B_AIR && amt(x, y - 1, z) == 0) {
    float v[4][5] = {{X,Y,Z,0,0},{X+1,Y,Z,1,0},{X,Y,Z+1,0,1},{X+1,Y,Z+1,1,1}};
    push_quad(b, v, (float[3]){0,-1,0}, shade);
  }
}

static int water_column_depth(int x, int y, int z) {
  int top = y; while (water_at(x, top + 1, z) > 0) top++;
  int bot = y; while (water_at(x, bot - 1, z) > 0) bot--;
  return top - bot + 1;
}

// buildChunkWater (game.js:634)
static void build_chunk_water(int cx, int cz) {
  Chunk *c = ensure_chunk(cx, cz);
  Builder S = {0}, F = {0}, LS = {0}, LF = {0};
  // frame counts of the animation strips: water 32, waterflow 32, lava 20, lavaflow 16
  S.vscale = 1.0f / 32.0f; F.vscale = 1.0f / 32.0f;
  LS.vscale = 1.0f / 20.0f; LF.vscale = 1.0f / 16.0f;
  int x0 = cx * CHUNK, z0 = cz * CHUNK;
  for (int x = x0; x < x0 + CHUNK; x++)
    for (int z = z0; z < z0 + CHUNK; z++)
      for (int y = 0; y < WORLD_H; y++) {
        if (water_at(x, y, z) > 0) {
          Builder *t = raw_water(x, y, z) == 9 ? &S : &F;
          float sh = 1.0f - (water_column_depth(x, y, z) - 1) * 0.08f;
          emit_fluid(t, water_at, x, y, z, sh < 0.4f ? 0.4f : sh);
        } else if (lava_at(x, y, z) > 0) {
          Builder *t = raw_water(x, y, z) == 19 ? &LS : &LF;
          emit_fluid(t, lava_at, x, y, z, 1.0f);
        }
      }
  if (c->has_wstill) { UnloadMesh(c->mesh_wstill); c->has_wstill = false; }
  if (c->has_wflow)  { UnloadMesh(c->mesh_wflow);  c->has_wflow = false; }
  if (c->has_lstill) { UnloadMesh(c->mesh_lstill); c->has_lstill = false; }
  if (c->has_lflow)  { UnloadMesh(c->mesh_lflow);  c->has_lflow = false; }
  int dbg_tris = S.tris;
  c->has_wstill = upload(&S, &c->mesh_wstill);
  c->has_wflow  = upload(&F, &c->mesh_wflow);
  c->has_lstill = upload(&LS, &c->mesh_lstill);
  c->has_lflow  = upload(&LF, &c->mesh_lflow);
  if (getenv("CRAFT_DEBUG"))
    TraceLog(LOG_INFO, "water chunk %d,%d: wstill tris=%d uploaded=%d", cx, cz, dbg_tris, c->has_wstill);
}

void mesh_chunk(int cx, int cz) {
  double t0 = prof_on() ? GetTime() : 0;
  build_chunk_solid(cx, cz);
  build_chunk_water(cx, cz);
  ensure_chunk(cx, cz)->meshed = true;
  if (prof_on()) prof_note(PH_MESH_CHUNK, (GetTime() - t0) * 1000.0);
}

void rebuild_chunk_solid(int cx, int cz) { build_chunk_solid(cx, cz); }
void rebuild_chunk_water(int cx, int cz) { build_chunk_water(cx, cz); }

// buildHeldCubeGeo (game.js:1137): unit cube centered on origin, atlas UVs.
Mesh build_block_cube_mesh(uint8_t block) {
  Builder b = {0};
  for (int fi = 0; fi < 6; fi++) {
    const FaceDef *f = &FACES[fi];
    int tile = tile_for(block, f->name);
    int col = tile % ATLAS_COLS, row = tile / ATLAS_COLS;
    b_reserve(&b, 4, 6);
    int start = b.verts;
    for (int k = 0; k < 4; k++) {
      const int *c = f->corners[k];
      float u = (col + c[3]) * TILE / ATLAS_W;
      float v = (row + 1 - c[4]) * TILE / ATLAS_H;
      b_vertex(&b, c[0] - 0.5f, c[1] - 0.5f, c[2] - 0.5f,
               (float)f->dir[0], (float)f->dir[1], (float)f->dir[2], u, v, 255, 255, 255, 255);
    }
    b_quad_indices(&b, start);
  }
  Mesh m;
  upload(&b, &m);
  return m;
}

static Material mat_cube;              // atlas + default shader, full bright
static bool mat_cube_ready = false;
Material *block_cube_material(void) {
  if (!mat_cube_ready) {
    mat_cube = LoadMaterialDefault();
    SetMaterialTexture(&mat_cube, MATERIAL_MAP_DIFFUSE, atlas_tex);
    mat_cube_ready = true;
  }
  return &mat_cube;
}

// Render-distance ceiling: (2*rd+5)^2 chunks must fit the chunk map (32768
// slots) and memory — 64 KB of block/light arrays per chunk plus CPU-side
// mesh copies. Native RAM handles 50; the wasm heap grows but is capped at
// 2 GB and phone tabs get OOM-killed well before that, so web stays lower.
#ifdef __EMSCRIPTEN__
#define RD_MAX 24
#else
#define RD_MAX 50
#endif
static int render_dist = RENDER_DIST;
int render_dist_get(void) { return render_dist; }
void render_dist_set(int d) { render_dist = d < 2 ? 2 : d > RD_MAX ? RD_MAX : d; }

// remeshAround (game.js:669): the chunk + boundary neighbors, meshed ones only.
void remesh_around(int x, int z) {
  int cx = chunk_of(x), cz = chunk_of(z);
  int lx = x - cx * CHUNK, lz = z - cz * CHUNK;
  int list[5][2] = {{cx,cz},{cx,cz},{cx,cz},{cx,cz},{cx,cz}};
  int n = 1;
  if (lx == 0)         { list[n][0] = cx - 1; list[n][1] = cz; n++; }
  if (lx == CHUNK - 1) { list[n][0] = cx + 1; list[n][1] = cz; n++; }
  if (lz == 0)         { list[n][0] = cx; list[n][1] = cz - 1; n++; }
  if (lz == CHUNK - 1) { list[n][0] = cx; list[n][1] = cz + 1; n++; }
  for (int i = 0; i < n; i++) {
    Chunk *c = chunk_get(list[i][0], list[i][1]);
    if (c && c->meshed) { build_chunk_solid(list[i][0], list[i][1]); build_chunk_water(list[i][0], list[i][1]); c->meshed = true; }
  }
}

// updateChunks (game.js:836): mesh nearest missing (budget 2/frame), unload far.
static int keep_dist(void) { return render_dist_get() + 2; }

typedef struct { int pcx, pcz; Chunk **doomed; int ndoomed, cap; } UnloadScan;
static void scan_far(Chunk *c, void *ud) {
  UnloadScan *s = ud;
  int dx = c->cx - s->pcx, dz = c->cz - s->pcz;
  int d = (dx < 0 ? -dx : dx) > (dz < 0 ? -dz : dz) ? (dx < 0 ? -dx : dx) : (dz < 0 ? -dz : dz);
  if (d > keep_dist() && !c->modified) {
    if (s->ndoomed == s->cap) { s->cap = s->cap ? s->cap * 2 : 32; s->doomed = realloc(s->doomed, s->cap * sizeof(Chunk *)); }
    s->doomed[s->ndoomed++] = c;
  } else if (d > keep_dist() && c->meshed) {
    unload_chunk_meshes(c);   // modified chunks: keep data, drop GPU memory
  }
}

// --- deferred remesh queue (explosion bursts): deduped, drained 3/frame ---
#define RQ_CAP 512
static int rq[RQ_CAP][2];
static int rq_head = 0, rq_count = 0;

void mesh_enqueue(int cx, int cz) {
  for (int i = 0; i < rq_count; i++) {
    int k = (rq_head + i) % RQ_CAP;
    if (rq[k][0] == cx && rq[k][1] == cz) return;
  }
  if (rq_count == RQ_CAP) { mesh_chunk(cx, cz); return; }   // full: do it now
  int k = (rq_head + rq_count) % RQ_CAP;
  rq[k][0] = cx; rq[k][1] = cz;
  rq_count++;
}

void update_chunks(double px, double pz) {
  // drain the burst queue first — these are visible holes, not new terrain
  for (int done = 0; rq_count > 0 && done < 3; done++) {
    int cx = rq[rq_head][0], cz = rq[rq_head][1];
    rq_head = (rq_head + 1) % RQ_CAP;
    rq_count--;
    Chunk *c = chunk_get(cx, cz);
    if (c && c->meshed) mesh_chunk(cx, cz);
  }
  int pcx = chunk_of((int)floor(px)), pcz = chunk_of((int)floor(pz));
  int budget = 2;
  // ring outward from player, nearest first
  for (int r = 0; r <= render_dist_get() && budget > 0; r++) {
    for (int dx = -r; dx <= r && budget > 0; dx++)
      for (int dz = -r; dz <= r && budget > 0; dz++) {
        if ((dx < 0 ? -dx : dx) != r && (dz < 0 ? -dz : dz) != r) continue; // ring edge only
        Chunk *c = chunk_get(pcx + dx, pcz + dz);
        if (!c || !c->meshed) { mesh_chunk(pcx + dx, pcz + dz); budget--; }
      }
  }
  UnloadScan s = { pcx, pcz, NULL, 0, 0 };
  world_each_chunk(scan_far, &s);
  for (int i = 0; i < s.ndoomed; i++) world_remove_chunk(s.doomed[i]);
  free(s.doomed);
}

// Frustum culling: 6 planes extracted from the current modelview*projection
// (Gribb-Hartmann). Essential at big render distances — /rd 50 keeps ~11k
// chunks meshed and drawing them all every frame is a slideshow.
typedef struct { float a, b, c, d; } Plane;
static Plane frustum[6];

static void frustum_extract(void) {
  Matrix m = MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection());
  // rows of the clip matrix in raymath field order
  float r[4][4] = {
    { m.m0, m.m4, m.m8,  m.m12 },
    { m.m1, m.m5, m.m9,  m.m13 },
    { m.m2, m.m6, m.m10, m.m14 },
    { m.m3, m.m7, m.m11, m.m15 },
  };
  for (int i = 0; i < 3; i++) {
    frustum[i * 2]     = (Plane){ r[3][0] + r[i][0], r[3][1] + r[i][1], r[3][2] + r[i][2], r[3][3] + r[i][3] };
    frustum[i * 2 + 1] = (Plane){ r[3][0] - r[i][0], r[3][1] - r[i][1], r[3][2] - r[i][2], r[3][3] - r[i][3] };
  }
}

static bool chunk_visible(const Chunk *c) {
  float x0 = c->cx * (float)CHUNK, x1 = x0 + CHUNK;
  float z0 = c->cz * (float)CHUNK, z1 = z0 + CHUNK;
  for (int i = 0; i < 6; i++) {
    const Plane *p = &frustum[i];
    // positive vertex: the AABB corner farthest along the plane normal
    float px = p->a >= 0 ? x1 : x0;
    float py = p->b >= 0 ? (float)WORLD_H : 0;
    float pz = p->c >= 0 ? z1 : z0;
    if (p->a * px + p->b * py + p->c * pz + p->d < 0) return false;
  }
  return true;
}

typedef struct { int phase; } DrawPass;
static void draw_chunk(Chunk *c, void *ud) {
  DrawPass *p = ud;
  if (!chunk_visible(c)) return;
  Matrix ident = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
  if (p->phase == 0) {
    if (c->has_solid) DrawMesh(c->mesh_solid, mat_solid, ident);
  } else {
    if (c->has_lstill) DrawMesh(c->mesh_lstill, mat_lstill, ident);
    if (c->has_lflow)  DrawMesh(c->mesh_lflow,  mat_lflow,  ident);
    if (c->has_wstill) DrawMesh(c->mesh_wstill, mat_wstill, ident);
    if (c->has_wflow)  DrawMesh(c->mesh_wflow,  mat_wflow,  ident);
  }
}

void draw_world(void) {
  if (!inited) return;
  frustum_extract();
  DrawPass p = { 0 };
  world_each_chunk(draw_chunk, &p);       // opaque
  rlDrawRenderBatchActive();
  rlDisableDepthMask();                    // water: no depth write (JS depthWrite:false)
  rlDisableBackfaceCulling();              // JS fluid materials are THREE.DoubleSide
  rlEnableColorBlend();
  p.phase = 1;
  world_each_chunk(draw_chunk, &p);       // transparent fluids
  rlDrawRenderBatchActive();
  rlEnableBackfaceCulling();
  rlEnableDepthMask();
}
