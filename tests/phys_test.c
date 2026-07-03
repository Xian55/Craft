// Physics sanity harness — no window, pure simulation against generated terrain.
#include "../src/physics.h"
#include "../src/world.h"
#include "../src/gen.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok: %s\n", msg); } while (0)

int main(void) {
  spawn_player();
  Input none = {0};

  // 1) gravity drops the spawned player onto the terrain surface
  for (int i = 0; i < 600; i++) update_player(&none, 1.0f / 60.0f);
  int gx = (int)floor(player.x), gz = (int)floor(player.z);
  int h = terrain_height(gx, gz);
  CHECK(player.on_ground, "player lands and stays on ground");
  CHECK(fabs(player.y - (h + 1)) < 0.01, "rest height = terrain height + 1 (feet on surface)");
  CHECK(player.health == 20, "spawn-drop of 2 blocks causes no fall damage");

  // 2) jump: leaves ground, returns
  double y0 = player.y;
  Input jump = { .jump = true };
  update_player(&jump, 1.0f / 60.0f);
  double peak = player.y;
  for (int i = 0; i < 120; i++) { update_player(&none, 1.0f / 60.0f); if (player.y > peak) peak = player.y; }
  CHECK(peak - y0 > 1.0 && peak - y0 < 1.6, "jump apex ~1.25 blocks");
  CHECK(player.on_ground && fabs(player.y - y0) < 0.01, "lands back at same height");

  // 3) wall collision: place stone wall ahead, walk into it, x/z blocked
  int wx = (int)floor(player.x), wz = (int)floor(player.z) - 1;  // yaw=0 faces -Z
  for (int y = 0; y < 4; y++) set_voxel(wx, (int)floor(player.y) + y, wz, B_STONE);
  double zb = player.z;
  Input fwd = { .fwd = true };
  for (int i = 0; i < 60; i++) update_player(&fwd, 1.0f / 60.0f);
  CHECK(zb - player.z < 0.75, "stone wall stops forward movement");

  // 4) fall damage: teleport 10 blocks up, drop
  player.y += 10; player.vy = 0;
  int hp0 = player.health;
  for (int i = 0; i < 300; i++) update_player(&none, 1.0f / 60.0f);
  CHECK(player.on_ground, "lands after 10-block drop");
  CHECK(player.health < hp0, "fall damage applied for >3 block fall");

  // 5) collides() basic geometry
  CHECK(collides(player.x, -0.5, player.z), "below-world is solid");
  CHECK(!collides(player.x, WORLD_H + 1, player.z), "above-world is air");

  // 6) sprint: 1.6x on foot, 2.0x on top of the flying multiplier
  player.y = WORLD_H + 5; player.vy = 0;        // free air: no walls, no water
  Input walk = { .fwd = true };
  Input sprint = { .fwd = true, .sprint = true };
  update_player(&walk, 1.0f / 60.0f);
  double wv = sqrt(player.vx * player.vx + player.vz * player.vz);
  update_player(&sprint, 1.0f / 60.0f);
  double sv = sqrt(player.vx * player.vx + player.vz * player.vz);
  CHECK(fabs(sv / wv - 1.6) < 0.01, "sprint on foot is 1.6x walk speed");
  flying = true;
  update_player(&walk, 1.0f / 60.0f);
  double fv = sqrt(player.vx * player.vx + player.vz * player.vz);
  update_player(&sprint, 1.0f / 60.0f);
  double fsv = sqrt(player.vx * player.vx + player.vz * player.vz);
  CHECK(fabs(fv / wv - 2.2) < 0.01, "flying is 2.2x walk speed");
  CHECK(fabs(fsv / fv - 2.0) < 0.01, "sprint while flying doubles fly speed");
  flying = false;

  printf(fails ? "PHYS TEST: %d FAILURES\n" : "PHYS TEST: all passed\n", fails);
  return fails ? 1 : 0;
}
