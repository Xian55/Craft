#include "world.h"
#include "gen.h"
#include <stdlib.h>
#include <string.h>

// Open-addressed hash map of chunk pointers. Sized for /rd 50: the live set
// is (2*rd+5)^2 ≈ 11k chunks, kept under ~35% load; grow is not needed, we
// assert-fail instead.
#define MAP_CAP 32768   // power of two
static Chunk *map[MAP_CAP];
static bool tomb[MAP_CAP];

static uint64_t ckey64(int cx, int cz) {
  return ((uint64_t)(uint32_t)cx << 32) | (uint32_t)cz;
}
static uint32_t khash(uint64_t k) {
  k ^= k >> 33; k *= 0xff51afd7ed558ccdULL; k ^= k >> 33;
  return (uint32_t)k & (MAP_CAP - 1);
}

Chunk *chunk_get(int cx, int cz) {
  uint64_t key = ckey64(cx, cz);
  uint32_t i = khash(key);
  for (uint32_t n = 0; n < MAP_CAP; n++, i = (i + 1) & (MAP_CAP - 1)) {
    if (map[i]) { if (map[i]->cx == cx && map[i]->cz == cz) return map[i]; }
    else if (!tomb[i]) return NULL;
  }
  return NULL;
}

static void map_put(Chunk *c) {
  uint32_t i = khash(ckey64(c->cx, c->cz));
  for (uint32_t n = 0; n < MAP_CAP; n++, i = (i + 1) & (MAP_CAP - 1)) {
    if (!map[i]) { map[i] = c; tomb[i] = false; return; }
  }
  // table full — should never happen with unloading active
  abort();
}

Chunk *ensure_chunk(int cx, int cz) {
  Chunk *c = chunk_get(cx, cz);
  if (c) return c;
  c = calloc(1, sizeof(Chunk));
  c->cx = cx; c->cz = cz; c->alive = true;
  gen_chunk_data(cx, cz, c->blocks, c->water);
  map_put(c);
  return c;
}

void world_each_chunk(void (*fn)(Chunk *c, void *ud), void *ud) {
  for (uint32_t i = 0; i < MAP_CAP; i++) if (map[i]) fn(map[i], ud);
}

void unload_chunk_meshes(Chunk *c) {
  if (c->has_solid)  { UnloadMesh(c->mesh_solid);  c->has_solid = false; }
  if (c->has_wstill) { UnloadMesh(c->mesh_wstill); c->has_wstill = false; }
  if (c->has_wflow)  { UnloadMesh(c->mesh_wflow);  c->has_wflow = false; }
  if (c->has_lstill) { UnloadMesh(c->mesh_lstill); c->has_lstill = false; }
  if (c->has_lflow)  { UnloadMesh(c->mesh_lflow);  c->has_lflow = false; }
  c->meshed = false;
}

void world_remove_chunk(Chunk *c) {
  unload_chunk_meshes(c);
  uint32_t i = khash(ckey64(c->cx, c->cz));
  for (uint32_t n = 0; n < MAP_CAP; n++, i = (i + 1) & (MAP_CAP - 1)) {
    if (map[i] == c) { map[i] = NULL; tomb[i] = true; break; }
    if (!map[i] && !tomb[i]) break;
  }
  free(c);
}

uint8_t get_voxel(int x, int y, int z) {
  if (y < 0) return B_STONE;
  if (y >= WORLD_H) return B_AIR;
  int cx = chunk_of(x), cz = chunk_of(z);
  return ensure_chunk(cx, cz)->blocks[LI(x - cx * CHUNK, y, z - cz * CHUNK)];
}

void set_voxel(int x, int y, int z, uint8_t v) {
  if (y < 0 || y >= WORLD_H) return;
  int cx = chunk_of(x), cz = chunk_of(z);
  Chunk *c = ensure_chunk(cx, cz);
  c->blocks[LI(x - cx * CHUNK, y, z - cz * CHUNK)] = v;
  c->modified = true;
  c->has_light = false;
}

uint8_t raw_water(int x, int y, int z) {
  if (y < 0 || y >= WORLD_H) return 0;
  int cx = chunk_of(x), cz = chunk_of(z);
  return ensure_chunk(cx, cz)->water[LI(x - cx * CHUNK, y, z - cz * CHUNK)];
}

void set_water_raw(int x, int y, int z, uint8_t val) {
  if (y < 0 || y >= WORLD_H) return;
  int cx = chunk_of(x), cz = chunk_of(z);
  Chunk *c = ensure_chunk(cx, cz);
  c->water[LI(x - cx * CHUNK, y, z - cz * CHUNK)] = val;
  c->modified = true;
  c->has_light = false;   // lava is a light source
}

int water_at(int x, int y, int z) {
  if (get_voxel(x, y, z) != B_AIR) return 0;
  uint8_t w = raw_water(x, y, z);
  if (w < 1 || w > 9) return 0;
  return w == 9 ? 8 : w;
}

int lava_at(int x, int y, int z) {
  if (get_voxel(x, y, z) != B_AIR) return 0;
  uint8_t v = raw_water(x, y, z);
  if (v < 11) return 0;
  return v == 19 ? 8 : v - 10;
}
