// Deterministic allocation accounting via GNU ld's -Wl,--wrap. Every malloc/
// calloc/realloc/free in the linked objects (our src/*.c, not raylib's static
// lib) routes through these wrappers; the runner brackets each timed region with
// bench_mem_reset()/bench_mem_allocs() so a case's allocs + gross bytes are exact
// and stable run-to-run — an exact alloc-count ceiling trips on a stray malloc.
#include "bench.h"
#include <stddef.h>

extern void *__real_malloc(size_t);
extern void *__real_calloc(size_t, size_t);
extern void *__real_realloc(void *, size_t);
extern void  __real_free(void *);

static long   n_alloc;
static size_t n_bytes;
static int    recording;   // off until the first bench_mem_reset (skips startup noise)

void   bench_mem_reset(void)  { n_alloc = 0; n_bytes = 0; recording = 1; }
long   bench_mem_allocs(void) { recording = 0; return n_alloc; }
size_t bench_mem_bytes(void)  { return n_bytes; }

void *__wrap_malloc(size_t n)             { if (recording) { n_alloc++; n_bytes += n; }     return __real_malloc(n); }
void *__wrap_calloc(size_t a, size_t b)   { if (recording) { n_alloc++; n_bytes += a * b; } return __real_calloc(a, b); }
void *__wrap_realloc(void *p, size_t n)   { if (recording) { n_alloc++; n_bytes += n; }     return __real_realloc(p, n); }
void  __wrap_free(void *p)                { __real_free(p); }
