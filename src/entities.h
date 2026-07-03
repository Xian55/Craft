// World entities: falling sand, primed TNT, dropped items, pigs.
// Ports of game.js:689-834 (sand/drops/TNT) and 1466-1621 (pigs).
#ifndef ENTITIES_H
#define ENTITIES_H

#include "raylib.h"
#include <stdbool.h>

void entities_init(void);                 // build shared meshes, load pig texture

void check_fall(int x, int y, int z);     // sand above a new hole starts falling
void update_falling(float dt);

void prime_tnt(int x, int y, int z, float fuse);
void update_primed(float dt);
void explode_at(int cx, int cy, int cz);

void spawn_drop(int id, int count, double x, double y, double z, float delay);
void drop_held(void);                     // Q: toss one from the selected slot
void update_drops(float dt);

void spawn_pigs(int n);
void update_pigs(float dt);
int  pig_count(void);

// Hostile mobs: zombies chase and hit, skeletons keep range and shoot.
// Spawn at night near the player, burn away in daylight. Client-local
// (like pigs — not synced over the wire).
enum { MOB_ZOMBIE, MOB_SKELETON, MOB_CREEPER };
void spawn_mobs(int type, int n);
void update_mobs(float dt);               // AI + arrows + night spawner
int  mob_count(void);
bool attack_creature(void);               // LMB: true if a mob or pig was hit

// --- mob sync (see net.c): one client is the authority, the rest mirror ---
#include "proto.h"
void entities_set_authority(bool is_master);  // false: no AI, lerp imported states
int  mobs_export(PMob *out, PArrow *aout, int *na);
void mobs_import(const PMob *in, int n, const PArrow *ain, int na);
void mob_apply_hit(int slot, int dmg, float kx, float kz);   // authority side
void explode_remote(int x, int y, int z);     // SV_BOOM: replicate the crater

void draw_entities(Camera3D cam);         // everything above, 3D pass

// Item shown in a remote player's hand: mini block cube (atlas) or flat
// items.png billboard. pos = world-space hand position, yaw = body yaw.
void draw_world_held(Camera3D cam, int id, Vector3 pos, float yaw);

#endif
