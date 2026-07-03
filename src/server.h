// Embedded game server — the same module backs `craft.exe --host` (play and
// host at once) and the headless `craft_server` binary (Pi/docker). Pure C,
// no raylib: sockets, websocket framing, relay, persistence.
#ifndef SERVER_H
#define SERVER_H

#include <stdbool.h>

// static_dir: directory served over HTTP (""/NULL = no static serving).
// data_dir:  where world.edits / players.json / world.meta.json live.
bool server_start(int port, const char *static_dir, const char *data_dir);
void server_pump(void);        // accept + read + relay + timers; call often
void server_stop(void);        // saves and closes everything

int  server_selftest(void);    // golden-byte self test, returns fail count
void server_sleep_ms(int ms);  // portable sleep for the dedicated loop

#endif
