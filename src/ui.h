// 2D UI: HUD (hearts/hunger/hotbar), inventory + crafting + chest panels,
// chat console with commands. Port of game.js sections 9-10.
#ifndef UI_H
#define UI_H

#include <stdbool.h>

void ui_init(void);
bool ui_panel_open(void);        // inventory or chest panel visible
bool ui_chat_open(void);
void ui_toggle_inventory(void);
void ui_open_chat(const char *prefill);
void ui_update(void);            // panel + chat input handling (call every frame)
void ui_draw(void);              // all 2D drawing (after EndMode3D)
void ui_chat_log(const char *msg);
void ui_run_command(const char *cmd);   // used by chat + the CRAFT_CMD debug hook

#endif
