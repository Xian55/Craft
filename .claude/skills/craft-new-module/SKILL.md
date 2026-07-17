---
name: craft-new-module
description: Scaffold a new single-responsibility C module (header + source) in src/, wired into the craft target and any test/bench exe that needs it, following craft's style and build conventions. Use when adding a new system or subsystem so it follows the project's SRP, C11 style, and CMake wiring.
---

# craft-new-module — scaffold an SRP C module

Creates a `src/<name>.h` / `src/<name>.c` pair in the flat `src/` layout and wires it into the
build. One responsibility per file — if you need "and" to describe it, split it.

## 1. Header `src/<name>.h`
`#pragma once`, a one-line top comment stating the single responsibility (and what it deliberately
is NOT), snake_case names, C11:
```c
#pragma once
// <One sentence: what this module owns — and what it does NOT.>

#include "world.h"   // only what the public API needs; forward-declare where you can

// Public API (snake_case). Keep raylib types out of the header unless the module is render-side.
void <name>_init(void);
void <name>_update(float dt);
```

## 2. Source `src/<name>.c`
```c
#include "<name>.h"

// file-static state and helpers here (static, not exported)

void <name>_init(void) { }
void <name>_update(float dt) { (void)dt; }
```
Rules that bite:
- **Keep `windows.h` out of any translation unit that includes raylib** — the win32 API clashes
  with raylib (`Rectangle`, `CloseWindow`, `DrawText`, `ShowCursor`). Win32 code goes in its own TU
  (see `wincon.c`).
- **Anything that must run headless (server path) must not include raylib** — `GetTime` needs
  `InitWindow`; keep an own clock (see `server.c`).
- **gen.c-adjacent code is determinism-gated** and compiled `-ffp-contract=off`. If your module
  feeds terrain generation, run the determinism gate (**craft-test**) and match the FP discipline.

## 3. Wire into the build (`CMakeLists.txt`)
Add `src/<name>.c` to the `add_executable(craft ...)` source list. Also add it to every **test or
bench exe** that links code depending on it (mirror an existing block — e.g. `sim_test` links the
headless world set; `bench_test` mirrors it). Emscripten-only or native-only sources go under the
existing `if (EMSCRIPTEN)` / `else()` guards.

## 4. Test (if the logic is headless-testable)
Add `tests/<name>_test.c`, wire an `add_executable(<name>_test ...)` in the `if (NOT EMSCRIPTEN)`
block (link `raylib` if it pulls world/mesh types, `m` if pure math), and make time/IO injectable so
assertions are deterministic. Rendering/OS glue is verified by running the game (**craft-build**),
not unit tests.

## 5. Verify
```bash
build.cmd
```
Then run any test you added (**craft-test**). Update docs (**craft-docs**) if the module adds a
subsystem or a gotcha worth recording in `CLAUDE.md`.
