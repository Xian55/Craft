// Headless simulation tests: fluid flow, TNT, falling sand, crafting.
// No window: chunk meshes are never built, so remesh calls are no-ops.
#include "../src/world.h"
#include "../src/fluids.h"
#include "../src/entities.h"
#include "../src/inventory.h"
#include "../src/physics.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok: %s\n", msg); } while (0)

static void ticks(int n) { for (int i = 0; i < n; i++) simulate_water(); }

int main(void) {
  // --- build a flat stone platform high above the terrain ---
  // Wide enough that the platform edge (a "hole" for the slope-seeking BFS)
  // stays outside SLOPE_RANGE of every test cell — otherwise water only flows
  // toward the nearest edge (which is correct, but not what we assert here).
  for (int x = -8; x < 24; x++)
    for (int z = -8; z < 24; z++)
      set_voxel(x, 50, z, B_STONE);

  // 1) water source spreads sideways on a supported floor
  set_water_raw(8, 51, 8, 9); touch_water(8, 51, 8);
  ticks(10);
  CHECK(water_at(10, 51, 8) > 0 && water_at(8, 51, 10) > 0, "water spreads from a source");
  CHECK(water_at(15, 51, 8) > 0 && water_at(16, 51, 8) == 0, "flow range limited (7 blocks)");

  // 2) removing the source retreats the water
  set_water_raw(8, 51, 8, 0); touch_water(8, 51, 8);
  ticks(20);
  CHECK(water_at(9, 51, 8) == 0 && water_at(8, 51, 9) == 0, "water retreats when source removed");

  // 3) infinite source: two sources one apart create a source between them
  set_water_raw(4, 51, 4, 9); touch_water(4, 51, 4);
  set_water_raw(6, 51, 4, 9); touch_water(6, 51, 4);
  ticks(10);
  CHECK(raw_water(5, 51, 4) == 9, "cell between two sources becomes a source");

  // 4) water + lava -> cobblestone
  set_water_raw(12, 51, 12, 9); touch_water(12, 51, 12);
  ticks(4);
  set_water_raw(14, 51, 12, 19); touch_water(14, 51, 12);   // lava source next to flowing water
  ticks(10);
  bool cobbled = false;
  for (int x = 11; x <= 15 && !cobbled; x++)
    for (int z = 11; z <= 13 && !cobbled; z++)
      if (get_voxel(x, 51, z) == B_COBBLE) cobbled = true;
  CHECK(cobbled, "water meeting lava turns a cell to cobblestone");

  // 5) falling sand: sand placed in the air falls to the platform
  set_voxel(3, 55, 3, B_SAND);
  check_fall(3, 55, 3);
  CHECK(get_voxel(3, 55, 3) == B_AIR, "floating sand detaches");
  for (int i = 0; i < 300; i++) update_falling(1.0f / 60.0f);
  CHECK(get_voxel(3, 51, 3) == B_SAND, "sand lands on the platform");

  // 6) TNT explosion carves a crater and hurts a close player
  player.x = 8.5; player.y = 53; player.z = 8.5; player.health = 20;
  for (int x = 6; x <= 10; x++) for (int z = 6; z <= 10; z++) set_voxel(x, 52, z, B_STONE);
  explode_at(8, 52, 8);
  CHECK(get_voxel(8, 52, 8) == B_AIR && get_voxel(9, 52, 8) == B_AIR, "explosion clears blocks");
  CHECK(player.health < 20, "explosion damages a nearby player");

  // 7) crafting: shapeless wood->planks, shaped 2 planks -> 4 sticks
  memset(craft_grid, 0, sizeof(craft_grid));
  craft_grid[4] = (Stack){ B_WOOD, 1 };
  int oid, on;
  CHECK(match_recipe(&oid, &on) && oid == B_PLANKS && on == 4, "shapeless: wood -> 4 planks");
  memset(craft_grid, 0, sizeof(craft_grid));
  craft_grid[1] = (Stack){ B_PLANKS, 1 }; craft_grid[4] = (Stack){ B_PLANKS, 1 };
  CHECK(match_recipe(&oid, &on) && oid == I_STICK && on == 4, "shaped: 2 planks -> 4 sticks");
  memset(craft_grid, 0, sizeof(craft_grid));
  craft_grid[0] = (Stack){ B_PLANKS, 2 }; craft_grid[3] = (Stack){ B_PLANKS, 2 }; craft_grid[6] = (Stack){ I_STICK, 1 };
  CHECK(match_recipe(&oid, &on) && oid == I_SWORD, "shaped anywhere in grid: sword");
  memset(&cursor_stack, 0, sizeof(cursor_stack));
  take_result();
  CHECK(cursor_stack.id == I_SWORD && craft_grid[0].count == 1, "take_result consumes one of each");
  memset(craft_grid, 0, sizeof(craft_grid));
  for (int i = 0; i < 9; i++) if (i != 4) craft_grid[i] = (Stack){ B_PLANKS, 1 };
  CHECK(match_recipe(&oid, &on) && oid == B_CHEST && on == 1, "shaped with hole: 8-plank ring -> chest");
  memset(craft_grid, 0, sizeof(craft_grid));
  craft_grid[0] = (Stack){ I_IRON_INGOT, 1 }; craft_grid[2] = (Stack){ I_IRON_INGOT, 1 }; craft_grid[4] = (Stack){ I_IRON_INGOT, 1 };
  CHECK(match_recipe(&oid, &on) && oid == I_BUCKET, "shaped with holes: iron bucket V");

  // 8) inventory add/remove round trip
  memset(slots, 0, sizeof(slots));
  add_item(B_COBBLE, 100);
  CHECK(item_count(B_COBBLE) == 100 && slots[0].count == 64 && slots[1].count == 36, "stacks split at 64");
  CHECK(remove_item(B_COBBLE, 70) && item_count(B_COBBLE) == 30, "remove spans stacks");

  printf(fails ? "SIM TEST: %d FAILURES\n" : "SIM TEST: all passed\n", fails);
  return fails ? 1 : 0;
}
