// Live metrics: ring buffers of frame time and ping RTT with sparkline
// graphs, plus world/net counters. Toggled by F3 in main.c (off -> compact
// -> full). Everything here is a few hundred floats and flat rectangles —
// no allocations, drawn only when visible.
#include "metrics.h"
#include "net.h"
#include "world.h"
#include "mesh.h"
#include "fluids.h"
#include "entities.h"
#include "physics.h"
#include "state.h"
#include "raylib.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define FT_N 120                     // ~2 s of frames
#define RTT_N 120                    // ~4 min of pings (one per 2 s)

static float ft[FT_N];               // frame time, ms
static int ft_head = 0, ft_count = 0;
static float rtt[RTT_N];             // ping roundtrip, ms
static int rtt_head = 0, rtt_count = 0;
static double last_rtt_seen = -2;    // dedupe: push only new pong values
static int mode_ = 0;                // 0 off, 1 compact, 2 graphs

int  metrics_mode(void) { return mode_; }
void metrics_set(int mode) { mode_ = mode < 0 ? (mode_ + 1) % 3 : mode % 3; }

void metrics_frame(void) {
  ft[ft_head] = GetFrameTime() * 1000.0f;
  ft_head = (ft_head + 1) % FT_N;
  if (ft_count < FT_N) ft_count++;

  double r = net_rtt_ms();
  if (r >= 0 && r != last_rtt_seen) {
    last_rtt_seen = r;
    rtt[rtt_head] = (float)r;
    rtt_head = (rtt_head + 1) % RTT_N;
    if (rtt_count < RTT_N) rtt_count++;
  }
}

static void stats(const float *buf, int head, int count, int cap,
                  float *avg, float *mx) {
  float sum = 0, m = 0;
  for (int i = 0; i < count; i++) {
    float v = buf[(head - 1 - i + cap) % cap];
    sum += v; if (v > m) m = v;
  }
  *avg = count ? sum / count : 0;
  *mx = m;
}

// One bar per sample, newest on the right. color_fn = NULL: single color.
static void sparkline(const float *buf, int head, int count, int cap,
                      int x, int y, int w, int h, float scale_max,
                      Color (*color_fn)(float), Color solid) {
  DrawRectangle(x, y, w, h, (Color){ 0, 0, 0, 140 });
  if (count == 0 || scale_max <= 0) return;
  float bw = (float)w / cap;
  for (int i = 0; i < count; i++) {
    float v = buf[(head - count + i + cap) % cap];
    int bh = (int)((v / scale_max) * h);
    if (bh < 1) bh = 1; if (bh > h) bh = h;
    Color c = color_fn ? color_fn(v) : solid;
    DrawRectangle(x + (int)(i * bw + (cap - count) * bw), y + h - bh, (int)ceilf(bw), bh, c);
  }
}

static Color frame_color(float ms) {
  if (ms <= 16.7f) return (Color){ 80, 200, 90, 255 };    // 60 fps budget
  if (ms <= 33.4f) return (Color){ 235, 200, 60, 255 };   // 30 fps
  return (Color){ 230, 70, 60, 255 };
}

static void count_chunk(Chunk *c, void *ud) {
  int *n = ud;
  n[0]++;
  if (c->meshed) n[1]++;
}

void metrics_draw(int mode) {
  float ft_avg, ft_max, rtt_avg, rtt_max;
  stats(ft, ft_head, ft_count, FT_N, &ft_avg, &ft_max);
  stats(rtt, rtt_head, rtt_count, RTT_N, &rtt_avg, &rtt_max);

  if (mode == 1) {
    const char *line = TextFormat("FPS %d  frame %.1f/%.1f ms  ping %s  %s",
      GetFPS(), ft_avg, ft_max,
      rtt_count ? TextFormat("%.0f ms", rtt[(rtt_head - 1 + RTT_N) % RTT_N]) : "-",
      net_active() ? "online" : "offline");
    DrawRectangle(6, 6, MeasureText(line, 16) + 12, 22, (Color){ 0, 0, 0, 140 });
    DrawText(line, 12, 10, 16, WHITE);
    return;
  }

  int chunks[2] = { 0, 0 };
  world_each_chunk(count_chunk, chunks);
  int remotes = 0;
  for (int i = 0; i < MAX_REMOTE; i++) if (remote_players[i].used) remotes++;

  const int x = 10, w = 250;
  int y = 10;
  DrawRectangle(x - 4, y - 4, w + 8, 232, (Color){ 0, 0, 0, 140 });

  DrawText(TextFormat("FPS %d   frame %.1f avg / %.1f max ms", GetFPS(), ft_avg, ft_max), x, y, 14, WHITE);
  y += 18;
  // floor: keep the 60 fps budget visible; ceiling: one giant hitch (world
  // gen) must not flatten the rest of the graph — spikes just clip at the top
  float ft_scale = ft_max < 33.4f ? 33.4f : (ft_max > 100.0f ? 100.0f : ft_max);
  sparkline(ft, ft_head, ft_count, FT_N, x, y, w, 40, ft_scale, frame_color, WHITE);
  y += 46;

  DrawText(TextFormat("ping %.0f avg / %.0f max ms   %s", rtt_avg, rtt_max,
                      net_active() ? "online" : "offline"), x, y, 14, WHITE);
  y += 18;
  sparkline(rtt, rtt_head, rtt_count, RTT_N, x, y, w, 30,
            rtt_max < 50 ? 50 : rtt_max, NULL, (Color){ 90, 150, 235, 255 });
  y += 36;

  DrawText(TextFormat("remotes %d   interp stalls %ld", remotes, interp_extrap_frames), x, y, 14, WHITE);
  y += 18;
  DrawText(TextFormat("chunks %d loaded / %d meshed   rd %d", chunks[0], chunks[1], render_dist_get()), x, y, 14, WHITE);
  y += 18;
  DrawText(TextFormat("fluids %d   pigs %d   mobs %d", fluids_active_count(), pig_count(), mob_count()), x, y, 14, WHITE);
  y += 18;
  DrawText(TextFormat("XYZ %.1f / %.1f / %.1f   chunk %d,%d",
                      player.x, player.y, player.z,
                      chunk_of((int)floor(player.x)), chunk_of((int)floor(player.z))), x, y, 14, WHITE);
  y += 18;
  DrawText(TextFormat("time %.2f", day_time), x, y, 14, WHITE);
}
