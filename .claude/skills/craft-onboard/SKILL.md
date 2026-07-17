---
name: craft-onboard
description: Bootstrap a new contributor (or a fresh machine) into the craft_raylib project — install the toolchain, build, and verify everything works. Use when setting up the project for the first time, or when a clean-machine setup / environment problem needs fixing. Windows is the primary target.
---

# craft-onboard — set up craft_raylib from scratch

Goal: from nothing to a green build + running client + green tests. Do the steps in order; each ends
in a check.

## 1. Prerequisites (install if missing)
- **git**, **CMake** ≥ 3.24.
- **MinGW-w64 UCRT (WinLibs)** — the C toolchain: `winget install BrechtSanders.WinLibs.POSIX.UCRT`.
  gcc/mingw32-make are **off-PATH** in most shells; `build.cmd` locates them under
  `%LOCALAPPDATA%\Microsoft\WinGet\...` automatically, so you rarely need them on PATH.
- **bun** — dev-tool only (determinism reference + e2e tests): `winget install Oven-sh.Bun`.
- Optional (web build): **emsdk** cloned to the `..\emsdk` sibling of this repo:
  ```bash
  git clone https://github.com/emscripten-core/emsdk ..\emsdk
  ..\emsdk\emsdk install latest && ..\emsdk\emsdk activate latest
  ```
Verify: `cmake --version`, `git --version`, `bun --version`. (See the `win-toolchain-paths` memory
for the exact gcc/emsdk/bun paths on this machine.)

## 2. Build (first configure fetches raylib 5.5 via FetchContent — needs network, a few minutes)
```bash
build.cmd            # native client + test exes -> ..\craft_raylib_build\native
build.cmd web        # optional wasm build (needs emsdk)  -> ..\craft_raylib_build\web
```
Artifacts land **out of tree** in `..\craft_raylib_build\` (relative paths only).

## 3. Verify — all must pass
```bash
# determinism gate (the critical one)
bun tools\terrain_ref.mjs > ref.txt
..\craft_raylib_build\native\gen_test.exe > c.txt
diff --strip-trailing-cr ref.txt c.txt        # empty

# unit + integration + perf/memory
..\craft_raylib_build\native\phys_test.exe
..\craft_raylib_build\native\sim_test.exe
..\craft_raylib_build\native\net_test.exe
..\craft_raylib_build\native\craft.exe --server-test
..\craft_raylib_build\native\bench_test.exe
bun tools\e2e_net.mjs

# run the client (F3 cycles the metrics overlay)
..\craft_raylib_build\native\craft.exe
```
Expected: determinism diff empty; every test PASS / exit 0; bench all within budget; a window shows
the voxel world. `serve.cmd` runs the server + browser client on http://localhost:8080/ .

## 4. Learn the project (read in this order)
- `CLAUDE.md` (root) — quick start, build, tests, architecture map, gotchas.
- The `memory/` dir (`MEMORY.md` index) — machine paths, port history, deploy host.
- Skills: **craft-feature** (the dev loop), **craft-test**, **craft-build**, **craft-bench**,
  **craft-new-module**, **craft-docs**, and **raylib** (pinned 5.5 API reference).

## Troubleshooting
- `gcc not found` → install WinLibs (step 1); `build.cmd` prints the exact winget command.
- Determinism diff non-empty on a clean checkout → wrong toolchain FP flags; `gen.c` must build
  `-ffp-contract=off` (CMakeLists sets it) and never with `-ffast-math`.
- `0xc0000139` running a test → a wrong MinGW runtime DLL is on PATH; the static link should prevent
  it — rebuild clean.
- First configure fails offline → the raylib FetchContent needs network once, then it's cached
  (`FETCHCONTENT_UPDATES_DISCONNECTED ON`).
