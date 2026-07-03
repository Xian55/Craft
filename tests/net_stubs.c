// No-op net layer for test binaries that link entities.c without net.c.
#include "../src/net.h"

RemotePlayer remote_players[MAX_REMOTE];
void net_send_edit(int x, int y, int z) { (void)x; (void)y; (void)z; }
void net_send_boom(int x, int y, int z) { (void)x; (void)y; (void)z; }
void net_send_mob_hit(uint8_t slot, uint8_t dmg, float kx, float kz) { (void)slot; (void)dmg; (void)kx; (void)kz; }
