# Craft Survival — C/Raylib (craft_raylib)

Minecraft-like voxel survival game in C11 + raylib 5.5 (CMake FetchContent) with an Emscripten WASM build. **One binary**: `craft.exe` is the client by default, `--server` makes it a headless dedicated server (src/server.c, no window/GPU), `--serve` plays and hosts at once. Bun is a dev-tool only (tests, terrain reference); no JS at runtime. Ported from the legacy vanilla-JS game in `..\craft_js` (that dir is single-player-only now; everything current is here). UI text is localized (`assets/lang/*.lang`, default English, `/lang hu` in chat, `CRAFT_LANG` env, persisted in `lang.txt` next to exe); lang values ASCII only (raylib default font has no accents). Code comments English.

## Skills & working agreement (.claude/skills)

Prefer these over ad-hoc steps: **craft-feature** (the dev loop for any new feature — design → test → gates → verify → docs → commit), **craft-build** (build native + `CRAFT_SHOT` screenshot verify), **craft-test** (the full battery below), **craft-bench** (the Release-only `bench_test` perf+memory gate: ms + allocation budgets per hot path), **craft-new-module** (scaffold `src/*.c/.h` + CMake wiring), **craft-onboard** (fresh-machine setup), **craft-docs** (keep this file + `memory/` current — the last step of craft-feature), **raylib** (pinned 5.5 API reference). Working agreement: features go through **craft-feature**; never skip the determinism gate; a hot-path change needs a **craft-bench** case within budget; finish by updating docs via **craft-docs**.

## Quick start

- `serve.cmd` — game server + browser game on `http://<host>:8080/` (runs `craft.exe --server` with STATIC=`..\craft_raylib_build\web`)
- `..\craft_raylib_build\native\craft.exe` — native client (auto-connects to localhost:8080; `--host <ip>` to join remote; `--serve` play+host; `--server` headless dedicated; `--port N`, env PORT/STATIC/CRAFT_DATA). Self-contained: assets copied next to exe, anchors cwd to exe dir.
- Server persists `world.edits` (binary) + `players.json` + `world.meta.json` + `chests.bin` ("KCHS" v1: chest contents by position) in CRAFT_DATA (default cwd), every 10 s + on exit (exit-save = SIGINT/SIGTERM; Windows hard-kill loses <=10 s). No legacy world.json migration in C — `tools/server_legacy.js` (bun) still reads it if ever needed.
- Chest contents are server-owned ("block entity" data): client fetches on open (CL_CHEST_GET -> SV_CHEST), writes on panel close (CL_CHEST_SET, broadcast to co-viewers, last-close wins), break online = CL_CHEST_BREAK -> SV_CHEST flags bit0 -> miner spawns the authoritative drops. Offline keeps the old local-RAM behavior.
- `publish.cmd` — shareable `..\craft_raylib_build\craft_publish.zip`: standalone `craft_server.exe` (bun build --compile; saves land next to exe via Bun.main bundle detection) + web client + start script
- Pi deployment is CI-driven now: merge to main -> Release workflow -> `ghcr.io/xian55/craft` (amd64+arm64) -> watchtower on the Pi (label-scoped, 5 min poll, `DOCKER_API_VERSION` pinned for docker 29) pulls + restarts the `craft` compose stack (`/root/craft-stack/docker-compose.yml`, world in the external `craft_world` volume). `deploy_pi.cmd` remains for local-source deploys (config in gitignored `.env`, template `.env.example`). NOTE: .cmd scripts must stay CRLF — LF-only endings make cmd.exe misparse (stray "'m' is not recognized" errors)

## Build (artifacts OUT of tree, relative paths only)

`build.cmd` / `build.cmd web` — fresh-checkout bootstrap (finds WinLibs gcc off-PATH, calls emsdk_env). Manual:

```
cmake -S . -B ..\craft_raylib_build\native -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build ..\craft_raylib_build\native -j
# web: run emsdk_env.bat first, then
emcmake cmake -S . -B ..\craft_raylib_build\web -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DPLATFORM=Web
cmake --build ..\craft_raylib_build\web -j
```

gcc/mingw32-make come from WinLibs (winget, not on PATH in every shell — see memory `win-toolchain-paths`); emsdk lives in `..\emsdk` (sibling of this project).

## Tests (all must stay green)

- **Determinism gate (critical):** `..\craft_raylib_build\native\gen_test.exe > c.txt` vs committed C golden `tests\gen_golden.txt`, `diff --strip-trailing-cr` must be empty. Gate ANY change near `src/gen.c`. gen forked from the legacy JS (JS cross-play dropped) so the gate now guards C↔C multiplayer determinism, not JS parity — gen stays a pure deterministic double-only fn (`-ffp-contract=off`, never -ffast-math). **Intentional** gen change = re-bless: `gen_test.exe > tests\gen_golden.txt` and commit it. (`tools\terrain_ref.mjs` retired from the gate; kept for reference.)
- `phys_test.exe`, `sim_test.exe` (fluids/TNT/sand/crafting), `net_test.exe` (codec golden bytes + interpolation) — in `..\craft_raylib_build\native`.
- `craft.exe --server-test` — server self-test (edits-file goldens, relay goldens, RFC 6455 accept vector, players.json round-trip, batch split).
- `bun tools\e2e_net.mjs` — live wire-spec e2e against `craft --server` (join order, 20 Hz relay, replay, heartbeat, reject).
- `bench_test.exe` — Release-only, native-Windows perf+memory gate (`bench/`): micro-benchmarks the hot paths (gen/mesh/light/fluids) with ms + allocation budgets, nonzero on breach. Windows-only target (windows.h/psapi + `-Wl,--wrap`; budgets calibrated on the dev box) — not a cross-platform CI check. Adds a `CRAFT_BENCH` macro (constructor-registered), a QPC clock, a `-Wl,--wrap` malloc shim, and a `CRAFT_BENCH_BUILD` `#ifdef` seam in mesh.c (`mesh_bench_solid`, build-without-upload) — shipping `craft` is byte-identical. See skill `craft-bench`; complements `CRAFT_PROF` (whole loop) + F3 metrics (live).

## Architecture (src/)

- `gen.c` — deterministic terrain noise (ECMA ToInt32 emulation, doubles only, `x*x*sqrt(x)` not pow, `-ffp-contract=off`, never -ffast-math). hash2/valueNoise/fbm are the original JS port; `terrain_height`/`gen_chunk_data` have FORKED from JS (masked ridged mountains + carved rivers; temp/humidity biomes: desert/plains/forest/snow + alpine snow caps, per-biome surface + tree density; more planned: ores/caves). Invariant is now C↔C determinism, guarded by the C golden `tests/gen_golden.txt`. **Don't touch without the gate; re-bless the golden for intentional changes.**
- `world.c` — chunk hash map (16×16×64, blocks+water arrays); `light.c` — two-channel flood-fill light + AO; `mesh.c` — chunk mesher, atlas UVs, day-night shader (r=sky darkens, g=torch stays), fluid buckets (double-sided, no depth write), chunk streaming.
- `physics.c` — AABB swept collision, stats/damage/eat; `interact.c` — mining w/ tool speeds + crack overlay, place, buckets; `fluids.c` — fixed-tick active-set water/lava sim; `entities.c` — sand, TNT (explosions enqueue deferred remesh), drops, pigs (boxel + MC UV unwrap), hostile mobs (zombie melee / skeleton arrows / creeper fuse+explode_at; classic 64x32 skin unwraps generated by `tools/make_mobs.mjs`; night spawn, day burn except creeper; flat arrays, no ECS — 16 mobs cost 0.015 ms/frame). **Mob sync**: server elects the oldest client as authority (SV_MASTER); it simulates AI targeting the nearest player and streams CL_MOBS snapshots at 10 Hz (SV_MOBS to others; server drops non-master snapshots); mirrors lerp + run damage vs their OWN player in local_hazards (each client owns its health); non-masters fight via CL_MOB_HIT; explosions broadcast CL_BOOM (replayed via explode_remote, no chain/re-emit) + per-block edits for the server log (apply_edit skips no-change edits to avoid remesh floods). Offline = authority, so single-player unchanged. Debug spawns: `/zombie /skeleton /creeper [n]` (authority only).
- `inventory.c` — 36 slots, shaped/shapeless recipes, chests; `ui.c` — HUD/panels/chat+commands (hit_test must mirror draw_panel layout exactly!); `sky.c` — day cycle, sun/moon, clouds; `lang.c` — key=value localization tables (`tr("key")`, falls back to the key; no raylib dep — linked into phys/sim tests via inventory.c's item_name); `hand.c` — first-person held item (port of game.js handScene); `touch.c` — mobile controls (latches on first touch; floating joystick + drag-look + mine/place/jump/bag buttons + hotbar taps; main.c MUST skip the desktop mouse path in touch mode or raylib's touch→virtual-mouse mapping makes every look-drag mine); `metrics.c` — F3-cycled live metrics (off → compact → graphs; frame/RTT ring buffers, sparklines, `CRAFT_METRICS=0..2` env for tests, interp stall counter lives in interp.c as `interp_extrap_frames`).
- `net.c` — protocol layer; `proto.c/h` — **binary protocol v2** codec (LE, byte0=type; golden fixtures in tests both sides; CL_HELD/SV_HELD 0x16/0x17 relay the equipped item — sent on change + with every ping, server keeps no held state; CL_CHAT/SV_CHAT 0x18/0x19 relay player chat — u8 len + ASCII, in ui.c plain chat text is anything not starting with '/'); `interp.c` — 120 ms snapshot interpolation, yaw shortest-arc, velocity extrapolation; `ws_native.c`/`ws_wasm.c` — WebSocket transports (binary frames 0x82); BOTH connect asynchronously (native: non-blocking connect state machine driven by ws_pump — a blocking connect froze the game when the server was down). 20 Hz pos, 2 s ping heartbeat, auto-reconnect w/ backoff, restore-once guard.
- `server.c` — embedded server (relay + edit log only; terrain regenerates deterministically on every client). Pure C, NO raylib (own monotonic clock — must run headless), own SHA1/base64 for the ws upgrade. Persistence split by concern: `world.edits` binary log ("KEDT" v1 header + 11-byte records, same layout as SV_EDITS wire batches; golden bytes in self-test), `players.json` (JSON on purpose; tiny tolerant parser), `world.meta.json` (time). Careful in parse_frames: handle_msg can close the client (REJECT) — bail before touching rbuf again.

## Debug hooks (craft.exe)

`CRAFT_SHOT=N` screenshot at frame N then exit (lands in LAUNCH cwd — raylib caches path at InitWindow); `CRAFT_POS="x,y,z,yaw,pitch"` pose+fly+creative; `CRAFT_TIME=0..1`; `CRAFT_CMD="cmd;cmd"` chat commands at frame 30 (incl. `/slot N` hotbar select and `/boom N` TNT cluster, for tests); `CRAFT_PROF=1` per-phase timing table at exit (prof.c; explosions/mesh_chunk logged per event). Explosion remesh goes through mesh_enqueue (deduped, 3 chunks/frame) — synchronous remesh was a 13-38 ms spike.

## Gotchas learned the hard way

- Emscripten/browsers: TextDecoder.decode AND WebGL texImage2D reject views into resizable ArrayBuffers (spec, not bug; retested 2026-07) → wasm links growth with `-sGROWABLE_ARRAYBUFFERS=0` (classic detach-style grow), 256 MB initial / 2 GB max. Never enable plain ALLOW_MEMORY_GROWTH without that flag.
- Render distance: RD_MAX 50 native / 24 web (mesh.c); chunk map 32768 slots sized for rd 50; draw_world frustum-culls (Gribb-Hartmann on modelview*projection) — without it big rd is a draw-call slideshow.
- raylib-web resizes the canvas behind GLFW's back → main.c pushes CSS size through SetWindowSize each change or mouse coords scale wrong (fullscreen bug).
- wasm connect is async — never retry ws_connect every frame (stacks CONNECTING sockets); backoff schedule handles it.
- Headless testing: playwright + system Edge (`--use-angle=swiftshader --enable-unsafe-swiftshader`); synthetic key/mouse presses must be HELD ~80 ms or raylib's edge detector misses them.
- Asset staleness (fixed, keep it that way): native copies assets via the always-run `copy_assets` target (a POST_BUILD copy silently skips when the exe is up to date); wasm bakes assets into craft.data at LINK time, so the web target has LINK_DEPENDS on assets/* (new asset FILES still need a reconfigure — glob). atlas.png tile 17 = chest (tools/patch_atlas_chest.mjs), tile 18 = snow (tools/patch_atlas_snow.mjs); the legacy craft_js make_atlas.js only knows tiles 0-13. mesh.c auto-detects ATLAS_ROWS from the PNG height, so new tiles can grow the atlas a row.
