// Fluid-sim bench. Fixed-tick water spread over an active set. Lower fidelity than
// gen/mesh/light: the active set empties at equilibrium, so each iteration seeds a
// FRESH source, spreads, then removes it and lets the water retreat — a
// self-resetting spread+retreat cycle (mirrors sim_test) that keeps every iteration
// identical work. Generous budget accordingly.
#include "bench.h"
#include "world.h"
#include "fluids.h"
#include <stdint.h>

#define PLAT_Y 50    // stone platform level
#define FY     51    // water level (platform top + 1)

static void build_platform(void) {
  // Wide enough that the platform edge stays outside the flow range (see sim_test).
  for (int x = -8; x < 24; x++)
    for (int z = -8; z < 24; z++)
      set_voxel(x, PLAT_Y, z, B_STONE);
}

// Measured: ~5.4 ms, ~100k allocs, ~106 MB gross over 20 iters — the fluid sim
// churns its active set hard; generous ceilings gate a gross regression only.
CRAFT_BENCH_FULL(fluid_spread_retreat, 20, 16.0, /*allocs*/ 130000, /*bytes*/ 140000000) {
  static int ready; if (!ready) { build_platform(); ready = 1; }
  for (int i = 0; i < B->iters; i++) {
    set_water_raw(8, FY, 8, 9); touch_water(8, FY, 8);   // seed source
    for (int t = 0; t < 6; t++) simulate_water();        // spread
    set_water_raw(8, FY, 8, 0); touch_water(8, FY, 8);   // remove source
    for (int t = 0; t < 8; t++) simulate_water();        // retreat (drains the active set)
    bench_sink_i64(water_at(9, FY, 8));
  }
}
