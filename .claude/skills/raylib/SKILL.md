---
name: raylib
description: raylib 5.5 API reference, version-exact for this repo's pinned build. Use when writing or modifying rendering, window, input, texture, mesh, shader, audio, raymath or rlgl code — grep the references for exact signatures instead of recalling them from memory (raylib's API shifted between versions; this matches OUR raylib).
---

# raylib 5.5 reference (pinned)

Generated from the exact headers this project builds against
(`_deps/raylib-src`, GIT_TAG 5.5). Regenerate after a raylib bump:
`bun tools/make_raylib_skill.mjs`.

## Look up a function

Grep the module file, don't guess:

    references/core.md       window, timing, input (keyboard/mouse/touch/gamepad), files, camera  (198 fns)
    references/shapes.md     2D shapes: DrawRectangle*, DrawCircle*, collisions                   (66)
    references/textures.md   Image (CPU) vs Texture (GPU), loading, drawing, pixels               (115)
    references/text.md       fonts, DrawText*, MeasureText*, TextFormat                           (50)
    references/models.md     Mesh/Model/Material, DrawMesh, ray collisions                        (76)
    references/audio.md      sounds, music, audio streams                                         (66)
    references/types.md      all structs, enums, color constants
    references/raymath.md    Vector2/3, Matrix, Quaternion helpers                                (143)
    references/rlgl.md       immediate mode + GL state (rlBegin, rlSetTexture, rlPushMatrix...)   (156)
    references/rgestures.md, references/rcamera.md — small extras

## Hard-won rules in THIS codebase

- **Batching**: raylib queues immediate-mode geometry. GL state toggles
  (depth test, backface culling, blend) apply when the batch flushes — call
  `rlDrawRenderBatchActive()` BEFORE re-enabling state, or your quads render
  with the wrong state (see hand.c, touch.c, draw_world). `DrawMesh` renders
  immediately; `DrawCube`/`DrawText`/rlBegin content is batched.
- **TextFormat returns a static ring buffer** — don't hold the pointer, and
  nested calls overwrite each other.
- **GetTime/GetFrameTime need InitWindow first.** Anything that must run
  headless (server.c) keeps its own clock and NEVER includes raylib.
- **windows.h clashes with raylib** (`Rectangle`, `CloseWindow`, `DrawText`,
  `ShowCursor`) — win32 API code goes in its own TU (see wincon.c).
- **Touch becomes a virtual mouse** on the first touch point: taps fire
  IsMouseButtonPressed. Gate desktop-mouse paths with `touch_mode()`.
- **TakeScreenshot writes to the LAUNCH cwd** (path cached at InitWindow),
  not the current cwd.
- **raymath composition**: `MatrixMultiply(A, B)` applies A first, then B —
  build transforms as scale → rotate → translate, left to right.
- **Meshes keep CPU-side copies** after UploadMesh — big worlds pay RAM for
  every live mesh; UnloadMesh frees both sides.
- raylib is fetched by CMake (FetchContent) — IDE "cannot open raylib.h"
  squiggles are IntelliSense noise, not build errors.
