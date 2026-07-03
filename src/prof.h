// Opt-in micro-profiler (CRAFT_PROF=1): per-phase avg/max/total, dumped at
// exit. Standalone TU so test binaries can link instrumented modules without
// dragging the whole metrics/net stack in.
#ifndef PROF_H
#define PROF_H

#include <stdbool.h>

enum {
  PH_INPUT_UI, PH_PLAYER, PH_CHUNKS, PH_NET, PH_ENTITIES, PH_MINING,
  PH_FLUIDS, PH_DRAW_WORLD, PH_DRAW_ENT, PH_DRAW_UI, PH_SWAP,
  PH_EXPLODE, PH_MESH_CHUNK,
  PH_COUNT
};

bool prof_on(void);
void prof_note(int phase, double ms);   // accumulate one sample
void prof_dump(void);                   // print the table (atexit-safe)

#endif
