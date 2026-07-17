// craft-bench: micro-benchmark + memory harness. See .claude/skills/craft-bench.
// A CRAFT_BENCH case times ONE batch of `iters` units of work against an ms
// budget; the runner (bench_main.c) drives warmup + min-of-N-reps timing and a
// -Wl,--wrap malloc shim (bench_alloc.c) for per-case allocation accounting.
#ifndef CRAFT_BENCH_H
#define CRAFT_BENCH_H

#include <stdint.h>
#include <stddef.h>

typedef struct { int iters; } BenchCtx;

typedef struct {
  const char *name;
  int         iters;
  double      budget_ms;
  long        alloc_budget;   // <0 = no allocation-count gate
  size_t      bytes_budget;   // SIZE_MAX = no byte gate
  void      (*fn)(BenchCtx *);
} BenchCase;

void bench_register(BenchCase c);   // called by the CRAFT_BENCH constructor

// Elision guard: feed every computed result to a sink so -O2 can't drop the work.
extern volatile uint64_t g_bench_sink;
static inline void bench_sink_i64(int64_t v)      { g_bench_sink ^= (uint64_t)v; }
static inline void bench_sink_ptr(const void *p)  { g_bench_sink ^= (uint64_t)(uintptr_t)p; }

// Per-case allocation accounting (bench_alloc.c, via -Wl,--wrap).
void   bench_mem_reset(void);    // start recording (call before a timed region)
long   bench_mem_allocs(void);   // stop recording, return alloc count
size_t bench_mem_bytes(void);    // gross bytes requested since reset

// Register a bench case with explicit memory ceilings.
#define CRAFT_BENCH_FULL(nm, it, bud, ab, bb)                                   \
  static void bench__##nm(BenchCtx *);                                          \
  __attribute__((constructor)) static void reg__##nm(void) {                    \
    bench_register((BenchCase){ #nm, (it), (bud), (ab), (bb), bench__##nm });   \
  }                                                                             \
  static void bench__##nm(BenchCtx *B)

// Register a bench case with only an ms budget (no memory gate).
#define CRAFT_BENCH(nm, it, bud) CRAFT_BENCH_FULL(nm, it, bud, -1, SIZE_MAX)

#endif
