// Small shared game state that several modules read/write.
#ifndef STATE_H
#define STATE_H

extern double day_time;      // 0..1 in the day cycle (0.25 = noon, 0.75 = midnight)
#define DAY_LENGTH 300.0     // seconds per full cycle — must match game.js/server.js

extern float cam_fov;        // vertical FOV in degrees (/fov 30..110, default 60)

#endif
