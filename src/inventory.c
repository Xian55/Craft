#include "inventory.h"
#include "config.h"
#include "lang.h"
#include <string.h>
#include <stdlib.h>

Stack slots[INV_SIZE];
int   sel_slot = 0;
Stack craft_grid[9];
Stack cursor_stack;

bool is_tool(int id) {
  return id == I_SHOVEL || id == I_PICKAXE || id == I_SWORD
      || id == I_BUCKET || id == I_WATER_BUCKET || id == I_LAVA_BUCKET;
}
bool is_block(int id) { return id >= B_GRASS && id <= B_TNT; }
int  stack_max(int id) { return is_tool(id) ? 1 : STACK_MAX; }

int item_count(int id) {
  int c = 0;
  for (int i = 0; i < INV_SIZE; i++) if (slots[i].id == id) c += slots[i].count;
  return c;
}

void add_item(int id, int n) {
  int max = stack_max(id);
  for (int i = 0; i < INV_SIZE && n > 0; i++) {
    Stack *s = &slots[i];
    if (s->id == id && s->count < max) { int add = max - s->count; if (add > n) add = n; s->count += add; n -= add; }
  }
  for (int i = 0; i < INV_SIZE && n > 0; i++) {
    if (slots[i].id == 0) { int add = max < n ? max : n; slots[i].id = id; slots[i].count = add; n -= add; }
  }
  // inventory full: remainder is lost (same as JS)
}

bool remove_item(int id, int n) {
  if (item_count(id) < n) return false;
  for (int i = 0; i < INV_SIZE && n > 0; i++) {
    Stack *s = &slots[i];
    if (s->id == id) { int take = s->count < n ? s->count : n; s->count -= take; n -= take; if (s->count == 0) s->id = 0; }
  }
  return true;
}

int drop_of(int block) {
  if (block == B_GRASS) return B_DIRT;
  if (block == B_STONE) return B_COBBLE;
  return block;
}

// Localized names via assets/lang/<code>.lang (see lang.c).
const char *item_name(int id) {
  switch (id) {
    case B_GRASS: return tr("item.grass");        case B_DIRT: return tr("item.dirt");
    case B_STONE: return tr("item.stone");        case B_WOOD: return tr("item.wood");
    case B_LEAVES: return tr("item.leaves");      case B_SAND: return tr("item.sand");
    case B_PLANKS: return tr("item.planks");      case B_COBBLE: return tr("item.cobble");
    case B_STONE_BRICK: return tr("item.stone_brick"); case B_BRICK: return tr("item.brick");
    case B_TNT: return tr("item.tnt");
    case B_TORCH: return tr("item.torch");        case B_CHEST: return tr("item.chest");
    case B_SNOW: return tr("item.snow");
    case I_SHOVEL: return tr("item.shovel");      case I_PICKAXE: return tr("item.pickaxe");
    case I_SWORD: return tr("item.sword");        case I_BUCKET: return tr("item.bucket");
    case I_WATER_BUCKET: return tr("item.water_bucket"); case I_PORK: return tr("item.pork");
    case I_STICK: return tr("item.stick");        case I_LAVA_BUCKET: return tr("item.lava_bucket");
    default: return "?";
  }
}

void item_color(int id, uint8_t rgb[3]) {
  uint32_t c;
  switch (id) {
    case B_GRASS: c = 0x5aa02c; break; case B_DIRT: c = 0x8a5a34; break;
    case B_STONE: c = 0x8d8d8d; break; case B_WOOD: c = 0x7a5a34; break;
    case B_LEAVES: c = 0x3f8f2f; break; case B_SAND: c = 0xe0d29a; break;
    case B_PLANKS: c = 0xb08a4e; break; case B_COBBLE: c = 0x7c7c7c; break;
    case B_STONE_BRICK: c = 0x9a9a9a; break; case B_BRICK: c = 0xa4432e; break;
    case B_TORCH: c = 0xff9a1f; break; case B_CHEST: c = 0x7a5230; break;
    case B_SNOW: c = 0xf4f8ff; break;
    case B_TNT: c = 0xc0392b; break;  case I_LAVA_BUCKET: c = 0xe2560f; break;
    default: c = 0xccaa88; break;
  }
  rgb[0] = (uint8_t)(c >> 16); rgb[1] = (uint8_t)(c >> 8); rgb[2] = (uint8_t)c;
}

// ITEM_TILE (game.js:48-53) — tile index in items.png (4-column grid)
int item_tile(int id) {
  switch (id) {
    case I_SHOVEL: return 0;  case I_PICKAXE: return 1; case I_SWORD: return 2;
    case I_BUCKET: return 3;  case I_WATER_BUCKET: return 4; case I_PORK: return 5;
    case I_STICK: return 6;   case B_PLANKS: return 7;  case B_WOOD: return 8;
    case B_STONE: return 9;   case B_COBBLE: return 10; case B_DIRT: return 11;
    case B_TORCH: return 12;  case B_LEAVES: return 13; case I_LAVA_BUCKET: return 14;
    case B_TNT: return 15;
    default: return -1;
  }
}

// RECIPES (game.js:1746-1757)
#define P B_PLANKS
#define C B_COBBLE
#define S I_STICK
const Recipe RECIPES[] = {
  { .h = 0, .w = 0, .shapeless = { B_WOOD }, .n_shapeless = 1, .out_id = P, .out_n = 4 },
  { .shape = {{P},{P}},            .h = 2, .w = 1, .out_id = S, .out_n = 4 },
  { .shape = {{C,C},{C,C}},        .h = 2, .w = 2, .out_id = B_STONE_BRICK, .out_n = 4 },
  { .shape = {{B_SAND,B_SAND},{B_SAND,B_SAND}}, .h = 2, .w = 2, .out_id = B_BRICK, .out_n = 4 },
  { .shape = {{P,P,P},{0,S,0},{0,S,0}}, .h = 3, .w = 3, .out_id = I_PICKAXE, .out_n = 1 },
  { .shape = {{P},{S},{S}},        .h = 3, .w = 1, .out_id = I_SHOVEL, .out_n = 1 },
  { .shape = {{P},{P},{S}},        .h = 3, .w = 1, .out_id = I_SWORD, .out_n = 1 },
  { .shape = {{C,0,C},{0,C,0}},    .h = 2, .w = 3, .out_id = I_BUCKET, .out_n = 1 },
  { .shape = {{P},{S}},            .h = 2, .w = 1, .out_id = B_TORCH, .out_n = 4 },
  { .shape = {{P,P,P},{P,0,P},{P,P,P}}, .h = 3, .w = 3, .out_id = B_CHEST, .out_n = 1 },
};
#undef P
#undef C
#undef S
const int N_RECIPES = (int)(sizeof(RECIPES) / sizeof(RECIPES[0]));

// trimGrid + shapedMatch (game.js:1772-1789)
static bool shaped_match(const int ids[9], const Recipe *r) {
  int minR = 3, maxR = -1, minC = 3, maxC = -1;
  for (int row = 0; row < 3; row++)
    for (int col = 0; col < 3; col++)
      if (ids[row * 3 + col]) {
        if (row < minR) minR = row; if (row > maxR) maxR = row;
        if (col < minC) minC = col; if (col > maxC) maxC = col;
      }
  if (maxR < 0) return false;
  int th = maxR - minR + 1, tw = maxC - minC + 1;
  if (th != r->h || tw != r->w) return false;
  for (int row = 0; row < th; row++)
    for (int col = 0; col < tw; col++)
      if (ids[(minR + row) * 3 + (minC + col)] != r->shape[row][col]) return false;
  return true;
}

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

bool match_recipe(int *out_id, int *out_n) {
  int ids[9], filled[9], nf = 0;
  for (int i = 0; i < 9; i++) {
    ids[i] = craft_grid[i].id;
    if (craft_grid[i].id) filled[nf++] = craft_grid[i].id;
  }
  for (int r = 0; r < N_RECIPES; r++) {
    const Recipe *rc = &RECIPES[r];
    if (rc->n_shapeless > 0) {
      if (nf != rc->n_shapeless) continue;
      int need[4]; memcpy(need, rc->shapeless, sizeof(int) * (size_t)rc->n_shapeless);
      int have[9]; memcpy(have, filled, sizeof(int) * (size_t)nf);
      qsort(need, (size_t)rc->n_shapeless, sizeof(int), cmp_int);
      qsort(have, (size_t)nf, sizeof(int), cmp_int);
      bool ok = true;
      for (int i = 0; i < nf; i++) if (have[i] != need[i]) { ok = false; break; }
      if (ok) { *out_id = rc->out_id; *out_n = rc->out_n; return true; }
    } else if (shaped_match(ids, rc)) {
      *out_id = rc->out_id; *out_n = rc->out_n; return true;
    }
  }
  return false;
}

void take_result(void) {
  int id, n;
  if (!match_recipe(&id, &n)) return;
  if (cursor_stack.id && (cursor_stack.id != id || cursor_stack.count + n > stack_max(id))) return;
  for (int i = 0; i < 9; i++)
    if (craft_grid[i].id) { craft_grid[i].count--; if (craft_grid[i].count == 0) craft_grid[i].id = 0; }
  if (cursor_stack.id) cursor_stack.count += n;
  else { cursor_stack.id = id; cursor_stack.count = n; }
}

// --- chests ---
typedef struct ChestEntry { int x, y, z; Stack arr[CHEST_SIZE]; bool used; } ChestEntry;
#define MAX_CHESTS 256
static ChestEntry chests[MAX_CHESTS];

Stack *chest_array_peek(int x, int y, int z) {
  for (int i = 0; i < MAX_CHESTS; i++)
    if (chests[i].used && chests[i].x == x && chests[i].y == y && chests[i].z == z) return chests[i].arr;
  return NULL;
}
Stack *chest_array_at(int x, int y, int z) {
  Stack *a = chest_array_peek(x, y, z);
  if (a) return a;
  for (int i = 0; i < MAX_CHESTS; i++)
    if (!chests[i].used) {
      chests[i].used = true; chests[i].x = x; chests[i].y = y; chests[i].z = z;
      memset(chests[i].arr, 0, sizeof(chests[i].arr));
      return chests[i].arr;
    }
  return chests[0].arr;   // full: reuse slot 0 (256 chests is plenty for play)
}
void chest_delete(int x, int y, int z) {
  for (int i = 0; i < MAX_CHESTS; i++)
    if (chests[i].used && chests[i].x == x && chests[i].y == y && chests[i].z == z) { chests[i].used = false; return; }
}
