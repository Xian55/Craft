// Binary protocol v2 codec — pure, no sockets, no raylib. Little-endian,
// byte 0 = message type. Must stay byte-identical to server.js's encoders
// (both test suites carry golden fixtures of the same messages).
#ifndef PROTO_H
#define PROTO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PROTO_VER 2

// client -> server
#define CL_HELLO   0x01
#define CL_SETTIME 0x04
#define CL_POS     0x10
#define CL_EDIT    0x12
#define CL_HELD    0x16
#define CL_CHAT    0x18
#define CL_MOBS    0x1A
#define CL_BOOM    0x1C
#define CL_MOB_HIT 0x1E
#define CL_SAVE    0x20
#define CL_CHEST_GET   0x22
#define CL_CHEST_SET   0x24
#define CL_CHEST_BREAK 0x26
#define CL_PING    0x40
// server -> client
#define SV_WELCOME 0x02
#define SV_TIME    0x03
#define SV_REJECT  0x0F
#define SV_POS     0x11
#define SV_EDIT    0x13
#define SV_EDITS   0x14
#define SV_HELD    0x17
#define SV_CHAT    0x19
#define SV_MOBS    0x1B
#define SV_BOOM    0x1D
#define SV_MOB_HIT 0x1F
#define SV_RESTORE 0x21
#define SV_CHEST   0x23
#define SV_LEAVE   0x30
#define SV_MASTER  0x32
#define SV_PONG    0x41

#define P_INV_SIZE   36
#define P_STATE_SIZE (1 + 16 + 3 + P_INV_SIZE * 4)   // 164
#define P_POS_SIZE   33
#define P_EDIT_SIZE  12

typedef struct PPos { float x, y, z, yaw, pitch, vx, vy, vz; } PPos;
typedef struct PEdit { int32_t x, z; uint8_t y, block, water; } PEdit;
// mob sync: authority client -> everyone else, 10 Hz full snapshots.
// flags bit0 = creeper fuse burning; yaw256 = yaw scaled to 0..255.
typedef struct PMob { uint8_t slot, type, flags, yaw256; float x, y, z; } PMob;
typedef struct PArrow { float x, y, z; } PArrow;
#define P_MOB_REC   16
#define P_ARROW_REC 12
#define P_MOBS_MAX  32
#define P_ARROWS_MAX 64

// chest contents: 27 slots, server-stored, fetched on open / written on close.
// SV_CHEST flags bit0 = chest was broken: spawn contents as drops.
#define P_CHEST_SLOTS 27
#define P_CHEST_SET_SIZE (1 + 9 + P_CHEST_SLOTS * 4)   // 118
#define P_SV_CHEST_SIZE  (1 + 9 + 1 + P_CHEST_SLOTS * 4) // 119
typedef struct PSlot { uint16_t id, count; } PSlot;
typedef struct PState {
  float x, y, z, yaw;
  uint8_t health, hunger, sel_slot;
  PSlot slots[P_INV_SIZE];
} PState;

// encoders return the number of bytes written
size_t proto_enc_hello(uint8_t *b, const char *uid);          // <= 66 bytes
size_t proto_enc_settime(uint8_t *b, float v);                // 5
size_t proto_enc_pos(uint8_t *b, const PPos *p);              // 33
size_t proto_enc_edit(uint8_t *b, const PEdit *e);            // 12
size_t proto_enc_save(uint8_t *b, const PState *s);           // 164
size_t proto_enc_ping(uint8_t *b, double t);                  // 9
size_t proto_enc_held(uint8_t *b, uint16_t item);             // 3
size_t proto_enc_chat(uint8_t *b, const char *msg);           // 2 + len (<=120)
size_t proto_enc_mobs(uint8_t *b, const PMob *m, int nm, const PArrow *a, int na);
size_t proto_enc_boom(uint8_t *b, int32_t x, uint8_t y, int32_t z);          // 10
size_t proto_enc_mob_hit(uint8_t *b, uint8_t slot, uint8_t dmg, float kx, float kz); // 11
size_t proto_enc_chest_req(uint8_t *b, uint8_t type, int32_t x, uint8_t y, int32_t z); // GET/BREAK, 10
size_t proto_enc_chest_set(uint8_t *b, int32_t x, uint8_t y, int32_t z, const PSlot *s);

// decoders return false if the buffer is too short
bool proto_dec_welcome(const uint8_t *b, size_t n, uint8_t *ver, uint32_t *id, float *time);
bool proto_dec_time(const uint8_t *b, size_t n, float *v);
bool proto_dec_sv_pos(const uint8_t *b, size_t n, uint32_t *id, PPos *p);
bool proto_dec_sv_edit(const uint8_t *b, size_t n, uint32_t *id, PEdit *e);
// EDITS: returns record count, sets *recs to the first record; use
// proto_edits_record to step (records are 11 bytes, unaligned).
int  proto_dec_edits_count(const uint8_t *b, size_t n);
void proto_edits_record(const uint8_t *b, int i, PEdit *e);
bool proto_dec_state(const uint8_t *b, size_t n, PState *s);  // SV_RESTORE body
bool proto_dec_leave(const uint8_t *b, size_t n, uint32_t *id);
bool proto_dec_pong(const uint8_t *b, size_t n, double *t);
bool proto_dec_sv_held(const uint8_t *b, size_t n, uint32_t *id, uint16_t *item);
bool proto_dec_sv_chat(const uint8_t *b, size_t n, uint32_t *id, char *out, size_t outsz);
bool proto_dec_sv_mobs(const uint8_t *b, size_t n, uint32_t *id,
                       PMob *m, int *nm, PArrow *a, int *na);
bool proto_dec_sv_boom(const uint8_t *b, size_t n, uint32_t *id, int32_t *x, uint8_t *y, int32_t *z);
bool proto_dec_sv_mob_hit(const uint8_t *b, size_t n, uint32_t *id,
                          uint8_t *slot, uint8_t *dmg, float *kx, float *kz);
bool proto_dec_master(const uint8_t *b, size_t n, bool *is_master);
bool proto_dec_sv_chest(const uint8_t *b, size_t n, int32_t *x, uint8_t *y, int32_t *z,
                        uint8_t *flags, PSlot *s);

#endif
