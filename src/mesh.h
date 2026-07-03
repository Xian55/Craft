// Chunk meshing + rendering — port of game.js:354-684.
#ifndef MESH_H
#define MESH_H

#include "world.h"

void mesh_init(void);                 // load atlas + fluid textures, materials
void mesh_chunk(int cx, int cz);      // build/upload solid + fluid meshes
void remesh_around(int x, int z);     // chunk of (x,z) + boundary neighbors
// Deferred remesh: dedupes and rebuilds a few chunks per frame in
// update_chunks. Use for bursts (explosions) — a synchronous 3x3 remesh per
// dirty chunk was a 13-38 ms frame spike.
void mesh_enqueue(int cx, int cz);
void rebuild_chunk_solid(int cx, int cz);  // one bucket only (fluid sim / relight)
void rebuild_chunk_water(int cx, int cz);
void update_chunks(double px, double pz);  // stream: mesh nearest, unload far
void draw_world(void);                // solids then fluids (alpha)

// Day-night: 0.2 (night) .. 1.0 (day). Darkens the sky light channel and the
// water tint; torch/lava light is unaffected (game.js uDaylight).
void mesh_set_daylight(float b);

// Unit cube centered on origin with the atlas UVs of a block (held/falling/
// dropped cubes — buildHeldCubeGeo). Caller owns the Mesh (UnloadMesh).
Mesh build_block_cube_mesh(uint8_t block);
Material *block_cube_material(void);  // atlas material for the cubes above
int render_dist_get(void);
void render_dist_set(int d);

#endif
