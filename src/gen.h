// Deterministic terrain generation — bit-faithful port of game.js:76-145.
// Everything runs in IEEE double, exact same expression order as the JS.
// Any change here desyncs multiplayer with JS clients. Gate changes on
// tests/gen_test.c vs tools/terrain_ref.mjs.
#ifndef GEN_H
#define GEN_H

#include <stdint.h>

double js_hash2(double x, double z);
double value_noise(double x, double z);
double fbm(double x, double z, int oct);
int    terrain_height(int x, int z);
// Fills blocks[CHUNK_VOL] and water[CHUNK_VOL] (caller-zeroed not required).
void   gen_chunk_data(int cx, int cz, uint8_t *blocks, uint8_t *water);

#endif
