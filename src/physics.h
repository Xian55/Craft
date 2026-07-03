// Player physics + voxel raycast — port of game.js:1203-1311 and raycastVoxel.
#ifndef PHYSICS_H
#define PHYSICS_H

#include <stdbool.h>

typedef struct Player {
  double x, y, z;          // feet position
  float vx, vy, vz;
  float yaw, pitch;
  bool on_ground;
  int health, hunger;
} Player;

typedef struct Input {
  bool fwd, back, left, right, jump, sink, sprint;   // WASD + Space + Ctrl + Shift
} Input;

extern Player player;
extern bool flying;
extern bool creative;
extern float move_speed;

void spawn_player(void);
void damage(int amount);          // respawns at 0 health
void eat_food(void);              // F key (pork restores more)
void update_stats(float dt);      // hunger drain + regen (game.js:1318)
bool collides(double px, double py, double pz);
bool player_in_water(void);
void update_player(const Input *in, float dt);

// DDA voxel raycast from the eye along the look direction.
// Returns true on hit; hit = solid cell, prev = last empty cell before it.
bool raycast_voxel(float max_dist, int hit[3], int prev[3]);

#endif
