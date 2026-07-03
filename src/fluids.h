// Water/lava flow simulation — port of game.js:862-1003.
// Fixed 0.2 s tick over an active set; idle oceans cost nothing.
#ifndef FLUIDS_H
#define FLUIDS_H

void touch_water(int x, int y, int z);   // mark a cell + 6 neighbors active
void simulate_water(void);               // one tick
int  fluids_active_count(void);          // debug

#endif
