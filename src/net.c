// Multiplayer protocol layer — binary v2 (see proto.h; server: craft_js/server.js).
// HELLO-first handshake, 20 Hz positions with velocity, snapshot interpolation
// for remotes, app-level heartbeat, auto-reconnect with backoff.
#include "net.h"
#include "proto.h"
#include "world.h"
#include "mesh.h"
#include "physics.h"
#include "inventory.h"
#include "fluids.h"
#include "entities.h"
#include "state.h"
#include "ui.h"
#include "raylib.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

RemotePlayer remote_players[MAX_REMOTE];

static char srv_host[128];
static int  srv_port = 8080;
static bool want_net = false;      // net_connect was called at least once
static bool rejected = false;      // server refused our protocol version
static uint32_t my_id = 0;
static bool have_id = false;
static bool hello_sent = false;
static bool restored_once = false; // ignore RESTORE after reconnects (rollback guard)

static float pos_accum = 0, save_accum = 0, ping_accum = 0, mobs_accum = 0;
static int last_held_sent = -1;    // resend on change; -1 = never sent
static bool am_master = true;      // SV_MASTER; offline defaults to authority
static double last_rx = 0;         // GetTime() of last received message
static double next_attempt = 0;    // reconnect schedule
static double backoff = 1.0;
static double rtt_ms = -1;

// Persistent player uid: file on native, localStorage via EM_ASM on wasm.
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
static void get_uid(char *out, int n) {
  EM_ASM({
    // 'kocka-uid' is the pre-rename key: migrate it so old players keep their saved state
    let uid = localStorage.getItem('craft-uid') || localStorage.getItem('kocka-uid');
    if (!uid) uid = 'c-' + Math.random().toString(36).slice(2, 12);
    localStorage.setItem('craft-uid', uid);
    stringToUTF8(uid, $0, $1);
  }, out, n);
}
#else
#include <time.h>
static void get_uid(char *out, int n) {
  FILE *f = fopen("uid.txt", "r");
  if (f) { if (fgets(out, n, f)) { out[strcspn(out, "\r\n")] = 0; fclose(f); return; } fclose(f); }
  snprintf(out, n, "c-%08x%04x", (unsigned)time(NULL), (unsigned)(rand() & 0xffff));
  f = fopen("uid.txt", "w");
  if (f) { fputs(out, f); fclose(f); }
}
#endif

void net_connect(const char *host, int port) {
  snprintf(srv_host, sizeof srv_host, "%s", host);
  srv_port = port;
  want_net = true;
  next_attempt = 0;                // try immediately in the first net_update
}

bool net_active(void) { return ws_is_open() && have_id; }
double net_rtt_ms(void) { return rtt_ms; }

void net_send_settime(double v) {
  if (!ws_is_open()) return;
  uint8_t b[8];
  ws_send_bin(b, proto_enc_settime(b, (float)v));
}

void net_send_chat(const char *msg) {
  if (!ws_is_open()) return;
  uint8_t b[124];
  ws_send_bin(b, proto_enc_chat(b, msg));
}

void net_send_boom(int x, int y, int z) {
  if (!ws_is_open()) return;
  uint8_t b[12];
  ws_send_bin(b, proto_enc_boom(b, x, (uint8_t)y, z));
}

void net_send_mob_hit(uint8_t slot, uint8_t dmg, float kx, float kz) {
  if (!ws_is_open()) return;
  uint8_t b[12];
  ws_send_bin(b, proto_enc_mob_hit(b, slot, dmg, kx, kz));
}

void net_send_chest_get(int x, int y, int z) {
  if (!ws_is_open()) return;
  uint8_t b[12];
  ws_send_bin(b, proto_enc_chest_req(b, CL_CHEST_GET, x, (uint8_t)y, z));
}

void net_send_chest_break(int x, int y, int z) {
  if (!ws_is_open()) return;
  uint8_t b[12];
  ws_send_bin(b, proto_enc_chest_req(b, CL_CHEST_BREAK, x, (uint8_t)y, z));
}

void net_send_chest_set(int x, int y, int z) {
  if (!ws_is_open()) return;
  Stack *arr = chest_array_peek(x, y, z);
  if (!arr) return;
  PSlot s[P_CHEST_SLOTS];
  for (int i = 0; i < P_CHEST_SLOTS && i < CHEST_SIZE; i++) {
    s[i].id = (uint16_t)arr[i].id;
    s[i].count = (uint16_t)arr[i].count;
  }
  uint8_t b[P_CHEST_SET_SIZE];
  ws_send_bin(b, proto_enc_chest_set(b, x, (uint8_t)y, z, s));
}

void net_send_edit(int x, int y, int z) {
  if (!ws_is_open()) return;
  PEdit e = { x, z, (uint8_t)y, get_voxel(x, y, z), raw_water(x, y, z) };
  uint8_t b[16];
  ws_send_bin(b, proto_enc_edit(b, &e));
}

void net_save_state(void) {
  if (!ws_is_open()) return;
  PState s = {0};
  s.x = (float)player.x; s.y = (float)player.y; s.z = (float)player.z; s.yaw = player.yaw;
  s.health = (uint8_t)player.health; s.hunger = (uint8_t)player.hunger; s.sel_slot = (uint8_t)sel_slot;
  for (int i = 0; i < P_INV_SIZE && i < INV_SIZE; i++) {
    s.slots[i].id = (uint16_t)slots[i].id;
    s.slots[i].count = (uint16_t)slots[i].count;
  }
  uint8_t b[P_STATE_SIZE];
  ws_send_bin(b, proto_enc_save(b, &s));
}

static void send_hello(void) {
  char uid[64]; get_uid(uid, sizeof uid);
  uint8_t b[70];
  ws_send_bin(b, proto_enc_hello(b, uid));
  hello_sent = true;
}

static void send_pos(void) {
  PPos p = { (float)player.x, (float)player.y, (float)player.z,
             player.yaw, player.pitch, player.vx, player.vy, player.vz };
  uint8_t b[P_POS_SIZE];
  ws_send_bin(b, proto_enc_pos(b, &p));
}

static RemotePlayer *remote_slot(uint32_t id) {
  for (int i = 0; i < MAX_REMOTE; i++)
    if (remote_players[i].used && remote_players[i].id == id) return &remote_players[i];
  for (int i = 0; i < MAX_REMOTE; i++)
    if (!remote_players[i].used) {
      memset(&remote_players[i], 0, sizeof(RemotePlayer));
      remote_players[i].used = true;
      remote_players[i].id = id;
      return &remote_players[i];
    }
  return NULL;
}

static void clear_remotes(void) {
  for (int i = 0; i < MAX_REMOTE; i++) remote_players[i].used = false;
}

// applyRemoteEdit — same world mutation as before, protocol-agnostic.
// No-change guard: explosion craters arrive as BOOM (already applied locally)
// plus per-block edits for the server log — skip the redundant remeshes.
static void apply_edit(const PEdit *e) {
  if (get_voxel(e->x, e->y, e->z) == e->block && raw_water(e->x, e->y, e->z) == e->water) return;
  set_voxel(e->x, e->y, e->z, e->block);
  set_water_raw(e->x, e->y, e->z, e->water);
  remesh_around(e->x, e->z);
  touch_water(e->x, e->y, e->z);
  check_fall(e->x, e->y + 1, e->z);
}

// join replay: apply everything, then remesh each dirty chunk once
static void apply_edits(const uint8_t *buf, int count) {
  enum { MAXD = 4096 };
  static int64_t dirty[MAXD]; int nd = 0;
  PEdit e;
  for (int k = 0; k < count; k++) {
    proto_edits_record(buf, k, &e);
    set_voxel(e.x, e.y, e.z, e.block);
    set_water_raw(e.x, e.y, e.z, e.water);
    touch_water(e.x, e.y, e.z);
    int cx = chunk_of(e.x), cz = chunk_of(e.z), lx = e.x - cx * CHUNK, lz = e.z - cz * CHUNK;
    int list_c[5][2] = {{cx,cz}}; int n = 1;
    if (lx == 0)         { list_c[n][0] = cx-1; list_c[n][1] = cz; n++; }
    if (lx == CHUNK - 1) { list_c[n][0] = cx+1; list_c[n][1] = cz; n++; }
    if (lz == 0)         { list_c[n][0] = cx; list_c[n][1] = cz-1; n++; }
    if (lz == CHUNK - 1) { list_c[n][0] = cx; list_c[n][1] = cz+1; n++; }
    for (int i = 0; i < n; i++) {
      int64_t key = ((int64_t)list_c[i][0] << 32) | (uint32_t)list_c[i][1];
      bool seen = false;
      for (int k2 = 0; k2 < nd; k2++) if (dirty[k2] == key) { seen = true; break; }
      if (!seen && nd < MAXD) dirty[nd++] = key;
    }
  }
  for (int k = 0; k < nd; k++) {
    int cx = (int)(dirty[k] >> 32), cz = (int)(int32_t)(uint32_t)dirty[k];
    Chunk *c = chunk_get(cx, cz);
    if (c && c->meshed) mesh_chunk(cx, cz);
  }
}

static void apply_restore(const PState *s) {
  for (int i = 0; i < P_INV_SIZE && i < INV_SIZE; i++) {
    slots[i].id = s->slots[i].id;
    slots[i].count = s->slots[i].count;
  }
  if (s->x != 0 || s->y != 0 || s->z != 0) {
    player.x = s->x; player.y = s->y; player.z = s->z;
    player.vx = player.vy = player.vz = 0;
  }
  player.yaw = s->yaw;
  player.health = s->health;
  player.hunger = s->hunger;
  sel_slot = s->sel_slot < HOTBAR_SIZE ? s->sel_slot : 0;
}

static void dispatch(const uint8_t *b, size_t n) {
  if (n < 1) return;
  switch (b[0]) {
    case SV_WELCOME: {
      uint8_t ver; uint32_t id; float t;
      if (!proto_dec_welcome(b, n, &ver, &id, &t)) return;
      if (ver != PROTO_VER) { rejected = true; ws_close(); return; }
      my_id = id; have_id = true;
      day_time = t;
      backoff = 1.0;               // healthy connection: reset reconnect backoff
      break;
    }
    case SV_REJECT:
      rejected = true;             // wrong protocol: stop trying
      TraceLog(LOG_WARNING, "NET: server rejected protocol version (wants v%d)", n >= 2 ? b[1] : -1);
      ws_close();
      break;
    case SV_TIME: {
      float t;
      if (proto_dec_time(b, n, &t)) day_time = t;
      break;
    }
    case SV_POS: {
      uint32_t id; PPos p;
      if (!proto_dec_sv_pos(b, n, &id, &p) || id == my_id) return;
      RemotePlayer *r = remote_slot(id);
      if (!r) return;
      Snap s = { GetTime(), p.x, p.y, p.z, p.yaw, p.pitch, p.vx, p.vy, p.vz };
      interp_push(&r->ring, &s);
      break;
    }
    case SV_EDIT: {
      uint32_t id; PEdit e;
      if (proto_dec_sv_edit(b, n, &id, &e) && id != my_id) apply_edit(&e);
      break;
    }
    case SV_EDITS: {
      int count = proto_dec_edits_count(b, n);
      if (count > 0) apply_edits(b, count);
      break;
    }
    case SV_RESTORE: {
      PState s;
      if (!restored_once && proto_dec_state(b, n, &s)) {
        apply_restore(&s);
        restored_once = true;
      }
      break;
    }
    case SV_LEAVE: {
      uint32_t id;
      if (proto_dec_leave(b, n, &id))
        for (int i = 0; i < MAX_REMOTE; i++)
          if (remote_players[i].used && remote_players[i].id == id) remote_players[i].used = false;
      break;
    }
    case SV_PONG: {
      double t0;
      if (proto_dec_pong(b, n, &t0)) rtt_ms = GetTime() * 1000.0 - t0;
      break;
    }
    case SV_HELD: {
      uint32_t id; uint16_t item;
      if (!proto_dec_sv_held(b, n, &id, &item) || id == my_id) return;
      RemotePlayer *r = remote_slot(id);
      if (r) r->held = item;
      break;
    }
    case SV_CHAT: {
      uint32_t id; char msg[124];
      if (proto_dec_sv_chat(b, n, &id, msg, sizeof msg) && id != my_id)
        ui_chat_log(TextFormat("<P%u> %s", id, msg));
      break;
    }
    case SV_MASTER: {
      bool m;
      if (proto_dec_master(b, n, &m)) { am_master = m; entities_set_authority(m); }
      break;
    }
    case SV_MOBS: {
      static PMob pm[P_MOBS_MAX]; static PArrow pa[P_ARROWS_MAX];
      uint32_t id; int nm, na;
      if (!am_master && proto_dec_sv_mobs(b, n, &id, pm, &nm, pa, &na))
        mobs_import(pm, nm, pa, na);
      break;
    }
    case SV_BOOM: {
      uint32_t id; int32_t x, z; uint8_t y;
      if (proto_dec_sv_boom(b, n, &id, &x, &y, &z) && id != my_id)
        explode_remote(x, y, z);
      break;
    }
    case SV_MOB_HIT: {
      uint32_t id; uint8_t slot, dmg; float kx, kz;
      if (am_master && proto_dec_sv_mob_hit(b, n, &id, &slot, &dmg, &kx, &kz))
        mob_apply_hit(slot, dmg, kx, kz);
      break;
    }
    case SV_CHEST: {
      int32_t x, z; uint8_t y, flags;
      PSlot s[P_CHEST_SLOTS];
      if (!proto_dec_sv_chest(b, n, &x, &y, &z, &flags, s)) return;
      if (flags & 1) {                 // broken: spill the authoritative contents
        for (int i = 0; i < P_CHEST_SLOTS; i++)
          if (s[i].id) spawn_drop(s[i].id, s[i].count, x + 0.5, y, z + 0.5, 0.5f);
        chest_delete(x, y, z);
      } else {                         // fetched/updated: refresh the local cache
        Stack *arr = chest_array_at(x, y, z);
        for (int i = 0; i < P_CHEST_SLOTS && i < CHEST_SIZE; i++) {
          arr[i].id = s[i].id;
          arr[i].count = s[i].count;
        }
      }
      break;
    }
  }
}

static void on_disconnect(void) {
  clear_remotes();
  have_id = false;
  hello_sent = false;
  last_held_sent = -1;
  am_master = true;
  entities_set_authority(true);      // solo again: simulate our own mobs
  next_attempt = GetTime() + backoff;
  backoff = backoff * 2.0 > 10.0 ? 10.0 : backoff * 2.0;
}

void net_update(float dt) {
  if (!want_net || rejected) return;
  double now = GetTime();

  // --- reconnect state machine ---
  if (!ws_is_open()) {
    if (now >= next_attempt) {
      // Connects are asynchronous on BOTH transports (wasm: browser socket;
      // native: non-blocking connect driven by ws_pump). ws_connect returns
      // immediately and keeps a pending attempt alive, so schedule the next
      // attempt regardless — parallel CONNECTING sockets never stack.
      next_attempt = now + backoff;
      backoff = backoff * 2.0 > 10.0 ? 10.0 : backoff * 2.0;
      if (ws_connect(srv_host, srv_port, "/")) last_rx = now;
    }
    ws_pump();                       // drive the native connect/handshake states
    if (!ws_is_open()) return;
  }

  if (!hello_sent) send_hello();   // socket just opened (covers async wasm open)

  ws_pump();
  size_t len;
  const uint8_t *msg;
  while ((msg = ws_poll(&len)) != NULL) { last_rx = now; dispatch(msg, len); }

  if (!ws_is_open()) { on_disconnect(); return; }   // dispatch may have closed (REJECT)

  // --- liveness: no traffic for 6 s -> assume dead, force reconnect ---
  if (now - last_rx > 6.0) {
    TraceLog(LOG_WARNING, "NET: connection silent for 6 s, reconnecting");
    ws_close();
    on_disconnect();
    return;
  }

  // --- periodic sends ---
  ping_accum += dt;
  bool ping_now = ping_accum >= 2.0f;
  if (ping_now) { ping_accum = 0; uint8_t b[9]; ws_send_bin(b, proto_enc_ping(b, now * 1000.0)); }
  if (have_id) {
    pos_accum += dt;
    if (pos_accum >= 0.05f) { pos_accum = 0; send_pos(); }   // 20 Hz
    save_accum += dt;
    if (save_accum >= 5.0f) { save_accum = 0; net_save_state(); }
    // held item: on change immediately, plus with every ping so late
    // joiners converge within 2 s (the server keeps no held state)
    int held = slots[sel_slot].id;
    if (held != last_held_sent || ping_now) {
      last_held_sent = held;
      uint8_t b[4];
      ws_send_bin(b, proto_enc_held(b, (uint16_t)held));
    }
    // mob authority: stream full snapshots at 10 Hz
    if (am_master) {
      mobs_accum += dt;
      if (mobs_accum >= 0.1f) {
        mobs_accum = 0;
        static PMob pm[P_MOBS_MAX]; static PArrow pa[P_ARROWS_MAX];
        int na;
        int nm = mobs_export(pm, pa, &na);
        static uint8_t mb[3 + P_MOBS_MAX * P_MOB_REC + P_ARROWS_MAX * P_ARROW_REC];
        ws_send_bin(mb, proto_enc_mobs(mb, pm, nm, pa, na));
      }
    }
  }

  // --- interpolate remotes into their render pose ---
  for (int i = 0; i < MAX_REMOTE; i++) {
    RemotePlayer *r = &remote_players[i];
    if (!r->used) continue;
    Snap out;
    if (interp_sample(&r->ring, now, &out)) {
      r->x = out.x; r->y = out.y; r->z = out.z;
      r->yaw = out.yaw; r->pitch = out.pitch;
    }
  }
}
