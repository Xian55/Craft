#include "ui.h"
#include "inventory.h"
#include "interact.h"
#include "physics.h"
#include "world.h"
#include "mesh.h"
#include "entities.h"
#include "fluids.h"
#include "net.h"
#include "state.h"
#include "lang.h"
#include "metrics.h"
#include "raylib.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>

Texture2D entities_items_tex(void);   // items.png, loaded by entities_init

// ============================== layout ==============================
#define CELL 52
#define GAP 6

typedef enum { REF_NONE, REF_SLOT, REF_CRAFT, REF_CHEST, REF_RESULT } RefKind;
typedef struct { RefKind kind; int i; } CellRef;

static bool panel_open = false;
static bool chest_mode = false;
static Stack *open_chest = NULL;
static int chest_x, chest_y, chest_z;   // position of the open chest

static bool chat_open = false;
static char chat_input[128];
static char chat_lines[8][128];
static int n_chat_lines = 0;

bool ui_panel_open(void) { return panel_open; }
bool ui_chat_open(void) { return chat_open; }

void ui_init(void) {}

void ui_chat_log(const char *msg) {
  if (n_chat_lines == 8) { memmove(chat_lines[0], chat_lines[1], sizeof(chat_lines[0]) * 7); n_chat_lines = 7; }
  snprintf(chat_lines[n_chat_lines++], sizeof(chat_lines[0]), "%s", msg);
}

// ============================== item icons ==============================
static void draw_item_icon(int id, float x, float y, float size) {
  Texture2D items = entities_items_tex();
  int tile = item_tile(id);
  if (tile >= 0 && items.id != 0) {
    Rectangle src = { (tile % 4) * 64.0f, (tile / 4) * 64.0f, 64, 64 };
    DrawTexturePro(items, src, (Rectangle){ x, y, size, size }, (Vector2){ 0, 0 }, 0, WHITE);
  } else {
    uint8_t rgb[3]; item_color(id, rgb);
    DrawRectangleRounded((Rectangle){ x + size * 0.12f, y + size * 0.12f, size * 0.76f, size * 0.76f },
                         0.2f, 4, (Color){ rgb[0], rgb[1], rgb[2], 255 });
  }
}

static void draw_cell(float x, float y, const Stack *s, int key_label, bool sel) {
  DrawRectangleRounded((Rectangle){ x, y, CELL, CELL }, 0.15f, 4, (Color){ 255, 255, 255, 30 });
  DrawRectangleRoundedLines((Rectangle){ x, y, CELL, CELL }, 0.15f, 4, sel ? WHITE : (Color){ 0, 0, 0, 128 });
  if (key_label > 0) DrawText(TextFormat("%d", key_label), (int)x + 4, (int)y + 2, 10, (Color){ 255, 255, 255, 180 });
  if (s && s->id) {
    draw_item_icon(s->id, x + 4, y + 4, CELL - 8);
    if (s->count > 1)
      DrawText(TextFormat("%d", s->count), (int)(x + CELL - 6 - MeasureText(TextFormat("%d", s->count), 14)),
               (int)(y + CELL - 16), 14, WHITE);
  }
}

// ============================== HUD ==============================
static void draw_hud(void) {
  int W = GetScreenWidth(), H = GetScreenHeight();
  float bar_w = HOTBAR_SIZE * CELL + (HOTBAR_SIZE - 1) * GAP;
  float bx = (W - bar_w) / 2.0f, by = H - CELL - 12.0f;
  for (int i = 0; i < HOTBAR_SIZE; i++)
    draw_cell(bx + i * (CELL + GAP), by, &slots[i], i + 1, i == sel_slot);

  // hearts + drumsticks above the hotbar's left edge (game.js #stats)
  float sx = bx, sy = by - 46;
  for (int i = 0; i < 10; i++) {   // hunger row on top
    bool full = player.hunger >= (i + 1) * 2 - 1;
    DrawRectangleRounded((Rectangle){ sx + i * 18, sy, 14, 14 }, 0.4f, 4,
                         full ? (Color){ 214, 121, 46, 255 } : (Color){ 40, 40, 40, 180 });
  }
  for (int i = 0; i < 10; i++) {   // hearts below
    bool full = player.health >= (i + 1) * 2 - 1;
    DrawRectangleRounded((Rectangle){ sx + i * 18, sy + 18, 14, 14 }, 0.4f, 4,
                         full ? (Color){ 220, 40, 40, 255 } : (Color){ 40, 40, 40, 180 });
  }

  // held item name above the hotbar
  Stack *hs = &slots[sel_slot];
  const char *held = hs->id ? TextFormat(tr("hud.held"), item_name(hs->id), hs->count) : tr("hud.held_empty");
  DrawText(held, (int)(W / 2 - MeasureText(held, 16) / 2), (int)(by - 70), 16, WHITE);

  // mining progress bar under the crosshair
  float prog = mine_progress01();
  if (prog >= 0) {
    float px = W / 2.0f - 32, py = H / 2.0f + 22;
    DrawRectangleRounded((Rectangle){ px, py, 64, 8 }, 0.5f, 4, (Color){ 0, 0, 0, 90 });
    DrawRectangleRounded((Rectangle){ px, py, 64 * (prog > 1 ? 1 : prog), 8 }, 0.5f, 4, (Color){ 255, 211, 77, 255 });
  }

  // underwater veil
  int ex = (int)floor(player.x), ey = (int)floor(player.y + P_EYE), ez = (int)floor(player.z);
  if (water_at(ex, ey, ez) > 0) DrawRectangle(0, 0, W, H, (Color){ 40, 110, 180, 115 });

  // chat log (bottom-left)
  for (int i = 0; i < n_chat_lines; i++) {
    int y = H - 140 - (n_chat_lines - i) * 20;
    DrawRectangle(8, y - 2, MeasureText(chat_lines[i], 14) + 12, 18, (Color){ 0, 0, 0, 128 });
    DrawText(chat_lines[i], 14, y, 14, WHITE);
  }
}

// ============================== panels ==============================
static CellRef hit_test(Vector2 m, float *ox, float *oy);   // fwd

static float panel_x, panel_y;    // top-left of the panel, computed per frame

static CellRef cell_at_mouse(void) {
  float ox, oy;
  return hit_test(GetMousePosition(), &ox, &oy);
}

static Stack *ref_stack(CellRef r) {
  switch (r.kind) {
    case REF_SLOT: return &slots[r.i];
    case REF_CRAFT: return &craft_grid[r.i];
    case REF_CHEST: return open_chest ? &open_chest[r.i] : NULL;
    default: return NULL;
  }
}

// clickCell (game.js:1990): pick up / put down / merge / swap
static void click_cell(CellRef r) {
  if (r.kind == REF_RESULT) { take_result(); return; }
  Stack *s = ref_stack(r);
  if (!s) return;
  if (!cursor_stack.id) {
    if (s->id) { cursor_stack = *s; s->id = 0; s->count = 0; }
  } else if (!s->id) {
    *s = cursor_stack; cursor_stack.id = 0; cursor_stack.count = 0;
  } else if (s->id == cursor_stack.id) {
    int add = stack_max(s->id) - s->count;
    if (add > cursor_stack.count) add = cursor_stack.count;
    s->count += add; cursor_stack.count -= add;
    if (cursor_stack.count == 0) cursor_stack.id = 0;
  } else {
    Stack tmp = *s; *s = cursor_stack; cursor_stack = tmp;
  }
}

// rightClickCell (game.js:2008): halve / place one
static void right_click_cell(CellRef r) {
  if (r.kind == REF_RESULT) { take_result(); return; }
  Stack *s = ref_stack(r);
  if (!s) return;
  if (!cursor_stack.id) {
    if (s->id) {
      int take = (s->count + 1) / 2;
      cursor_stack.id = s->id; cursor_stack.count = take;
      s->count -= take; if (s->count == 0) s->id = 0;
    }
  } else if (!s->id) {
    s->id = cursor_stack.id; s->count = 1; cursor_stack.count--;
  } else if (s->id == cursor_stack.id && s->count < stack_max(s->id)) {
    s->count++; cursor_stack.count--;
  } else return;
  if (cursor_stack.count == 0) cursor_stack.id = 0;
}

static void close_panel(void) {
  panel_open = false;
  if (cursor_stack.id) { add_item(cursor_stack.id, cursor_stack.count); cursor_stack.id = 0; cursor_stack.count = 0; }
  for (int i = 0; i < 9; i++)
    if (craft_grid[i].id) { add_item(craft_grid[i].id, craft_grid[i].count); craft_grid[i].id = 0; craft_grid[i].count = 0; }
  if (chest_mode && open_chest) net_send_chest_set(chest_x, chest_y, chest_z);  // persist to the server
  open_chest = NULL; chest_mode = false;
  DisableCursor();
}

void ui_toggle_inventory(void) {
  if (panel_open) { close_panel(); return; }
  panel_open = true; chest_mode = false;
  set_mining(false);
  EnableCursor();
}

void open_chest_at(int x, int y, int z) {   // interact.h hook
  panel_open = true; chest_mode = true;
  chest_x = x; chest_y = y; chest_z = z;
  open_chest = chest_array_at(x, y, z);
  net_send_chest_get(x, y, z);              // server truth arrives via SV_CHEST
  set_mining(false);
  EnableCursor();
}

// Panel geometry: 9 columns of cells; rows depend on mode.
// MUST advance y exactly like draw_panel (including the 16 px section
// headings), otherwise hit boxes drift from the drawn cells.
static CellRef hit_test(Vector2 m, float *ox, float *oy) {
  float col_w = 9 * CELL + 8 * GAP;
  panel_x = (GetScreenWidth() - col_w) / 2.0f;
  panel_y = 60;
  float y = panel_y;
  CellRef ref = { REF_NONE, 0 };
  #define TRY_GRID(KIND, BASE, COUNT, COLS) do { \
    for (int i = 0; i < (COUNT); i++) { \
      float cx = panel_x + (i % (COLS)) * (CELL + GAP), cy = y + (i / (COLS)) * (CELL + GAP); \
      if (m.x >= cx && m.x < cx + CELL && m.y >= cy && m.y < cy + CELL) { ref.kind = (KIND); ref.i = (BASE) + i; return ref; } \
    } \
    y += (((COUNT) + (COLS) - 1) / (COLS)) * (CELL + GAP) + 14; \
  } while (0)

  if (chest_mode) {
    TRY_GRID(REF_CHEST, 0, CHEST_SIZE, 9);
    y += 16;                              // bag heading
    TRY_GRID(REF_SLOT, HOTBAR_SIZE, INV_SIZE - HOTBAR_SIZE, 9);
    TRY_GRID(REF_SLOT, 0, HOTBAR_SIZE, 9);
  } else {
    TRY_GRID(REF_SLOT, HOTBAR_SIZE, INV_SIZE - HOTBAR_SIZE, 9);
    TRY_GRID(REF_SLOT, 0, HOTBAR_SIZE, 9);
    y += 16;                              // crafting heading
    float craft_y = y;
    TRY_GRID(REF_CRAFT, 0, 9, 3);
    // result cell sits right of the craft grid, second row (see draw_panel)
    float rx = panel_x + 3 * (CELL + GAP) + 40, ry = craft_y + (CELL + GAP);
    if (m.x >= rx && m.x < rx + CELL && m.y >= ry && m.y < ry + CELL) { ref.kind = REF_RESULT; return ref; }
  }
  #undef TRY_GRID
  if (ox) *ox = panel_x;
  if (oy) *oy = y;
  return ref;
}

static void draw_grid(RefKind kind, int base, int count, int cols, float *y) {
  for (int i = 0; i < count; i++) {
    float cx = panel_x + (i % cols) * (CELL + GAP), cy = *y + (i / cols) * (CELL + GAP);
    const Stack *s = kind == REF_SLOT ? &slots[base + i] : kind == REF_CRAFT ? &craft_grid[base + i] : &open_chest[base + i];
    draw_cell(cx, cy, s, kind == REF_SLOT && base == 0 ? i + 1 : 0, kind == REF_SLOT && base + i == sel_slot);
  }
  *y += ((count + cols - 1) / cols) * (CELL + GAP) + 14;
}

static void draw_panel(void) {
  int W = GetScreenWidth(), H = GetScreenHeight();
  DrawRectangle(0, 0, W, H, (Color){ 0, 0, 0, 140 });
  float col_w = 9 * CELL + 8 * GAP;
  panel_x = (W - col_w) / 2.0f;
  panel_y = 60;
  float y = panel_y;
  DrawRectangleRounded((Rectangle){ panel_x - 20, y - 40, col_w + 40, (float)H - y - 20 }, 0.03f, 6, (Color){ 58, 47, 40, 235 });

  if (chest_mode && open_chest) {
    DrawText(tr("panel.chest"), (int)panel_x, (int)y - 28, 20, WHITE);
    draw_grid(REF_CHEST, 0, CHEST_SIZE, 9, &y);
    DrawText(tr("panel.bag"), (int)panel_x, (int)y - 8, 20, WHITE); y += 16;
    draw_grid(REF_SLOT, HOTBAR_SIZE, INV_SIZE - HOTBAR_SIZE, 9, &y);
    draw_grid(REF_SLOT, 0, HOTBAR_SIZE, 9, &y);
  } else {
    DrawText(tr("panel.bag"), (int)panel_x, (int)y - 28, 20, WHITE);
    draw_grid(REF_SLOT, HOTBAR_SIZE, INV_SIZE - HOTBAR_SIZE, 9, &y);
    draw_grid(REF_SLOT, 0, HOTBAR_SIZE, 9, &y);
    DrawText(tr("panel.craft"), (int)panel_x, (int)y - 8, 20, WHITE); y += 16;
    float craft_y = y;
    draw_grid(REF_CRAFT, 0, 9, 3, &y);
    // arrow + result
    float rx = panel_x + 3 * (CELL + GAP) + 40, ry = craft_y + (CELL + GAP);
    DrawText("->", (int)(panel_x + 3 * (CELL + GAP) + 8), (int)(ry + 14), 24, WHITE);
    int oid, on;
    Stack out = {0};
    if (match_recipe(&oid, &on)) { out.id = oid; out.count = on; }
    draw_cell(rx, ry, &out, 0, false);
    DrawText(tr("panel.hint"),
             (int)panel_x, (int)y + 4, 14, (Color){ 255, 255, 255, 200 });
  }

  // tooltip + carried stack at the mouse
  Vector2 m = GetMousePosition();
  CellRef hov = cell_at_mouse();
  Stack *hs = ref_stack(hov);
  if (hs && hs->id && !cursor_stack.id) {
    const char *nm = item_name(hs->id);
    DrawRectangle((int)m.x + 12, (int)m.y + 12, MeasureText(nm, 15) + 12, 20, (Color){ 0, 0, 0, 220 });
    DrawText(nm, (int)m.x + 18, (int)m.y + 15, 15, WHITE);
  }
  if (cursor_stack.id) {
    draw_item_icon(cursor_stack.id, m.x - 20, m.y - 20, 40);
    if (cursor_stack.count > 1)
      DrawText(TextFormat("%d", cursor_stack.count), (int)m.x + 6, (int)m.y + 6, 14, WHITE);
  }
}

// ============================== chat & commands ==============================
static int resolve_item(const char *name) {
  for (int id = 1; id <= 27; id++) {
    const char *n = item_name(id);
    if (strcmp(n, "?") == 0) continue;
    bool eq = true;
    for (int i = 0; ; i++) {
      char a = (char)tolower((unsigned char)n[i]), b = (char)tolower((unsigned char)name[i]);
      if (a != b) { eq = false; break; }
      if (!a) break;
    }
    if (eq) return id;
  }
  return 0;
}

static void run_command(const char *cmd);
void ui_run_command(const char *cmd) { run_command(cmd); }

void ui_open_chat(const char *prefill) {
  chat_open = true;
  snprintf(chat_input, sizeof(chat_input), "%s", prefill ? prefill : "");
  while (GetCharPressed() > 0) {}   // drop the 't'/'/' char of the opening keystroke
  set_mining(false);
  EnableCursor();
}

static void close_chat(bool run) {
  chat_open = false;
  if (run && chat_input[0]) {
    if (chat_input[0] == '/') {
      run_command(chat_input);
    } else {
      // plain text is player chat: echo locally, send to everyone
      ui_chat_log(TextFormat("<%s> %s", tr("chat.you"), chat_input));
      net_send_chat(chat_input);
    }
  }
  chat_input[0] = 0;
  if (!panel_open) DisableCursor();
}

static void update_chat(void) {
  int ch;
  while ((ch = GetCharPressed()) > 0) {
    size_t l = strlen(chat_input);
    if (ch >= 32 && ch < 127 && l < sizeof(chat_input) - 1) { chat_input[l] = (char)ch; chat_input[l + 1] = 0; }
  }
  if (IsKeyPressed(KEY_BACKSPACE)) { size_t l = strlen(chat_input); if (l) chat_input[l - 1] = 0; }
  if (IsKeyPressed(KEY_ENTER)) close_chat(true);
  if (IsKeyPressed(KEY_ESCAPE)) close_chat(false);
}

static void draw_chat(void) {
  int H = GetScreenHeight();
  DrawRectangle(10, H - 40, 460, 26, (Color){ 0, 0, 0, 190 });
  DrawText(TextFormat("%s_", chat_input), 16, H - 35, 16, WHITE);
}

static void run_command(const char *raw) {
  char buf[128];
  snprintf(buf, sizeof(buf), "%s", raw[0] == '/' ? raw + 1 : raw);
  char *args[8]; int n = 0;
  for (char *t = strtok(buf, " "); t && n < 8; t = strtok(NULL, " ")) args[n++] = t;
  if (!n) return;
  for (char *p = args[0]; *p; p++) *p = (char)tolower((unsigned char)*p);

  if (!strcmp(args[0], "help")) {
    ui_chat_log(tr("cmd.help"));
  } else if (!strcmp(args[0], "give") && n >= 2) {
    int count = 1, last = n - 1;
    if (n >= 3 && args[last][0] >= '0' && args[last][0] <= '9') { count = atoi(args[last]); last--; }
    char name[64] = ""; for (int i = 1; i <= last; i++) { strcat(name, args[i]); if (i < last) strcat(name, " "); }
    int id = resolve_item(name);
    if (!id) { ui_chat_log(TextFormat(tr("cmd.no_item"), name)); return; }
    add_item(id, count);
    ui_chat_log(TextFormat("+%d %s", count, item_name(id)));
  } else if (!strcmp(args[0], "tp") && n >= 4) {
    player.x = atof(args[1]); player.y = atof(args[2]); player.z = atof(args[3]);
    player.vx = player.vy = player.vz = 0;
    ui_chat_log(TextFormat(tr("cmd.tp"), args[1], args[2], args[3]));
  } else if (!strcmp(args[0], "heal")) {
    player.health = 20; player.hunger = 20;
    ui_chat_log(tr("cmd.heal"));
  } else if (!strcmp(args[0], "speed")) {
    float v = n >= 2 ? (float)atof(args[1]) : 5;
    move_speed = v < 1 ? 1 : v > 40 ? 40 : v;
    ui_chat_log(TextFormat(tr("cmd.speed"), move_speed));
  } else if (!strcmp(args[0], "pig")) {
    spawn_pigs(n >= 2 ? (atoi(args[1]) > 0 ? atoi(args[1]) : 1) : 1);
    ui_chat_log(tr("cmd.pig"));
  } else if (!strcmp(args[0], "zombie") || !strcmp(args[0], "skeleton") || !strcmp(args[0], "creeper")) {
    int cnt = n >= 2 ? (atoi(args[1]) > 0 ? atoi(args[1]) : 1) : 1;
    spawn_mobs(args[0][0] == 'z' ? MOB_ZOMBIE : args[0][0] == 's' ? MOB_SKELETON : MOB_CREEPER, cnt);
    ui_chat_log(TextFormat(tr("cmd.mob"), cnt));
  } else if (!strcmp(args[0], "creative")) {
    creative = true; ui_chat_log(tr("cmd.creative"));
  } else if (!strcmp(args[0], "survival")) {
    creative = false; flying = false; ui_chat_log(tr("cmd.survival"));
  } else if (!strcmp(args[0], "time") && n >= 2) {
    double v;
    if (!strcmp(args[1], "day") || !strcmp(args[1], "nappal")) v = 0.25;
    else if (!strcmp(args[1], "night") || !strcmp(args[1], "ejjel")) v = 0.75;
    else v = fmod(fmod(atof(args[1]), 1.0) + 1.0, 1.0);
    day_time = v;
    net_send_settime(v);
    ui_chat_log(TextFormat(tr("cmd.time"), v));
  } else if (!strcmp(args[0], "rd") || !strcmp(args[0], "renderdistance")) {
    render_dist_set(n >= 2 ? atoi(args[1]) : 4);
    ui_chat_log(TextFormat(tr("cmd.rd"), render_dist_get()));
  } else if (!strcmp(args[0], "fov")) {
    float v = n >= 2 ? (float)atof(args[1]) : 60;
    cam_fov = v < 30 ? 30 : v > 110 ? 110 : v;
    ui_chat_log(TextFormat(tr("cmd.fov"), cam_fov));
  } else if (!strcmp(args[0], "place") && n >= 5) {
    char name[64] = ""; for (int i = 1; i <= n - 4; i++) { strcat(name, args[i]); if (i < n - 4) strcat(name, " "); }
    int id = resolve_item(name);
    int x = atoi(args[n - 3]), y = atoi(args[n - 2]), z = atoi(args[n - 1]);
    if (!id || !is_block(id)) { ui_chat_log(TextFormat(tr("cmd.no_block"), name)); return; }
    set_voxel(x, y, z, (uint8_t)id); remesh_around(x, z); touch_water(x, y, z);
    if (id == B_SAND) check_fall(x, y, z);
    ui_chat_log(TextFormat(tr("cmd.place"), item_name(id), x, y, z));
  } else if (!strcmp(args[0], "slot") && n >= 2) {
    int s = atoi(args[1]) - 1;          // debug/test hook: select hotbar slot 1-9
    if (s >= 0 && s < HOTBAR_SIZE) sel_slot = s;
  } else if (!strcmp(args[0], "boom")) {
    // debug/profiling: drop an n-TNT cluster ~6 blocks ahead and light it
    int cnt = (n >= 2) ? atoi(args[1]) : 1;
    cnt = cnt < 1 ? 1 : cnt > 27 ? 27 : cnt;
    float cp = cosf(player.yaw);
    int bx = (int)floor(player.x - sinf(player.yaw) * 6.0), bz = (int)floor(player.z - cp * 6.0);
    int by = (int)floor(player.y);
    int placed = 0;
    for (int dy = 0; dy < 3 && placed < cnt; dy++)
      for (int dx = -1; dx <= 1 && placed < cnt; dx++)
        for (int dz = -1; dz <= 1 && placed < cnt; dz++) {
          set_voxel(bx + dx, by + dy, bz + dz, B_TNT);
          placed++;
        }
    remesh_around(bx, bz);
    prime_tnt(bx, by, bz, 0.5f);
  } else if (!strcmp(args[0], "metrics")) {
    // same as F3; the explicit form helps where the browser eats F-keys
    metrics_set(n >= 2 ? atoi(args[1]) : -1);
  } else if (!strcmp(args[0], "lang") && n >= 2) {
    if (lang_load(args[1])) {
      FILE *f = fopen("lang.txt", "w");
      if (f) { fprintf(f, "%s\n", lang_current()); fclose(f); }
      ui_chat_log(TextFormat(tr("cmd.lang"), lang_current()));
    } else {
      ui_chat_log(TextFormat(tr("cmd.lang_missing"), args[1]));
    }
  } else {
    ui_chat_log(TextFormat(tr("cmd.unknown"), args[0]));
  }
}

// ============================== per-frame ==============================
void ui_update(void) {
  if (chat_open) { update_chat(); return; }
  if (!panel_open) return;
  // E is handled by game_keys() in main.c (it toggles); handling it here too
  // would close the panel in the same frame it was opened.
  if (IsKeyPressed(KEY_ESCAPE)) { close_panel(); return; }
  if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) click_cell(cell_at_mouse());
  if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) right_click_cell(cell_at_mouse());
}

void ui_draw(void) {
  draw_hud();
  if (panel_open) draw_panel();
  if (chat_open) draw_chat();
}
