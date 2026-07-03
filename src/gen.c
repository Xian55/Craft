// Deterministic terrain — bit-faithful port of game.js hash2/valueNoise/fbm/
// terrainHeight/genChunk. All arithmetic in double, same expression order as
// the JS source; JS-specific semantics (ToInt32, Math.imul, typed-array
// out-of-bounds writes being silently dropped) are emulated explicitly.
// Do NOT compile with -ffast-math.
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

// game.js:95 — terrainHeight(x, z)
int terrain_height(int x, int z) {
  double cont = fbm((double)x * 0.004, (double)z * 0.004, 4);
  if (cont < 0.42) {                        // ocean floor below sea level
    double dep = js_round_nonneg((0.42 - cont) / 0.42 * 12.0);
    int h = SEA_Y - 2 - (int)dep;
    return h < 1 ? 1 : h;
  }
  double land = (cont - 0.42) / 0.58;
  double coast = land * 3.0; if (coast > 1.0) coast = 1.0;
  double hills = fbm((double)x * 0.018, (double)z * 0.018, 5);
  // hills^2.5 spelled as mul+sqrt: pow is not correctly rounded across
  // platforms, mul/sqrt are — keeps terrain identical to (patched) game.js.
  double relief = hills * hills * sqrt(hills) * 55.0;
  int h = SEA_Y - 1 + (int)js_round_nonneg(land * 4.0 + relief * coast);
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

// game.js:111 — genChunk(cx, cz)
void gen_chunk_data(int cx, int cz, uint8_t *blocks, uint8_t *water) {
  for (int i = 0; i < CHUNK_VOL; i++) { blocks[i] = 0; water[i] = 0; }
  for (int lx = 0; lx < CHUNK; lx++)
    for (int lz = 0; lz < CHUNK; lz++) {
      int wx = cx * CHUNK + lx, wz = cz * CHUNK + lz;
      int h = terrain_height(wx, wz);
      for (int y = 0; y <= h; y++) {
        blocks[LI(lx, y, lz)] =
          y == 0 ? B_STONE :
          y == h ? (h <= SEA_Y + 1 ? B_SAND : B_GRASS) :
          y > h - 3 ? B_DIRT : B_STONE;
      }
      if (h < SEA_Y) for (int y = h + 1; y <= SEA_Y; y++) water[LI(lx, y, lz)] = 9;
      double forest = fbm((double)wx * 0.012 + 500.0, (double)wz * 0.012 + 500.0, 3);
      double tree_chance = forest > 0.6 ? 0.09 : forest > 0.45 ? 0.02 : 0.0;
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
