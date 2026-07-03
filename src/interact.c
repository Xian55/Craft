#include "interact.h"
#include "world.h"
#include "mesh.h"
#include "physics.h"
#include "inventory.h"
#include "entities.h"
#include "fluids.h"
#include "net.h"
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <math.h>

// breakTime + tool tables (game.js:1381-1402)
static float break_time_base(uint8_t b) {
  switch (b) {
    case B_LEAVES: return 0.2f;
    case B_GRASS: case B_DIRT: case B_SAND: return 0.4f;
    case B_WOOD: return 0.8f;  case B_PLANKS: return 0.7f;
    case B_STONE: case B_STONE_BRICK: return 1.4f;
    case B_COBBLE: case B_BRICK: return 1.2f;
    case B_TORCH: return 0.1f; case B_CHEST: return 0.9f; case B_TNT: return 0.3f;
    default: return 1.0f;
  }
}
static int tool_for(uint8_t b) {
  switch (b) {
    case B_DIRT: case B_GRASS: case B_SAND: return I_SHOVEL;
    case B_STONE: case B_COBBLE: case B_STONE_BRICK: case B_BRICK: return I_PICKAXE;
    default: return 0;
  }
}
static float break_time_for(uint8_t block) {
  float t = break_time_base(block);
  Stack *held = &slots[sel_slot];
  if (held->id && tool_for(block) == held->id) t *= 0.35f;
  else if (held->id == I_SWORD && block == B_LEAVES) t *= 0.5f;
  return t;
}

// --- mining state ---
static bool mining = false;
static int mine_target[3];
static bool have_target = false;
static float mine_progress = 0, mine_need = 1;

void set_mining(bool m) { mining = m; }
float mine_progress01(void) { return (mining && have_target) ? mine_progress / mine_need : -1.0f; }

void update_mining(float dt) {
  int hit[3], prev[3];
  if (!mining || !raycast_voxel(6.0f, hit, prev)) { have_target = false; mine_progress = 0; return; }
  if (!have_target || mine_target[0] != hit[0] || mine_target[1] != hit[1] || mine_target[2] != hit[2]) {
    mine_target[0] = hit[0]; mine_target[1] = hit[1]; mine_target[2] = hit[2];
    have_target = true; mine_progress = 0;
  }
  uint8_t block = get_voxel(hit[0], hit[1], hit[2]);
  mine_need = break_time_for(block);
  mine_progress += dt;
  if (mine_progress >= mine_need) {
    if (block == B_CHEST) {                     // chest spills its contents
      if (net_active()) {
        // server owns the contents: it replies SV_CHEST(broken) with the
        // authoritative slots, which spawns the drops and clears the cache
        net_send_chest_break(hit[0], hit[1], hit[2]);
      } else {
        Stack *arr = chest_array_peek(hit[0], hit[1], hit[2]);
        if (arr) {
          for (int i = 0; i < CHEST_SIZE; i++)
            if (arr[i].id) spawn_drop(arr[i].id, arr[i].count, hit[0] + 0.5, hit[1], hit[2] + 0.5, 0.5f);
          chest_delete(hit[0], hit[1], hit[2]);
        }
      }
    }
    set_voxel(hit[0], hit[1], hit[2], B_AIR);
    net_send_edit(hit[0], hit[1], hit[2]);
    spawn_drop(drop_of(block), 1, hit[0] + 0.5, hit[1] + 0.5, hit[2] + 0.5, 0.5f);
    remesh_around(hit[0], hit[2]);
    touch_water(hit[0], hit[1], hit[2]);
    check_fall(hit[0], hit[1] + 1, hit[2]);
    have_target = false; mine_progress = 0;
  }
}

// --- placing (game.js:1623) ---
static void place_block(void) {
  Stack *s = &slots[sel_slot];
  if (!s->id || s->count <= 0 || !is_block(s->id)) return;
  int hit[3], prev[3];
  if (!raycast_voxel(6.0f, hit, prev)) return;
  if (prev[1] < 0 || prev[1] >= WORLD_H) return;
  int feet_x = (int)floor(player.x), feet_z = (int)floor(player.z);
  if (prev[0] == feet_x && prev[2] == feet_z
      && (prev[1] == (int)floor(player.y) || prev[1] == (int)floor(player.y + 1))) return;
  int id = s->id;
  s->count--; if (s->count == 0) s->id = 0;
  set_voxel(prev[0], prev[1], prev[2], (uint8_t)id);
  net_send_edit(prev[0], prev[1], prev[2]);
  remesh_around(prev[0], prev[2]);
  touch_water(prev[0], prev[1], prev[2]);
  if (id == B_SAND) check_fall(prev[0], prev[1], prev[2]);
}

// --- buckets (game.js:1645-1678) ---
static bool is_source_w(int x, int y, int z) { return get_voxel(x, y, z) == B_AIR && raw_water(x, y, z) == 9; }
static bool is_source_l(int x, int y, int z) { return get_voxel(x, y, z) == B_AIR && raw_water(x, y, z) == 19; }

// step along the look ray for the first SOURCE cell (small steps like the JS)
static bool raycast_fluid_source(int cell[3], bool *lava) {
  double ox = player.x, oy = player.y + P_EYE, oz = player.z;
  double cp = cos(player.pitch);
  double dx = -sin(player.yaw) * cp, dy = sin(player.pitch), dz = -cos(player.yaw) * cp;
  for (double t = 0; t < 6; t += 0.05) {
    int x = (int)floor(ox + dx * t), y = (int)floor(oy + dy * t), z = (int)floor(oz + dz * t);
    if (get_voxel(x, y, z) != B_AIR) return false;
    if (is_source_w(x, y, z)) { cell[0] = x; cell[1] = y; cell[2] = z; *lava = false; return true; }
    if (is_source_l(x, y, z)) { cell[0] = x; cell[1] = y; cell[2] = z; *lava = true; return true; }
  }
  return false;
}

static void fill_bucket(void) {
  int cell[3]; bool lava;
  if (!raycast_fluid_source(cell, &lava)) return;
  set_water_raw(cell[0], cell[1], cell[2], 0);
  touch_water(cell[0], cell[1], cell[2]);
  net_send_edit(cell[0], cell[1], cell[2]);
  slots[sel_slot].id = lava ? I_LAVA_BUCKET : I_WATER_BUCKET;
}

static void empty_bucket(bool lava) {
  int hit[3], prev[3];
  if (!raycast_voxel(6.0f, hit, prev)) return;
  if (get_voxel(prev[0], prev[1], prev[2]) != B_AIR) return;
  set_water_raw(prev[0], prev[1], prev[2], lava ? 19 : 9);
  net_send_edit(prev[0], prev[1], prev[2]);
  touch_water(prev[0], prev[1], prev[2]);
  if (lava) remesh_around(prev[0], prev[2]);   // lava light shows immediately
  slots[sel_slot].id = I_BUCKET;
}

void use_right_click(void) {
  int hit[3], prev[3];
  if (raycast_voxel(6.0f, hit, prev)) {
    uint8_t b = get_voxel(hit[0], hit[1], hit[2]);
    if (b == B_CHEST) { open_chest_at(hit[0], hit[1], hit[2]); return; }
    if (b == B_TNT) { prime_tnt(hit[0], hit[1], hit[2], 1.5f); return; }
  }
  Stack *s = &slots[sel_slot];
  if (!s->id) return;
  if (s->id == I_BUCKET) fill_bucket();
  else if (s->id == I_WATER_BUCKET) empty_bucket(false);
  else if (s->id == I_LAVA_BUCKET) empty_bucket(true);
  else if (is_block(s->id)) place_block();
}

// --- highlight wireframe + crack overlay ---
#define CRACK_STAGES 10
static Texture2D crack_tex;
static Mesh crack_cube[CRACK_STAGES];
static bool crack_ready = false;
static Material crack_mat;

void interact_init(void) {
  if (!FileExists("assets/cracks.png")) return;
  // cracks.png: transparent background, dark lines, 10 stages side by side.
  // Draw onto white so BLEND_MULTIPLIED leaves the block texture intact
  // outside the crack lines (game.js makeCracksTexture).
  Image img = LoadImage("assets/cracks.png");
  Image white = GenImageColor(img.width, img.height, WHITE);
  ImageDraw(&white, img, (Rectangle){ 0, 0, (float)img.width, (float)img.height },
            (Rectangle){ 0, 0, (float)img.width, (float)img.height }, WHITE);
  crack_tex = LoadTextureFromImage(white);
  SetTextureFilter(crack_tex, TEXTURE_FILTER_POINT);
  UnloadImage(img); UnloadImage(white);
  crack_mat = LoadMaterialDefault();
  SetMaterialTexture(&crack_mat, MATERIAL_MAP_DIFFUSE, crack_tex);
  // one cube mesh per stage: same geometry, u-window shifted to the stage
  for (int s = 0; s < CRACK_STAGES; s++) {
    Mesh m = GenMeshCube(1.002f, 1.002f, 1.002f);
    for (int v = 0; v < m.vertexCount; v++)
      m.texcoords[v * 2] = (s + m.texcoords[v * 2]) / CRACK_STAGES;
    UpdateMeshBuffer(m, 1, m.texcoords, m.vertexCount * 2 * (int)sizeof(float), 0);
    crack_cube[s] = m;
  }
  crack_ready = true;
}

void draw_highlight(void) {
  int hit[3], prev[3];
  if (!raycast_voxel(6.0f, hit, prev)) return;
  Vector3 c = { hit[0] + 0.5f, hit[1] + 0.5f, hit[2] + 0.5f };
  DrawCubeWires(c, 1.003f, 1.003f, 1.003f, BLACK);
  if (crack_ready && mining && have_target
      && mine_target[0] == hit[0] && mine_target[1] == hit[1] && mine_target[2] == hit[2]) {
    int stage = (int)((mine_progress / mine_need) * CRACK_STAGES);
    if (stage > CRACK_STAGES - 1) stage = CRACK_STAGES - 1;
    rlDrawRenderBatchActive();
    BeginBlendMode(BLEND_MULTIPLIED);
    DrawMesh(crack_cube[stage], crack_mat, MatrixTranslate(c.x, c.y, c.z));
    rlDrawRenderBatchActive();
    EndBlendMode();
  }
}
