// Terrain determinism harness — must produce byte-identical output to
// tools/terrain_ref.mjs. Build target: gen_test. Usage:
//   bun tools/terrain_ref.mjs > ref.txt && build/gen_test.exe > c.txt && diff ref.txt c.txt
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
  // 3) FNV-1a of chunk contents
  static uint8_t blocks[CHUNK_VOL], water[CHUNK_VOL];
  for (int cx = -3; cx <= 3; cx++)
    for (int cz = -3; cz <= 3; cz++) {
      gen_chunk_data(cx, cz, blocks, water);
      uint32_t h = fnv1a(2166136261u, blocks, CHUNK_VOL);
      h = fnv1a(h, water, CHUNK_VOL);
      printf("c %d %d %x\n", cx, cz, h);
    }
  return 0;
}
