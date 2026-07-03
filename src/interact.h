// Targeting, mining, placing, buckets — port of game.js:1359-1695.
#ifndef INTERACT_H
#define INTERACT_H

#include <stdbool.h>

void interact_init(void);              // crack texture + meshes
void set_mining(bool m);               // LMB held
void update_mining(float dt);
void use_right_click(void);            // place / bucket / chest / TNT prime
void draw_highlight(void);             // wireframe + crack overlay (3D pass)
float mine_progress01(void);           // 0..1 for the HUD progress bar (<0 = idle)

// chest UI hook (implemented in ui.c)
void open_chest_at(int x, int y, int z);

#endif
