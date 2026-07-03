// Multiplayer protocol layer — same JSON WebSocket protocol as game.js/server.js.
#ifndef NET_H
#define NET_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// --- transport (ws_native.c / ws_wasm.c), binary frames ---
bool           ws_connect(const char *host, int port, const char *path);
void           ws_pump(void);               // native: recv+parse frames; wasm: no-op
const uint8_t *ws_poll(size_t *len);        // next queued binary msg or NULL (caller must not free)
void           ws_send_bin(const void *buf, size_t len);
bool           ws_is_open(void);
void           ws_close(void);

// --- protocol ---
#include "interp.h"

typedef struct RemotePlayer {
  uint32_t id;
  SnapRing ring;                 // raw snapshots from the wire
  double x, y, z;                // interpolated pose, refreshed each net_update
  float yaw, pitch;
  uint16_t held;                 // item id in their hand (0 = empty/unknown)
  bool used;
} RemotePlayer;
#define MAX_REMOTE 32
extern RemotePlayer remote_players[MAX_REMOTE];

void net_connect(const char *host, int port);
void net_update(float dt);      // pump + dispatch + 20 Hz pos + ping + reconnect
void net_send_edit(int x, int y, int z);   // reads current voxel+water like netEdit
void net_send_settime(double v);
void net_send_chat(const char *msg);       // player chat line (<=120 chars)
void net_send_boom(int x, int y, int z);   // explosion event (mob sync)
void net_send_mob_hit(uint8_t slot, uint8_t dmg, float kx, float kz);
// chest contents live on the server: fetch on open, write on close,
// break spills the authoritative contents as drops
void net_send_chest_get(int x, int y, int z);
void net_send_chest_set(int x, int y, int z);   // reads chest_array_peek(x,y,z)
void net_send_chest_break(int x, int y, int z);
void net_save_state(void);      // player state -> server (called on exit too)
bool net_active(void);
double net_rtt_ms(void);        // last measured ping roundtrip, -1 if unknown

#endif
