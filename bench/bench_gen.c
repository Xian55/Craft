// Terrain generation benches. gen is deterministic and DETERMINISM-GATED — these
// only READ it (terrain_height / fbm / gen_chunk_data), never alter behavior.
// gen.c inherits -ffp-contract=off (dir-scoped CMake property), so the noise here
// matches the shipping build. Pure math + a caller buffer -> zero heap allocs.
#include "bench.h"
#include "gen.h"
#include "config.h"
#include <stdint.h>

CRAFT_BENCH(gen_height_scan, 20, 0.30) {
  int64_t acc = 0;
  for (int i = 0; i < B->iters; i++)
    for (int x = -64; x <= 64; x += 8)
      for (int z = -64; z <= 64; z += 8)
        acc += terrain_height((x * (i + 1)) % 997, z);
  bench_sink_i64(acc);
}

CRAFT_BENCH(gen_fbm_scan, 40, 0.25) {
  double acc = 0;
  for (int i = 0; i < B->iters; i++)
    for (int x = -32; x <= 32; x += 4)
      for (int z = -32; z <= 32; z += 4)
        acc += fbm((double)x * 0.01, (double)(z + i) * 0.01, 4);
  bench_sink_i64((int64_t)acc);
}

// One full chunk generated into a static buffer -> provably zero heap allocs, so
// the memory ceiling is an exact 0 (a stray malloc in the gen path would FAIL).
// Budget grew with the generator: terrain shape -> biomes -> ores -> MC-1.18-style
// caves (3 cached-noise fields). ~0.33 ms measured; 0.55 leaves regression headroom.
CRAFT_BENCH_FULL(gen_chunk_fill, 200, 0.55, /*allocs*/ 0, /*bytes*/ 0) {
  static uint8_t blk[CHUNK_VOL], wat[CHUNK_VOL];
  for (int i = 0; i < B->iters; i++) {
    gen_chunk_data(i & 7, (i >> 3) & 7, blk, wat);
    bench_sink_i64(blk[0] ^ wat[CHUNK_VOL - 1]);
  }
}
