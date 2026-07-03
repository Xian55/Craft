// Live metrics overlay: frame time + ping ring buffers, sparklines, counts.
#ifndef METRICS_H
#define METRICS_H

#include <stdbool.h>

void metrics_frame(void);        // sample once per frame (cheap, call always)
void metrics_draw(int mode);     // 1 = compact line, 2 = full panel with graphs

// Overlay mode state (0 off, 1 compact, 2 graphs) shared by the F3 key and
// the /metrics chat command.
int  metrics_mode(void);
void metrics_set(int mode);      // clamps; -1 = cycle to the next mode

#endif
