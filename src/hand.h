// First-person hand/held-item overlay — port of game.js handScene (1125-1181).
#ifndef HAND_H
#define HAND_H

// Draw the held item (or the empty arm) over the world. Call between the
// world's EndMode3D() and the 2D UI. Uses its own camera and ignores the
// depth buffer so the hand never clips into world geometry.
void hand_draw(void);

#endif
