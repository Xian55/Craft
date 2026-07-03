// Day-night cycle, sky color, sun/moon discs, drifting clouds.
// Port of game.js:1027-1072 and 2322-2359.
#ifndef SKY_H
#define SKY_H

#include "raylib.h"

void sky_update(float dt);          // advance day_time, set daylight on materials
Color sky_color(void);              // current sky/fog color (clear background)
void sky_draw_3d(Camera3D cam);     // sun, moon, clouds (inside BeginMode3D)
float sky_daylight(void);           // 0.2 night .. 1.0 day

#endif
