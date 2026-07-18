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
      || id == I_BUCKET || id == I_WATER_BUCKET || id == I_LAVA_BUCKET
      || id == I_IRON_PICKAXE || id == I_IRON_SWORD || id == I_IRON_SHOVEL;
}
// placeable blocks: the original range (grass..tnt) plus the new craftable
// blocks (furnace, glass). Snow/ores stay natural-only (not placeable).
bool is_block(int id) { return (id >= B_GRASS && id <= B_TNT) || id == B_FURNACE || id == B_GLASS; }
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
  if (block == B_COAL_ORE) return I_COAL;      // coal ore -> coal item (fuel)
  return block;                                // iron ore drops itself (smelt -> ingot)
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
    case B_COAL_ORE: return tr("item.coal_ore"); case B_IRON_ORE: return tr("item.iron_ore");
    case I_SHOVEL: return tr("item.shovel");      case I_PICKAXE: return tr("item.pickaxe");
    case I_SWORD: return tr("item.sword");        case I_BUCKET: return tr("item.bucket");
    case I_WATER_BUCKET: return tr("item.water_bucket"); case I_PORK: return tr("item.pork");
    case I_STICK: return tr("item.stick");        case I_LAVA_BUCKET: return tr("item.lava_bucket");
    case B_FURNACE: return tr("item.furnace");    case B_GLASS: return tr("item.glass");
    case I_COAL: return tr("item.coal");          case I_IRON_INGOT: return tr("item.iron_ingot");
    case I_COOKED_PORK: return tr("item.cooked_pork");
    case I_IRON_PICKAXE: return tr("item.iron_pickaxe"); case I_IRON_SWORD: return tr("item.iron_sword");
    case I_IRON_SHOVEL: return tr("item.iron_shovel");
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
    case B_COAL_ORE: c = 0x4a4a4a; break; case B_IRON_ORE: c = 0xc8a082; break;
    case B_TNT: c = 0xc0392b; break;  case I_LAVA_BUCKET: c = 0xe2560f; break;
    case B_FURNACE: c = 0x6f6f6f; break; case B_GLASS: c = 0xbfe0f0; break;
    case I_COAL: c = 0x1c1c1c; break; case I_IRON_INGOT: c = 0xd8d8e0; break;
    case I_COOKED_PORK: c = 0xc07845; break;
    case I_IRON_PICKAXE: case I_IRON_SWORD: case I_IRON_SHOVEL: c = 0xcfd2da; break;
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
    case I_COAL: return 16;   case I_IRON_INGOT: return 17; case I_COOKED_PORK: return 18;
    case I_IRON_PICKAXE: return 19; case I_IRON_SWORD: return 20; case I_IRON_SHOVEL: return 21;
    case B_FURNACE: return 22; case B_GLASS: return 23;
    default: return -1;
  }
}

// RECIPES (game.js:1746-1757)
#define P B_PLANKS
#define C B_COBBLE
#define S I_STICK
#define Ir I_IRON_INGOT
const Recipe RECIPES[] = {
  { .h = 0, .w = 0, .shapeless = { B_WOOD }, .n_shapeless = 1, .out_id = P, .out_n = 4 },
  { .shape = {{P},{P}},            .h = 2, .w = 1, .out_id = S, .out_n = 4 },
  { .shape = {{C,C},{C,C}},        .h = 2, .w = 2, .out_id = B_STONE_BRICK, .out_n = 4 },
  { .shape = {{B_SAND,B_SAND},{B_SAND,B_SAND}}, .h = 2, .w = 2, .out_id = B_BRICK, .out_n = 4 },
  { .shape = {{P,P,P},{0,S,0},{0,S,0}}, .h = 3, .w = 3, .out_id = I_PICKAXE, .out_n = 1 },
  { .shape = {{P},{S},{S}},        .h = 3, .w = 1, .out_id = I_SHOVEL, .out_n = 1 },
  { .shape = {{P},{P},{S}},        .h = 3, .w = 1, .out_id = I_SWORD, .out_n = 1 },
  { .shape = {{Ir,0,Ir},{0,Ir,0}}, .h = 2, .w = 3, .out_id = I_BUCKET, .out_n = 1 },
  { .shape = {{P},{S}},            .h = 2, .w = 1, .out_id = B_TORCH, .out_n = 4 },
  { .shape = {{P,P,P},{P,0,P},{P,P,P}}, .h = 3, .w = 3, .out_id = B_CHEST, .out_n = 1 },
  { .shape = {{C,C,C},{C,0,C},{C,C,C}}, .h = 3, .w = 3, .out_id = B_FURNACE, .out_n = 1 },
  { .shape = {{Ir,Ir,Ir},{0,S,0},{0,S,0}}, .h = 3, .w = 3, .out_id = I_IRON_PICKAXE, .out_n = 1 },
  { .shape = {{Ir},{S},{S}},       .h = 3, .w = 1, .out_id = I_IRON_SHOVEL, .out_n = 1 },
  { .shape = {{Ir},{Ir},{S}},      .h = 3, .w = 1, .out_id = I_IRON_SWORD, .out_n = 1 },
};
#undef P
#undef C
#undef S
#undef Ir
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

// --- furnaces / smelting ---
int smelt_result(int id) {
  switch (id) {
    case B_IRON_ORE: return I_IRON_INGOT;
    case B_SAND:     return B_GLASS;
    case B_COBBLE:   return B_STONE;
    case I_PORK:     return I_COOKED_PORK;
    default:         return 0;
  }
}
float fuel_burn(int id) {
  switch (id) {
    case I_COAL:   return 8.0f;
    case B_WOOD:   return 3.0f;
    case B_PLANKS: return 2.0f;
    case B_LEAVES: return 0.6f;
    default:       return 0.0f;
  }
}
static bool furnace_can_smelt(const FurnaceState *f) {
  if (f->in.count <= 0) return false;
  int out = smelt_result(f->in.id);
  if (!out) return false;
  return f->out.id == 0 || (f->out.id == out && f->out.count < STACK_MAX);
}
// Advance the smelt by (now - last_t) in a bounded loop: each pass either lights
// a fuel, burns idle time, or completes a smelt, so even a huge elapsed dt (server
// slept) converges (bounded by fuel+input counts).
void furnace_advance(FurnaceState *f, double now) {
  double dt = now - f->last_t;
  f->last_t = now;
  if (dt <= 0) return;
  for (int guard = 0; guard < 100000 && dt > 1e-6; guard++) {
    if (f->burn <= 0.0f) {                       // need to light fuel
      if (furnace_can_smelt(f) && fuel_burn(f->fuel.id) > 0.0f) {
        f->burn_max = fuel_burn(f->fuel.id);
        f->burn = f->burn_max;
        if (--f->fuel.count == 0) f->fuel.id = 0;
      } else {                                   // idle: cool progress, stop
        f->cook -= (float)dt * 2.0f; if (f->cook < 0) f->cook = 0;
        break;
      }
    }
    if (!furnace_can_smelt(f)) {                 // lit but nothing valid: burn down
      double bs = dt < f->burn ? dt : f->burn;
      f->burn -= (float)bs; dt -= bs;
      f->cook -= (float)bs * 2.0f; if (f->cook < 0) f->cook = 0;
      continue;
    }
    double step = dt;                            // smelt
    if (step > f->burn) step = f->burn;
    if (step > FURNACE_COOK - f->cook) step = FURNACE_COOK - f->cook;
    f->burn -= (float)step; f->cook += (float)step; dt -= step;
    if (f->cook >= FURNACE_COOK - 1e-4f) {
      int out = smelt_result(f->in.id);
      if (f->out.id == 0) { f->out.id = out; f->out.count = 1; }
      else f->out.count++;
      if (--f->in.count == 0) f->in.id = 0;
      f->cook = 0;
    }
  }
}

typedef struct FurnaceEntry { int x, y, z; FurnaceState st; bool used; } FurnaceEntry;
#define MAX_FURNACES 256
static FurnaceEntry furnaces[MAX_FURNACES];

FurnaceState *furnace_peek(int x, int y, int z) {
  for (int i = 0; i < MAX_FURNACES; i++)
    if (furnaces[i].used && furnaces[i].x == x && furnaces[i].y == y && furnaces[i].z == z) return &furnaces[i].st;
  return NULL;
}
FurnaceState *furnace_at(int x, int y, int z) {
  FurnaceState *s = furnace_peek(x, y, z);
  if (s) return s;
  for (int i = 0; i < MAX_FURNACES; i++)
    if (!furnaces[i].used) {
      furnaces[i].used = true; furnaces[i].x = x; furnaces[i].y = y; furnaces[i].z = z;
      memset(&furnaces[i].st, 0, sizeof(furnaces[i].st));
      return &furnaces[i].st;
    }
  return &furnaces[0].st;   // full: reuse slot 0
}
void furnace_delete(int x, int y, int z) {
  for (int i = 0; i < MAX_FURNACES; i++)
    if (furnaces[i].used && furnaces[i].x == x && furnaces[i].y == y && furnaces[i].z == z) { furnaces[i].used = false; return; }
}
