#include "fluids.h"
#include "world.h"
#include "mesh.h"
#include <stdlib.h>
#include <string.h>

// --- tiny open-addressed set/map of packed cell coordinates ---
// Pack: 21 bits x, 21 bits z (signed, offset), 7 bits y. Plenty for play range.
static inline uint64_t pack3(int x, int y, int z) {
  return ((uint64_t)(uint32_t)(x + (1 << 20)) << 43)
       | ((uint64_t)(uint32_t)(z + (1 << 20)) << 22)
       | ((uint64_t)(uint32_t)(y + 64) << 15);
}
static inline void unpack3(uint64_t k, int *x, int *y, int *z) {
  *x = (int)((k >> 43) & 0x1FFFFF) - (1 << 20);
  *z = (int)((k >> 22) & 0x1FFFFF) - (1 << 20);
  *y = (int)((k >> 15) & 0x7F) - 64;
}

typedef struct {
  uint64_t *keys; uint8_t *vals;   // vals optional (NULL for pure set)
  int cap, count;
} Set;

static void set_init(Set *s, int cap, bool with_vals) {
  s->cap = cap; s->count = 0;
  s->keys = calloc((size_t)cap, sizeof(uint64_t));
  s->vals = with_vals ? calloc((size_t)cap, 1) : NULL;
}
static void set_free(Set *s) { free(s->keys); free(s->vals); memset(s, 0, sizeof(*s)); }
static uint32_t set_hash(uint64_t k, int cap) {
  k ^= k >> 33; k *= 0xff51afd7ed558ccdULL; k ^= k >> 33;
  return (uint32_t)k & (uint32_t)(cap - 1);
}
static bool set_add_kv(Set *s, uint64_t key, uint8_t val);
static void set_grow(Set *s) {
  Set n; set_init(&n, s->cap * 2, s->vals != NULL);
  for (int i = 0; i < s->cap; i++)
    if (s->keys[i]) set_add_kv(&n, s->keys[i], s->vals ? s->vals[i] : 1);
  set_free(s); *s = n;
}
// key 0 = empty slot; pack3 never returns 0 for reachable coords (offsets non-zero)
static bool set_add_kv(Set *s, uint64_t key, uint8_t val) {
  if ((s->count + 1) * 4 >= s->cap * 3) set_grow(s);
  uint32_t i = set_hash(key, s->cap);
  for (;;) {
    if (s->keys[i] == key) { if (s->vals) s->vals[i] = val; return false; }
    if (s->keys[i] == 0) { s->keys[i] = key; if (s->vals) s->vals[i] = val; s->count++; return true; }
    i = (i + 1) & (uint32_t)(s->cap - 1);
  }
}
static bool set_get(const Set *s, uint64_t key, uint8_t *val) {
  if (!s->cap) return false;
  uint32_t i = set_hash(key, s->cap);
  for (;;) {
    if (s->keys[i] == key) { if (val && s->vals) *val = s->vals[i]; return true; }
    if (s->keys[i] == 0) return false;
    i = (i + 1) & (uint32_t)(s->cap - 1);
  }
}

// --- active set (double-buffered per tick, like JS) ---
static Set active;   // lazily initialized

void touch_water(int x, int y, int z) {
  if (!active.cap) set_init(&active, 1024, false);
  set_add_kv(&active, pack3(x, y, z), 1);
  set_add_kv(&active, pack3(x + 1, y, z), 1); set_add_kv(&active, pack3(x - 1, y, z), 1);
  set_add_kv(&active, pack3(x, y + 1, z), 1); set_add_kv(&active, pack3(x, y - 1, z), 1);
  set_add_kv(&active, pack3(x, y, z + 1), 1); set_add_kv(&active, pack3(x, y, z - 1), 1);
}

int fluids_active_count(void) { return active.count; }

// --- water helpers (game.js:182-205) ---
static bool is_source(int x, int y, int z)      { return get_voxel(x, y, z) == B_AIR && raw_water(x, y, z) == 9; }
static bool is_lava_source(int x, int y, int z) { return get_voxel(x, y, z) == B_AIR && raw_water(x, y, z) == 19; }
static bool supported_water(int x, int y, int z) {
  if (get_voxel(x, y - 1, z) != B_AIR) return true;
  return is_source(x, y - 1, z);
}
static bool supported_lava(int x, int y, int z) {
  if (get_voxel(x, y - 1, z) != B_AIR) return true;
  return is_lava_source(x, y - 1, z);
}

// --- slope seeking (game.js:871-910) ---
static const int DIRS4[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
#define SLOPE_RANGE 5
static Set flow_cache;   // pack3 -> 4-bit direction mask (bit i = DIRS4[i]); 0x10 = "computed, empty"

static bool can_drop(int x, int y, int z) {
  return get_voxel(x, y - 1, z) == B_AIR && water_at(x, y - 1, z) < 8;
}

static int slope_dist(int x, int y, int z, int dx, int dz) {
  int sx = x + dx, sz = z + dz;
  if (get_voxel(sx, y, sz) != B_AIR) return 999;
  // small BFS over the horizontal plane; bounded by SLOPE_RANGE ring (~61 cells)
  enum { QMAX = 256 };
  static int qx[QMAX], qz[QMAX], qd[QMAX];
  Set seen; set_init(&seen, 128, false);
  set_add_kv(&seen, pack3(sx, 0, sz), 1);
  int head = 0, tail = 0;
  qx[tail] = sx; qz[tail] = sz; qd[tail] = 1; tail++;
  int result = 999;
  while (head < tail) {
    int cx = qx[head], cz = qz[head], d = qd[head]; head++;
    if (can_drop(cx, y, cz)) { result = d; break; }
    if (d >= SLOPE_RANGE) continue;
    for (int i = 0; i < 4; i++) {
      int nx = cx + DIRS4[i][0], nz = cz + DIRS4[i][1];
      if (set_get(&seen, pack3(nx, 0, nz), NULL)) continue;
      if (get_voxel(nx, y, nz) != B_AIR) continue;
      set_add_kv(&seen, pack3(nx, 0, nz), 1);
      if (tail < QMAX) { qx[tail] = nx; qz[tail] = nz; qd[tail] = d + 1; tail++; }
    }
  }
  set_free(&seen);
  return result;
}

static uint8_t flow_dirs_mask(int x, int y, int z) {
  uint64_t key = pack3(x, y, z);
  uint8_t hit;
  if (set_get(&flow_cache, key, &hit)) return hit & 0x0F;
  uint8_t mask;
  if (!is_source(x, y, z) && !supported_water(x, y, z)) {
    mask = 0;
  } else {
    int dist[4], best = 999;
    for (int i = 0; i < 4; i++) { dist[i] = slope_dist(x, y, z, DIRS4[i][0], DIRS4[i][1]); if (dist[i] < best) best = dist[i]; }
    if (best >= 999) mask = 0x0F;                       // no hole in range: flow all ways
    else { mask = 0; for (int i = 0; i < 4; i++) if (dist[i] == best) mask |= (uint8_t)(1 << i); }
  }
  set_add_kv(&flow_cache, key, (uint8_t)(mask | 0x10)); // 0x10 marks "computed"
  return mask;
}

static bool flows_toward(int x, int y, int z, int tx, int tz) {
  uint8_t m = flow_dirs_mask(x, y, z);
  for (int i = 0; i < 4; i++)
    if ((m & (1 << i)) && DIRS4[i][0] == tx && DIRS4[i][1] == tz) return true;
  return false;
}

// --- one tick (game.js simulateWater) ---
static const int NB6[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};

void simulate_water(void) {
  if (!active.cap || active.count == 0) return;
  Set current = active;
  set_init(&active, 1024, false);
  if (flow_cache.cap) set_free(&flow_cache);
  set_init(&flow_cache, 512, true);

  Set changed; set_init(&changed, 256, false);   // dirty chunk keys (pack cx,0,cz)
  Set relight; set_init(&relight, 64, false);
  #define MARK_CHUNK(X, Z) do { \
    int cx_ = chunk_of(X), cz_ = chunk_of(Z), lx_ = (X) - cx_ * CHUNK, lz_ = (Z) - cz_ * CHUNK; \
    set_add_kv(&changed, pack3(cx_, 0, cz_), 1); \
    if (lx_ == 0) set_add_kv(&changed, pack3(cx_ - 1, 0, cz_), 1); \
    if (lx_ == CHUNK - 1) set_add_kv(&changed, pack3(cx_ + 1, 0, cz_), 1); \
    if (lz_ == 0) set_add_kv(&changed, pack3(cx_, 0, cz_ - 1), 1); \
    if (lz_ == CHUNK - 1) set_add_kv(&changed, pack3(cx_, 0, cz_ + 1), 1); \
  } while (0)

  for (int slot = 0; slot < current.cap; slot++) {
    if (!current.keys[slot]) continue;
    int x, y, z; unpack3(current.keys[slot], &x, &y, &z);
    if (y < 0 || y >= WORLD_H) continue;

    if (get_voxel(x, y, z) != B_AIR) {           // became solid: purge fluid
      if (raw_water(x, y, z) != 0) { set_water_raw(x, y, z, 0); MARK_CHUNK(x, z); touch_water(x, y, z); }
      continue;
    }
    int raw = raw_water(x, y, z);

    // water + lava contact -> cobblestone
    bool mine_w = raw >= 1 && raw <= 9, mine_l = raw >= 11;
    if (mine_w || mine_l) {
      bool other_above = mine_w ? lava_at(x, y + 1, z) > 0 : water_at(x, y + 1, z) > 0;
      bool lava_side_water = mine_l && (water_at(x + 1, y, z) || water_at(x - 1, y, z) || water_at(x, y, z + 1) || water_at(x, y, z - 1));
      if (other_above || lava_side_water) {
        set_water_raw(x, y, z, 0); set_voxel(x, y, z, B_COBBLE);
        remesh_around(x, z); MARK_CHUNK(x, z);
        for (int i = 0; i < 6; i++) touch_water(x + NB6[i][0], y + NB6[i][1], z + NB6[i][2]);
        continue;
      }
    }
    if (raw == 9 || raw == 19) continue;         // sources stay

    // water candidate
    int wl;
    if (water_at(x, y + 1, z) > 0) wl = 8;
    else {
      int m = 0;
      #define CONSIDER_W(NX, NZ) do { \
        if (is_source(NX, y, NZ) || supported_water(NX, y, NZ)) \
          if (flows_toward(NX, y, NZ, x - (NX), z - (NZ))) { int w_ = water_at(NX, y, NZ); if (w_ > m) m = w_; } \
      } while (0)
      CONSIDER_W(x + 1, z); CONSIDER_W(x - 1, z); CONSIDER_W(x, z + 1); CONSIDER_W(x, z - 1);
      #undef CONSIDER_W
      wl = m > 0 ? m - 1 : 0;
    }
    if (wl > 0) {                                 // infinite source: >=2 source neighbors
      int c = 0;
      if (is_source(x + 1, y, z)) c++; if (is_source(x - 1, y, z)) c++;
      if (is_source(x, y, z + 1)) c++; if (is_source(x, y, z - 1)) c++;
      if (c >= 2) wl = 9;
    }

    // lava candidate (drops 2 per step, no infinite sources)
    int ll;
    if (lava_at(x, y + 1, z) > 0) ll = 8;
    else {
      int m = 0;
      #define CONSIDER_L(NX, NZ) do { \
        if (is_lava_source(NX, y, NZ) || supported_lava(NX, y, NZ)) { int l_ = lava_at(NX, y, NZ); if (l_ > m) m = l_; } \
      } while (0)
      CONSIDER_L(x + 1, z); CONSIDER_L(x - 1, z); CONSIDER_L(x, z + 1); CONSIDER_L(x, z - 1);
      #undef CONSIDER_L
      ll = m > 0 ? m - 2 : 0;
    }

    int nv = wl > 0 ? wl : ll > 0 ? 10 + ll : 0;
    if (nv != raw) {
      set_water_raw(x, y, z, (uint8_t)nv); MARK_CHUNK(x, z); touch_water(x, y, z);
      if (raw >= 11 || nv >= 11) set_add_kv(&relight, pack3(chunk_of(x), 0, chunk_of(z)), 1);
    }
  }

  for (int i = 0; i < changed.cap; i++) {
    if (!changed.keys[i]) continue;
    int cx, yy, cz; unpack3(changed.keys[i], &cx, &yy, &cz);
    Chunk *c = chunk_get(cx, cz);
    if (c && c->meshed) rebuild_chunk_water(cx, cz);
  }
  for (int i = 0; i < relight.cap; i++) {
    if (!relight.keys[i]) continue;
    int cx, yy, cz; unpack3(relight.keys[i], &cx, &yy, &cz);
    Chunk *c = chunk_get(cx, cz);
    if (c && c->meshed) rebuild_chunk_solid(cx, cz);
  }
  set_free(&changed); set_free(&relight); set_free(&current);
  #undef MARK_CHUNK
}
