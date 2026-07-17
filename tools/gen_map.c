// Headless top-down terrain map (squaremap-style) for fast gen iteration.
// Reuses the deterministic gen_chunk_data (no window/GPU), colors each column by
// its surface block + water depth, hillshades by height, writes a PNG via
// raylib's ExportImage (CPU only, no GL context needed).
//   gen_map [centerX centerZ radius out.png [sliceY]]   defaults: 0 0 768 map.png
// radius = half-size in blocks, so the image is (2*radius) square.
// sliceY (optional): render the block at that Y as a flat cross-section (air =
// dark) instead of the hillshaded surface — for eyeballing ore veins / caves.
#include "gen.h"
#include "config.h"
#include "raylib.h"
#include <stdlib.h>
#include <stdio.h>

static int chunk_of_(int v) { return v >= 0 ? v / CHUNK : -((-v - 1) / CHUNK) - 1; }

static void surf_color(uint8_t b, unsigned char *r, unsigned char *g, unsigned char *bl) {
  switch (b) {
    case B_GRASS:  *r = 90;  *g = 150; *bl = 55;  break;
    case B_SAND:   *r = 224; *g = 210; *bl = 150; break;
    case B_SNOW:   *r = 240; *g = 246; *bl = 252; break;
    case B_DIRT:   *r = 134; *g = 94;  *bl = 58;  break;
    case B_STONE:  *r = 138; *g = 138; *bl = 138; break;
    case B_WOOD:   *r = 110; *g = 80;  *bl = 48;  break;
    case B_LEAVES: *r = 54;  *g = 118; *bl = 46;  break;
    case B_COAL_ORE: *r = 40;  *g = 40;  *bl = 44;  break;
    case B_IRON_ORE: *r = 200; *g = 152; *bl = 108; break;
    default:       *r = 150; *g = 150; *bl = 150; break;
  }
}

int main(int argc, char **argv) {
  int cx0 = argc > 1 ? atoi(argv[1]) : 0;
  int cz0 = argc > 2 ? atoi(argv[2]) : 0;
  int R   = argc > 3 ? atoi(argv[3]) : 768;
  const char *out = argc > 4 ? argv[4] : "map.png";
  int sliceY = argc > 5 ? atoi(argv[5]) : -1;   // >=0: flat cross-section at Y
  if (R < 16) R = 16;
  if (R > 2048) R = 2048;                 // cap ~16M px (~128 MB working buffers)
  int W = 2 * R, H = 2 * R;

  Color *px = malloc((size_t)W * H * sizeof(Color));
  int   *hs = malloc((size_t)W * H * sizeof(int));   // solid-surface height, for hillshade
  if (!px || !hs) { fprintf(stderr, "gen_map: out of memory\n"); return 1; }

  int wx0 = cx0 - R, wz0 = cz0 - R;       // top-left world coord of the image
  int ccx0 = chunk_of_(wx0), ccx1 = chunk_of_(wx0 + W - 1);
  int ccz0 = chunk_of_(wz0), ccz1 = chunk_of_(wz0 + H - 1);
  static uint8_t blocks[CHUNK_VOL], water[CHUNK_VOL];

  // pass 1: generate each covered chunk exactly once; fill color + height.
  for (int ccz = ccz0; ccz <= ccz1; ccz++)
    for (int ccx = ccx0; ccx <= ccx1; ccx++) {
      gen_chunk_data(ccx, ccz, blocks, water);
      for (int lz = 0; lz < CHUNK; lz++)
        for (int lx = 0; lx < CHUNK; lx++) {
          int i = ccx * CHUNK + lx - wx0, j = ccz * CHUNK + lz - wz0;
          if (i < 0 || i >= W || j < 0 || j >= H) continue;
          unsigned char r, g, b;
          if (sliceY >= 0) {              // flat cross-section at Y (ore/cave view)
            uint8_t blk = blocks[LI(lx, sliceY, lz)];
            if (blk == B_AIR) { r = 22; g = 22; b = 32; }   // empty / cave
            else surf_color(blk, &r, &g, &b);
            px[(size_t)j * W + i] = (Color){ r, g, b, 255 };
            hs[(size_t)j * W + i] = 0;    // flat: no hillshade
            continue;
          }
          int st = -1, wt = -1;
          for (int y = WORLD_H - 1; y >= 0; y--) {
            if (st < 0 && blocks[LI(lx, y, lz)] != B_AIR) st = y;
            if (wt < 0 && water[LI(lx, y, lz)] > 0) wt = y;
            if (st >= 0 && wt >= 0) break;
          }
          if (wt > st) {                  // water surface above land -> blue by depth
            int depth = wt - st; if (depth > 12) depth = 12;
            float t = depth / 12.0f;
            r = (unsigned char)(90 - 50 * t);
            g = (unsigned char)(150 - 70 * t);
            b = (unsigned char)(210 - 40 * t);
          } else {
            surf_color(st >= 0 ? blocks[LI(lx, st, lz)] : B_STONE, &r, &g, &b);
          }
          px[(size_t)j * W + i] = (Color){ r, g, b, 255 };
          hs[(size_t)j * W + i] = st;
        }
    }

  // pass 2: hillshade from the NW height gradient (fake sun from the north-west).
  for (int j = 0; j < H; j++)
    for (int i = 0; i < W; i++) {
      int h = hs[(size_t)j * W + i];
      int hl = i > 0 ? hs[(size_t)j * W + i - 1] : h;
      int hu = j > 0 ? hs[(size_t)(j - 1) * W + i] : h;
      float shade = 1.0f + ((h - hl) + (h - hu)) * 0.10f;
      if (shade < 0.55f) shade = 0.55f;
      if (shade > 1.40f) shade = 1.40f;
      Color c = px[(size_t)j * W + i];
      int r = (int)(c.r * shade), g = (int)(c.g * shade), b = (int)(c.b * shade);
      px[(size_t)j * W + i] = (Color){ r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b, 255 };
    }

  Image img = { px, W, H, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
  bool ok = ExportImage(img, out);
  free(px); free(hs);
  printf("gen_map: %dx%d centered (%d,%d) -> %s (%s)\n", W, H, cx0, cz0, out, ok ? "ok" : "FAIL");
  return ok ? 0 : 1;
}
