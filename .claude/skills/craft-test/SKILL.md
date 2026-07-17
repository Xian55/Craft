---
name: craft-test
description: Build and run craft's full headless test battery — the terrain determinism gate, the phys/sim/net unit exes, the server self-test, the live-wire e2e, and the bench_test perf+memory gate. Use after changing anything in src/ to check for regressions before committing. All must stay green.
---

# craft-test — the full test battery

All tests are native, headless, and live in `..\craft_raylib_build\native` (built by `build.cmd`).
Run the ones your change can touch; run all of them before a commit.

## Build first
```bash
build.cmd          # builds craft.exe + gen_test/phys_test/sim_test/net_test/bench_test
```

## 1. Determinism gate (CRITICAL — gate ANY change near src/gen.c)
gen must stay a pure, deterministic, double-only function so every client on the same build generates
the same world (C↔C multiplayer determinism). The gate compares `gen_test.exe` output against a
committed **C golden** (`tests/gen_golden.txt`); the legacy JS ref is retired (JS cross-play dropped):
```bash
..\craft_raylib_build\native\gen_test.exe > c.txt
diff --strip-trailing-cr tests\gen_golden.txt c.txt      # MUST be empty
```
If this diffs **unintentionally**, stop — something changed gen output (watch FP contraction, `pow`
vs `x*x*sqrt(x)`, ToInt32 emulation, `-ffp-contract=off`). If the change is **intentional** (a new
terrain feature), re-bless the golden and commit it, calling it out in the commit message:
```bash
..\craft_raylib_build\native\gen_test.exe > tests\gen_golden.txt
```

## 2. Unit exes (run, expect PASS / exit 0)
```bash
..\craft_raylib_build\native\phys_test.exe    # swept AABB, stats/damage/eat
..\craft_raylib_build\native\sim_test.exe     # fluids / TNT / sand / crafting
..\craft_raylib_build\native\net_test.exe     # proto codec golden bytes + interpolation
```

## 3. Server self-test (golden bytes)
```bash
..\craft_raylib_build\native\craft.exe --server-test
```
Covers the edits-file goldens, relay goldens, the RFC 6455 accept vector, players.json round-trip,
and batch split.

## 4. Live-wire e2e (bun against a real `craft --server`)
```bash
bun tools\e2e_net.mjs
```
Exercises join order, the 20 Hz relay, replay, heartbeat, and reject against a live server.

## 5. Perf + memory gate (bench_test)
Release-only micro-benchmarks with ms + allocation budgets; exits nonzero if any hot path
(gen/mesh/light/fluids) is over its ms budget or allocates more than its ceiling:
```bash
..\craft_raylib_build\native\bench_test.exe
```
See the **craft-bench** skill to add a case or read a regression. This is the perf half of the
craft-feature commit gate.

## Notes
- MinGW exes statically link the runtimes; a `0xc0000139` at startup means a wrong runtime DLL is on
  PATH (the static link should prevent it — rebuild clean).
- Add a unit test next to the code: a new `tests/<name>_test.c`, wired into `CMakeLists.txt`
  (mirror the existing `add_executable` blocks; link `raylib` if it pulls world/mesh, `m` if pure).
- Determinism is the one gate you can never skip. When unsure whether a change is "near gen.c", run
  the gate anyway.
