#include "proto.h"
#include <string.h>

// explicit little-endian primitives (host-endianness-independent)
static void wr_u16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static void wr_u32(uint8_t *b, uint32_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24); }
static void wr_i32(uint8_t *b, int32_t v)  { wr_u32(b, (uint32_t)v); }
static void wr_f32(uint8_t *b, float v)    { uint32_t u; memcpy(&u, &v, 4); wr_u32(b, u); }
static void wr_f64(uint8_t *b, double v)   { uint64_t u; memcpy(&u, &v, 8); for (int i = 0; i < 8; i++) b[i] = (uint8_t)(u >> (8 * i)); }
static uint16_t rd_u16(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }
static uint32_t rd_u32(const uint8_t *b) { return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24); }
static int32_t  rd_i32(const uint8_t *b) { return (int32_t)rd_u32(b); }
static float    rd_f32(const uint8_t *b) { uint32_t u = rd_u32(b); float v; memcpy(&v, &u, 4); return v; }
static double   rd_f64(const uint8_t *b) { uint64_t u = 0; for (int i = 0; i < 8; i++) u |= (uint64_t)b[i] << (8 * i); double v; memcpy(&v, &u, 8); return v; }

size_t proto_enc_hello(uint8_t *b, const char *uid) {
  size_t len = strlen(uid);
  if (len > 63) len = 63;
  b[0] = CL_HELLO; b[1] = PROTO_VER; b[2] = (uint8_t)len;
  memcpy(b + 3, uid, len);
  return 3 + len;
}

size_t proto_enc_settime(uint8_t *b, float v) {
  b[0] = CL_SETTIME; wr_f32(b + 1, v); return 5;
}

size_t proto_enc_pos(uint8_t *b, const PPos *p) {
  b[0] = CL_POS;
  wr_f32(b + 1, p->x); wr_f32(b + 5, p->y); wr_f32(b + 9, p->z);
  wr_f32(b + 13, p->yaw); wr_f32(b + 17, p->pitch);
  wr_f32(b + 21, p->vx); wr_f32(b + 25, p->vy); wr_f32(b + 29, p->vz);
  return P_POS_SIZE;
}

size_t proto_enc_edit(uint8_t *b, const PEdit *e) {
  b[0] = CL_EDIT;
  wr_i32(b + 1, e->x); wr_i32(b + 5, e->z);
  b[9] = e->y; b[10] = e->block; b[11] = e->water;
  return P_EDIT_SIZE;
}

size_t proto_enc_save(uint8_t *b, const PState *s) {
  b[0] = CL_SAVE;
  wr_f32(b + 1, s->x); wr_f32(b + 5, s->y); wr_f32(b + 9, s->z); wr_f32(b + 13, s->yaw);
  b[17] = s->health; b[18] = s->hunger; b[19] = s->sel_slot;
  for (int i = 0; i < P_INV_SIZE; i++) {
    wr_u16(b + 20 + i * 4, s->slots[i].id);
    wr_u16(b + 22 + i * 4, s->slots[i].count);
  }
  return P_STATE_SIZE;
}

size_t proto_enc_ping(uint8_t *b, double t) {
  b[0] = CL_PING; wr_f64(b + 1, t); return 9;
}

size_t proto_enc_held(uint8_t *b, uint16_t item) {
  b[0] = CL_HELD; wr_u16(b + 1, item); return 3;
}

size_t proto_enc_chat(uint8_t *b, const char *msg) {
  size_t len = strlen(msg);
  if (len > 120) len = 120;
  b[0] = CL_CHAT; b[1] = (uint8_t)len;
  memcpy(b + 2, msg, len);
  return 2 + len;
}

bool proto_dec_welcome(const uint8_t *b, size_t n, uint8_t *ver, uint32_t *id, float *time) {
  if (n < 10 || b[0] != SV_WELCOME) return false;
  *ver = b[1]; *id = rd_u32(b + 2); *time = rd_f32(b + 6);
  return true;
}

bool proto_dec_time(const uint8_t *b, size_t n, float *v) {
  if (n < 5 || b[0] != SV_TIME) return false;
  *v = rd_f32(b + 1);
  return true;
}

static void dec_pos_body(const uint8_t *b, PPos *p) {   // 32-byte body
  p->x = rd_f32(b); p->y = rd_f32(b + 4); p->z = rd_f32(b + 8);
  p->yaw = rd_f32(b + 12); p->pitch = rd_f32(b + 16);
  p->vx = rd_f32(b + 20); p->vy = rd_f32(b + 24); p->vz = rd_f32(b + 28);
}

bool proto_dec_sv_pos(const uint8_t *b, size_t n, uint32_t *id, PPos *p) {
  if (n < 37 || b[0] != SV_POS) return false;
  *id = rd_u32(b + 1);
  dec_pos_body(b + 5, p);
  return true;
}

bool proto_dec_sv_edit(const uint8_t *b, size_t n, uint32_t *id, PEdit *e) {
  if (n < 16 || b[0] != SV_EDIT) return false;
  *id = rd_u32(b + 1);
  e->x = rd_i32(b + 5); e->z = rd_i32(b + 9);
  e->y = b[13]; e->block = b[14]; e->water = b[15];
  return true;
}

int proto_dec_edits_count(const uint8_t *b, size_t n) {
  if (n < 3 || b[0] != SV_EDITS) return -1;
  int count = rd_u16(b + 1);
  if (n < 3 + (size_t)count * 11) return -1;
  return count;
}

void proto_edits_record(const uint8_t *b, int i, PEdit *e) {
  const uint8_t *r = b + 3 + i * 11;
  e->x = rd_i32(r); e->z = rd_i32(r + 4);
  e->y = r[8]; e->block = r[9]; e->water = r[10];
}

bool proto_dec_state(const uint8_t *b, size_t n, PState *s) {
  if (n < P_STATE_SIZE || (b[0] != SV_RESTORE && b[0] != CL_SAVE)) return false;
  s->x = rd_f32(b + 1); s->y = rd_f32(b + 5); s->z = rd_f32(b + 9); s->yaw = rd_f32(b + 13);
  s->health = b[17]; s->hunger = b[18]; s->sel_slot = b[19];
  for (int i = 0; i < P_INV_SIZE; i++) {
    s->slots[i].id = rd_u16(b + 20 + i * 4);
    s->slots[i].count = rd_u16(b + 22 + i * 4);
  }
  return true;
}

bool proto_dec_leave(const uint8_t *b, size_t n, uint32_t *id) {
  if (n < 5 || b[0] != SV_LEAVE) return false;
  *id = rd_u32(b + 1);
  return true;
}

bool proto_dec_pong(const uint8_t *b, size_t n, double *t) {
  if (n < 9 || b[0] != SV_PONG) return false;
  *t = rd_f64(b + 1);
  return true;
}

bool proto_dec_sv_held(const uint8_t *b, size_t n, uint32_t *id, uint16_t *item) {
  if (n < 7 || b[0] != SV_HELD) return false;
  *id = rd_u32(b + 1);
  *item = rd_u16(b + 5);
  return true;
}

// payload after the type byte: [u8 nm][nm x 16][u8 na][na x 12]
size_t proto_enc_mobs(uint8_t *b, const PMob *m, int nm, const PArrow *a, int na) {
  if (nm > P_MOBS_MAX) nm = P_MOBS_MAX;
  if (na > P_ARROWS_MAX) na = P_ARROWS_MAX;
  b[0] = CL_MOBS;
  size_t o = 1;
  b[o++] = (uint8_t)nm;
  for (int i = 0; i < nm; i++) {
    b[o] = m[i].slot; b[o + 1] = m[i].type; b[o + 2] = m[i].flags; b[o + 3] = m[i].yaw256;
    wr_f32(b + o + 4, m[i].x); wr_f32(b + o + 8, m[i].y); wr_f32(b + o + 12, m[i].z);
    o += P_MOB_REC;
  }
  b[o++] = (uint8_t)na;
  for (int i = 0; i < na; i++) {
    wr_f32(b + o, a[i].x); wr_f32(b + o + 4, a[i].y); wr_f32(b + o + 8, a[i].z);
    o += P_ARROW_REC;
  }
  return o;
}

bool proto_dec_sv_mobs(const uint8_t *b, size_t n, uint32_t *id,
                       PMob *m, int *nm, PArrow *a, int *na) {
  if (n < 7 || b[0] != SV_MOBS) return false;
  *id = rd_u32(b + 1);
  size_t o = 5;
  int cm = b[o++];
  if (cm > P_MOBS_MAX || n < o + (size_t)cm * P_MOB_REC + 1) return false;
  for (int i = 0; i < cm; i++) {
    m[i].slot = b[o]; m[i].type = b[o + 1]; m[i].flags = b[o + 2]; m[i].yaw256 = b[o + 3];
    m[i].x = rd_f32(b + o + 4); m[i].y = rd_f32(b + o + 8); m[i].z = rd_f32(b + o + 12);
    o += P_MOB_REC;
  }
  int ca = b[o++];
  if (ca > P_ARROWS_MAX || n < o + (size_t)ca * P_ARROW_REC) return false;
  for (int i = 0; i < ca; i++) {
    a[i].x = rd_f32(b + o); a[i].y = rd_f32(b + o + 4); a[i].z = rd_f32(b + o + 8);
    o += P_ARROW_REC;
  }
  *nm = cm; *na = ca;
  return true;
}

size_t proto_enc_boom(uint8_t *b, int32_t x, uint8_t y, int32_t z) {
  b[0] = CL_BOOM;
  wr_i32(b + 1, x); wr_i32(b + 5, z); b[9] = y;
  return 10;
}

bool proto_dec_sv_boom(const uint8_t *b, size_t n, uint32_t *id, int32_t *x, uint8_t *y, int32_t *z) {
  if (n < 14 || b[0] != SV_BOOM) return false;
  *id = rd_u32(b + 1);
  *x = rd_i32(b + 5); *z = rd_i32(b + 9); *y = b[13];
  return true;
}

size_t proto_enc_mob_hit(uint8_t *b, uint8_t slot, uint8_t dmg, float kx, float kz) {
  b[0] = CL_MOB_HIT; b[1] = slot; b[2] = dmg;
  wr_f32(b + 3, kx); wr_f32(b + 7, kz);
  return 11;
}

bool proto_dec_sv_mob_hit(const uint8_t *b, size_t n, uint32_t *id,
                          uint8_t *slot, uint8_t *dmg, float *kx, float *kz) {
  if (n < 15 || b[0] != SV_MOB_HIT) return false;
  *id = rd_u32(b + 1);
  *slot = b[5]; *dmg = b[6];
  *kx = rd_f32(b + 7); *kz = rd_f32(b + 11);
  return true;
}

bool proto_dec_master(const uint8_t *b, size_t n, bool *is_master) {
  if (n < 2 || b[0] != SV_MASTER) return false;
  *is_master = b[1] != 0;
  return true;
}

size_t proto_enc_chest_req(uint8_t *b, uint8_t type, int32_t x, uint8_t y, int32_t z) {
  b[0] = type;
  wr_i32(b + 1, x); wr_i32(b + 5, z); b[9] = y;
  return 10;
}

size_t proto_enc_chest_set(uint8_t *b, int32_t x, uint8_t y, int32_t z, const PSlot *s) {
  b[0] = CL_CHEST_SET;
  wr_i32(b + 1, x); wr_i32(b + 5, z); b[9] = y;
  for (int i = 0; i < P_CHEST_SLOTS; i++) {
    wr_u16(b + 10 + i * 4, s[i].id);
    wr_u16(b + 12 + i * 4, s[i].count);
  }
  return P_CHEST_SET_SIZE;
}

bool proto_dec_sv_chest(const uint8_t *b, size_t n, int32_t *x, uint8_t *y, int32_t *z,
                        uint8_t *flags, PSlot *s) {
  if (n < P_SV_CHEST_SIZE || b[0] != SV_CHEST) return false;
  *x = rd_i32(b + 1); *z = rd_i32(b + 5); *y = b[9];
  *flags = b[10];
  for (int i = 0; i < P_CHEST_SLOTS; i++) {
    s[i].id = rd_u16(b + 11 + i * 4);
    s[i].count = rd_u16(b + 13 + i * 4);
  }
  return true;
}

size_t proto_enc_furnace_set(uint8_t *b, int32_t x, uint8_t y, int32_t z, const PSlot *s3) {
  b[0] = CL_FURNACE_SET;
  wr_i32(b + 1, x); wr_i32(b + 5, z); b[9] = y;
  for (int i = 0; i < P_FURN_SLOTS; i++) { wr_u16(b + 10 + i * 4, s3[i].id); wr_u16(b + 12 + i * 4, s3[i].count); }
  return P_FURN_SET_SIZE;
}
size_t proto_enc_sv_furnace(uint8_t *b, int32_t x, uint8_t y, int32_t z, uint8_t flags, const PFurn *f) {
  b[0] = SV_FURNACE;
  wr_i32(b + 1, x); wr_i32(b + 5, z); b[9] = y; b[10] = flags;
  const PSlot sl[3] = { f->in, f->fuel, f->out };
  for (int i = 0; i < P_FURN_SLOTS; i++) { wr_u16(b + 11 + i * 4, sl[i].id); wr_u16(b + 13 + i * 4, sl[i].count); }
  wr_f32(b + 23, f->cook); wr_f32(b + 27, f->burn); wr_f32(b + 31, f->burn_max);
  return P_SV_FURN_SIZE;
}
bool proto_dec_furnace_set(const uint8_t *b, size_t n, int32_t *x, uint8_t *y, int32_t *z, PSlot *s3) {
  if (n < P_FURN_SET_SIZE || b[0] != CL_FURNACE_SET) return false;
  *x = rd_i32(b + 1); *z = rd_i32(b + 5); *y = b[9];
  for (int i = 0; i < P_FURN_SLOTS; i++) { s3[i].id = rd_u16(b + 10 + i * 4); s3[i].count = rd_u16(b + 12 + i * 4); }
  return true;
}
bool proto_dec_sv_furnace(const uint8_t *b, size_t n, int32_t *x, uint8_t *y, int32_t *z, uint8_t *flags, PFurn *f) {
  if (n < P_SV_FURN_SIZE || b[0] != SV_FURNACE) return false;
  *x = rd_i32(b + 1); *z = rd_i32(b + 5); *y = b[9]; *flags = b[10];
  f->in.id = rd_u16(b + 11);   f->in.count = rd_u16(b + 13);
  f->fuel.id = rd_u16(b + 15); f->fuel.count = rd_u16(b + 17);
  f->out.id = rd_u16(b + 19);  f->out.count = rd_u16(b + 21);
  f->cook = rd_f32(b + 23); f->burn = rd_f32(b + 27); f->burn_max = rd_f32(b + 31);
  return true;
}

bool proto_dec_sv_chat(const uint8_t *b, size_t n, uint32_t *id, char *out, size_t outsz) {
  if (n < 6 || b[0] != SV_CHAT) return false;
  size_t len = b[5];
  if (n < 6 + len || outsz == 0) return false;
  *id = rd_u32(b + 1);
  if (len >= outsz) len = outsz - 1;
  memcpy(out, b + 6, len);
  out[len] = 0;
  return true;
}
