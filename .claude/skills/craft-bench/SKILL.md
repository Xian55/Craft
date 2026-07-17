---
name: craft-bench
description: craft's Release-only micro-benchmark + memory harness (bench_test.exe). Use when adding or changing a hot path (gen, mesh, light, fluids), setting a CPU or memory budget, or investigating a ms/allocation regression. Run it before commit as the perf+memory gate; complements CRAFT_PROF (whole render loop) and the F3 metrics overlay (live).
---

# craft-bench — the perf + memory gate

`bench_test.exe` micro-times craft's hot paths against **ms budgets** and reports per-case
**allocation count + bytes** against **memory budgets**. Headless, deterministic, no raylib clock
(QPC). Exits **nonzero** if any case is over its ms or allocation ceiling — that non-zero is the gate.

Native **Windows** only (the runner uses windows.h/psapi for the QPC clock + peak RSS,
and `-Wl,--wrap`, which Apple ld64 lacks). The CMake target is `if (WIN32)`-gated, so the
linux/macos CI jobs skip it — the ms budgets are calibrated on the dev box, not shared CI runners.

## Run it
```bash
cmake --build ..\craft_raylib_build\native -j --target bench_test
..\craft_raylib_build\native\bench_test.exe        # nonzero = a budget breached
```
Optional `CRAFT_BENCH_CSV=bench.csv` env dumps a CSV row per case (off by default).

## Add a bench case
```c
// in bench/bench_<area>.c
CRAFT_BENCH(my_hot_path, /*iters*/200, /*budget_ms*/3.0) {
    for (int i = 0; i < B->iters; i++)
        bench_sink_i64( do_one_unit_of_work(i) );   // feed the result to the sink
}
```
- The body is **one batch** of `iters` units of work. The runner does the warmup + timed reps.
- **Feed every result to `bench_sink_i64()` / `bench_sink_ptr()`** or the optimizer elides the work.
- Untimed setup goes behind `static int ready; if (!ready) { ...; ready = 1; }` — the warmup pass
  pays for it.
- `ensure_chunk(cx, cz)` gives a real, deterministically-generated populated chunk — no fixtures.
- Auto-registers via a constructor; **no editing `main`**. Just add the file to `bench_test` in
  `CMakeLists.txt` if it's a new file.
- `CRAFT_BENCH_FULL(name, iters, budget_ms, alloc_budget, bytes_budget)` adds memory ceilings.

## Memory checks
- A `-Wl,--wrap` malloc/calloc/realloc/free shim counts **allocs + gross bytes** per case,
  deterministically. Set an **exact `alloc_budget`** (e.g. `0` = must not heap-allocate) to trip on
  a single stray malloc — the tightest, most reliable gate. Byte ceiling ≈ measured × 1.3.
- `bench_mesh` also reports the **resident CPU-side mesh-copy bytes** raylib keeps after
  `UploadMesh` (raylib retains a CPU copy) — the memory cost the `raylib` skill warns about.
- Peak working set (psapi) is printed once at the end as a coarse whole-run RSS sanity number — not
  a gate.

## Budgets philosophy
Budgets are regression **ceilings with headroom, not targets**. Measure once on the dev box, then
set: time budget ≈ measured × 2.5–3 (Win/MinGW timing is noisy), alloc **count** exact
(deterministic), bytes ≈ measured × 1.3. Budgets live **inline in the `CRAFT_BENCH` call** — single
source of truth that shows in the diff whenever someone changes a hot path.

## Headless / determinism
No raylib (QPC clock; `GetTime` needs `InitWindow`). The mesh bench builds geometry **without**
`UploadMesh` through the `#ifdef CRAFT_BENCH_BUILD` seam in `mesh.c` (`mesh_bench_solid`) — only
`bench_test` defines `CRAFT_BENCH_BUILD`, so the shipping `craft` build is byte-identical. gen is
**read-only** here and keeps `-ffp-contract=off`; a bench must never mutate gen behavior — the
`bun tools\terrain_ref.mjs` vs `gen_test.exe` determinism gate still rules.

## The three perf tiers (distinct roles)
- **F3 metrics** (`metrics.c`) — live, in-game, interactive feel.
- **`CRAFT_PROF=1`** (`prof.c`) — whole render loop, per-phase ms table at exit. Interactive, **not
  a gate**; use it to catch a whole-frame regression.
- **`bench_test`** — isolated hot-path micro, deterministic, **hard budgets — the gate**. Use it to
  prove a "faster/leaner" change actually is.
