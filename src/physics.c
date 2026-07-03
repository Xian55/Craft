#include "physics.h"
#include "world.h"
#include "gen.h"
#include "config.h"
#include <math.h>

#include "inventory.h"

Player player = { 8.5, 0, 8.5, 0, 0, 0, 0, 0, false, 20, 20 };
bool flying = false;
bool creative = false;
float move_speed = 5.0f;
static double fall_start_y = 0;

void damage(int amount) {
  player.health -= amount;
  if (player.health <= 0) { player.health = 0; spawn_player(); }
}

// updateStats (game.js:1318): hunger drains ~1/15 s; fed heals, starving hurts.
static float stat_timer = 0, regen_timer = 0;
void update_stats(float dt) {
  stat_timer += dt; regen_timer += dt;
  if (stat_timer >= 15) { stat_timer = 0; if (player.hunger > 0) player.hunger--; }
  if (regen_timer >= 4) {
    regen_timer = 0;
    if (player.hunger >= 18 && player.health < 20) player.health++;
    else if (player.hunger == 0 && player.health > 0) damage(1);
  }
}

void eat_food(void) {
  if (item_count(I_PORK) > 0) { remove_item(I_PORK, 1); player.hunger += 8; }
  else if (player.hunger < 20) player.hunger += 6;
  if (player.hunger > 20) player.hunger = 20;
}

void spawn_player(void) {
  int sx = 8, sz = 8;
  // find land nearby so we don't spawn mid-ocean (game.js:1215)
  for (int r = 0; r <= 240; r += 4) {
    bool found = false;
    for (int a = 0; a < 8; a++) {
      int tx = 8 + (int)floor(cos((double)a) * r + 0.5);
      int tz = 8 + (int)floor(sin((double)a) * r + 0.5);
      if (terrain_height(tx, tz) > SEA_Y) { sx = tx; sz = tz; found = true; break; }
    }
    if (found) break;
  }
  player.x = sx + 0.5; player.z = sz + 0.5;
  int th = terrain_height(sx, sz);
  player.y = (th > SEA_Y ? th : SEA_Y) + 2;
  player.vx = player.vy = player.vz = 0;
  player.health = 20; player.hunger = 20;
}

bool collides(double px, double py, double pz) {
  for (int x = (int)floor(px - P_HALF); x <= (int)floor(px + P_HALF); x++)
    for (int y = (int)floor(py); y <= (int)floor(py + P_BODY); y++)
      for (int z = (int)floor(pz - P_HALF); z <= (int)floor(pz + P_HALF); z++)
        if (!passable(get_voxel(x, y, z))) return true;
  return false;
}

bool player_in_water(void) {
  return water_at((int)floor(player.x), (int)floor(player.y), (int)floor(player.z)) > 0
      || water_at((int)floor(player.x), (int)floor(player.y + 0.9), (int)floor(player.z)) > 0;
}

void update_player(const Input *in, float dt) {
  bool water = player_in_water();
  float speed = flying ? move_speed * 2.2f : water ? move_speed * 0.55f : move_speed;
  if (in->sprint && !water) speed *= flying ? 2.0f : 1.6f;   // Shift: sprint (fly too)

  int forward = (in->fwd ? 1 : 0) - (in->back ? 1 : 0);
  int strafe  = (in->right ? 1 : 0) - (in->left ? 1 : 0);
  float s = sinf(player.yaw), c = cosf(player.yaw);
  float mx = -s * forward + c * strafe;
  float mz = -c * forward - s * strafe;
  float len = sqrtf(mx * mx + mz * mz); if (len == 0) len = 1;
  int moving = (mx != 0 || mz != 0) ? 1 : 0;
  player.vx = (mx / len) * speed * moving;
  player.vz = (mz / len) * speed * moving;

  if (flying) {
    player.vy = in->jump ? 9 : in->sink ? -9 : 0;
  } else if (water) {
    player.vy += P_GRAVITY * 0.3f * dt;
    player.vy *= 0.85f;
    if (player.vy < -3) player.vy = -3;
    if (in->sink) player.vy = -4;
  } else {
    player.vy += P_GRAVITY * dt;
  }

  bool was_on_ground = player.on_ground;
  player.on_ground = false;

  double nx = player.x + player.vx * dt;
  if (!collides(nx, player.y, player.z)) player.x = nx; else player.vx = 0;

  double nz = player.z + player.vz * dt;
  if (!collides(player.x, player.y, nz)) player.z = nz; else player.vz = 0;

  double ny = player.y + player.vy * dt;
  if (!collides(player.x, ny, player.z)) {
    player.y = ny;
  } else {
    if (player.vy < 0) {
      player.on_ground = true;
      double fall = fall_start_y - player.y;
      if (fall > 3 && !water && !creative) damage((int)floor(fall - 3));
    }
    player.vy = 0;
  }

  if (was_on_ground && !player.on_ground) fall_start_y = player.y;
  if (player.on_ground || water) fall_start_y = player.y;

  if (!flying && in->jump) {
    if (water) player.vy = 3.5f;
    else if (player.on_ground) { player.vy = P_JUMP; player.on_ground = false; fall_start_y = player.y; }
  }

  if (player.y < -10) damage(20);   // fell out of the world
}

// DDA voxel raycast (port of raycastVoxel).
bool raycast_voxel(float max_dist, int hit[3], int prev[3]) {
  double ox = player.x, oy = player.y + P_EYE, oz = player.z;
  // look direction from yaw/pitch, matching three.js camera (rotation order YXZ,
  // forward is -Z): d = (-sin(yaw)cos(pitch), sin(pitch), -cos(yaw)cos(pitch))
  double cp = cos(player.pitch);
  double dx = -sin(player.yaw) * cp;
  double dy = sin(player.pitch);
  double dz = -cos(player.yaw) * cp;

  int x = (int)floor(ox), y = (int)floor(oy), z = (int)floor(oz);
  int px = x, py = y, pz = z;
  int step_x = dx > 0 ? 1 : -1, step_y = dy > 0 ? 1 : -1, step_z = dz > 0 ? 1 : -1;
  double tdx = dx != 0 ? fabs(1.0 / dx) : 1e30;
  double tdy = dy != 0 ? fabs(1.0 / dy) : 1e30;
  double tdz = dz != 0 ? fabs(1.0 / dz) : 1e30;
  double tmx = dx != 0 ? (dx > 0 ? (x + 1 - ox) : (ox - x)) * tdx : 1e30;
  double tmy = dy != 0 ? (dy > 0 ? (y + 1 - oy) : (oy - y)) * tdy : 1e30;
  double tmz = dz != 0 ? (dz > 0 ? (z + 1 - oz) : (oz - z)) * tdz : 1e30;
  double t = 0;
  while (t <= max_dist) {
    uint8_t b = get_voxel(x, y, z);
    if (b != B_AIR) {
      hit[0] = x; hit[1] = y; hit[2] = z;
      prev[0] = px; prev[1] = py; prev[2] = pz;
      return true;
    }
    px = x; py = y; pz = z;
    if (tmx < tmy && tmx < tmz) { x += step_x; t = tmx; tmx += tdx; }
    else if (tmy < tmz)         { y += step_y; t = tmy; tmy += tdy; }
    else                        { z += step_z; t = tmz; tmz += tdz; }
  }
  return false;
}
