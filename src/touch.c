// Mobile touch controls (Minecraft PE style). Zones and roles:
//   - a touch starting on a button drives that button while held
//   - a touch starting in the lower-left quarter is a floating joystick: move
//   - any other touch drags the camera (yaw/pitch)
//   - tapping a hotbar slot selects it
// Panel/chest clicks need no code here — raylib maps the first touch to a
// virtual mouse, and the inventory UI listens to mouse events. For the same
// reason main.c must NOT run its desktop mouse-capture/mining path while in
// touch mode, or every look-drag would also mine.
#include "touch.h"
#include "interact.h"
#include "entities.h"
#include "inventory.h"
#include "ui.h"
#include "raylib.h"
#include <math.h>
#include <stdint.h>

Texture2D entities_items_tex(void);   // items.png (pickaxe icon)

static bool active = false;

// touch roles by touch id (-1 = unassigned)
static int move_id = -1, look_id = -1, mine_id = -1, jump_id = -1;
static Vector2 move_origin, move_cur, look_prev;

#define JOY_R 85.0f
#define BTN_R 44.0f
#define LOOK_SENS 0.0045f

typedef struct Btn { float x, y; } Btn;
static Btn btn_jump, btn_mine, btn_place, btn_bag;

static void layout(void) {
  float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();
  btn_jump  = (Btn){ W - 80,  H - 100 };
  btn_mine  = (Btn){ W - 80,  H - 230 };
  btn_place = (Btn){ W - 195, H - 150 };
  btn_bag   = (Btn){ W - 60,  60 };
}

static bool in_btn(Vector2 p, Btn b) {
  float dx = p.x - b.x, dy = p.y - b.y;
  return dx * dx + dy * dy <= BTN_R * BTN_R;
}

// mirror of draw_hud's hotbar layout (CELL 52, GAP 6, bottom-centered)
static int hotbar_slot_at(Vector2 p) {
  const float CELL = 52, GAP = 6;
  float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();
  float bar_w = HOTBAR_SIZE * CELL + (HOTBAR_SIZE - 1) * GAP;
  float bx = (W - bar_w) / 2.0f, by = H - CELL - 12.0f;
  if (p.y < by || p.y >= by + CELL) return -1;
  for (int i = 0; i < HOTBAR_SIZE; i++) {
    float cx = bx + i * (CELL + GAP);
    if (p.x >= cx && p.x < cx + CELL) return i;
  }
  return -1;
}

bool touch_mode(void) { return active; }

void touch_update(Input *in, bool ui_blocked) {
  int n = GetTouchPointCount();
  if (n > 0) active = true;
  if (!active) return;
  layout();

  // which of last frame's role ids are still down?
  bool seen_move = false, seen_look = false, seen_mine = false, seen_jump = false;

  for (int i = 0; i < n; i++) {
    int id = GetTouchPointId(i);
    Vector2 p = GetTouchPosition(i);

    if (id == move_id)      { seen_move = true; move_cur = p; continue; }
    if (id == mine_id)      { seen_mine = true; continue; }
    if (id == jump_id)      { seen_jump = true; continue; }
    if (id == look_id) {
      seen_look = true;
      if (!ui_blocked) {
        player.yaw   -= (p.x - look_prev.x) * LOOK_SENS;
        player.pitch -= (p.y - look_prev.y) * LOOK_SENS;
        if (player.pitch >  1.55f) player.pitch =  1.55f;
        if (player.pitch < -1.55f) player.pitch = -1.55f;
      }
      look_prev = p;
      continue;
    }

    // new touch: assign a role. Buttons win over zones.
    if (in_btn(p, btn_bag)) { ui_toggle_inventory(); continue; }
    if (ui_blocked) continue;              // panel/chat open: only the bag button
    if (in_btn(p, btn_jump))  { jump_id = id; seen_jump = true; continue; }
    if (in_btn(p, btn_mine))  { mine_id = id; seen_mine = true; attack_creature(); continue; }
    if (in_btn(p, btn_place)) { use_right_click(); continue; }
    int slot = hotbar_slot_at(p);
    if (slot >= 0) { sel_slot = slot; continue; }
    float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();
    if (move_id < 0 && p.x < W * 0.45f && p.y > H * 0.35f) {
      move_id = id; move_origin = p; move_cur = p; seen_move = true;
    } else if (look_id < 0) {
      look_id = id; look_prev = p; seen_look = true;
    }
  }

  if (!seen_move) move_id = -1;
  if (!seen_look) look_id = -1;
  if (!seen_mine) mine_id = -1;
  if (!seen_jump) jump_id = -1;

  if (ui_blocked) { set_mining(false); return; }

  // joystick vector -> the same 4 digital directions the keyboard feeds;
  // pushing the stick near/past its rim sprints
  if (move_id >= 0) {
    float dx = move_cur.x - move_origin.x, dy = move_cur.y - move_origin.y;
    float dead = JOY_R * 0.25f;
    if (dy < -dead) in->fwd = true;
    if (dy >  dead) in->back = true;
    if (dx < -dead) in->left = true;
    if (dx >  dead) in->right = true;
    if (dx * dx + dy * dy >= JOY_R * JOY_R * 0.85f * 0.85f) in->sprint = true;
  }
  if (jump_id >= 0) in->jump = true;
  set_mining(mine_id >= 0);
}

static void draw_btn(Btn b, bool held) {
  DrawCircleV((Vector2){ b.x, b.y }, BTN_R, (Color){ 255, 255, 255, held ? 90 : 40 });
  DrawCircleLinesV((Vector2){ b.x, b.y }, BTN_R, (Color){ 255, 255, 255, 140 });
}

static void draw_item_icon_at(int id, float x, float y, float size) {
  Texture2D items = entities_items_tex();
  int tile = item_tile(id);
  if (tile >= 0 && items.id != 0) {
    Rectangle src = { (tile % 4) * 64.0f, (tile / 4) * 64.0f, 64, 64 };
    DrawTexturePro(items, src, (Rectangle){ x - size / 2, y - size / 2, size, size },
                   (Vector2){ 0, 0 }, 0, WHITE);
  } else {
    uint8_t rgb[3]; item_color(id, rgb);
    DrawRectangleRounded((Rectangle){ x - size * 0.35f, y - size * 0.35f, size * 0.7f, size * 0.7f },
                         0.25f, 4, (Color){ rgb[0], rgb[1], rgb[2], 220 });
  }
}

void touch_draw(void) {
  if (!active) return;
  layout();
  bool blocked = ui_panel_open() || ui_chat_open();

  // bag: 2x2 grid glyph, always available
  draw_btn(btn_bag, false);
  for (int gy = 0; gy < 2; gy++)
    for (int gx = 0; gx < 2; gx++)
      DrawRectangle((int)(btn_bag.x - 14 + gx * 16), (int)(btn_bag.y - 14 + gy * 16), 12, 12,
                    (Color){ 255, 255, 255, 170 });
  if (blocked) return;

  // joystick (only while touched; it floats where the thumb lands)
  if (move_id >= 0) {
    DrawCircleV(move_origin, JOY_R, (Color){ 255, 255, 255, 30 });
    DrawCircleLinesV(move_origin, JOY_R, (Color){ 255, 255, 255, 120 });
    float dx = move_cur.x - move_origin.x, dy = move_cur.y - move_origin.y;
    float d = sqrtf(dx * dx + dy * dy);
    if (d > JOY_R) { dx = dx / d * JOY_R; dy = dy / d * JOY_R; }
    DrawCircleV((Vector2){ move_origin.x + dx, move_origin.y + dy }, 34, (Color){ 255, 255, 255, 110 });
  } else {
    float H = (float)GetScreenHeight();
    DrawCircleLinesV((Vector2){ 130, H - 170 }, JOY_R, (Color){ 255, 255, 255, 60 });
  }

  draw_btn(btn_jump, jump_id >= 0);
  Vector2 j = { btn_jump.x, btn_jump.y };
  DrawTriangle((Vector2){ j.x, j.y - 18 }, (Vector2){ j.x - 16, j.y + 12 }, (Vector2){ j.x + 16, j.y + 12 },
               (Color){ 255, 255, 255, 190 });

  draw_btn(btn_mine, mine_id >= 0);
  draw_item_icon_at(I_PICKAXE, btn_mine.x, btn_mine.y, 52);

  draw_btn(btn_place, false);
  if (slots[sel_slot].id) draw_item_icon_at(slots[sel_slot].id, btn_place.x, btn_place.y, 52);
  else DrawText("+", (int)btn_place.x - 6, (int)btn_place.y - 12, 24, (Color){ 255, 255, 255, 190 });
}
