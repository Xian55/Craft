// Greedy chunk mesher bench. ensure_chunk(cx,cz) auto-generates deterministic
// terrain, so a fixed (cx,cz) gives a realistic, repeatable populated chunk with
// no fixture files. mesh_bench_solid builds the solid geometry (incl. a full
// compute_light relight) WITHOUT the GPU upload (the #ifdef CRAFT_BENCH seam in
// mesh.c) and reports the resident CPU-side mesh bytes raylib keeps after upload.
#include "bench.h"
#include "world.h"
#include "mesh.h"
#include <stdint.h>

// Timed: rebuild the same chunk's solid geometry. Allocs come from the Builder's
// growable arrays (deterministic for a fixed chunk); the chunk itself is generated
// once during warmup. Measured chunk (3,5): ~2.2 ms, 600 allocs, ~41 MB gross
// realloc churn over 40 iters — ceilings catch a mesher/alloc regression.
CRAFT_BENCH_FULL(mesh_solid_chunk, 40, 7.0, /*allocs*/ 600, /*bytes*/ 45000000) {
  MeshStats s = { 0 };
  for (int i = 0; i < B->iters; i++) {
    s = mesh_bench_solid(3, 5);
    bench_sink_i64(s.tris);
  }
  bench_sink_i64((int64_t)s.cpu_bytes);   // resident CPU-side mesh copy the game keeps
}
