---
name: craft-feature
description: The end-to-end harness for building a requested feature in craft autonomously — design it, implement it, test it, gate it on determinism + performance + memory, verify it renders, update docs, and commit. Use whenever the user requests a new gameplay/engine feature (a block, item, mob, mechanic, fluid, UI panel, protocol message) rather than a trivial fix. Defines the required steps and quality gates so the work is reliable and doesn't regress.
---

# craft-feature — the autonomous feature loop

When the user requests a feature, follow this loop. Do not skip the gates. A feature request should
turn into working, tested, non-regressing code without hand-holding.

## 1. Frame it as acceptance criteria
Restate the request as a short list of **observable** "done" conditions (what a player sees, or what
a test can assert). If a requirement is genuinely ambiguous *and* the choice changes the design, ask
one focused question; otherwise pick the sensible default and state it.

## 2. Explore before writing
- Search `src/` for code to reuse (a block enum slot, a recipe table, a mob array, an atlas tile,
  an existing proto message). Prefer reuse over new code.
- Identify the **owner module** (flat `src/`, one responsibility each — see the CLAUDE.md
  architecture map). If it needs a new file, use **craft-new-module**.
- Identify the **client/server split**: terrain regenerates deterministically on every client;
  the server is a relay + edit log only (no raylib, own clock). Block-entity data (chests) is
  server-owned. Mob AI runs on the elected master client. Get the authority right.
- Identify the **hot-path surface**: does it run per-frame, per-chunk, or per-entity? If yes, it
  needs a bench case (**craft-bench**) and a look at `CRAFT_PROF` phases.

## 3. Design (write it down)
State the file list, key data structures, and where it hooks into the frame (input/ui → player →
chunk stream → net → entities → mining → fluids → draw). Keep `windows.h` out of any raylib TU
(win32 code goes in its own TU, see `wincon.c`). Keep anything headless (server path) free of
raylib. If it touches the wire, define the proto message both sides with a golden fixture.

## 4. Test thoroughly (not optional — write tests AS you implement)
Cover happy path, edges (empty/zero/max, chunk borders, 0 hp, first/last tick), and failure modes
(bad input, missing chunk, out-of-range — must fail cleanly, not crash). Put logic where a headless
exe can reach it and add/extend a `tests/<name>_test.c` wired into `CMakeLists.txt`. Rendering/feel
is verified by running the game (**craft-build**), not unit tests.

## 5. Implement
Match the surrounding style (craft has no clang-format — read the neighboring file and mirror it:
snake_case, C11, lean comments). Wrap a new per-frame phase in the `prof_note` timing if it belongs
in the `CRAFT_PROF` table.

## 6. Gates — ALL must pass before commit (run in order; fix and re-run on any failure)
1. **Determinism gate** — if the change is anywhere near `src/gen.c`: `gen_test.exe` vs the committed
   C golden `tests\gen_golden.txt`, `diff --strip-trailing-cr` empty. For an **intentional** gen
   change, re-bless (`gen_test.exe > tests\gen_golden.txt`) and commit the golden. gen must stay a
   pure deterministic double-only fn (C↔C multiplayer determinism). Never skip.
2. **Unit + integration tests** — `phys_test`, `sim_test`, `net_test`, `craft.exe --server-test`,
   and `bun tools\e2e_net.mjs` as relevant (see **craft-test**). Green.
3. **Perf + memory gate** — if the feature touches a hot path, add a bench case with ms + alloc
   budgets (**craft-bench**), then run `bench_test.exe`. No case over its ms or allocation ceiling.
4. **Loop profile (no regression)** — if it runs every frame, `CRAFT_PROF=1 ./craft.exe` before and
   after; report the per-phase ms and confirm no regression against the previous run.
5. **Visual verify** — if it renders, **craft-build** a screenshot with a fixed `CRAFT_POS`/
   `CRAFT_TIME` and confirm it looks right.

## 7. Self-review, update docs, commit
Check: correct? within budget? right authority (client/server/master)? no `windows.h`↔raylib clash?
no raylib in the headless path? comments lean? Then **update docs** with **craft-docs** (root
`CLAUDE.md` + the `memory/` dir for anything that changed). Commit with a clear message (what + why +
before/after perf if relevant) and, if the user asked, push.

## Performance & correctness philosophy
Measure, don't guess. Determinism is sacred (the gen gate is non-negotiable). Every hot path is
budgeted (**craft-bench**), profilable (`CRAFT_PROF`), and observable live (F3 metrics). A feature
isn't done until it's proven not to regress frame time, memory, or the determinism gate.
