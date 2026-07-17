cmake_minimum_required(VERSION 3.24)
project(craft_raylib C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
# Once raylib is cloned, don't hit git again on reconfigure (delete the
# _deps dir to force a refetch). Cuts configure time drastically.
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
FetchContent_Declare(raylib
  GIT_REPOSITORY https://github.com/raysan5/raylib.git
  GIT_TAG 5.5
  GIT_SHALLOW ON)
FetchContent_MakeAvailable(raylib)

# Baked-in semver: CI passes the release tag, local builds identify as dev.
set(CRAFT_VERSION "dev" CACHE STRING "version string shown by --version, the server banner and the window title")

add_executable(craft
  src/main.c
  src/gen.c
  src/world.c
  src/light.c
  src/mesh.c
  src/physics.c
  src/net.c
  src/proto.c
  src/interp.c
  src/fluids.c
  src/entities.c
  src/interact.c
  src/inventory.c
  src/ui.c
  src/lang.c
  src/hand.c
  src/metrics.c
  src/prof.c
  src/touch.c
  src/sky.c)

if (EMSCRIPTEN)
  target_sources(craft PRIVATE src/ws_wasm.c)
else()
  # native gets the websocket transport AND the embedded server
  # (`craft --server` headless / `craft --serve` play-and-host)
  target_sources(craft PRIVATE src/ws_native.c src/server.c src/wincon.c)
endif()

# Determinism: no FP contraction in the noise pipeline (must match JS doubles).
set_source_files_properties(src/gen.c PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")

target_include_directories(craft PRIVATE vendor src)
target_link_libraries(craft raylib)
target_compile_definitions(craft PRIVATE CRAFT_VERSION="${CRAFT_VERSION}")

# Client mode should not drag a terminal window along on Windows. GUI
# subsystem, but main.c re-attaches the parent console (so start_server.cmd,
# CI and terminal runs keep their output) and --console can force one.
option(CRAFT_WIN_GUI "Windows: GUI subsystem (no console window for the client)" ON)
if (WIN32 AND CRAFT_WIN_GUI AND NOT EMSCRIPTEN)
  target_link_options(craft PRIVATE -mwindows)
  target_compile_definitions(craft PRIVATE CRAFT_GUI_SUBSYSTEM)
endif()

if (NOT EMSCRIPTEN)
  # ship assets next to the exe so out-of-tree builds are self-contained.
  # A dedicated ALL target, not POST_BUILD: POST_BUILD only fires when the
  # exe relinks, which left stale assets after asset-only changes.
  add_custom_target(copy_assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:craft>/assets)
  add_dependencies(copy_assets craft)
endif()

if (EMSCRIPTEN)
  set_target_properties(craft PROPERTIES SUFFIX ".html")
  # Growable heap, but with GROWABLE_ARRAYBUFFERS=0: grow by detach-and-replace
  # (classic emscripten) instead of resizable ArrayBuffers, which browsers
  # reject in TextDecoder.decode() AND WebGL texImage2D (Encoding/WebGL spec,
  # not a browser bug — retested 2026-07). 256 MB up front covers normal play
  # without growth pauses; big render distances grow up to 2 GB.
  option(CRAFT_WASM_GROWTH "wasm: growable heap (detach-style)" ON)
  if (CRAFT_WASM_GROWTH)
    target_link_options(craft PRIVATE -sALLOW_MEMORY_GROWTH=1 -sGROWABLE_ARRAYBUFFERS=0
                        -sINITIAL_MEMORY=268435456 -sMAXIMUM_MEMORY=2147483648)
  else()
    target_link_options(craft PRIVATE -sINITIAL_MEMORY=268435456)
  endif()
  target_link_options(craft PRIVATE
    -sUSE_GLFW=3 -sSTACK_SIZE=1048576
    -lwebsocket.js
    --preload-file ${CMAKE_SOURCE_DIR}/assets@assets
    --shell-file ${CMAKE_SOURCE_DIR}/shell.html)
  # assets + shell are baked in at link time: relink when they change
  # (asset-only edits otherwise ship stale craft.data)
  file(GLOB CRAFT_ASSET_FILES ${CMAKE_SOURCE_DIR}/assets/* ${CMAKE_SOURCE_DIR}/assets/lang/*)
  set_property(TARGET craft APPEND PROPERTY LINK_DEPENDS
               ${CRAFT_ASSET_FILES} ${CMAKE_SOURCE_DIR}/shell.html)
elseif (WIN32)
  target_link_libraries(craft ws2_32)
endif()

# Terrain determinism harness (native only; no raylib dependency).
if (NOT EMSCRIPTEN)
  add_executable(gen_test tests/gen_test.c src/gen.c)
  target_include_directories(gen_test PRIVATE src)
  target_link_libraries(gen_test m)

  add_executable(phys_test tests/phys_test.c src/physics.c src/world.c src/gen.c src/inventory.c src/lang.c)
  target_include_directories(phys_test PRIVATE src)
  target_link_libraries(phys_test raylib)

  add_executable(sim_test tests/sim_test.c tests/net_stubs.c src/fluids.c src/entities.c src/world.c
                 src/gen.c src/inventory.c src/lang.c src/physics.c src/mesh.c src/light.c
                 src/prof.c src/sky.c src/proto.c)
  target_include_directories(sim_test PRIVATE src)
  target_link_libraries(sim_test raylib)

  add_executable(net_test tests/net_test.c src/proto.c src/interp.c)
  target_include_directories(net_test PRIVATE src)
  target_link_libraries(net_test m)

  # Perf + memory gate: Release-only micro-benchmarks with ms + allocation budgets
  # (bench_test.exe, nonzero on any breach). Headless — mirrors sim_test's link set
  # (known good with mesh+light). CRAFT_BENCH_BUILD unlocks the no-upload mesh seam;
  # -Wl,--wrap gives deterministic per-case malloc accounting. gen.c inherits
  # -ffp-contract=off via the dir-scoped set_source_files_properties above.
  # WIN32-only: the runner uses windows.h/psapi (QPC clock + peak RSS) and --wrap
  # (GNU ld; Apple ld64 lacks it). ms budgets are calibrated on the Windows dev box,
  # so this is a dev-box/Windows gate, not a cross-platform CI check (shared linux/
  # macos runners would make the hard ms budgets flaky).
  if (WIN32)
    add_executable(bench_test
      bench/bench_main.c bench/bench_alloc.c
      bench/bench_gen.c bench/bench_mesh.c bench/bench_light.c bench/bench_fluids.c
      src/gen.c src/world.c src/light.c src/mesh.c src/fluids.c src/physics.c
      src/entities.c src/inventory.c src/lang.c src/prof.c src/sky.c src/proto.c
      tests/net_stubs.c)
    target_include_directories(bench_test PRIVATE src bench vendor)
    target_compile_definitions(bench_test PRIVATE CRAFT_BENCH_BUILD)
    target_link_libraries(bench_test raylib psapi)
    target_link_options(bench_test PRIVATE
      -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free)
  endif()
endif()
