---
name: craft-build
description: Build the native craft client, run it headless, and capture an in-game screenshot to visually confirm a rendering or gameplay change looks right (not just that it compiled). Use when asked to build and run, screenshot the game, or verify a change on screen. Drives the built-in CRAFT_SHOT one-shot screenshot hook and the CRAFT_POS/TIME/CMD scene-staging hooks.
---

# craft-build — build, run, screenshot

Confirms a change actually renders (not just compiles), headlessly, via craft's one-shot
screenshot hook. No manual window closing.

## Steps

1. **Build native** (first configure fetches raylib 5.5 via FetchContent — needs network):
   ```bash
   build.cmd
   ```
   Or the raw commands (gcc/mingw32-make are WinLibs, off-PATH — `build.cmd` locates them):
   ```bash
   cmake -S . -B ..\craft_raylib_build\native -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
   cmake --build ..\craft_raylib_build\native -j
   ```
   Web build: `build.cmd web` then `serve.cmd` → http://localhost:8080/ .

2. **Run with the screenshot hook.** `CRAFT_SHOT=N` renders N frames, writes `craft_shot.png`,
   then `exit(0)` — no window to close. **The PNG lands in the LAUNCH cwd** (raylib caches the path
   at `InitWindow`), so `cd` to where you want it first:
   ```bash
   cd ..\craft_raylib_build\native
   CRAFT_SHOT=30 ./craft.exe > run.log 2>&1
   ```
   The client auto-connects to localhost:8080; if no server is up it still renders local terrain
   (async connect never blocks the frame). Add `--serve` to play-and-host, or `--server` for a
   headless dedicated server (no window/GPU).

3. **Stage the scene** with the debug env hooks (compose several):
   - `CRAFT_POS="x,y,z,yaw,pitch"` — teleport + implies fly + creative (frame a specific spot).
   - `CRAFT_TIME=0..1` — initial day_time (0/1 = midnight, 0.5 = noon) to check the day-night shader.
   - `CRAFT_CMD="/cmd;/cmd"` — run `;`-separated chat commands at frame 30 (e.g. `/slot 3`,
     `/boom 2`, `/zombie 4`, `/lang hu`).
   - `CRAFT_METRICS=0..2` — start with the F3 metrics overlay (0 off, 1 compact, 2 graphs).
   ```bash
   CRAFT_POS="8,80,8,45,-20" CRAFT_TIME=0.5 CRAFT_SHOT=40 ./craft.exe
   ```

4. **View it.** Read `craft_shot.png`.
   - craft's window is 1280×720, no `FLAG_WINDOW_HIGHDPI`, so the shot is normally 1:1 with no
     borders. **If** it comes back with black at top/right (raylib over-capturing by the display
     DPI scale), crop the bottom-left `1/dpi` region — this box is at 125% (`dpi = 1.25`). There is
     **no ImageMagick** here; crop with a Playwright+canvas snippet (see the `win-toolchain-paths`
     memory / `tools/make_raylib_skill.mjs` for the Edge-headless pattern).

## Notes
- Assets are copied next to the exe by the always-run `copy_assets` CMake target, and `main.c`
  anchors cwd to the exe dir — the run is self-contained wherever the exe lives.
- To confirm a *change* (not the whole scene), screenshot before and after with the SAME
  `CRAFT_POS`/`CRAFT_TIME` so the frames are comparable.
- Profiling the real loop (not a gate) is `CRAFT_PROF=1 ./craft.exe` (per-phase ms table at exit).
  Isolated hot-path budgets are the **craft-bench** skill. Full test battery is **craft-test**.
