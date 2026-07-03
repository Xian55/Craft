// Baked lighting — port of game.js:419-517 (floodLight/computeLight/cornerShade).
#ifndef LIGHT_H
#define LIGHT_H

#include "world.h"

void compute_light(Chunk *c);                 // fills light_sky/light_blk, sets has_light
int  get_light_sky(int x, int y, int z);      // 0..15, lazy per-chunk compute
int  get_light_blk(int x, int y, int z);

// Shade of one face corner: smooth light + ambient occlusion.
// dir = face normal, corner = FACES corner entry (x,y,z,u,v), tang = the two
// in-plane axes, dir_f = directional face factor. out = {sky, blk} in 0..1.
void corner_shade(int x, int y, int z, const int dir[3], const int corner[5],
                  const int tang[2], float dir_f, float out[2]);

#endif
