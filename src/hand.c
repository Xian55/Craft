// First-person hand: a separate mini-scene drawn OVER the world, so the held
// item never pokes into blocks (game.js:1125-1181, render order game.js:2361).
// Empty hand shows a skin-colored arm; a real block shows a small atlas-
// textured cube; tools/torch/food show the flat items.png icon tilted ~45°.
#include "hand.h"
#include "mesh.h"
#include "inventory.h"
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"

Texture2D entities_items_tex(void);   // items.png, loaded by entities_init

#define DEG(r) ((r) * 180.0f / PI)

static Mesh cube;                     // held-block cube, rebuilt on slot change
static int cube_id = 0;

// game.js updateHeldMesh: cube transform pos(0.5,-0.5,-0.85) rotXYZ(-0.15,0.7,0.05) scale 0.42
static Matrix cube_transform(void) {
  Matrix m = MatrixScale(0.42f, 0.42f, 0.42f);                 // applied first
  m = MatrixMultiply(m, MatrixRotateZ(0.05f));                 // three.js XYZ euler:
  m = MatrixMultiply(m, MatrixRotateY(0.7f));                  // Z, then Y, then X
  m = MatrixMultiply(m, MatrixRotateX(-0.15f));
  return MatrixMultiply(m, MatrixTranslate(0.5f, -0.5f, -0.85f));
}

// Flat items.png icon: 1x1 quad facing the camera, tilted 45° (game.js:1174-1178).
static void draw_item_quad(int id) {
  Texture2D items = entities_items_tex();
  int tile = item_tile(id);
  if (items.id == 0 || tile < 0) return;
  float u0 = (tile % 4) / 4.0f, u1 = u0 + 0.25f;
  float v0 = (tile / 4) / 4.0f, v1 = v0 + 0.25f;   // v0 = top of the tile

  rlPushMatrix();
  rlTranslatef(0.55f, -0.5f, -0.9f);
  rlRotatef(DEG(0.79f), 0, 0, 1);
  rlScalef(0.7f, 0.7f, 1.0f);
  rlDisableBackfaceCulling();                       // double-sided like the JS material
  rlSetTexture(items.id);
  rlBegin(RL_QUADS);
  rlColor4ub(255, 255, 255, 255);
  rlTexCoord2f(u0, v1); rlVertex3f(-0.5f, -0.5f, 0);
  rlTexCoord2f(u1, v1); rlVertex3f( 0.5f, -0.5f, 0);
  rlTexCoord2f(u1, v0); rlVertex3f( 0.5f,  0.5f, 0);
  rlTexCoord2f(u0, v0); rlVertex3f(-0.5f,  0.5f, 0);
  rlEnd();
  rlSetTexture(0);
  rlDrawRenderBatchActive();                        // flush while culling is off
  rlEnableBackfaceCulling();
  rlPopMatrix();
}

// Skin-colored arm, only when the hand is empty (game.js:1128-1131, 1165).
static void draw_arm(void) {
  rlPushMatrix();
  rlTranslatef(0.6f, -0.7f, -0.9f);
  rlRotatef(DEG(0.35f), 1, 0, 0);
  rlRotatef(DEG(-0.3f), 0, 1, 0);
  rlRotatef(DEG(0.2f), 0, 0, 1);
  DrawCube((Vector3){ 0, 0, 0 }, 0.28f, 0.28f, 0.95f, (Color){ 224, 160, 112, 255 });
  rlPopMatrix();
}

void hand_draw(void) {
  int id = slots[sel_slot].id;

  Camera3D cam = {0};
  cam.position = (Vector3){ 0, 0, 0 };
  cam.target = (Vector3){ 0, 0, -1 };
  cam.up = (Vector3){ 0, 1, 0 };
  cam.fovy = 70.0f;
  cam.projection = CAMERA_PERSPECTIVE;

  BeginMode3D(cam);
  rlDisableDepthTest();                 // draw over the world (JS clearDepth)

  if (!id) {
    draw_arm();
  } else if (is_block(id) && id != B_TORCH) {
    if (id != cube_id) {                // rebuild the cube on hotbar change
      if (cube_id) UnloadMesh(cube);
      cube = build_block_cube_mesh((uint8_t)id);
      cube_id = id;
    }
    DrawMesh(cube, *block_cube_material(), cube_transform());
  } else {
    draw_item_quad(id);
  }

  rlDrawRenderBatchActive();            // flush batched quads before re-enabling depth
  rlEnableDepthTest();
  EndMode3D();
}
