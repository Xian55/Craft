#include "prof.h"
#include <stdio.h>
#include <stdlib.h>

static const char *ph_names[PH_COUNT] = {
  "input+ui", "player", "chunk stream", "net", "entities", "mining",
  "fluids", "draw world", "draw entities", "draw ui", "swap+vsync",
  "EXPLODE (event)", "mesh_chunk (event)",
};
static struct { double total, mx; long n; } prof[PH_COUNT];
static int prof_enabled = -1;

bool prof_on(void) {
  if (prof_enabled < 0) { const char *e = getenv("CRAFT_PROF"); prof_enabled = e && e[0] && e[0] != '0'; }
  return prof_enabled == 1;
}

void prof_note(int phase, double ms) {
  if (phase < 0 || phase >= PH_COUNT) return;
  prof[phase].total += ms;
  prof[phase].n++;
  if (ms > prof[phase].mx) prof[phase].mx = ms;
}

void prof_dump(void) {
  if (!prof_on()) return;
  printf("\n--- CRAFT_PROF: per-phase timings (ms) ---\n");
  printf("%-20s %10s %10s %10s %8s\n", "phase", "avg", "max", "total", "samples");
  for (int i = 0; i < PH_COUNT; i++) {
    if (!prof[i].n) continue;
    printf("%-20s %10.3f %10.3f %10.1f %8ld\n",
           ph_names[i], prof[i].total / prof[i].n, prof[i].mx, prof[i].total, prof[i].n);
  }
}
