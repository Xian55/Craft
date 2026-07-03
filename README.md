# Craft Survival — C/Raylib port

C port of the browser voxel game originally in `../craft_js` (that directory is
now legacy: single-player game.js only; everything current lives here,
including the multiplayer server). Native C clients and the WASM build play
together in one world on the same server.

**One binary.** `craft.exe` is the client by default; `--server` turns it into
a headless dedicated server (no window — runs on the Pi in docker);
`--serve` plays and hosts at the same time. The server lives in
`src/server.c` and shares the protocol codec with the client. No JS runtime
anywhere — bun is only needed for the dev test tools.

## First build from a fresh checkout

Prerequisites (Windows):

```
winget install BrechtSanders.WinLibs.POSIX.UCRT   # gcc + mingw32-make
winget install Kitware.CMake
winget install Oven-sh.Bun                        # dev only: JS test tools (not needed to play or host)
# web build only:
git clone https://github.com/emscripten-core/emsdk ..\emsdk
..\emsdk\emsdk install latest && ..\emsdk\emsdk activate latest
```

Then:

```
build.cmd          # native client + test exes -> ..\craft_raylib_build\native
build.cmd web      # wasm build               -> ..\craft_raylib_build\web
```

`build.cmd` finds the WinLibs gcc even when it isn't on PATH yet (fresh
winget install). The first configure downloads raylib 5.5 via CMake
FetchContent, so it needs git and network access once; after that builds are
offline. Build artifacts live OUTSIDE the project in the sibling directory
`..\craft_raylib_build` (`native\` and `web\`). The native exe is
self-contained (assets copied next to it) and can be launched from anywhere.

## Quick start (after building)

```
serve.cmd                                        # server + browser game on http://<host>:8080/ (needs build.cmd web)
..\craft_raylib_build\native\craft.exe           # native client (auto-connects to localhost:8080)
..\craft_raylib_build\native\craft.exe --host <ip>   # join a remote server
..\craft_raylib_build\native\craft.exe --serve   # play AND host at the same time
..\craft_raylib_build\native\craft.exe --server  # headless dedicated server (env: PORT, STATIC, CRAFT_DATA)
publish.cmd                                      # shareable ~1 MB zip: one exe plays and hosts + web client
```

**Ported feature set:** terrain generation (bit-identical to JS), chunk meshing,
baked flood-fill lighting + AO, day-night cycle (two-channel light shader: sky
darkens, torches don't), AABB player physics, tool-speed mining with crack
overlay, inventory + 3×3 crafting + chests, dropped items, water/lava flow
simulation (active set, slope-seeking, water+lava→cobble), falling sand, TNT
(chain explosions), pigs (boxel model + wander AI, pork), hostile mobs at
night — zombies (melee), skeletons (arrows), creepers (sssss... boom),
hunger/health/eating,
buckets, chat commands (/give /tp /time /pig /creative ...), multiplayer with
state save/restore (join sync, live edits, remote players, shared clock).

## Manual build (what build.cmd runs)

Native — gcc+mingw32-make on PATH:

```
cmake -S . -B ..\craft_raylib_build\native -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build ..\craft_raylib_build\native -j
```

Web — run emsdk's `emsdk_env.bat` in the shell first:

```
emcmake cmake -S . -B ..\craft_raylib_build\web -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DPLATFORM=Web
cmake --build ..\craft_raylib_build\web -j
```

Or run the published server image (linux amd64/arm64, serves the web client
and the world sync on one port):

```
docker run -d --name craft --restart unless-stopped -p 8080:8080 -v craft_world:/data ghcr.io/xian55/craft:latest
```

Easiest locally: run `serve.cmd` — it starts `craft.exe --server` with
`STATIC=build-web`, so **one port (8080) serves both** the page and the world
sync. Open `http://<host>:8080/` in a browser and play. Must be plain
**http**, not file:// or https — the game opens `ws://<host>:8080`, and
https pages refuse ws.

## Controls

WASD move, Shift sprint (works flying too; on touch, push the joystick to its
rim), mouse look (click to capture, Esc releases), Space jump/swim
(double-tap in creative = fly), LMB hold mine / hit pig / light TNT, RMB place
/ bucket / open chest, 1-9 or mouse wheel hotbar, E inventory+crafting, F eat,
Q drop one, T chat, / command, F3 metrics (cycles: off / compact / graphs).

**Touch (mobile browser):** activates on the first tap — left thumb = floating
joystick (move), drag anywhere right = look, buttons: jump / mine (hold) /
place (shows the held item), bag toggle top-right, tap hotbar slots to select.
Inventory and chest screens use normal taps.

## Tests

```
set B=..\craft_raylib_build\native
%B%\gen_test.exe > c.txt
bun tools\terrain_ref.mjs > ref.txt
diff --strip-trailing-cr ref.txt c.txt    # must be empty — terrain determinism gate
%B%\phys_test.exe                          # physics sanity
%B%\sim_test.exe                           # fluids, TNT, sand, crafting, inventory
%B%\net_test.exe                           # protocol codec + interpolation
```

Debug hooks: `CRAFT_SHOT=N` screenshot at frame N then exit;
`CRAFT_POS="x,y,z,yaw,pitch"` spawn pose (implies fly+creative);
`CRAFT_TIME=0..1` initial time of day; `CRAFT_CMD="cmd;cmd"` chat commands at frame 30.

## Network protocol v2 (binary)

Little-endian binary frames over WebSocket, byte 0 = message type (`src/proto.h`
is the reference; the server in `src/server.c` shares it). Handshake:
client sends `HELLO(ver, uid)` first; server replies `WELCOME, TIME, EDITS
batches, RESTORE` or `REJECT` on version mismatch. Positions run at 20 Hz with
pitch + velocity; remotes are rendered ~120 ms in the past via snapshot
interpolation (`src/interp.c`) with velocity extrapolation on gaps. Liveness:
client pings every 2 s, reconnects with 1→10 s backoff if the line goes silent
for 6 s; the server drops peers silent for 10 s. Player state (bag/position/
health) saves every 5 s and restores on the first join of a session.
On disk, persistence is split by concern: `world.edits` (binary block-edit
log — "KEDT" v1 header + 11-byte records, same layout as the SV_EDITS wire
batches), `players.json` (saved player states, deliberately human-readable)
and `world.meta.json` (time of day). A legacy `world.json` is migrated on
first load and kept as `world.json.bak`. The old JSON protocol (game.js
browser client) is retired — v2 servers ignore text frames.

Tests: `net_test.exe` (codec golden bytes shared with the server
self-test + interpolation math), `craft.exe --server-test` (server
self-test), `bun tools\e2e_net.mjs` (live wire-spec against
`craft --server`: join order, 20 Hz relay, replay, heartbeat kick, reject).

## ⚠️ Determinism

`src/gen.c` must stay bit-identical to game.js's noise: doubles only, exact JS
expression order, ECMA ToInt32 emulation, no `-ffast-math`, no FP contraction
(see CMakeLists), and `x*x*sqrt(x)` instead of `pow(x,2.5)`. Any change must be
mirrored in game.js AND pass the gen_test diff.
