// Deterministic terrain. hash2/valueNoise/fbm are the original bit-faithful JS
// port (ToInt32 / Math.imul / OOB-write-drop emulated); terrainHeight/genChunk
// have since FORKED from the JS (JS cross-play dropped) to add masked mountains
// and carved rivers. The invariant is now C<->C determinism, guarded by the C
// golden (tests/gen_golden.txt): all arithmetic in double, no pow (mul+sqrt).
// Do NOT compile with -ffast-math; gen.c keeps -ffp-contract=off.
#include "gen.h"
#include "config.h"
#include <math.h>

// ECMA-262 ToInt32 of a double (the JS `x | 0`). Valid for |d| < 2^63, which
// covers every value this generator produces. C cast to int64 truncates
// toward zero, same as ToInt32's truncation step; low 32 bits keep JS's
// modular wrap.
static int32_t to_i32(double d) {
  if (!(d == d) || isinf(d)) return 0;      // NaN / Inf -> 0 (ECMA)
  return (int32_t)(uint32_t)(uint64_t)(int64_t)d;
}

// Math.imul: int32 multiply with modular wrap.
static int32_t js_imul(int32_t a, int32_t b) {
  return (int32_t)((uint32_t)a * (uint32_t)b);
}

// game.js:76 — hash2(x, z)
// JS evaluates (x|0)*374761393 + (z|0)*668265263 in DOUBLE (products can
// exceed 2^53 and round!), then applies |0. Reproduce exactly.
double js_hash2(double x, double z) {
  double sum = (double)to_i32(x) * 374761393.0 + (double)to_i32(z) * 668265263.0;
  int32_t h = to_i32(sum);
  h = js_imul((int32_t)((uint32_t)h ^ ((uint32_t)h >> 13)), 1274126177);
  uint32_t r = (uint32_t)h ^ ((uint32_t)h >> 16);
  return (double)r / 4294967296.0;          // 0..1
}

// game.js:81 — valueNoise(x, z), smoothstep-interpolated
double value_noise(double x, double z) {
  double xi = floor(x), zi = floor(z), xf = x - xi, zf = z - zi;
  double u = xf * xf * (3.0 - 2.0 * xf), v = zf * zf * (3.0 - 2.0 * zf);
  double a = js_hash2(xi, zi),       b = js_hash2(xi + 1.0, zi);
  double c = js_hash2(xi, zi + 1.0), d = js_hash2(xi + 1.0, zi + 1.0);
  return a * (1.0 - u) * (1.0 - v) + b * u * (1.0 - v) + c * (1.0 - u) * v + d * u * v;
}

// game.js:87 — fbm(x, z, oct)
double fbm(double x, double z, int oct) {
  double sum = 0.0, amp = 1.0, freq = 1.0, norm = 0.0;
  for (int i = 0; i < oct; i++) {
    sum += amp * value_noise(x * freq, z * freq);
    norm += amp; amp *= 0.5; freq *= 2.0;
  }
  return sum / norm;
}

// JS Math.round rounds halves toward +infinity; both call sites are
// non-negative, where floor(x + 0.5) is identical.
static double js_round_nonneg(double d) { return floor(d + 0.5); }

// terrainHeight(x, z) — continental ocean/land, plus masked ridged mountains and
// carved rivers (forked from JS). Deterministic doubles only; no pow (mul+sqrt).
int terrain_height(int x, int z) {
  double dx = (double)x, dz = (double)z;
  double cont = fbm(dx * 0.004, dz * 0.004, 4);
  if (cont < 0.42) {                        // ocean floor below sea level
    double dep = js_round_nonneg((0.42 - cont) / 0.42 * 12.0);
    int h = SEA_Y - 2 - (int)dep;
    return h < 1 ? 1 : h;
  }
  double land = (cont - 0.42) / 0.58;       // 0 at coast .. 1 inland
  double coast = land * 2.0; if (coast > 1.0) coast = 1.0;   // gentle beaches

  // rolling base hills: a small flat floor + hills^2 (gentler than ^2.5 so
  // valleys don't lie dead flat and the hills stand up more).
  double hills = fbm(dx * 0.016, dz * 0.016, 5);
  double relief = (0.15 + hills * hills) * 28.0;

  // mountains: a low-freq mask gates ridged noise so ranges cluster; a broad
  // rise (ridge) plus sharp crests (ridge^2), scaled by the mask (m, not m^2)
  // and a big amplitude so peaks tower toward the world ceiling and snow-cap.
  double mask = fbm(dx * 0.0015 + 1000.0, dz * 0.0015 + 1000.0, 3);
  double m = (mask - 0.42) / 0.58;          // <=0 lowland, ramps to 1 alpine
  if (m > 0.0) {
    if (m > 1.0) m = 1.0;
    double rn = fbm(dx * 0.009 + 2000.0, dz * 0.009 + 2000.0, 5);
    double ridge = 1.0 - fabs(2.0 * rn - 1.0);
    relief += m * (ridge * 26.0 + ridge * ridge * 48.0);
  }

  double height = land * 3.0 + relief * coast;

  // rivers: a thin winding ridge band carves a fixed-depth channel; where it
  // dips below SEA_Y, gen_chunk_data fills it with water (lowland rivers hold
  // water, mountain crossings become dry notches).
  double rv = fbm(dx * 0.0035 + 5000.0, dz * 0.0035 + 5000.0, 4);
  double river = 1.0 - fabs(2.0 * rv - 1.0);
  if (river > 0.86) {
    double carve = (river - 0.86) / 0.14;   // 0 at bank .. 1 at channel center
    height -= carve * carve * 7.0;
  }

  int h = SEA_Y - 1 + (int)floor(height + 0.5);
  if (h < 1) h = 1;
  if (h > WORLD_H - 4) h = WORLD_H - 4;
  return h;
}

// In JS, writes/reads outside a Uint8Array are silently dropped / undefined.
// Tree canopies at max terrain height do go past WORLD_H — emulate the drop.
static void set_safe(uint8_t *blocks, int lx, int y, int lz, uint8_t v) {
  if (y >= 0 && y < WORLD_H) blocks[LI(lx, y, lz)] = v;
}
static int get_is_air(const uint8_t *blocks, int lx, int y, int lz) {
  if (y < 0 || y >= WORLD_H) return 0;      // JS: undefined === AIR is false
  return blocks[LI(lx, y, lz)] == B_AIR;
}

// Biomes (forked): low-freq temperature + humidity noise pick the surface
// palette and tree density; peaks above the snow line are always snow-capped.
enum { BIOME_PLAINS, BIOME_DESERT, BIOME_FOREST, BIOME_SNOW };
// Altitude surface bands (dithered by a per-column hash so the lines aren't hard
// contours): grass/biome -> bare ROCK -> SNOW cap. Trees stop at the rock line.
#define ROCK_LINE (SEA_Y + 20)   // 36: bare stone above this
#define SNOW_LINE (SEA_Y + 30)   // 46: snow cap above this

static int biome_at(int wx, int wz) {          // climate only (altitude handled separately)
  double temp  = fbm((double)wx * 0.0009 + 8000.0, (double)wz * 0.0009 + 8000.0, 3);
  double humid = fbm((double)wx * 0.0009 + 9000.0, (double)wz * 0.0009 + 9000.0, 3);
  if (temp < 0.44) return BIOME_SNOW;                    // cold
  if (temp > 0.58 && humid < 0.46) return BIOME_DESERT;  // hot + dry
  if (humid > 0.56) return BIOME_FOREST;                 // wet
  return BIOME_PLAINS;
}

// 3D hash for ore veins: fold y into the 2D hash with distinct primes.
// (>>2 groups blocks into 4x4x4 cells; arithmetic shift = floor-div, stable.)
static double hash3(int x, int y, int z) {
  return js_hash2((double)x + (double)y * 1013.0, (double)z - (double)y * 1409.0);
}
// Deep stone -> coal (shallow-mid) or iron (deeper) where a coarse-cell vein hash
// AND a finer per-block hash both hit, giving small ragged blobs.
static uint8_t orify(int wx, int wy, int wz, int h) {
  int cx = wx >> 2, cy = wy >> 2, cz = wz >> 2;
  if (wy >= 5 && wy <= h - 5 && hash3(cx, cy, cz) < 0.030
      && hash3(wx * 2 + 1, wy * 2, wz * 2 + 1) < 0.72) return B_COAL_ORE;
  if (wy >= 2 && wy <= 30 && hash3(cx + 777, cy, cz) < 0.020
      && hash3(wx * 2, wy * 2 + 1, wz * 2) < 0.62) return B_IRON_ORE;
  return B_STONE;
}

// Caves — MC 1.18-style "noise caves", stateless + deterministic (no Perlin
// worms, which need cross-chunk state our per-chunk gen can't carry):
//   * cheese    — one 3D value-noise field over a high threshold -> big caverns
//   * spaghetti — the intersection of TWO isosurfaces (|n-0.5| < eps on both
//                 fields) -> long, winding, interconnected tunnels
// Each field is a coarse hashed lattice (freq ~0.04-0.08) cached once per chunk
// and trilinearly interpolated per voxel, so is_cave does ZERO hashing in the
// loop; the spaghetti B field is only sampled when A's isosurface already hit.
static double smooth1(double t) { return t * t * (3.0 - 2.0 * t); }
typedef struct { double lat[6][12][6]; int lx0, lz0; double fx, fy; int seed; } CaveField;
static CaveField g_cheese = { .fx = 0.040, .fy = 0.085, .seed = 0 };
static CaveField g_spagA  = { .fx = 0.080, .fy = 0.080, .seed = 7001 };
static CaveField g_spagB  = { .fx = 0.080, .fy = 0.080, .seed = 43051 };

static double cave_hash(int x, int y, int z, int seed) {   // 0..1 at a lattice point
  return js_hash2((double)x + (double)z * 57.0 + seed, (double)y * 131.0 - (double)z * 19.0 - seed);
}
static void field_build(CaveField *f, int x0, int z0) {
  f->lx0 = (int)floor(x0 * f->fx);
  f->lz0 = (int)floor(z0 * f->fx);
  int lx1 = (int)floor((x0 + CHUNK - 1) * f->fx);
  int lz1 = (int)floor((z0 + CHUNK - 1) * f->fx);
  int LX = lx1 - f->lx0 + 2, LZ = lz1 - f->lz0 + 2;
  int LY = (int)floor((WORLD_H - 1) * f->fy) + 2;
  for (int ix = 0; ix < LX; ix++)
    for (int iy = 0; iy < LY; iy++)
      for (int iz = 0; iz < LZ; iz++)
        f->lat[ix][iy][iz] = cave_hash(f->lx0 + ix, iy, f->lz0 + iz, f->seed);
}
static double field_at(const CaveField *f, int wx, int wy, int wz) {
  double nx = wx * f->fx, ny = wy * f->fy, nz = wz * f->fx;
  int xi = (int)floor(nx) - f->lx0, yi = (int)floor(ny), zi = (int)floor(nz) - f->lz0;
  double u = smooth1(nx - floor(nx)), v = smooth1(ny - floor(ny)), w = smooth1(nz - floor(nz));
  double c000 = f->lat[xi][yi][zi],         c100 = f->lat[xi + 1][yi][zi];
  double c010 = f->lat[xi][yi + 1][zi],     c110 = f->lat[xi + 1][yi + 1][zi];
  double c001 = f->lat[xi][yi][zi + 1],     c101 = f->lat[xi + 1][yi][zi + 1];
  double c011 = f->lat[xi][yi + 1][zi + 1], c111 = f->lat[xi + 1][yi + 1][zi + 1];
  double x00 = c000 + (c100 - c000) * u, x10 = c010 + (c110 - c010) * u;
  double x01 = c001 + (c101 - c001) * u, x11 = c011 + (c111 - c011) * u;
  double y0 = x00 + (x10 - x00) * v, y1 = x01 + (x11 - x01) * v;
  return y0 + (y1 - y0) * w;
}
static void cave_build(int x0, int z0) {
  field_build(&g_cheese, x0, z0);
  field_build(&g_spagA, x0, z0);
  field_build(&g_spagB, x0, z0);
}
static int is_cave(int wx, int wy, int wz) {
  if (field_at(&g_cheese, wx, wy, wz) > 0.835) return 1;                // cheese cavern
  if (fabs(field_at(&g_spagA, wx, wy, wz) - 0.5) >= 0.060) return 0;    // early out
  return fabs(field_at(&g_spagB, wx, wy, wz) - 0.5) < 0.060;            // spaghetti tunnel
}

// genChunk(cx, cz) — forked: biome-aware surface + tree density
void gen_chunk_data(int cx, int cz, uint8_t *blocks, uint8_t *water) {
  for (int i = 0; i < CHUNK_VOL; i++) { blocks[i] = 0; water[i] = 0; }
  cave_build(cx * CHUNK, cz * CHUNK);          // cache the cave-noise lattice once
  for (int lx = 0; lx < CHUNK; lx++)
    for (int lz = 0; lz < CHUNK; lz++) {
      int wx = cx * CHUNK + lx, wz = cz * CHUNK + lz;
      int h = terrain_height(wx, wz);
      int biome = biome_at(wx, wz);
      // Surface + sub-band: altitude ROCK then SNOW bands (dithered by a per-
      // column hash so the lines aren't hard contours) layered over the climate
      // biome, so mountains grade grass -> bare rock -> snow cap, not grass->snow.
      double alt = h + (js_hash2((double)wx * 7.0 + 1.0, (double)wz * 7.0 + 2.0) - 0.5) * 7.0;
      uint8_t top, sub;
      if (h <= SEA_Y + 1)             { top = B_SAND;  sub = B_SAND;  }   // beach
      else if (alt >= SNOW_LINE)      { top = B_SNOW;  sub = B_STONE; }   // snow cap
      else if (alt >= ROCK_LINE)      { top = B_STONE; sub = B_STONE; }   // bare rock
      else if (biome == BIOME_DESERT) { top = B_SAND;  sub = B_SAND;  }
      else if (biome == BIOME_SNOW)   { top = B_SNOW;  sub = B_DIRT;  }   // cold-biome snow (low)
      else                            { top = B_GRASS; sub = B_DIRT;  }
      for (int y = 0; y <= h; y++) {
        uint8_t blk = y == 0 ? B_STONE :
                      y == h ? top :
                      y > h - 3 ? sub : B_STONE;
        if (blk == B_STONE && y > 0) blk = orify(wx, y, wz, h);   // ore veins in deep stone
        if (y >= 3 && (blk == B_STONE || blk == B_COAL_ORE || blk == B_IRON_ORE)
            && is_cave(wx, y, wz)) blk = B_AIR;                    // carve caverns (exposes ore)
        blocks[LI(lx, y, lz)] = blk;
      }
      if (h < SEA_Y) for (int y = h + 1; y <= SEA_Y; y++) water[LI(lx, y, lz)] = 9;
      double forest = fbm((double)wx * 0.012 + 500.0, (double)wz * 0.012 + 500.0, 3);
      double tree_chance = forest > 0.6 ? 0.09 : forest > 0.45 ? 0.02 : 0.0;
      // desert bare, snow (taiga) sparse, forest denser
      tree_chance *= biome == BIOME_DESERT ? 0.0 : biome == BIOME_SNOW ? 0.5
                   : biome == BIOME_FOREST ? 1.2 : 1.0;
      if (h > SEA_Y + 1 && alt < ROCK_LINE && lx >= 2 && lx <= CHUNK - 3 && lz >= 2 && lz <= CHUNK - 3
          && tree_chance > 0.0
          && js_hash2((double)wx * 131.0 + 7.0, (double)wz * 131.0 + 13.0) < tree_chance) {
        int base = h + 1;
        for (int i = 0; i < 4; i++) set_safe(blocks, lx, base + i, lz, B_WOOD);
        int top = base + 4;
        for (int dx = -2; dx <= 2; dx++)
          for (int dz = -2; dz <= 2; dz++)
            for (int dy = -1; dy <= 1; dy++) {
              if ((dx == 2 || dx == -2) && (dz == 2 || dz == -2)) continue;
              if (get_is_air(blocks, lx + dx, top + dy, lz + dz))
                set_safe(blocks, lx + dx, top + dy, lz + dz, B_LEAVES);
            }
      }
    }
}
