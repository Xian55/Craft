#include "sky.h"
#include "state.h"
#include "mesh.h"
#include "physics.h"
#include "rlgl.h"
#include <math.h>

double day_time = 0.12;    // early morning, like the JS

static float daylight = 1.0f;
static Vector3 sun_dir = { 0.35f, 0.8f, 0.4f };
static float cloud_drift = 0;

#define CLOUD_Y 100.0f
#define CLOUD_SPAN 700.0f
#define CLOUD_NEAR 120.0f
#define CLOUD_FAR (CLOUD_SPAN / 2 - 20)
#define N_CLOUDS 40

float sky_daylight(void) { return daylight; }

void sky_update(float dt) {
  day_time = fmod(day_time + dt / DAY_LENGTH, 1.0);
  double a = day_time * 2.0 * 3.14159265358979;
  float len;
  sun_dir = (Vector3){ (float)cos(a), (float)sin(a), 0.35f };
  len = sqrtf(sun_dir.x * sun_dir.x + sun_dir.y * sun_dir.y + sun_dir.z * sun_dir.z);
  sun_dir.x /= len; sun_dir.y /= len; sun_dir.z /= len;
  float day_t = (sun_dir.y + 0.15f) / 0.35f;
  if (day_t < 0) day_t = 0; if (day_t > 1) day_t = 1;
  daylight = 0.2f + 0.8f * day_t;
  mesh_set_daylight(daylight);
  cloud_drift += dt * 1.6f;
}

// lerp NIGHT_SKY(0x0b1230) -> DAY_SKY(0x78a7ff)
Color sky_color(void) {
  float t = (daylight - 0.2f) / 0.8f;
  return (Color){ (uint8_t)(11 + (120 - 11) * t), (uint8_t)(18 + (167 - 18) * t),
                  (uint8_t)(48 + (255 - 48) * t), 255 };
}

static float wrap_cloud(float v) {
  float m = fmodf(v, CLOUD_SPAN);
  if (m < 0) m += CLOUD_SPAN;
  return m - CLOUD_SPAN / 2;
}

void sky_draw_3d(Camera3D cam) {
  // Sun & moon: billboards far along the celestial direction FROM the camera,
  // drawn with depth writes off so the world always covers them.
  rlDrawRenderBatchActive();
  rlDisableDepthMask();
  float D = 400.0f;
  if (sun_dir.y > -0.15f) {
    Vector3 p = { cam.position.x + sun_dir.x * D, cam.position.y + sun_dir.y * D, cam.position.z + sun_dir.z * D };
    DrawSphere(p, 18.0f, (Color){ 255, 242, 176, 255 });
  }
  if (sun_dir.y < 0.15f) {
    Vector3 p = { cam.position.x - sun_dir.x * D, cam.position.y - sun_dir.y * D, cam.position.z - sun_dir.z * D };
    DrawSphere(p, 13.0f, (Color){ 223, 230, 240, 255 });
  }
  rlDrawRenderBatchActive();
  rlEnableDepthMask();

  // clouds: white slabs drifting around the player, fading with distance
  float b = daylight;
  for (int i = 0; i < N_CLOUDS; i++) {
    float w = 16 + (i * 37 % 30), d = 16 + (i * 53 % 30);
    float ox = (float)(i * 197 % (int)CLOUD_SPAN) - CLOUD_SPAN / 2;
    float oz = (float)(i * 313 % (int)CLOUD_SPAN) - CLOUD_SPAN / 2;
    float wx = wrap_cloud(ox - (float)player.x + cloud_drift), wz = wrap_cloud(oz - (float)player.z);
    float dist = sqrtf(wx * wx + wz * wz);
    float fade = (CLOUD_FAR - dist) / (CLOUD_FAR - CLOUD_NEAR);
    if (fade < 0.02f) continue;
    if (fade > 1) fade = 1;
    Color cc = { (uint8_t)(255 * b), (uint8_t)(255 * b), (uint8_t)(255 * b), (uint8_t)(0.55f * fade * 255) };
    DrawCube((Vector3){ (float)player.x + wx, CLOUD_Y, (float)player.z + wz }, w, 5, d, cc);
  }
}
