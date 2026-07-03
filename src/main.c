// Craft Survival — C/Raylib port. Full feature set: terrain, meshing, lighting,
// day-night, physics, mining/tools, inventory/crafting/chests, fluids sim,
// sand/TNT/drops/pigs, chat commands, multiplayer (same protocol as game.js).
#include "raylib.h"
#include "raymath.h"
#include "config.h"
#include "state.h"
#include "world.h"
#include "mesh.h"
#include "physics.h"
#include "net.h"
#include "fluids.h"
#include "entities.h"
#include "interact.h"
#include "inventory.h"
#include "ui.h"
#include "sky.h"
#include "hand.h"
#include "metrics.h"
#include "prof.h"
#include "touch.h"
#include "lang.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#else
#include "server.h"        // embedded server: --server (headless) / --host
#include <signal.h>
static bool hosting = false;
#endif

#ifndef CRAFT_VERSION
#define CRAFT_VERSION "dev"
#endif

void win_console(int argc, char **argv);   // wincon.c (no-op off Windows/GUI)

float cam_fov = 60.0f;                  // /fov chat command (state.h)

static bool mouse_captured = false;
static int shot_frames = 0;             // CRAFT_SHOT=N: screenshot at frame N, then exit
static int frame_no = 0;
static float tick_accum = 0;
static double last_space_tap = -1;

static Color remote_color(int id) {
  float h = fmodf((float)id * 0.618f, 1.0f) * 360.0f;
  return ColorFromHSV(h, 0.6f, 0.72f);
}

static void handle_mouse_look(void) {
  if (!mouse_captured) return;
  Vector2 d = GetMouseDelta();
  player.yaw   -= d.x * 0.0025f;
  player.pitch -= d.y * 0.0025f;
  if (player.pitch >  1.55f) player.pitch =  1.55f;
  if (player.pitch < -1.55f) player.pitch = -1.55f;
}

static void capture_mouse(void) { DisableCursor(); mouse_captured = true; }
static void release_mouse(void) { EnableCursor(); mouse_captured = false; }

static void game_keys(void) {
  if (ui_chat_open()) return;                 // typing; chat handles its own keys
  if (IsKeyPressed(KEY_E)) { ui_toggle_inventory(); mouse_captured = !ui_panel_open(); return; }
  if (ui_panel_open()) return;
  if (IsKeyPressed(KEY_T)) { ui_open_chat(""); mouse_captured = false; return; }
  if (IsKeyPressed(KEY_SLASH)) { ui_open_chat("/"); mouse_captured = false; return; }
  for (int k = 0; k < 9; k++)
    if (IsKeyPressed(KEY_ONE + k)) sel_slot = k;
  // mouse wheel cycles the hotbar (up = previous, down = next, wraps)
  float wheel = GetMouseWheelMove();
  if (wheel > 0) sel_slot = (sel_slot + HOTBAR_SIZE - 1) % HOTBAR_SIZE;
  else if (wheel < 0) sel_slot = (sel_slot + 1) % HOTBAR_SIZE;
  if (IsKeyPressed(KEY_F)) eat_food();
  if (IsKeyPressed(KEY_Q)) drop_held();
  // creative: double-tap Space toggles flying (game.js:1834)
  if (creative && IsKeyPressed(KEY_SPACE)) {
    double now = GetTime();
    if (last_space_tap >= 0 && now - last_space_tap < 0.3) { flying = !flying; player.vy = 0; }
    last_space_tap = now;
  }
  if (IsKeyPressed(KEY_F3)) metrics_set(-1);   // cycle off -> compact -> graphs
}

static void frame(void) {
  float dt = GetFrameTime();
  if (dt > 0.05f) dt = 0.05f;
  frame_no++;

#ifdef __EMSCRIPTEN__
  // Mouse-coordinate fix: raylib's web resize path resizes the canvas
  // directly, bypassing GLFW — GLFW keeps its InitWindow-time size and keeps
  // scaling mouse events by old_width/css_width (2/3 at 1920 wide), so panel
  // hover/clicks land on the wrong cells (worst in fullscreen). Push the CSS
  // size through SetWindowSize (= glfwSetWindowSize) so GLFW's internal size
  // — and with it the mouse scale — matches the canvas again.
  {
    static int synced_w = 0, synced_h = 0;
    double cw, ch;
    if (emscripten_get_element_css_size("#canvas", &cw, &ch) == EMSCRIPTEN_RESULT_SUCCESS) {
      int iw = (int)cw, ih = (int)ch;
      if (iw > 0 && ih > 0 && (iw != synced_w || ih != synced_h)) {
        SetWindowSize(iw, ih);
        synced_w = iw; synced_h = ih;
      }
    }
  }
#endif

  // CRAFT_CMD debug hook: run ';'-separated commands once the world is meshed
  static const char *pending_cmds = NULL;
  static bool cmds_taken = false;
  if (!cmds_taken && frame_no == 1) { pending_cmds = getenv("CRAFT_CMD"); cmds_taken = true; }
  if (pending_cmds && frame_no == 30) {
    static char buf[512];
    snprintf(buf, sizeof(buf), "%s", pending_cmds);
    // manual split: run_command uses strtok internally, which would reset
    // an outer strtok and silently drop every command after the first
    for (char *p = buf; p; ) {
      char *semi = strchr(p, ';');
      if (semi) *semi = 0;
      ui_run_command(p);
      p = semi ? semi + 1 : NULL;
    }
    pending_cmds = NULL;
  }

  bool ui_blocked = ui_panel_open() || ui_chat_open();

  // capture / release mouse (desktop only: touch mode never pointer-locks,
  // and raylib's touch->virtual-mouse mapping would trigger this on every tap)
  if (!touch_mode()) {
    if (!mouse_captured && !ui_blocked && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) capture_mouse();
#ifndef __EMSCRIPTEN__
    if (mouse_captured && IsKeyPressed(KEY_ESCAPE)) release_mouse();
#else
    if (mouse_captured && !ui_blocked && IsCursorOnScreen() && !IsCursorHidden()) mouse_captured = false;
#endif
  }

  // CRAFT_PROF: wrap a phase in a timer only when profiling is on
  #define PROF_DO(ph, ...) do { \
    if (prof_on()) { double t0_ = GetTime(); __VA_ARGS__; prof_note(ph, (GetTime() - t0_) * 1000.0); } \
    else { __VA_ARGS__; } \
  } while (0)

  double tin0 = GetTime();
  game_keys();
  ui_update();                          // panel / chat input
  bool was_blocked = ui_blocked;
  ui_blocked = ui_panel_open() || ui_chat_open();
  if (was_blocked && !ui_blocked && !touch_mode()) mouse_captured = true;   // panel just closed

  if (!ui_blocked && !touch_mode()) handle_mouse_look();

  Input in = { 0 };
  if (!ui_blocked) {
    in.fwd = IsKeyDown(KEY_W); in.back = IsKeyDown(KEY_S);
    in.left = IsKeyDown(KEY_A); in.right = IsKeyDown(KEY_D);
    in.jump = IsKeyDown(KEY_SPACE);
    in.sink = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    in.sprint = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
  }
  touch_update(&in, ui_blocked);        // joystick/look/buttons (no-op untouched)

  // sprint FOV kick: ease +12° in/out while actually sprint-moving
  static float sprint_fov = 0;
  bool sprint_moving = in.sprint && (in.fwd || in.back || in.left || in.right);
  float fov_target = sprint_moving ? 12.0f : 0.0f;
  sprint_fov += (fov_target - sprint_fov) * (dt * 10.0f > 1 ? 1 : dt * 10.0f);
  if (prof_on()) prof_note(PH_INPUT_UI, (GetTime() - tin0) * 1000.0);

  PROF_DO(PH_PLAYER, update_player(&in, dt); update_stats(dt));
  metrics_frame();
#ifndef __EMSCRIPTEN__
  if (hosting) server_pump();           // --host: serve while playing
#endif
  PROF_DO(PH_CHUNKS, update_chunks(player.x, player.z));
  PROF_DO(PH_NET, net_update(dt));
  sky_update(dt);
  PROF_DO(PH_ENTITIES,
    update_falling(dt);
    update_primed(dt);
    update_drops(dt);
    update_pigs(dt);
    update_mobs(dt));

  // interaction (touch mode drives set_mining/use_right_click in touch_update)
  double tmine0 = GetTime();
  if (!touch_mode() && mouse_captured && !ui_blocked) {
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
      int hit[3], prev[3];
      if (raycast_voxel(6.0f, hit, prev) && get_voxel(hit[0], hit[1], hit[2]) == B_TNT) {
        prime_tnt(hit[0], hit[1], hit[2], 1.5f);       // left click lights TNT
      } else if (!attack_creature()) {
        set_mining(true);
      }
    }
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) set_mining(false);
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) use_right_click();
    update_mining(dt);
  } else if (touch_mode()) {
    update_mining(dt);
  } else {
    set_mining(false);
    update_mining(dt);
  }
  if (prof_on()) prof_note(PH_MINING, (GetTime() - tmine0) * 1000.0);

  // fixed-tick fluid simulation (game.js TICK_SECONDS)
  tick_accum += dt;
  PROF_DO(PH_FLUIDS,
    while (tick_accum >= 0.2f) { tick_accum -= 0.2f; simulate_water(); });

  // camera
  Camera3D cam = {0};
  cam.position = (Vector3){ (float)player.x, (float)(player.y + P_EYE), (float)player.z };
  float cp = cosf(player.pitch);
  Vector3 look = { -sinf(player.yaw) * cp, sinf(player.pitch), -cosf(player.yaw) * cp };
  cam.target = Vector3Add(cam.position, look);
  cam.up = (Vector3){ 0, 1, 0 };
  cam.fovy = cam_fov + sprint_fov;
  cam.projection = CAMERA_PERSPECTIVE;

  BeginDrawing();
  ClearBackground(sky_color());
  BeginMode3D(cam);
  sky_draw_3d(cam);
  PROF_DO(PH_DRAW_WORLD, draw_world());   // draw_world flushes its own batches
  PROF_DO(PH_DRAW_ENT, draw_entities(cam));
  for (int i = 0; i < MAX_REMOTE; i++) {
    if (!remote_players[i].used) continue;
    RemotePlayer *r = &remote_players[i];
    Vector3 c = { (float)r->x, (float)(r->y + 0.9), (float)r->z };
    DrawCube(c, 0.6f, 1.8f, 0.6f, remote_color((int)r->id));
    // nose tracks yaw AND pitch (the interpolated look direction)
    float cp2 = cosf(r->pitch);
    Vector3 nose = { c.x - sinf(r->yaw) * 0.35f * cp2,
                     c.y + 0.55f + sinf(r->pitch) * 0.25f,
                     c.z - cosf(r->yaw) * 0.35f * cp2 };
    DrawCube(nose, 0.25f, 0.25f, 0.2f, (Color){ 34, 34, 34, 255 });
    // held item at the right hand: body center + right*0.45 + forward*0.25
    Vector3 hand = { c.x + cosf(r->yaw) * 0.45f - sinf(r->yaw) * 0.25f,
                     c.y + 0.1f,
                     c.z - sinf(r->yaw) * 0.45f - cosf(r->yaw) * 0.25f };
    draw_world_held(cam, r->held, hand, r->yaw);
  }
  if ((mouse_captured || touch_mode()) && !ui_blocked) draw_highlight();
  EndMode3D();

  double tui0 = GetTime();
  hand_draw();                          // first-person held item, over the world

  DrawText("+", GetScreenWidth() / 2 - 4, GetScreenHeight() / 2 - 10, 20, WHITE);
  ui_draw();
  touch_draw();
  if (prof_on()) prof_note(PH_DRAW_UI, (GetTime() - tui0) * 1000.0);
  if (metrics_mode()) metrics_draw(metrics_mode());
  else DrawFPS(10, 10);
  #undef PROF_DO
  if (!mouse_captured && !ui_blocked && !touch_mode()) {
    const char *hint = tr("hint.click");
    DrawText(hint, GetScreenWidth() / 2 - MeasureText(hint, 18) / 2,
             GetScreenHeight() / 2 + 30, 18, WHITE);
  }
  if (prof_on()) {
    double ts0 = GetTime();
    EndDrawing();
    prof_note(PH_SWAP, (GetTime() - ts0) * 1000.0);
  } else {
    EndDrawing();
  }

  if (shot_frames > 0 && frame_no >= shot_frames) {
    TakeScreenshot("craft_shot.png");
#ifdef __EMSCRIPTEN__
    emscripten_cancel_main_loop();
#else
    CloseWindow();
    exit(0);
#endif
  }
}

#ifndef __EMSCRIPTEN__
static volatile sig_atomic_t server_quit = 0;
static void on_quit_signal(int s) { (void)s; server_quit = 1; }

// `craft --server`: headless dedicated server, no window, no GPU. Same
// binary, same module as --host; runs fine on a box with no display.
static int run_dedicated(int port) {
  const char *sdir = getenv("STATIC");
  const char *ddir = getenv("CRAFT_DATA");
  if (!server_start(port, sdir ? sdir : "", ddir ? ddir : ".")) return 1;
  signal(SIGINT, on_quit_signal);
#ifdef SIGTERM
  signal(SIGTERM, on_quit_signal);   // docker stop
#endif
  while (!server_quit) { server_pump(); server_sleep_ms(2); }
  server_stop();
  return 0;
}
#endif

int main(int argc, char **argv) {
  const char *host = NULL;
#ifndef __EMSCRIPTEN__
  win_console(argc, argv);
  bool dedicated = false;
  int port = getenv("PORT") ? atoi(getenv("PORT")) : 8080;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--version") == 0) { printf("%s\n", CRAFT_VERSION); return 0; }
    if (strcmp(argv[i], "--server") == 0) dedicated = true;
    if (strcmp(argv[i], "--server-test") == 0) return server_selftest();
    if (strcmp(argv[i], "--serve") == 0 || strcmp(argv[i], "--host-game") == 0) hosting = true;
    if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = atoi(argv[i + 1]);
  }
  if (dedicated) return run_dedicated(port);
#endif
  atexit(prof_dump);                        // CRAFT_PROF table (no-op otherwise)
  for (int i = 1; i < argc - 1; i++)
    if (strcmp(argv[i], "--host") == 0) host = argv[i + 1];
  const char *shot = getenv("CRAFT_SHOT");
  if (shot) shot_frames = atoi(shot);
  const char *pose = getenv("CRAFT_POS");   // debug: "x,y,z,yaw,pitch", implies fly+creative
  const char *t0 = getenv("CRAFT_TIME");    // debug: initial day_time 0..1
  if (t0) day_time = atof(t0);
  const char *dm = getenv("CRAFT_METRICS"); // debug: start with metrics overlay 0..2
  if (dm) metrics_set(atoi(dm));

  SetConfigFlags(FLAG_VSYNC_HINT | FLAG_WINDOW_RESIZABLE);
  InitWindow(1280, 720, "Craft Survival " CRAFT_VERSION);
  SetExitKey(KEY_NULL);
#ifndef __EMSCRIPTEN__
  // Out-of-tree builds: assets/ is copied next to the exe (CMake post-build).
  // Anchor the cwd there so assets, uid.txt and screenshots resolve the same
  // no matter where the exe is launched from.
  ChangeDirectory(GetApplicationDirectory());
#endif

  lang_init();                          // after cwd anchor: reads assets/lang + lang.txt
  mesh_init();
  entities_init();
  interact_init();
  ui_init();
  spawn_player();
  if (pose) {
    double x, y, z; float yaw, pitch;
    if (sscanf(pose, "%lf,%lf,%lf,%f,%f", &x, &y, &z, &yaw, &pitch) == 5) {
      player.x = x; player.y = y; player.z = z;
      player.yaw = yaw; player.pitch = pitch;
      flying = true; creative = true;
    }
  }
  update_chunks(player.x, player.z);

  // starter pack (game.js:2242)
  add_item(B_DIRT, 10);
  add_item(I_WATER_BUCKET, 1);
  add_item(B_TORCH, 8);
  spawn_pigs(6);

#ifdef __EMSCRIPTEN__
  static char page_host[128];
  EM_ASM({ stringToUTF8(location.hostname || 'localhost', $0, $1); }, page_host, sizeof page_host);
  net_connect(page_host, 8080);
  emscripten_set_main_loop(frame, 0, 1);
#else
  if (hosting) {
    const char *sdir = getenv("STATIC");
    if (server_start(port, sdir ? sdir : "", ".")) host = "localhost";
    else hosting = false;
  }
  net_connect(host ? host : "localhost", hosting ? port : 8080);
  while (!WindowShouldClose()) frame();
#endif

  net_save_state();                     // don't lose the bag on exit
#ifndef __EMSCRIPTEN__
  if (hosting) server_stop();
#endif
  CloseWindow();
  return 0;
}
