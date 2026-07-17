// craft-bench runner. Headless, no raylib (QueryPerformanceCounter clock —
// GetTime needs InitWindow). For each registered case: one untimed warmup pass
// (pays fixture setup + caches + code-in), then REPS timed reps; the per-iter
// value is the MIN across reps (least scheduler interference ~= true cost on a
// noisy Win/MinGW box). Exits nonzero if any case is over its ms or memory
// budget — that nonzero is the perf+memory gate.
#include "bench.h"
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <psapi.h>

volatile uint64_t g_bench_sink;

#define MAX_CASES 128
#define REPS      7

static BenchCase g_cases[MAX_CASES];
static int       g_ncases;

void bench_register(BenchCase c) { if (g_ncases < MAX_CASES) g_cases[g_ncases++] = c; }

static double qpc_ms(LARGE_INTEGER a, LARGE_INTEGER b, LARGE_INTEGER f) {
  return (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
}

int main(void) {
  LARGE_INTEGER freq; QueryPerformanceFrequency(&freq);

  const char *csv_path = getenv("CRAFT_BENCH_CSV");
  FILE *csv = csv_path ? fopen(csv_path, "w") : NULL;
  if (csv) fprintf(csv, "name,ms_per_iter,budget_ms,allocs,bytes,result\n");

  int fails = 0;
  printf("%-24s %11s %10s %9s %11s %8s\n",
         "bench", "ms/iter", "budget", "allocs", "bytes", "result");
  printf("--------------------------------------------------------------------------------\n");

  for (int i = 0; i < g_ncases; i++) {
    BenchCase *c = &g_cases[i];

    BenchCtx warm = { c->iters < 32 ? c->iters : 32 };
    c->fn(&warm);                                  // untimed warmup

    double best = 1e30; long best_al = 0; size_t best_by = 0;
    BenchCtx B = { c->iters };
    for (int r = 0; r < REPS; r++) {
      bench_mem_reset();
      LARGE_INTEGER t0, t1;
      QueryPerformanceCounter(&t0);
      c->fn(&B);
      QueryPerformanceCounter(&t1);
      long   al = bench_mem_allocs();
      size_t by = bench_mem_bytes();
      double per = qpc_ms(t0, t1, freq) / (double)c->iters;
      if (per < best) { best = per; best_al = al; best_by = by; }
    }

    int time_ok = best <= c->budget_ms;
    int mem_ok  = (c->alloc_budget < 0   || best_al <= c->alloc_budget)
               && (c->bytes_budget == SIZE_MAX || best_by <= c->bytes_budget);
    if (!time_ok || !mem_ok) fails++;
    const char *res = !time_ok ? "SLOW" : !mem_ok ? "MEM-FAIL" : "PASS";

    printf("%-24s %11.4f %10.3f %9ld %11llu %8s\n",
           c->name, best, c->budget_ms, best_al, (unsigned long long)best_by, res);
    if (csv) fprintf(csv, "%s,%.6f,%.3f,%ld,%llu,%s\n",
                     c->name, best, c->budget_ms, best_al, (unsigned long long)best_by, res);
  }

  if (csv) fclose(csv);

  PROCESS_MEMORY_COUNTERS pmc;
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc))
    printf("\npeak working set: %.1f MB\n", pmc.PeakWorkingSetSize / 1048576.0);

  printf(fails ? "\nBENCH: %d FAILURE(S)\n" : "\nBENCH: all within budget\n", fails);
  return fails ? 1 : 0;
}
