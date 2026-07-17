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
  double coast = land * 3.0; if (coast > 1.0) coast = 1.0;   // gentle beaches

  // rolling base hills. hills^2.5 as mul+sqrt (correctly rounded; pow is not).
  double hills = fbm(dx * 0.018, dz * 0.018, 5);
  double relief = hills * hills * sqrt(hills) * 26.0;

  // mountains: a low-freq mask gates ridged noise, so ranges cluster instead of
  // spiking everywhere. ridge = 1-|2n-1| makes sharp crests along noise lines.
  double mask = fbm(dx * 0.0016 + 1000.0, dz * 0.0016 + 1000.0, 3);
  double m = (mask - 0.55) / 0.45;          // <=0 lowland, ramps to 1 alpine
  if (m > 0.0) {
    if (m > 1.0) m = 1.0;
    double rn = fbm(dx * 0.01 + 2000.0, dz * 0.01 + 2000.0, 5);
    double ridge = 1.0 - fabs(2.0 * rn - 1.0);
    relief += m * m * ridge * ridge * 34.0;
  }

  double height = land * 4.0 + relief * coast;

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
#define SNOW_LINE (SEA_Y + 30)

static int biome_at(int wx, int wz, int h) {
  if (h >= SNOW_LINE) return BIOME_SNOW;     // alpine caps regardless of climate
  double temp  = fbm((double)wx * 0.0009 + 8000.0, (double)wz * 0.0009 + 8000.0, 3);
  double humid = fbm((double)wx * 0.0009 + 9000.0, (double)wz * 0.0009 + 9000.0, 3);
  if (temp < 0.44) return BIOME_SNOW;                    // cold
  if (temp > 0.58 && humid < 0.46) return BIOME_DESERT;  // hot + dry
  if (humid > 0.56) return BIOME_FOREST;                 // wet
  return BIOME_PLAINS;
}

// genChunk(cx, cz) — forked: biome-aware surface + tree density
void gen_chunk_data(int cx, int cz, uint8_t *blocks, uint8_t *water) {
  for (int i = 0; i < CHUNK_VOL; i++) { blocks[i] = 0; water[i] = 0; }
  for (int lx = 0; lx < CHUNK; lx++)
    for (int lz = 0; lz < CHUNK; lz++) {
      int wx = cx * CHUNK + lx, wz = cz * CHUNK + lz;
      int h = terrain_height(wx, wz);
      int biome = biome_at(wx, wz, h);
      // surface (y==h) + the y>h-3 sub-band vary by biome; beaches stay sand.
      uint8_t top, sub;
      if (h <= SEA_Y + 1)             { top = B_SAND;  sub = B_SAND; }
      else if (biome == BIOME_DESERT) { top = B_SAND;  sub = B_SAND; }
      else if (biome == BIOME_SNOW)   { top = B_SNOW;  sub = B_DIRT; }
      else                            { top = B_GRASS; sub = B_DIRT; }
      for (int y = 0; y <= h; y++) {
        blocks[LI(lx, y, lz)] =
          y == 0 ? B_STONE :
          y == h ? top :
          y > h - 3 ? sub : B_STONE;
      }
      if (h < SEA_Y) for (int y = h + 1; y <= SEA_Y; y++) water[LI(lx, y, lz)] = 9;
      double forest = fbm((double)wx * 0.012 + 500.0, (double)wz * 0.012 + 500.0, 3);
      double tree_chance = forest > 0.6 ? 0.09 : forest > 0.45 ? 0.02 : 0.0;
      // desert bare, snow (taiga) sparse, forest denser
      tree_chance *= biome == BIOME_DESERT ? 0.0 : biome == BIOME_SNOW ? 0.5
                   : biome == BIOME_FOREST ? 1.2 : 1.0;
      if (h > SEA_Y + 1 && lx >= 2 && lx <= CHUNK - 3 && lz >= 2 && lz <= CHUNK - 3
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
