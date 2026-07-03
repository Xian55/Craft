#include "interp.h"
#include <math.h>

#define PI_F 3.14159265358979f

long interp_extrap_frames = 0;

void interp_push(SnapRing *r, const Snap *s) {
  r->head = (r->head + 1) % SNAP_RING;
  r->s[r->head] = *s;
  if (r->count < SNAP_RING) r->count++;
}

float lerp_yaw(float a, float b, float u) {
  float d = fmodf(b - a + 3.0f * PI_F, 2.0f * PI_F) - PI_F;
  return a + d * u;
}

static float lerpf(float a, float b, float u) { return a + (b - a) * u; }

// snapshot i steps back from newest (0 = newest)
static const Snap *back(const SnapRing *r, int i) {
  return &r->s[(r->head - i + SNAP_RING * 2) % SNAP_RING];
}

bool interp_sample(const SnapRing *r, double now, Snap *out) {
  if (r->count == 0) return false;
  double t = now - INTERP_DELAY;
  const Snap *newest = back(r, 0);

  if (t >= newest->t) {
    // past the newest snapshot: extrapolate along its velocity, clamped
    double dt = t - newest->t;
    if (dt > 0.001 && r->count > 1) interp_extrap_frames++;
    if (dt > EXTRAP_MAX) dt = (t - newest->t > HOLD_TIMEOUT) ? 0 : EXTRAP_MAX;
    *out = *newest;
    out->x += newest->vx * (float)dt;
    out->y += newest->vy * (float)dt;
    out->z += newest->vz * (float)dt;
    return true;
  }

  // find the bracketing pair: s[i+1].t <= t < s[i].t
  for (int i = 0; i < r->count - 1; i++) {
    const Snap *a = back(r, i + 1), *b = back(r, i);
    if (t >= a->t && t <= b->t) {
      float u = (b->t - a->t) > 1e-9 ? (float)((t - a->t) / (b->t - a->t)) : 1.0f;
      out->t = t;
      out->x = lerpf(a->x, b->x, u);
      out->y = lerpf(a->y, b->y, u);
      out->z = lerpf(a->z, b->z, u);
      out->yaw = lerp_yaw(a->yaw, b->yaw, u);
      out->pitch = lerpf(a->pitch, b->pitch, u);
      out->vx = b->vx; out->vy = b->vy; out->vz = b->vz;
      return true;
    }
  }
  // older than everything we kept: use the oldest
  *out = *back(r, r->count - 1);
  return true;
}
