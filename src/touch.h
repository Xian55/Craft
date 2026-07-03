// Mobile touch controls: floating joystick (move), drag-to-look, action
// buttons. Activates on the first touch; desktop input is untouched.
#ifndef TOUCH_H
#define TOUCH_H

#include "physics.h"
#include <stdbool.h>

bool touch_mode(void);                 // true once any touch was seen
void touch_update(Input *in, bool ui_blocked);   // feed movement/look/actions
void touch_draw(void);                 // overlay: joystick + buttons

#endif
