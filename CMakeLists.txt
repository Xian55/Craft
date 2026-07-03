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
  target_sources(craft PRIVATE src/ws_native.c src/server.c)
endif()

# Determinism: no FP contraction in the noise pipeline (must match JS doubles).
set_source_files_properties(src/gen.c PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")

target_include_directories(craft PRIVATE vendor src)
target_link_libraries(craft raylib)

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
endif()
