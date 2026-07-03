// Chunk storage + voxel/water accessors — port of game.js:69-207.
#ifndef CRAFT_WORLD_H
#define CRAFT_WORLD_H

#include <stdint.h>
#include <stdbool.h>
#include "raylib.h"
#include "config.h"

typedef struct Chunk {
  int cx, cz;
  uint8_t blocks[CHUNK_VOL];
  uint8_t water[CHUNK_VOL];
  uint8_t light_sky[CHUNK_VOL];
  uint8_t light_blk[CHUNK_VOL];
  bool has_light;          // lazily computed, invalidated on edit (c.light in JS)
  bool meshed;             // has GPU meshes (even if some are empty)
  bool modified;           // contains player edits -> never unload
  // GPU meshes: solid + 4 fluid buckets (water still/flow, lava still/flow)
  Mesh mesh_solid, mesh_wstill, mesh_wflow, mesh_lstill, mesh_lflow;
  bool has_solid, has_wstill, has_wflow, has_lstill, has_lflow;
  bool alive;              // slot in use (hash map)
} Chunk;

Chunk  *ensure_chunk(int cx, int cz);
Chunk  *chunk_get(int cx, int cz);           // NULL if not generated
void    world_each_chunk(void (*fn)(Chunk *c, void *ud), void *ud);
void    unload_chunk_meshes(Chunk *c);       // free GPU meshes only
void    world_remove_chunk(Chunk *c);        // free slot entirely

// floor-division chunk coordinate of a world coordinate
static inline int chunk_of(int v) { return v >= 0 ? v / CHUNK : -((-v - 1) / CHUNK) - 1; }

uint8_t get_voxel(int x, int y, int z);      // y<0 -> STONE, y>=WORLD_H -> AIR
void    set_voxel(int x, int y, int z, uint8_t v);
uint8_t raw_water(int x, int y, int z);
void    set_water_raw(int x, int y, int z, uint8_t val);
int     water_at(int x, int y, int z);       // 0 or 1..8 (source counts as 8)
int     lava_at(int x, int y, int z);        // 0 or 1..8

// game.js TRANSPARENT set: AIR, LEAVES, TORCH don't occlude
static inline bool occludes(uint8_t id) { return !(id == B_AIR || id == B_LEAVES || id == B_TORCH); }
static inline bool passable(uint8_t id) { return id == B_AIR || id == B_TORCH; }

#endif
