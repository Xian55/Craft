// Terrain determinism harness. The gate is now a committed C golden
// (tests/gen_golden.txt) — craft's gen forked from the legacy JS (JS cross-play
// dropped), so this guards C<->C multiplayer determinism, not JS parity. Usage:
//   gen_test.exe > c.txt && diff --strip-trailing-cr tests/gen_golden.txt c.txt   # must be empty
// Bless an intentional gen change: gen_test.exe > tests/gen_golden.txt (commit it).
#include "../src/gen.h"
#include "../src/config.h"
#include <stdio.h>
#include <stdint.h>

static uint32_t fnv1a(uint32_t h, const uint8_t *arr, int n) {
  for (int i = 0; i < n; i++) { h ^= arr[i]; h *= 16777619u; }
  return h;
}

int main(void) {
  // 1) height samples
  for (int x = -512; x <= 512; x += 7)
    for (int z = -512; z <= 512; z += 7)
      printf("h %d %d %d\n", x, z, terrain_height(x, z));
  // 2) hash2 direct samples with huge coords (ToInt32 wrap territory)
  for (int64_t x = -3000000000LL; x <= 3000000000LL; x += 700000001LL)
    for (int64_t z = -3000000000LL; z <= 3000000000LL; z += 900000007LL)
      printf("n %lld %lld %.17f\n", (long long)x, (long long)z, js_hash2((double)x, (double)z));
  // 3) FNV-1a of full chunk contents (blocks + water) over an 11x11 grid — a
  // single byte change anywhere (shape, biome, ore, cave) flips a hash.
  static uint8_t blocks[CHUNK_VOL], water[CHUNK_VOL];
  for (int cx = -5; cx <= 5; cx++)
    for (int cz = -5; cz <= 5; cz++) {
      gen_chunk_data(cx, cz, blocks, water);
      uint32_t h = fnv1a(2166136261u, blocks, CHUNK_VOL);
      h = fnv1a(h, water, CHUNK_VOL);
      printf("c %d %d %x\n", cx, cz, h);
    }
  return 0;
}
