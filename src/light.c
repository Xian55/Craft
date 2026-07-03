#include "light.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

static const int LIGHT_NB[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};

// BFS flood over one chunk (no cross-chunk light, same as JS).
// Queue capacity: every cell can enter the queue a few times; JS uses growing
// arrays. Worst case is bounded by pushes with strictly increasing light per
// cell (max 15 levels), but stay generous.
typedef struct { uint8_t x, y, z; } QCell;

static void flood_light(Chunk *c, uint8_t *arr, const QCell *seeds, const uint8_t *seedv,
                        int nseeds, int leaf_cost, int step_cost) {
  static QCell *q = NULL; static int qcap = 0;
  if (!q) { qcap = CHUNK_VOL * 8; q = malloc(sizeof(QCell) * qcap); }
  int head = 0, tail = 0;
  for (int i = 0; i < nseeds; i++) {
    int idx = LI(seeds[i].x, seeds[i].y, seeds[i].z);
    if (arr[idx] < seedv[i]) { arr[idx] = seedv[i]; q[tail++] = seeds[i]; }
  }
  while (head < tail) {
    QCell cur = q[head++];
    int l = arr[LI(cur.x, cur.y, cur.z)];
    if (l <= 1) continue;
    for (int k = 0; k < 6; k++) {
      int nx = cur.x + LIGHT_NB[k][0], ny = cur.y + LIGHT_NB[k][1], nz = cur.z + LIGHT_NB[k][2];
      if (nx < 0 || ny < 0 || nz < 0 || nx >= CHUNK || ny >= WORLD_H || nz >= CHUNK) continue;
      int ni = LI(nx, ny, nz);
      if (occludes(c->blocks[ni])) continue;
      int nlv = l - (c->blocks[ni] == B_LEAVES ? leaf_cost : step_cost);
      if (nlv > 0 && arr[ni] < nlv) {
        arr[ni] = (uint8_t)nlv;
        if (tail >= qcap) { qcap *= 2; q = realloc(q, sizeof(QCell) * qcap); }
        q[tail++] = (QCell){ (uint8_t)nx, (uint8_t)ny, (uint8_t)nz };
      }
    }
  }
}

void compute_light(Chunk *c) {
  memset(c->light_sky, 0, CHUNK_VOL);
  memset(c->light_blk, 0, CHUNK_VOL);

  // Sky seeds: from the top of each column down while transparent; leaves get
  // lit themselves but shadow everything below.
  // Capacity: sky seeds max CHUNK_VOL; block seeds max 6 per torch + lava
  // cells, bounded by 7*CHUNK_VOL.
  static QCell seeds[CHUNK_VOL * 7]; static uint8_t seedv[CHUNK_VOL * 7];
  int n = 0;
  for (int lx = 0; lx < CHUNK; lx++)
    for (int lz = 0; lz < CHUNK; lz++) {
      bool open = true;
      for (int y = WORLD_H - 1; y >= 0; y--) {
        uint8_t b = c->blocks[LI(lx, y, lz)];
        if (occludes(b)) { open = false; continue; }
        if (open) { seeds[n] = (QCell){ (uint8_t)lx, (uint8_t)y, (uint8_t)lz }; seedv[n++] = 15; }
        if (b != B_AIR) open = false;
      }
    }
  flood_light(c, c->light_sky, seeds, seedv, n, 3, 2);

  // Block-light seeds: torches light their open neighbors at 14, lava glows at 15.
  n = 0;
  for (int lx = 0; lx < CHUNK; lx++)
    for (int lz = 0; lz < CHUNK; lz++)
      for (int y = 0; y < WORLD_H; y++) {
        int idx = LI(lx, y, lz);
        if (c->blocks[idx] == B_TORCH) {
          for (int k = 0; k < 6; k++) {
            int nx = lx + LIGHT_NB[k][0], ny = y + LIGHT_NB[k][1], nz = lz + LIGHT_NB[k][2];
            if (nx < 0 || ny < 0 || nz < 0 || nx >= CHUNK || ny >= WORLD_H || nz >= CHUNK) continue;
            if (!occludes(c->blocks[LI(nx, ny, nz)])) {
              seeds[n] = (QCell){ (uint8_t)nx, (uint8_t)ny, (uint8_t)nz }; seedv[n++] = 14;
            }
          }
        } else if (c->blocks[idx] == B_AIR && c->water[idx] >= 11) {
          seeds[n] = (QCell){ (uint8_t)lx, (uint8_t)y, (uint8_t)lz }; seedv[n++] = 15;
        }
      }
  flood_light(c, c->light_blk, seeds, seedv, n, 2, 1);

  c->has_light = true;
}

static Chunk *lit_chunk(int cx, int cz) {
  Chunk *c = ensure_chunk(cx, cz);
  if (!c->has_light) compute_light(c);
  return c;
}

int get_light_sky(int x, int y, int z) {
  if (y >= WORLD_H) return 15;
  if (y < 0) return 0;
  int cx = chunk_of(x), cz = chunk_of(z);
  return lit_chunk(cx, cz)->light_sky[LI(x - cx * CHUNK, y, z - cz * CHUNK)];
}

int get_light_blk(int x, int y, int z) {
  if (y >= WORLD_H || y < 0) return 0;
  int cx = chunk_of(x), cz = chunk_of(z);
  return lit_chunk(cx, cz)->light_blk[LI(x - cx * CHUNK, y, z - cz * CHUNK)];
}

static const int AXES[3][3] = {{1,0,0},{0,1,0},{0,0,1}};
static const float AO_LEVEL[4] = { 0.4f, 0.62f, 0.82f, 1.0f };

void corner_shade(int x, int y, int z, const int dir[3], const int corner[5],
                  const int tang[2], float dir_f, float out[2]) {
  int fx = x + dir[0], fy = y + dir[1], fz = z + dir[2];
  const int *A = AXES[tang[0]], *B = AXES[tang[1]];
  int sa = 2 * corner[tang[0]] - 1, sb = 2 * corner[tang[1]] - 1;
  int s1x = fx + A[0] * sa, s1y = fy + A[1] * sa, s1z = fz + A[2] * sa;
  int s2x = fx + B[0] * sb, s2y = fy + B[1] * sb, s2z = fz + B[2] * sb;
  int ccx = s1x + B[0] * sb, ccy = s1y + B[1] * sb, ccz = s1z + B[2] * sb;
  bool o1 = occludes(get_voxel(s1x, s1y, s1z));
  bool o2 = occludes(get_voxel(s2x, s2y, s2z));
  bool oc = occludes(get_voxel(ccx, ccy, ccz));
  int ao = (o1 && o2) ? 0 : 3 - ((o1 ? 1 : 0) + (o2 ? 1 : 0) + (oc ? 1 : 0));
  float ss = (float)get_light_sky(fx, fy, fz), bs = (float)get_light_blk(fx, fy, fz);
  int cnt = 1;
  if (!o1) { ss += get_light_sky(s1x, s1y, s1z); bs += get_light_blk(s1x, s1y, s1z); cnt++; }
  if (!o2) { ss += get_light_sky(s2x, s2y, s2z); bs += get_light_blk(s2x, s2y, s2z); cnt++; }
  if (!oc) { ss += get_light_sky(ccx, ccy, ccz); bs += get_light_blk(ccx, ccy, ccz); cnt++; }
  float g = dir_f * AO_LEVEL[ao];
  float sky = powf(0.8f, 15.0f - ss / cnt) * g;
  float blk = powf(0.8f, 15.0f - bs / cnt) * g;
  out[0] = sky < 0.06f ? 0.06f : sky;
  out[1] = blk < 0.0f ? 0.0f : blk;
}
