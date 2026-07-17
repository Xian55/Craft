// Lighting bench: the two-channel flood-fill + AO relight in isolation.
// compute_light always recomputes (unconditional memset + reflood), so this is a
// clean measure of one full chunk relight. Note the overlap: mesh_solid_chunk
// already includes one compute_light per build, so subtracting light_compute_chunk
// from it approximates the pure greedy-mesher cost.
#include "bench.h"
#include "world.h"
#include "light.h"
#include <stdint.h>

// Measured: 0 allocs / 0 bytes — compute_light never heap-allocates, so the
// memory ceiling is an exact 0 (a per-call malloc regression would FAIL).
CRAFT_BENCH_FULL(light_compute_chunk, 200, 1.0, /*allocs*/ 0, /*bytes*/ 0) {
  Chunk *c = ensure_chunk(1, 1);   // generated once during warmup
  for (int i = 0; i < B->iters; i++) {
    compute_light(c);
    bench_sink_i64(c->light_sky[0] ^ c->light_blk[CHUNK_VOL - 1]);
  }
}
