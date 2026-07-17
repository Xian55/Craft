// Shared constants — must stay identical to game.js for multiplayer cross-play.
#ifndef CONFIG_H
#define CONFIG_H

#define CHUNK      16   // chunk width/depth
#define WORLD_H    64   // world height
#define SEA_Y      16   // sea level
#define CHUNK_VOL  (CHUNK * CHUNK * WORLD_H)

// local index inside a chunk — must match li() in game.js
#define LI(lx, y, lz) ((lx) + (lz) * CHUNK + (y) * CHUNK * CHUNK)

// Block ids (game.js)
#define B_AIR          0
#define B_GRASS        1
#define B_DIRT         2
#define B_STONE        3
#define B_WOOD         4
#define B_LEAVES       5
#define B_SAND         6
#define B_PLANKS       7
#define B_COBBLE       8
#define B_STONE_BRICK  9
#define B_BRICK       10
#define B_TORCH       11
#define B_CHEST       12
#define B_TNT         13
#define B_SNOW        14
#define B_COAL_ORE    15
#define B_IRON_ORE    16
#define B_FURNACE     17
#define B_GLASS       18

// Water array semantics: 0 none, 1..8 flowing water, 9 source,
// 11..18 flowing lava, 19 lava source.

// Player physics (game.js)
#define P_EYE     1.6f
#define P_HALF    0.3f
#define P_BODY    1.8f
#define P_GRAVITY (-26.0f)
#define P_JUMP    8.5f

#define RENDER_DIST 4

#endif
