// Embedded game server — C port of the retired server.js, byte-identical on
// the wire and on disk (golden fixtures in server_selftest + net_test).
//
// A very simple "mailman": whatever one player does (block edits, movement,
// chat, held item) is forwarded to the other players. The world itself is
// NOT stored — every client regenerates the deterministic terrain; only the
// edit log, player states and the shared clock persist:
//   world.edits      binary edit log ("KEDT" v1: magic, u8 ver, u32le count,
//                    count x 11-byte records — same layout as SV_EDITS)
//   players.json     per-uid states (JSON on purpose: hand-editable)
//   world.meta.json  time of day
//
// No raylib in here: `craft.exe --server` must run on a headless box, so the
// module keeps its own clock and never touches a window.
#include "server.h"
#include "proto.h"
#include "inventory.h"     // FurnaceState + furnace_advance (raylib-free) — shared smelting engine
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET sock_t;
#define CLOSESOCK closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
typedef int sock_t;
#define INVALID_SOCKET (-1)
#define CLOSESOCK close
#endif

#define DAY_LEN 300.0
#define HEARTBEAT_S 10.0
#define SAVE_EVERY_S 10.0
#define JOIN_BATCH 16384

#ifndef CRAFT_VERSION
#define CRAFT_VERSION "dev"
#endif

// ============================== small utils ==============================
static double now_s(void) {
#ifdef _WIN32
  static LARGE_INTEGER freq; static int init = 0;
  LARGE_INTEGER t;
  if (!init) { QueryPerformanceFrequency(&freq); init = 1; }
  QueryPerformanceCounter(&t);
  return (double)t.QuadPart / (double)freq.QuadPart;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + ts.tv_nsec / 1e9;
#endif
}

void server_sleep_ms(int ms) {
#ifdef _WIN32
  Sleep(ms);
#else
  struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
  nanosleep(&ts, NULL);
#endif
}

static void set_nonblocking(sock_t s) {
#ifdef _WIN32
  u_long on = 1; ioctlsocket(s, FIONBIO, &on);
#else
  fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

static bool wouldblock(void) {
#ifdef _WIN32
  return WSAGetLastError() == WSAEWOULDBLOCK;
#else
  return errno == EWOULDBLOCK || errno == EAGAIN;
#endif
}

// --- SHA-1 (public-domain style compact implementation, RFC 3174) ---
typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; int n; } Sha1;
static uint32_t rol(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }
static void sha1_block(Sha1 *s, const uint8_t *p) {
  uint32_t w[80];
  for (int i = 0; i < 16; i++) w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) | ((uint32_t)p[i*4+2] << 8) | p[i*4+3];
  for (int i = 16; i < 80; i++) w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
  uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20)      { f = (b & c) | ((~b) & d);        k = 0x5A827999; }
    else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
    else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
    else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
    uint32_t t = rol(a, 5) + f + e + k + w[i];
    e = d; d = c; c = rol(b, 30); b = a; a = t;
  }
  s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}
static void sha1_init(Sha1 *s) {
  s->h[0] = 0x67452301; s->h[1] = 0xEFCDAB89; s->h[2] = 0x98BADCFE;
  s->h[3] = 0x10325476; s->h[4] = 0xC3D2E1F0; s->len = 0; s->n = 0;
}
static void sha1_update(Sha1 *s, const void *data, size_t n) {
  const uint8_t *p = data;
  s->len += n;
  while (n > 0) {
    size_t take = 64 - (size_t)s->n; if (take > n) take = n;
    memcpy(s->buf + s->n, p, take); s->n += (int)take; p += take; n -= take;
    if (s->n == 64) { sha1_block(s, s->buf); s->n = 0; }
  }
}
static void sha1_final(Sha1 *s, uint8_t out[20]) {
  uint64_t bits = s->len * 8;
  uint8_t pad = 0x80; sha1_update(s, &pad, 1);
  uint8_t z = 0;
  while (s->n != 56) sha1_update(s, &z, 1);
  uint8_t lb[8]; for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
  sha1_update(s, lb, 8);
  for (int i = 0; i < 5; i++) {
    out[i*4] = (uint8_t)(s->h[i] >> 24); out[i*4+1] = (uint8_t)(s->h[i] >> 16);
    out[i*4+2] = (uint8_t)(s->h[i] >> 8); out[i*4+3] = (uint8_t)s->h[i];
  }
}
static void b64_encode(const uint8_t *in, size_t n, char *out) {
  static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16;
    if (i + 1 < n) v |= (uint32_t)in[i+1] << 8;
    if (i + 2 < n) v |= in[i+2];
    out[o++] = T[(v >> 18) & 63];
    out[o++] = T[(v >> 12) & 63];
    out[o++] = i + 1 < n ? T[(v >> 6) & 63] : '=';
    out[o++] = i + 2 < n ? T[v & 63] : '=';
  }
  out[o] = 0;
}

static void wr_u16le(uint8_t *b, uint16_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static void wr_u32le(uint8_t *b, uint32_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24); }
static void wr_i32le(uint8_t *b, int32_t v)  { wr_u32le(b, (uint32_t)v); }
static void wr_f32le(uint8_t *b, float v)    { uint32_t u; memcpy(&u, &v, 4); wr_u32le(b, u); }
static uint16_t rd_u16le(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }
static uint32_t rd_u32le(const uint8_t *b) { return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24); }
static int32_t  rd_i32le(const uint8_t *b) { return (int32_t)rd_u32le(b); }
static float    rd_f32le(const uint8_t *b) { uint32_t u = rd_u32le(b); float v; memcpy(&v, &u, 4); return v; }

// ============================== state ==============================
typedef struct SClient {
  bool used, ws;                 // ws = websocket upgrade completed
  sock_t sock;
  uint32_t id;
  char uid[64];
  double last_seen;
  uint8_t *rbuf; size_t rlen, rcap;
} SClient;
#define MAX_CLIENTS 64
static SClient clients[MAX_CLIENTS];
static uint32_t next_id = 1;

typedef struct EditRec { int32_t x, z; uint8_t y, b, w; bool used; } EditRec;
#define EDIT_CAP (1 << 18)             // 262144 edits, plenty
static EditRec *edits = NULL;
static int n_edits = 0;

typedef struct SavedPlayer { char uid[64]; PState st; bool used; } SavedPlayer;
#define MAX_SAVED 256
static SavedPlayer saved[MAX_SAVED];

// chest contents keyed by block position ("block entity" data)
typedef struct SChest { int32_t x, z; uint8_t y; PSlot slots[P_CHEST_SLOTS]; bool used; } SChest;
#define MAX_SCHESTS 1024
static SChest schests[MAX_SCHESTS];

static SClient *mob_master = NULL;   // oldest ws client simulates the mobs
static sock_t listener = INVALID_SOCKET;
static bool running = false;
static bool dirty = false;
static char static_dir[260], data_dir[260];
static double time_base = 0.12, time_base_at = 0;
static double last_tick = 0;           // save/time timer (10 s)
static double last_sweep = 0;          // heartbeat sweep (5 s, like server.js)
static double last_furn = 0;           // furnace advance/broadcast (1 s, only while players online)

static double server_time(void) {
  double v = time_base + (now_s() - time_base_at) / DAY_LEN;
  return v - (double)(int64_t)v + ((v < 0) ? 1.0 : 0.0);
}
static void set_server_time(double v) {
  v = v - (double)(int64_t)v; if (v < 0) v += 1.0;
  time_base = v; time_base_at = now_s();
}

// ============================== edit log ==============================
static uint32_t edit_hash(int32_t x, uint8_t y, int32_t z) {
  uint64_t k = ((uint64_t)(uint32_t)x << 40) ^ ((uint64_t)y << 32) ^ (uint32_t)z;
  k ^= k >> 33; k *= 0xff51afd7ed558ccdULL; k ^= k >> 33;
  return (uint32_t)k & (EDIT_CAP - 1);
}
static void edit_put(int32_t x, uint8_t y, int32_t z, uint8_t b, uint8_t w) {
  uint32_t i = edit_hash(x, y, z);
  for (uint32_t n = 0; n < EDIT_CAP; n++, i = (i + 1) & (EDIT_CAP - 1)) {
    if (!edits[i].used) {
      if (n_edits >= EDIT_CAP - 1) { fprintf(stderr, "edit log full, edit dropped\n"); return; }
      edits[i] = (EditRec){ x, z, y, b, w, true }; n_edits++; return;
    }
    if (edits[i].x == x && edits[i].y == y && edits[i].z == z) { edits[i].b = b; edits[i].w = w; return; }
  }
}
static void edits_clear(void) { memset(edits, 0, sizeof(EditRec) * EDIT_CAP); n_edits = 0; }

static SChest *schest_find(int32_t x, uint8_t y, int32_t z, bool create) {
  for (int i = 0; i < MAX_SCHESTS; i++)
    if (schests[i].used && schests[i].x == x && schests[i].y == y && schests[i].z == z) return &schests[i];
  if (!create) return NULL;
  for (int i = 0; i < MAX_SCHESTS; i++)
    if (!schests[i].used) {
      memset(&schests[i], 0, sizeof schests[i]);
      schests[i].used = true;
      schests[i].x = x; schests[i].y = y; schests[i].z = z;
      return &schests[i];
    }
  return NULL;
}

// furnace block-entities: server-authoritative, advanced from now_s() (monotonic),
// so they smelt while closed and catch up any elapsed time on the next touch/tick.
typedef struct SFurnace { int32_t x, z; uint8_t y; FurnaceState st; bool used; } SFurnace;
#define MAX_SFURN 1024
static SFurnace sfurnaces[MAX_SFURN];
static SFurnace *sfurn_find(int32_t x, uint8_t y, int32_t z, bool create) {
  for (int i = 0; i < MAX_SFURN; i++)
    if (sfurnaces[i].used && sfurnaces[i].x == x && sfurnaces[i].y == y && sfurnaces[i].z == z) return &sfurnaces[i];
  if (!create) return NULL;
  for (int i = 0; i < MAX_SFURN; i++)
    if (!sfurnaces[i].used) {
      memset(&sfurnaces[i], 0, sizeof sfurnaces[i]);
      sfurnaces[i].used = true;
      sfurnaces[i].x = x; sfurnaces[i].y = y; sfurnaces[i].z = z;
      sfurnaces[i].st.last_t = now_s();
      return &sfurnaces[i];
    }
  return NULL;
}

// ============================== persistence ==============================
#define EDITS_MAGIC "KEDT"
#define EDITS_VER 1
#define CHESTS_MAGIC "KCHS"
#define CHESTS_VER 1
#define SCHEST_REC (9 + P_CHEST_SLOTS * 4)   // i32 x, i32 z, u8 y, 27 x (u16,u16)
#define FURN_MAGIC "KFRN"
#define FURN_VER 1
#define SFURN_REC (9 + 3 * 4 + 12)           // pos + 3 slots + cook,burn,burn_max (f32); last_t not persisted

static size_t encode_chests_file(uint8_t **out) {
  int count = 0;
  for (int i = 0; i < MAX_SCHESTS; i++) if (schests[i].used) count++;
  size_t sz = 9 + (size_t)count * SCHEST_REC;
  uint8_t *b = malloc(sz);
  memcpy(b, CHESTS_MAGIC, 4); b[4] = CHESTS_VER; wr_u32le(b + 5, (uint32_t)count);
  size_t o = 9;
  for (int i = 0; i < MAX_SCHESTS; i++) {
    if (!schests[i].used) continue;
    wr_i32le(b + o, schests[i].x); wr_i32le(b + o + 4, schests[i].z); b[o + 8] = schests[i].y;
    for (int k = 0; k < P_CHEST_SLOTS; k++) {
      wr_u16le(b + o + 9 + k * 4, schests[i].slots[k].id);
      wr_u16le(b + o + 11 + k * 4, schests[i].slots[k].count);
    }
    o += SCHEST_REC;
  }
  *out = b;
  return sz;
}

static bool decode_chests_file(const uint8_t *b, size_t n) {
  if (n < 9 || memcmp(b, CHESTS_MAGIC, 4) != 0 || b[4] != CHESTS_VER) return false;
  uint32_t count = rd_u32le(b + 5);
  if (n < 9 + (size_t)count * SCHEST_REC) return false;
  for (uint32_t i = 0; i < count; i++) {
    const uint8_t *o = b + 9 + (size_t)i * SCHEST_REC;
    SChest *c = schest_find(rd_i32le(o), o[8], rd_i32le(o + 4), true);
    if (!c) break;
    for (int k = 0; k < P_CHEST_SLOTS; k++) {
      c->slots[k].id = rd_u16le(o + 9 + k * 4);
      c->slots[k].count = rd_u16le(o + 11 + k * 4);
    }
  }
  return true;
}

static size_t encode_furnaces_file(uint8_t **out) {
  int count = 0;
  for (int i = 0; i < MAX_SFURN; i++) if (sfurnaces[i].used) count++;
  size_t sz = 9 + (size_t)count * SFURN_REC;
  uint8_t *b = malloc(sz);
  memcpy(b, FURN_MAGIC, 4); b[4] = FURN_VER; wr_u32le(b + 5, (uint32_t)count);
  size_t o = 9;
  for (int i = 0; i < MAX_SFURN; i++) {
    if (!sfurnaces[i].used) continue;
    FurnaceState *s = &sfurnaces[i].st;
    wr_i32le(b + o, sfurnaces[i].x); wr_i32le(b + o + 4, sfurnaces[i].z); b[o + 8] = sfurnaces[i].y;
    const Stack sl[3] = { s->in, s->fuel, s->out };
    for (int k = 0; k < 3; k++) { wr_u16le(b + o + 9 + k * 4, (uint16_t)sl[k].id); wr_u16le(b + o + 11 + k * 4, (uint16_t)sl[k].count); }
    wr_f32le(b + o + 21, s->cook); wr_f32le(b + o + 25, s->burn); wr_f32le(b + o + 29, s->burn_max);
    o += SFURN_REC;
  }
  *out = b;
  return sz;
}
static bool decode_furnaces_file(const uint8_t *b, size_t n) {
  if (n < 9 || memcmp(b, FURN_MAGIC, 4) != 0 || b[4] != FURN_VER) return false;
  uint32_t count = rd_u32le(b + 5);
  if (n < 9 + (size_t)count * SFURN_REC) return false;
  for (uint32_t i = 0; i < count; i++) {
    const uint8_t *o = b + 9 + (size_t)i * SFURN_REC;
    SFurnace *f = sfurn_find(rd_i32le(o), o[8], rd_i32le(o + 4), true);
    if (!f) break;
    f->st.in.id = rd_u16le(o + 9);   f->st.in.count = rd_u16le(o + 11);
    f->st.fuel.id = rd_u16le(o + 13); f->st.fuel.count = rd_u16le(o + 15);
    f->st.out.id = rd_u16le(o + 17);  f->st.out.count = rd_u16le(o + 19);
    f->st.cook = rd_f32le(o + 21); f->st.burn = rd_f32le(o + 25); f->st.burn_max = rd_f32le(o + 29);
    f->st.last_t = now_s();          // resume from saved progress (no downtime catch-up)
  }
  return true;
}

static size_t encode_edits_file(uint8_t **out) {
  size_t sz = 9 + (size_t)n_edits * 11;
  uint8_t *b = malloc(sz);
  memcpy(b, EDITS_MAGIC, 4); b[4] = EDITS_VER; wr_u32le(b + 5, (uint32_t)n_edits);
  size_t k = 0;
  for (uint32_t i = 0; i < EDIT_CAP; i++) {
    if (!edits[i].used) continue;
    uint8_t *o = b + 9 + k * 11;
    wr_i32le(o, edits[i].x); wr_i32le(o + 4, edits[i].z);
    o[8] = edits[i].y; o[9] = edits[i].b; o[10] = edits[i].w;
    k++;
  }
  *out = b;
  return sz;
}
static bool decode_edits_file(const uint8_t *b, size_t n) {
  if (n < 9 || memcmp(b, EDITS_MAGIC, 4) != 0 || b[4] != EDITS_VER) return false;
  uint32_t count = rd_u32le(b + 5);
  if (n < 9 + (size_t)count * 11) return false;
  for (uint32_t k = 0; k < count; k++) {
    const uint8_t *o = b + 9 + (size_t)k * 11;
    edit_put(rd_i32le(o), o[8], rd_i32le(o + 4), o[9], o[10]);
  }
  return true;
}

static void path_join(char *out, size_t n, const char *dir, const char *name) {
  snprintf(out, n, "%s/%s", dir[0] ? dir : ".", name);
}

static bool write_atomic(const char *path, const void *data, size_t n) {
  char tmp[300]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
  FILE *f = fopen(tmp, "wb");
  if (!f) return false;
  bool ok = fwrite(data, 1, n, f) == n;
  fclose(f);
  if (!ok) { remove(tmp); return false; }
  remove(path);                       // Windows rename won't overwrite
  return rename(tmp, path) == 0;
}

// players.json writer — flat known schema, field order matches the JS server
static size_t players_json(char **out) {
  size_t cap = 4096, len = 0;
  char *s = malloc(cap);
  #define EMIT(...) do { \
    char piece[256]; int pn = snprintf(piece, sizeof piece, __VA_ARGS__); \
    if (len + (size_t)pn + 1 > cap) { cap = cap * 2 + (size_t)pn; s = realloc(s, cap); } \
    memcpy(s + len, piece, (size_t)pn); len += (size_t)pn; \
  } while (0)
  EMIT("{");
  bool first = true;
  for (int i = 0; i < MAX_SAVED; i++) {
    if (!saved[i].used) continue;
    PState *st = &saved[i].st;
    EMIT("%s\"%s\":{\"x\":%.9g,\"y\":%.9g,\"z\":%.9g,\"yaw\":%.9g,\"health\":%d,\"hunger\":%d,\"selSlot\":%d,\"slots\":[",
         first ? "" : ",", saved[i].uid, st->x, st->y, st->z, st->yaw, st->health, st->hunger, st->sel_slot);
    first = false;
    for (int k = 0; k < P_INV_SIZE; k++) {
      if (st->slots[k].id) EMIT("%s{\"id\":%d,\"count\":%d}", k ? "," : "", st->slots[k].id, st->slots[k].count);
      else EMIT("%snull", k ? "," : "");
    }
    EMIT("]}");
  }
  EMIT("}");
  #undef EMIT
  s[len] = 0;
  *out = s;
  return len;
}

// tiny JSON reader for the same schema (tolerant of key order / whitespace)
static const char *js_ws(const char *p) { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; return p; }
static const char *js_str(const char *p, char *out, size_t n) {
  p = js_ws(p);
  if (*p != '"') return NULL;
  p++;
  size_t o = 0;
  while (*p && *p != '"') { if (o + 1 < n) out[o++] = *p; p++; }
  if (*p != '"') return NULL;
  out[o] = 0;
  return p + 1;
}
static const char *js_num(const char *p, double *v) {
  p = js_ws(p);
  char *end;
  *v = strtod(p, &end);
  return end == p ? NULL : end;
}
static bool players_parse(const char *p) {
  p = js_ws(p);
  if (*p != '{') return false;
  p = js_ws(p + 1);
  if (*p == '}') return true;
  while (1) {
    char uid[64];
    p = js_str(p, uid, sizeof uid); if (!p) return false;
    p = js_ws(p); if (*p != ':') return false;
    p = js_ws(p + 1); if (*p != '{') return false;
    p++;
    PState st; memset(&st, 0, sizeof st);
    st.health = 20; st.hunger = 20;
    while (1) {
      char key[32];
      p = js_str(p, key, sizeof key); if (!p) return false;
      p = js_ws(p); if (*p != ':') return false;
      p = js_ws(p + 1);
      if (strcmp(key, "slots") == 0) {
        if (*p != '[') return false;
        p = js_ws(p + 1);
        for (int k = 0; *p != ']'; k++) {
          if (*p == 'n') { p = js_ws(p + 4); }                       // null
          else if (*p == '{') {
            p++;
            double id = 0, count = 0;
            while (1) {
              char sk[16];
              p = js_str(p, sk, sizeof sk); if (!p) return false;
              p = js_ws(p); if (*p != ':') return false;
              double v; p = js_num(p + 1, &v); if (!p) return false;
              if (strcmp(sk, "id") == 0) id = v; else if (strcmp(sk, "count") == 0) count = v;
              p = js_ws(p);
              if (*p == ',') { p = js_ws(p + 1); continue; }
              break;
            }
            if (*p != '}') return false;
            p = js_ws(p + 1);
            if (k < P_INV_SIZE) { st.slots[k].id = (uint16_t)id; st.slots[k].count = (uint16_t)count; }
          } else return false;
          if (*p == ',') p = js_ws(p + 1);
        }
        p = js_ws(p + 1);
      } else {
        double v; p = js_num(p, &v); if (!p) return false;
        if      (strcmp(key, "x") == 0) st.x = (float)v;
        else if (strcmp(key, "y") == 0) st.y = (float)v;
        else if (strcmp(key, "z") == 0) st.z = (float)v;
        else if (strcmp(key, "yaw") == 0) st.yaw = (float)v;
        else if (strcmp(key, "health") == 0) st.health = (uint8_t)v;
        else if (strcmp(key, "hunger") == 0) st.hunger = (uint8_t)v;
        else if (strcmp(key, "selSlot") == 0) st.sel_slot = (uint8_t)v;
        p = js_ws(p);
      }
      if (*p == ',') { p = js_ws(p + 1); continue; }
      break;
    }
    if (*p != '}') return false;
    for (int i = 0; i < MAX_SAVED; i++)
      if (!saved[i].used || strcmp(saved[i].uid, uid) == 0) {
        saved[i].used = true;
        snprintf(saved[i].uid, sizeof saved[i].uid, "%s", uid);
        saved[i].st = st;
        break;
      }
    p = js_ws(p + 1);
    if (*p == ',') { p = js_ws(p + 1); continue; }
    break;
  }
  return *p == '}';
}

static void save_world(void) {
  if (!dirty) return;
  dirty = false;
  char path[300];
  uint8_t *eb; size_t en = encode_edits_file(&eb);
  path_join(path, sizeof path, data_dir, "world.edits");
  bool ok = write_atomic(path, eb, en);
  free(eb);
  char *pj; size_t pn = players_json(&pj);
  path_join(path, sizeof path, data_dir, "players.json");
  ok = write_atomic(path, pj, pn) && ok;
  free(pj);
  char meta[64];
  int mn = snprintf(meta, sizeof meta, "{\"time\":%.9g}", server_time());
  path_join(path, sizeof path, data_dir, "world.meta.json");
  ok = write_atomic(path, meta, (size_t)mn) && ok;
  uint8_t *cb; size_t cn = encode_chests_file(&cb);
  path_join(path, sizeof path, data_dir, "chests.bin");
  ok = write_atomic(path, cb, cn) && ok;
  free(cb);
  uint8_t *fb; size_t fn = encode_furnaces_file(&fb);
  path_join(path, sizeof path, data_dir, "furnaces.bin");
  ok = write_atomic(path, fb, fn) && ok;
  free(fb);
  if (!ok) { fprintf(stderr, "save failed\n"); dirty = true; }
}

static char *read_file(const char *path, size_t *n) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  if (sz < 0) { fclose(f); return NULL; }
  char *b = malloc((size_t)sz + 1);
  size_t got = fread(b, 1, (size_t)sz, f);
  fclose(f);
  b[got] = 0;
  if (n) *n = got;
  return b;
}

static void load_world(void) {
  char path[300]; size_t n;
  path_join(path, sizeof path, data_dir, "world.edits");
  char *b = read_file(path, &n);
  if (b) {
    if (!decode_edits_file((uint8_t *)b, n)) fprintf(stderr, "world.edits: bad magic/version or truncated - ignored\n");
    free(b);
  }
  path_join(path, sizeof path, data_dir, "players.json");
  b = read_file(path, NULL);
  if (b) { if (!players_parse(b)) fprintf(stderr, "players.json unreadable - ignored\n"); free(b); }
  path_join(path, sizeof path, data_dir, "world.meta.json");
  b = read_file(path, NULL);
  if (b) { double t; const char *q = strstr(b, "\"time\""); if (q && js_num(strchr(q, ':') + 1, &t)) set_server_time(t); free(b); }
  path_join(path, sizeof path, data_dir, "chests.bin");
  b = read_file(path, &n);
  if (b) {
    if (!decode_chests_file((uint8_t *)b, n)) fprintf(stderr, "chests.bin: bad magic/version or truncated - ignored\n");
    free(b);
  }
  path_join(path, sizeof path, data_dir, "furnaces.bin");
  b = read_file(path, &n);
  if (b) {
    if (!decode_furnaces_file((uint8_t *)b, n)) fprintf(stderr, "furnaces.bin: bad magic/version or truncated - ignored\n");
    free(b);
  }
  int np = 0; for (int i = 0; i < MAX_SAVED; i++) if (saved[i].used) np++;
  if (n_edits || np) printf("World loaded: %d edits, %d players.\n", n_edits, np);
}

// ============================== ws send ==============================
static bool send_all(SClient *c, const uint8_t *p, size_t n) {
  while (n > 0) {
    int w = send(c->sock, (const char *)p, (int)n, 0);
    if (w <= 0) { if (wouldblock()) continue; return false; }
    p += w; n -= (size_t)w;
  }
  return true;
}

// one BINARY websocket frame, server->client = unmasked (opcode 0x82)
static void ws_send(SClient *c, const uint8_t *buf, size_t len) {
  if (!c->ws) return;
  uint8_t hdr[10]; size_t hn;
  hdr[0] = 0x82;
  if (len < 126) { hdr[1] = (uint8_t)len; hn = 2; }
  else if (len < 65536) { hdr[1] = 126; hdr[2] = (uint8_t)(len >> 8); hdr[3] = (uint8_t)len; hn = 4; }
  else {
    hdr[1] = 127;
    for (int i = 0; i < 8; i++) hdr[2 + i] = (uint8_t)((uint64_t)len >> (56 - 8 * i));
    hn = 10;
  }
  if (!send_all(c, hdr, hn) || !send_all(c, buf, len)) { /* dead: sweep will reap */ }
}

static void broadcast(SClient *except, const uint8_t *buf, size_t len) {
  for (int i = 0; i < MAX_CLIENTS; i++)
    if (clients[i].used && clients[i].ws && &clients[i] != except) ws_send(&clients[i], buf, len);
}

// SV_FURNACE: send the full furnace state (bcast=true -> everyone; else just c).
static void send_sv_furnace(SClient *c, int32_t x, uint8_t y, int32_t z,
                            uint8_t flags, const FurnaceState *st, bool bcast) {
  uint8_t r[P_SV_FURN_SIZE];
  r[0] = SV_FURNACE;
  wr_i32le(r + 1, x); wr_i32le(r + 5, z); r[9] = y; r[10] = flags;
  const Stack sl[3] = { st->in, st->fuel, st->out };
  for (int k = 0; k < 3; k++) { wr_u16le(r + 11 + k * 4, (uint16_t)sl[k].id); wr_u16le(r + 13 + k * 4, (uint16_t)sl[k].count); }
  wr_f32le(r + 23, st->cook); wr_f32le(r + 27, st->burn); wr_f32le(r + 31, st->burn_max);
  if (bcast) broadcast(NULL, r, P_SV_FURN_SIZE); else ws_send(c, r, P_SV_FURN_SIZE);
}
static int server_client_count(void) {
  int n = 0;
  for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i].used && clients[i].ws) n++;
  return n;
}

// SV-type + sender id + client message body, verbatim (server.js encRelay)
static size_t enc_relay(uint8_t *out, uint8_t sv_type, uint32_t sender, const uint8_t *body, size_t n) {
  out[0] = sv_type; wr_u32le(out + 1, sender);
  memcpy(out + 5, body, n);
  return 5 + n;
}
static size_t enc_welcome(uint8_t *b, uint32_t id, float t) {
  b[0] = SV_WELCOME; b[1] = PROTO_VER; wr_u32le(b + 2, id); wr_f32le(b + 6, t); return 10;
}
static size_t enc_time(uint8_t *b, float v) { b[0] = SV_TIME; wr_f32le(b + 1, v); return 5; }
static size_t enc_leave(uint8_t *b, uint32_t id) { b[0] = SV_LEAVE; wr_u32le(b + 1, id); return 5; }

// Mob authority: the oldest (lowest-id) connected player simulates mobs and
// streams snapshots; everyone gets told their role whenever the roster moves.
static void elect_master(void) {
  SClient *best = NULL;
  for (int i = 0; i < MAX_CLIENTS; i++)
    if (clients[i].used && clients[i].ws && (!best || clients[i].id < best->id)) best = &clients[i];
  mob_master = best;
  for (int i = 0; i < MAX_CLIENTS; i++)
    if (clients[i].used && clients[i].ws) {
      uint8_t b[2] = { SV_MASTER, clients[i].id == (best ? best->id : 0) };
      ws_send(&clients[i], b, 2);
    }
}

static void send_edit_batches(SClient *c) {
  if (n_edits == 0) return;
  uint8_t *b = malloc(3 + (size_t)JOIN_BATCH * 11);
  uint32_t i = 0;
  int remaining = n_edits;
  while (remaining > 0) {
    int n = remaining < JOIN_BATCH ? remaining : JOIN_BATCH;
    b[0] = SV_EDITS; wr_u16le(b + 1, (uint16_t)n);
    int k = 0;
    for (; i < EDIT_CAP && k < n; i++) {
      if (!edits[i].used) continue;
      uint8_t *o = b + 3 + (size_t)k * 11;
      wr_i32le(o, edits[i].x); wr_i32le(o + 4, edits[i].z);
      o[8] = edits[i].y; o[9] = edits[i].b; o[10] = edits[i].w;
      k++;
    }
    ws_send(c, b, 3 + (size_t)n * 11);
    remaining -= n;
  }
  free(b);
}

// ============================== message handling ==============================
static SavedPlayer *saved_slot(const char *uid, bool create) {
  for (int i = 0; i < MAX_SAVED; i++)
    if (saved[i].used && strcmp(saved[i].uid, uid) == 0) return &saved[i];
  if (!create) return NULL;
  for (int i = 0; i < MAX_SAVED; i++)
    if (!saved[i].used) {
      saved[i].used = true;
      snprintf(saved[i].uid, sizeof saved[i].uid, "%s", uid);
      return &saved[i];
    }
  return NULL;
}

static void client_close(SClient *c);

static void handle_msg(SClient *c, const uint8_t *b, size_t n) {
  if (n < 1) return;
  c->last_seen = now_s();
  uint8_t out[256];
  switch (b[0]) {
    case CL_HELLO: {
      if (n < 3) return;
      if (b[1] != PROTO_VER) {
        uint8_t rej[2] = { SV_REJECT, PROTO_VER };
        ws_send(c, rej, 2);
        client_close(c);
        return;
      }
      size_t ul = b[2];
      if (ul < 1 || ul > 63 || n < 3 + ul) return;
      memcpy(c->uid, b + 3, ul); c->uid[ul] = 0;
      ws_send(c, out, enc_welcome(out, c->id, (float)server_time()));
      ws_send(c, out, enc_time(out, (float)server_time()));
      send_edit_batches(c);
      SavedPlayer *sp = saved_slot(c->uid, false);
      if (sp) {
        uint8_t st[P_STATE_SIZE];
        proto_enc_save(st, &sp->st);
        st[0] = SV_RESTORE;
        ws_send(c, st, P_STATE_SIZE);
      }
      elect_master();
      return;
    }
    case CL_SETTIME: {
      if (n < 5) return;
      set_server_time(rd_f32le(b + 1)); dirty = true;
      uint8_t t[8]; size_t tn = enc_time(t, (float)server_time());
      broadcast(NULL, t, tn);
      return;
    }
    case CL_SAVE: {
      if (n < P_STATE_SIZE || !c->uid[0]) return;
      SavedPlayer *sp = saved_slot(c->uid, true);
      if (sp && proto_dec_state(b, n, &sp->st)) dirty = true;
      return;
    }
    case CL_POS: {
      if (n < 33) return;
      uint8_t *r = malloc(5 + (n - 1));
      broadcast(c, r, enc_relay(r, SV_POS, c->id, b + 1, n - 1));
      free(r);
      return;
    }
    case CL_HELD: {
      if (n < 3) return;
      broadcast(c, out, enc_relay(out, SV_HELD, c->id, b + 1, n - 1));
      return;
    }
    case CL_CHAT: {
      if (n < 2 || n < 2 + (size_t)b[1]) return;
      uint8_t *r = malloc(5 + (n - 1));
      broadcast(c, r, enc_relay(r, SV_CHAT, c->id, b + 1, n - 1));
      free(r);
      return;
    }
    case CL_MOBS: {
      if (n < 3 || c != mob_master) return;   // only the authority streams mobs
      uint8_t *r = malloc(5 + (n - 1));
      broadcast(c, r, enc_relay(r, SV_MOBS, c->id, b + 1, n - 1));
      free(r);
      return;
    }
    case CL_BOOM: {
      if (n < 10) return;
      uint8_t r[16];
      broadcast(c, r, enc_relay(r, SV_BOOM, c->id, b + 1, 9));
      return;
    }
    case CL_MOB_HIT: {
      if (n < 11) return;
      uint8_t r[20];
      broadcast(c, r, enc_relay(r, SV_MOB_HIT, c->id, b + 1, 10));
      return;
    }
    case CL_EDIT: {
      if (n < 12) return;
      edit_put(rd_i32le(b + 1), b[9], rd_i32le(b + 5), b[10], b[11]);
      dirty = true;
      uint8_t r[32];
      broadcast(c, r, enc_relay(r, SV_EDIT, c->id, b + 1, n - 1 < 27 ? n - 1 : 27));
      return;
    }
    case CL_PING: {
      if (n < 9) return;
      uint8_t p[9]; p[0] = SV_PONG; memcpy(p + 1, b + 1, 8);
      ws_send(c, p, 9);
      return;
    }
    case CL_CHEST_GET: case CL_CHEST_BREAK: {
      if (n < 10) return;
      int32_t x = rd_i32le(b + 1), z = rd_i32le(b + 5); uint8_t y = b[9];
      SChest *ch = schest_find(x, y, z, false);
      uint8_t r[P_SV_CHEST_SIZE];
      r[0] = SV_CHEST;
      wr_i32le(r + 1, x); wr_i32le(r + 5, z); r[9] = y;
      r[10] = (b[0] == CL_CHEST_BREAK) ? 1 : 0;
      for (int k = 0; k < P_CHEST_SLOTS; k++) {
        wr_u16le(r + 11 + k * 4, ch ? ch->slots[k].id : 0);
        wr_u16le(r + 13 + k * 4, ch ? ch->slots[k].count : 0);
      }
      ws_send(c, r, P_SV_CHEST_SIZE);
      if (b[0] == CL_CHEST_BREAK && ch) { ch->used = false; dirty = true; }
      return;
    }
    case CL_CHEST_SET: {
      if (n < P_CHEST_SET_SIZE) return;
      int32_t x = rd_i32le(b + 1), z = rd_i32le(b + 5); uint8_t y = b[9];
      SChest *ch = schest_find(x, y, z, true);
      if (!ch) return;
      bool empty = true;
      for (int k = 0; k < P_CHEST_SLOTS; k++) {
        ch->slots[k].id = rd_u16le(b + 10 + k * 4);
        ch->slots[k].count = rd_u16le(b + 12 + k * 4);
        if (ch->slots[k].id) empty = false;
      }
      if (empty) ch->used = false;       // don't store air
      dirty = true;
      // co-viewers see the update: relay as an SV_CHEST to everyone else
      uint8_t r[P_SV_CHEST_SIZE];
      r[0] = SV_CHEST;
      memcpy(r + 1, b + 1, 9);
      r[10] = 0;
      memcpy(r + 11, b + 10, P_CHEST_SLOTS * 4);
      broadcast(c, r, P_SV_CHEST_SIZE);
      return;
    }
    case CL_FURNACE_GET: case CL_FURNACE_BREAK: {
      if (n < 10) return;
      int32_t x = rd_i32le(b + 1), z = rd_i32le(b + 5); uint8_t y = b[9];
      SFurnace *fu = sfurn_find(x, y, z, false);
      if (fu) furnace_advance(&fu->st, now_s());
      FurnaceState empty = {0};
      send_sv_furnace(c, x, y, z, (b[0] == CL_FURNACE_BREAK) ? 1 : 0, fu ? &fu->st : &empty, false);
      if (b[0] == CL_FURNACE_BREAK && fu) { fu->used = false; dirty = true; }
      return;
    }
    case CL_FURNACE_SET: {
      if (n < P_FURN_SET_SIZE) return;
      int32_t x = rd_i32le(b + 1), z = rd_i32le(b + 5); uint8_t y = b[9];
      SFurnace *fu = sfurn_find(x, y, z, true);
      if (!fu) return;
      furnace_advance(&fu->st, now_s());           // apply smelting so far, then take the player's slots
      fu->st.in.id = rd_u16le(b + 10);   fu->st.in.count = rd_u16le(b + 12);
      fu->st.fuel.id = rd_u16le(b + 14); fu->st.fuel.count = rd_u16le(b + 16);
      fu->st.out.id = rd_u16le(b + 18);  fu->st.out.count = rd_u16le(b + 20);
      fu->st.last_t = now_s();
      dirty = true;
      if (!fu->st.in.id && !fu->st.fuel.id && !fu->st.out.id && fu->st.burn <= 0) fu->used = false;
      send_sv_furnace(NULL, x, y, z, 0, &fu->st, true);   // correct the setter + co-viewers
      return;
    }
  }
}

// ============================== frame parsing ==============================
// client->server frames are ALWAYS masked (RFC 6455); mirror of server.js drain()
static void parse_frames(SClient *c) {
  size_t off_total = 0;
  while (c->rlen - off_total >= 2) {
    uint8_t *b = c->rbuf + off_total;
    size_t avail = c->rlen - off_total;
    uint8_t opcode = b[0] & 0x0f;
    bool masked = (b[1] & 0x80) != 0;
    uint64_t len = b[1] & 0x7f;
    size_t off = 2;
    if (len == 126) { if (avail < 4) break; len = ((uint64_t)b[2] << 8) | b[3]; off = 4; }
    else if (len == 127) {
      if (avail < 10) break;
      len = 0; for (int i = 0; i < 8; i++) len = (len << 8) | b[2 + i];
      off = 10;
    }
    size_t mask_len = masked ? 4 : 0;
    if (avail < off + mask_len + len) break;
    uint8_t *payload = b + off + mask_len;
    if (masked) {
      const uint8_t *mask = b + off;
      for (uint64_t i = 0; i < len; i++) payload[i] ^= mask[i & 3];
    }
    if (opcode == 0x8) { client_close(c); return; }
    if (opcode == 0x9) {                     // ping -> pong (server: unmasked)
      uint8_t p[131]; size_t pl = len > 125 ? 125 : (size_t)len;
      p[0] = 0x8A; p[1] = (uint8_t)pl;
      memcpy(p + 2, payload, pl);
      send_all(c, p, 2 + pl);
    }
    if (opcode == 0x2) handle_msg(c, payload, (size_t)len);
    if (!c->used) return;                    // handle_msg may close (REJECT) — rbuf is gone
    // text frames (0x1) — legacy clients — are ignored
    off_total += off + mask_len + (size_t)len;
  }
  if (off_total > 0) { memmove(c->rbuf, c->rbuf + off_total, c->rlen - off_total); c->rlen -= off_total; }
}

// ============================== http ==============================
static const char *mime_for(const char *path) {
  const char *dot = strrchr(path, '.');
  if (!dot) return "application/octet-stream";
  if (!strcmp(dot, ".html")) return "text/html; charset=utf-8";
  if (!strcmp(dot, ".js"))   return "text/javascript";
  if (!strcmp(dot, ".wasm")) return "application/wasm";
  if (!strcmp(dot, ".data")) return "application/octet-stream";
  if (!strcmp(dot, ".png"))  return "image/png";
  if (!strcmp(dot, ".json")) return "application/json";
  return "application/octet-stream";
}

static void http_respond(SClient *c, int code, const char *status, const char *mime, const void *body, size_t n) {
  char hdr[256];
  int hn = snprintf(hdr, sizeof hdr,
    "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n",
    code, status, mime, n);
  send_all(c, (const uint8_t *)hdr, (size_t)hn);
  if (n) send_all(c, body, n);
  client_close(c);
}

static void serve_static(SClient *c, const char *url) {
  if (!static_dir[0] || strstr(url, "..")) {
    const char *msg = "Craft Survival server is running. Open the game in a browser!";
    http_respond(c, 200, "OK", "text/plain; charset=utf-8", msg, strlen(msg));
    return;
  }
  char path[300]; size_t n; char *body = NULL;
  if (strcmp(url, "/") == 0 || url[0] == 0) {
    path_join(path, sizeof path, static_dir, "craft.html");
    body = read_file(path, &n);
    if (!body) { path_join(path, sizeof path, static_dir, "index.html"); body = read_file(path, &n); }
  } else {
    path_join(path, sizeof path, static_dir, url + 1);
    body = read_file(path, &n);
  }
  if (!body) { http_respond(c, 404, "Not Found", "text/plain", "not found", 9); return; }
  http_respond(c, 200, "OK", mime_for(path), body, n);
  free(body);
}

// pre-upgrade: buffer until we have full headers, then upgrade or serve
static void handle_http(SClient *c) {
  c->rbuf[c->rlen] = 0;
  char *end = strstr((char *)c->rbuf, "\r\n\r\n");
  if (!end) {
    if (c->rlen > 8000) client_close(c);   // header flood
    return;
  }
  char *req = (char *)c->rbuf;
  char url[256] = "/";
  sscanf(req, "GET %255s", url);
  // websocket upgrade?
  char *keyh = strstr(req, "Sec-WebSocket-Key:");
  if (keyh && (strstr(req, "Upgrade: websocket") || strstr(req, "Upgrade: WebSocket"))) {
    keyh += 18;
    while (*keyh == ' ') keyh++;
    char key[64]; int kn = 0;
    while (*keyh && *keyh != '\r' && kn < 60) key[kn++] = *keyh++;
    key[kn] = 0;
    char cat[128];
    snprintf(cat, sizeof cat, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    Sha1 sh; uint8_t dig[20]; char acc[32];
    sha1_init(&sh); sha1_update(&sh, cat, strlen(cat)); sha1_final(&sh, dig);
    b64_encode(dig, 20, acc);
    char resp[256];
    int rn = snprintf(resp, sizeof resp,
      "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", acc);
    send_all(c, (const uint8_t *)resp, (size_t)rn);
    // leftover after headers is already frame data
    size_t consumed = (size_t)(end + 4 - (char *)c->rbuf);
    memmove(c->rbuf, c->rbuf + consumed, c->rlen - consumed);
    c->rlen -= consumed;
    c->ws = true;
    c->id = next_id++;
    printf("Player %u joined\n", c->id);
    return;
  }
  serve_static(c, url);
}

// ============================== lifecycle ==============================
static void client_close(SClient *c) {
  if (!c->used) return;
  if (c->sock != INVALID_SOCKET) { CLOSESOCK(c->sock); c->sock = INVALID_SOCKET; }
  bool was_ws = c->ws;
  uint32_t id = c->id;
  free(c->rbuf);
  memset(c, 0, sizeof *c);
  c->sock = INVALID_SOCKET;
  if (was_ws) {
    printf("Player %u left\n", id);
    uint8_t b[8];
    broadcast(NULL, b, enc_leave(b, id));
    elect_master();                    // authority may have just left
  }
}

bool server_start(int port, const char *sdir, const char *ddir) {
  setvbuf(stdout, NULL, _IOLBF, 0);   // line-buffered: docker logs show up live
#ifdef _WIN32
  WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#else
  signal(SIGPIPE, SIG_IGN);
#endif
  snprintf(static_dir, sizeof static_dir, "%s", sdir ? sdir : "");
  snprintf(data_dir, sizeof data_dir, "%s", ddir && ddir[0] ? ddir : ".");
  if (!edits) edits = calloc(EDIT_CAP, sizeof(EditRec));
  for (int i = 0; i < MAX_CLIENTS; i++) { clients[i].used = false; clients[i].sock = INVALID_SOCKET; }
  time_base_at = now_s();
  load_world();

  listener = socket(AF_INET, SOCK_STREAM, 0);
  if (listener == INVALID_SOCKET) return false;
  int yes = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons((uint16_t)port);
  if (bind(listener, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(listener, 16) != 0) {
    CLOSESOCK(listener); listener = INVALID_SOCKET;
    fprintf(stderr, "server: cannot listen on port %d\n", port);
    return false;
  }
  set_nonblocking(listener);
  running = true;
  last_tick = now_s();
  printf("Craft Survival server %s running on port %d (C, v2 binary). Stop: Ctrl+C\n", CRAFT_VERSION, port);
  if (static_dir[0]) printf("Static dir served: %s -> http://localhost:%d/\n", static_dir, port);
  return true;
}

void server_pump(void) {
  if (!running) return;
  // accept
  for (;;) {
    sock_t s = accept(listener, NULL, NULL);
    if (s == INVALID_SOCKET) break;
    set_nonblocking(s);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    SClient *c = NULL;
    for (int i = 0; i < MAX_CLIENTS; i++) if (!clients[i].used) { c = &clients[i]; break; }
    if (!c) { CLOSESOCK(s); continue; }
    memset(c, 0, sizeof *c);
    c->used = true; c->sock = s; c->last_seen = now_s();
    c->rcap = 8192; c->rbuf = malloc(c->rcap);
  }
  // read
  for (int i = 0; i < MAX_CLIENTS; i++) {
    SClient *c = &clients[i];
    if (!c->used) continue;
    for (;;) {
      if (c->rcap - c->rlen < 65536) { c->rcap = c->rcap * 2 + 65536; c->rbuf = realloc(c->rbuf, c->rcap); }
      int r = recv(c->sock, (char *)c->rbuf + c->rlen, (int)(c->rcap - c->rlen - 1), 0);
      if (r > 0) { c->rlen += (size_t)r; continue; }
      if (r == 0) { client_close(c); break; }
      if (!wouldblock()) { client_close(c); }
      break;
    }
    if (!c->used) continue;
    if (c->ws) parse_frames(c);
    else if (c->rlen > 0) { handle_http(c); if (c->used && c->ws) parse_frames(c); }
  }
  // timers
  double now = now_s();
  if (now - last_sweep >= 5.0) {         // heartbeat: silent 10 s -> close
    last_sweep = now;
    for (int i = 0; i < MAX_CLIENTS; i++) {
      SClient *c = &clients[i];
      if (c->used && c->ws && now - c->last_seen > HEARTBEAT_S) client_close(c);
    }
  }
  // furnaces: advance + push live state to viewers, but ONLY while players are
  // online. When empty the server sleeps (last_t frozen), so the first tick after
  // someone joins catches up the whole elapsed span in one furnace_advance.
  if (server_client_count() > 0 && now - last_furn >= 1.0) {
    last_furn = now;
    for (int i = 0; i < MAX_SFURN; i++) {
      if (!sfurnaces[i].used) continue;
      FurnaceState *st = &sfurnaces[i].st;
      int before = st->out.count; float pc = st->cook, pb = st->burn;
      furnace_advance(st, now_s());
      if (st->cook > 0 || st->burn > 0 || pc > 0 || pb > 0 || st->out.count != before) {
        send_sv_furnace(NULL, sfurnaces[i].x, sfurnaces[i].y, sfurnaces[i].z, 0, st, true);
        if (st->out.count != before) dirty = true;
      }
    }
  }
  if (now - last_tick >= SAVE_EVERY_S) { // time broadcast + save, every 10 s
    last_tick = now;
    uint8_t t[8]; size_t tn = enc_time(t, (float)server_time());
    broadcast(NULL, t, tn);
    save_world();
  }
}

void server_stop(void) {
  if (!running) return;
  running = false;
  dirty = dirty || n_edits > 0;   // belt & braces: final save
  save_world();
  for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i].used) { CLOSESOCK(clients[i].sock); free(clients[i].rbuf); clients[i].used = false; }
  if (listener != INVALID_SOCKET) { CLOSESOCK(listener); listener = INVALID_SOCKET; }
}

// ============================== self test ==============================
#define ST_CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok: %s\n", msg); } while (0)

static void hex_of(const uint8_t *b, size_t n, char *out) {
  for (size_t i = 0; i < n; i++) sprintf(out + i * 2, "%02x", b[i]);
}

int server_selftest(void) {
  int fails = 0;
  char h[512];
  if (!edits) edits = calloc(EDIT_CAP, sizeof(EditRec));

  // GOLDEN: world.edits file bytes (identical constants in net_test/old server.js)
  edits_clear();
  edit_put(5, 6, 7, 3, 0);
  uint8_t *eb; size_t en = encode_edits_file(&eb);
  hex_of(eb, en, h);
  ST_CHECK(strcmp(h, "4b454454" "01" "01000000" "0500000007000000060300") == 0,
           "GOLDEN: world.edits magic+version+count+record bytes");
  edits_clear();
  ST_CHECK(decode_edits_file(eb, en) && n_edits == 1, "world.edits round-trips");
  free(eb);
  edit_put(-100, 63, -200, 8, 9);
  en = encode_edits_file(&eb);
  edits_clear();
  ST_CHECK(decode_edits_file(eb, en) && n_edits == 2, "negative coords survive the file round-trip");
  ST_CHECK(!decode_edits_file((const uint8_t *)"junk", 4), "garbage edits file rejected");
  ST_CHECK(!decode_edits_file(eb, en - 3), "truncated edits file rejected");
  free(eb);

  // GOLDEN: relay frames
  uint8_t out[64];
  uint8_t held_body[2] = { 0x07, 0x00 };
  size_t rn = enc_relay(out, SV_HELD, 3, held_body, 2);
  hex_of(out, rn, h);
  ST_CHECK(strcmp(h, "17" "03000000" "0700") == 0, "GOLDEN: held relay (sender 3, item 7) bytes");
  uint8_t chat_body[3] = { 2, 'h', 'i' };
  rn = enc_relay(out, SV_CHAT, 3, chat_body, 3);
  hex_of(out, rn, h);
  ST_CHECK(strcmp(h, "19" "03000000" "02" "6869") == 0, "GOLDEN: chat relay (sender 3, \"hi\") bytes");
  rn = enc_welcome(out, 7, 0.5f);
  hex_of(out, rn, h);
  ST_CHECK(strcmp(h, "02" "02" "07000000" "0000003f") == 0, "GOLDEN: welcome(id7,time.5) bytes");
  rn = enc_leave(out, 9);
  hex_of(out, rn, h);
  ST_CHECK(strcmp(h, "3009000000") == 0, "GOLDEN: leave(9) bytes");

  // websocket accept key (RFC 6455 test vector)
  {
    const char *cat = "dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    Sha1 s; uint8_t dig[20]; char acc[32];
    sha1_init(&s); sha1_update(&s, cat, strlen(cat)); sha1_final(&s, dig);
    b64_encode(dig, 20, acc);
    ST_CHECK(strcmp(acc, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0, "RFC 6455 accept-key vector");
  }

  // players.json writer <-> parser round-trip
  memset(saved, 0, sizeof saved);
  SavedPlayer *sp = saved_slot("uid-A", true);
  sp->st.x = 1; sp->st.y = 2; sp->st.z = 3; sp->st.yaw = 0.5f;
  sp->st.health = 15; sp->st.hunger = 18; sp->st.sel_slot = 4;
  sp->st.slots[0] = (PSlot){ 7, 4 };
  sp->st.slots[2] = (PSlot){ 300, 64 };
  char *pj; players_json(&pj);
  memset(saved, 0, sizeof saved);
  ST_CHECK(players_parse(pj), "players.json parses back");
  free(pj);
  sp = saved_slot("uid-A", false);
  ST_CHECK(sp && sp->st.health == 15 && sp->st.sel_slot == 4, "player state fields survive");
  ST_CHECK(sp && sp->st.slots[0].id == 7 && sp->st.slots[1].id == 0
           && sp->st.slots[2].id == 300 && sp->st.slots[2].count == 64,
           "slots (including empties) round-trip");
  ST_CHECK(players_parse("{}"), "empty players.json accepted");
  ST_CHECK(!players_parse("{\"x\":"), "garbage players.json rejected");

  // join-batch split: 40000 records -> 16384 + 16384 + 7232 (counted, not sent)
  edits_clear();
  for (int i = 0; i < 40000; i++) edit_put(i, 0, 0, 1, 0);
  ST_CHECK(n_edits == 40000, "40k edits stored");
  int batches = (n_edits + JOIN_BATCH - 1) / JOIN_BATCH;
  int last = n_edits - (batches - 1) * JOIN_BATCH;
  ST_CHECK(batches == 3 && last == 7232, "40k records split into 16384+16384+7232");
  edits_clear();

  printf(fails ? "SERVER SELF-TEST: %d FAILURES\n" : "Self-test OK: framing, codecs, persistence and day cycle are correct.\n", fails);
  return fails;
}
