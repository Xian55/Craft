// Remote-player interpolation — pure math, no raylib.
// Snapshot ring per remote; sample ~120 ms in the past, lerp between
// brackets (yaw via shortest arc), extrapolate with velocity on gaps.
#ifndef INTERP_H
#define INTERP_H

#include <stdbool.h>

#define SNAP_RING 8
#define INTERP_DELAY 0.120       // render this far in the past (seconds)
#define EXTRAP_MAX 0.25          // extrapolate at most this far past newest snap
#define HOLD_TIMEOUT 3.0         // older than this: hold last position

typedef struct Snap {
  double t;
  float x, y, z, yaw, pitch, vx, vy, vz;
} Snap;

typedef struct SnapRing {
  Snap s[SNAP_RING];
  int head, count;               // head = index of newest
} SnapRing;

void interp_push(SnapRing *r, const Snap *s);
// Sample at (now - INTERP_DELAY). Returns false if the ring is empty.
bool interp_sample(const SnapRing *r, double now, Snap *out);
float lerp_yaw(float a, float b, float u);   // shortest-arc

// Frames spent extrapolating past the newest snapshot (network gap wider than
// the interp delay). Cumulative; read by the metrics overlay.
extern long interp_extrap_frames;

#endif
