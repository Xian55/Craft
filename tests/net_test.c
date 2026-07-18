// Codec + interpolation tests. No sockets, no raylib.
// Golden fixtures must match server.js's self-test byte-for-byte.
#include "../src/proto.h"
#include "../src/interp.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok: %s\n", msg); } while (0)

static void hex(const uint8_t *b, size_t n, char *out) {
  for (size_t i = 0; i < n; i++) sprintf(out + i * 2, "%02x", b[i]);
}

int main(void) {
  uint8_t buf[512]; char h[1200];

  // --- golden fixtures (identical constants in server.js test) ---
  PEdit ge = { 5, 7, 6, 3, 0 };
  size_t n = proto_enc_edit(buf, &ge);
  hex(buf, n, h);
  CHECK(strcmp(h, "120500000007000000060300") == 0, "GOLDEN: edit(5,6,7,b3,w0) bytes");

  PPos gp = { 1.5f, 20.0f, -3.25f, 0.5f, -0.25f, 1.0f, 0.0f, -1.0f };
  n = proto_enc_pos(buf, &gp);
  hex(buf, n, h);
  CHECK(strcmp(h, "100000c03f0000a041000050c00000003f000080be0000803f00000000000080bf") == 0,
        "GOLDEN: pos bytes match server fixture");

  // welcome decode from server-encoded golden bytes: 02 02 07000000 0000003f
  const uint8_t gw[] = { 0x02, 0x02, 0x07, 0, 0, 0, 0x00, 0x00, 0x00, 0x3f };
  uint8_t ver; uint32_t id; float t;
  CHECK(proto_dec_welcome(gw, sizeof gw, &ver, &id, &t) && ver == 2 && id == 7 && fabsf(t - 0.5f) < 1e-6,
        "GOLDEN: welcome decodes to (ver2, id7, t0.5)");

  const uint8_t gl[] = { 0x30, 0x09, 0, 0, 0 };
  CHECK(proto_dec_leave(gl, sizeof gl, &id) && id == 9, "GOLDEN: leave(9)");

  // held item: enc golden + relay decode (server fixture: 17 03000000 0700)
  n = proto_enc_held(buf, 7);
  hex(buf, n, h);
  CHECK(strcmp(h, "160700") == 0, "GOLDEN: held(7) bytes");
  const uint8_t gh[] = { 0x17, 0x03, 0, 0, 0, 0x07, 0x00 };
  uint16_t hitem;
  CHECK(proto_dec_sv_held(gh, sizeof gh, &id, &hitem) && id == 3 && hitem == 7,
        "GOLDEN: sv_held relay decodes (sender 3, item 7)");
  CHECK(!proto_dec_sv_held(gh, 6, &id, &hitem), "truncated sv_held rejected");

  // chat: enc golden + relay decode (server fixture: 19 03000000 02 6869)
  n = proto_enc_chat(buf, "hi");
  hex(buf, n, h);
  CHECK(strcmp(h, "18026869") == 0, "GOLDEN: chat(\"hi\") bytes");
  const uint8_t gc[] = { 0x19, 0x03, 0, 0, 0, 0x02, 'h', 'i' };
  char cmsg[124];
  CHECK(proto_dec_sv_chat(gc, sizeof gc, &id, cmsg, sizeof cmsg) && id == 3 && strcmp(cmsg, "hi") == 0,
        "GOLDEN: sv_chat relay decodes (sender 3, \"hi\")");
  CHECK(!proto_dec_sv_chat(gc, 7, &id, cmsg, sizeof cmsg), "truncated sv_chat rejected");
  char tiny[3];
  CHECK(proto_dec_sv_chat(gc, sizeof gc, &id, tiny, sizeof tiny) && strcmp(tiny, "hi") == 0,
        "sv_chat clamps to the output buffer");

  // mob sync codecs
  n = proto_enc_boom(buf, 5, 6, 7);
  hex(buf, n, h);
  CHECK(strcmp(h, "1c" "05000000" "07000000" "06") == 0, "GOLDEN: boom(5,6,7) bytes");
  const uint8_t gb[] = { 0x1D, 0x03, 0, 0, 0, 0x05, 0, 0, 0, 0x07, 0, 0, 0, 0x06 };
  int32_t bx, bz; uint8_t by;
  CHECK(proto_dec_sv_boom(gb, sizeof gb, &id, &bx, &by, &bz) && id == 3 && bx == 5 && by == 6 && bz == 7,
        "sv_boom relay decodes");
  n = proto_enc_mob_hit(buf, 4, 3, 1.0f, -1.0f);
  CHECK(n == 11 && buf[0] == CL_MOB_HIT && buf[1] == 4 && buf[2] == 3, "mob_hit encodes");
  uint8_t svh[15]; svh[0] = SV_MOB_HIT; svh[1] = 9; svh[2] = svh[3] = svh[4] = 0;
  memcpy(svh + 5, buf + 1, 10);
  uint8_t hslot, hdmg; float kx, kz;
  CHECK(proto_dec_sv_mob_hit(svh, sizeof svh, &id, &hslot, &hdmg, &kx, &kz)
        && id == 9 && hslot == 4 && hdmg == 3 && kx == 1.0f && kz == -1.0f,
        "mob_hit roundtrip through the relay");
  const uint8_t gm[] = { 0x32, 0x01 };
  bool is_m;
  CHECK(proto_dec_master(gm, 2, &is_m) && is_m, "master flag decodes");
  PMob pm1 = { 7, 2, 1, 128, 1.5f, 20.0f, -3.25f }, pm2[P_MOBS_MAX];
  PArrow pa1 = { 4.0f, 5.0f, 6.0f }, pa2[P_ARROWS_MAX];
  n = proto_enc_mobs(buf, &pm1, 1, &pa1, 1);
  CHECK(n == 1 + 1 + 16 + 1 + 12, "mobs snapshot size (1 mob, 1 arrow)");
  uint8_t svm[64]; svm[0] = SV_MOBS; svm[1] = 3; svm[2] = svm[3] = svm[4] = 0;
  memcpy(svm + 5, buf + 1, n - 1);
  int nm2, na2;
  CHECK(proto_dec_sv_mobs(svm, 5 + n - 1, &id, pm2, &nm2, pa2, &na2)
        && id == 3 && nm2 == 1 && na2 == 1
        && pm2[0].slot == 7 && pm2[0].type == 2 && pm2[0].flags == 1 && pm2[0].yaw256 == 128
        && pm2[0].x == 1.5f && pm2[0].z == -3.25f && pa2[0].y == 5.0f,
        "mobs snapshot roundtrip through the relay");
  CHECK(!proto_dec_sv_mobs(svm, 5 + n - 2, &id, pm2, &nm2, pa2, &na2), "truncated mobs snapshot rejected");

  // chest codecs
  n = proto_enc_chest_req(buf, CL_CHEST_GET, 7, 20, -3);
  hex(buf, n, h);
  CHECK(strcmp(h, "22" "07000000" "fdffffff" "14") == 0, "GOLDEN: chest_get(7,20,-3) bytes");
  PSlot cs[P_CHEST_SLOTS]; memset(cs, 0, sizeof cs);
  cs[0] = (PSlot){ 8, 64 }; cs[26] = (PSlot){ 22, 1 };
  n = proto_enc_chest_set(buf, 7, 20, -3, cs);
  CHECK(n == P_CHEST_SET_SIZE && buf[0] == CL_CHEST_SET, "chest_set encodes to 118 bytes");
  uint8_t svc[P_SV_CHEST_SIZE];
  svc[0] = SV_CHEST;
  memcpy(svc + 1, buf + 1, 9);
  svc[10] = 1;
  memcpy(svc + 11, buf + 10, P_CHEST_SLOTS * 4);
  int32_t cx, cz2; uint8_t cy, cfl;
  PSlot cs2[P_CHEST_SLOTS];
  CHECK(proto_dec_sv_chest(svc, sizeof svc, &cx, &cy, &cz2, &cfl, cs2)
        && cx == 7 && cy == 20 && cz2 == -3 && cfl == 1
        && cs2[0].id == 8 && cs2[0].count == 64 && cs2[1].id == 0 && cs2[26].id == 22,
        "sv_chest roundtrip (27 slots incl. empties)");
  CHECK(!proto_dec_sv_chest(svc, sizeof svc - 1, &cx, &cy, &cz2, &cfl, cs2), "truncated sv_chest rejected");

  // furnace codecs (raw ids: 16=iron ore, 28=coal, 29=iron ingot)
  n = proto_enc_chest_req(buf, CL_FURNACE_GET, 4, 30, -2);
  hex(buf, n, h);
  CHECK(strcmp(h, "28" "04000000" "feffffff" "1e") == 0, "GOLDEN: furnace_get(4,30,-2) bytes");
  PSlot fs3[3] = { { 16, 5 }, { 28, 3 }, { 29, 2 } };
  n = proto_enc_furnace_set(buf, 4, 30, -2, fs3);
  CHECK(n == P_FURN_SET_SIZE && buf[0] == CL_FURNACE_SET, "furnace_set encodes to 22 bytes");
  int32_t fx, fz; uint8_t fy; PSlot fs2[3];
  CHECK(proto_dec_furnace_set(buf, n, &fx, &fy, &fz, fs2)
        && fx == 4 && fy == 30 && fz == -2
        && fs2[0].id == 16 && fs2[0].count == 5 && fs2[1].id == 28 && fs2[2].id == 29 && fs2[2].count == 2,
        "furnace_set roundtrip");
  PFurn pf = { { 16, 5 }, { 28, 3 }, { 29, 2 }, 2.0f, 4.0f, 8.0f };
  n = proto_enc_sv_furnace(buf, 4, 30, -2, 0, &pf);
  CHECK(n == P_SV_FURN_SIZE, "sv_furnace encodes to 35 bytes");
  int32_t gx, gz; uint8_t gy, gfl; PFurn pf2;
  CHECK(proto_dec_sv_furnace(buf, n, &gx, &gy, &gz, &gfl, &pf2)
        && gx == 4 && gy == 30 && gz == -2 && gfl == 0
        && pf2.in.id == 16 && pf2.in.count == 5 && pf2.fuel.id == 28 && pf2.out.count == 2
        && pf2.cook == 2.0f && pf2.burn == 4.0f && pf2.burn_max == 8.0f,
        "sv_furnace roundtrip (slots + cook/burn/burn_max)");
  CHECK(!proto_dec_sv_furnace(buf, n - 1, &gx, &gy, &gz, &gfl, &pf2), "truncated sv_furnace rejected");

  // --- roundtrips ---
  n = proto_enc_hello(buf, "c-abc123");
  CHECK(n == 11 && buf[0] == CL_HELLO && buf[1] == PROTO_VER && buf[2] == 8
        && memcmp(buf + 3, "c-abc123", 8) == 0, "hello encodes ver+len+uid");

  PEdit e2 = { -100, -200, 63, 8, 9 }, e3;
  proto_enc_edit(buf, &e2);
  // re-read through the SV_EDIT path (server prepends type+id; body identical)
  uint8_t sv[16]; sv[0] = SV_EDIT; sv[1] = 42; sv[2] = sv[3] = sv[4] = 0;
  memcpy(sv + 5, buf + 1, 11);
  uint32_t sid;
  CHECK(proto_dec_sv_edit(sv, 16, &sid, &e3) && sid == 42
        && e3.x == -100 && e3.z == -200 && e3.y == 63 && e3.block == 8 && e3.water == 9,
        "edit roundtrip with negative coords + y=63");

  PPos p1 = { -12345.5f, 63.0f, 99999.25f, 3.1f, -1.5f, -26.0f, 8.5f, 0.001f }, p2;
  proto_enc_pos(buf, &p1);
  uint8_t svp[37]; svp[0] = SV_POS;
  svp[1] = 7; svp[2] = svp[3] = svp[4] = 0;
  memcpy(svp + 5, buf + 1, 32);
  CHECK(proto_dec_sv_pos(svp, 37, &sid, &p2) && sid == 7
        && p2.x == p1.x && p2.z == p1.z && p2.pitch == p1.pitch && p2.vy == p1.vy,
        "pos roundtrip preserves all fields");

  PState s1 = {0}, s2;
  s1.x = 11; s1.y = 22; s1.z = 33; s1.yaw = 0.7f;
  s1.health = 13; s1.hunger = 17; s1.sel_slot = 2;
  s1.slots[0] = (PSlot){ 25, 3 };
  s1.slots[35] = (PSlot){ 300, 64 };
  n = proto_enc_save(buf, &s1);
  CHECK(n == P_STATE_SIZE, "state encodes to 164 bytes");
  buf[0] = SV_RESTORE;   // restore body is identical
  CHECK(proto_dec_state(buf, n, &s2)
        && s2.health == 13 && s2.sel_slot == 2
        && s2.slots[0].id == 25 && s2.slots[0].count == 3
        && s2.slots[1].id == 0 && s2.slots[35].id == 300 && s2.slots[35].count == 64,
        "state roundtrip: 36 slots incl. empties");

  // edits batch: build 3 records by hand, decode
  uint8_t eb[3 + 33]; eb[0] = SV_EDITS; eb[1] = 3; eb[2] = 0;
  for (int i = 0; i < 3; i++) {
    PEdit rec = { i * 10 - 5, i * -7, (uint8_t)(i + 1), (uint8_t)(i + 2), 0 };
    uint8_t tmp[12]; proto_enc_edit(tmp, &rec);
    memcpy(eb + 3 + i * 11, tmp + 1, 11);
  }
  CHECK(proto_dec_edits_count(eb, sizeof eb) == 3, "edits batch count");
  PEdit r1;
  proto_edits_record(eb, 1, &r1);
  CHECK(r1.x == 5 && r1.z == -7 && r1.y == 2 && r1.block == 3, "edits record 1 fields");
  CHECK(proto_dec_edits_count(eb, 10) == -1, "truncated batch rejected");

  // ping/pong
  n = proto_enc_ping(buf, 12345.5);
  buf[0] = SV_PONG;
  double pt;
  CHECK(proto_dec_pong(buf, n, &pt) && pt == 12345.5, "ping/pong f64 roundtrip");

  // --- interpolation ---
  SnapRing r = {0};
  Snap a = { 1.000, 0, 0, 0, 0.1f, 0, 1, 0, 0 };
  Snap b = { 1.050, 1, 0, 0, 0.2f, 0, 1, 0, 0 };
  interp_push(&r, &a); interp_push(&r, &b);
  Snap out;
  // sample time 1.025 => now = 1.145 (delay 0.120); u = 0.5
  CHECK(interp_sample(&r, 1.145, &out) && fabsf(out.x - 0.5f) < 1e-5
        && fabsf(out.yaw - 0.15f) < 1e-5, "bracket lerp at u=0.5");

  // yaw wrap: 350deg -> 10deg must pass through 0, not 180
  SnapRing r2 = {0};
  Snap w1 = { 1.000, 0, 0, 0, 6.1087f /*350deg*/, 0, 0, 0, 0 };
  Snap w2 = { 1.100, 0, 0, 0, 0.1745f /*10deg*/, 0, 0, 0, 0 };
  interp_push(&r2, &w1); interp_push(&r2, &w2);
  CHECK(interp_sample(&r2, 1.170, &out), "yaw-wrap sample ok");   // t=1.05, u=0.5
  // midpoint of 350->10 through zero is 0 (mod 2pi) -> cos(yaw) ~ 1
  CHECK(fabsf(cosf(out.yaw) - 1.0f) < 1e-3, "yaw wraps through 0 (midpoint ~0 rad)");

  // extrapolation: past newest, position advances with velocity, clamped
  CHECK(interp_sample(&r, 1.270, &out) && fabsf(out.x - (1.0f + 1.0f * 0.1f)) < 1e-4,
        "extrapolates x + v*dt past newest snap");
  CHECK(interp_sample(&r, 3.000, &out) && out.x <= 1.0f + 1.0f * (float)EXTRAP_MAX + 1e-4,
        "extrapolation clamped at EXTRAP_MAX");

  // single snapshot: hold
  SnapRing r3 = {0};
  Snap only = { 5.0, 9, 8, 7, 0, 0, 0, 0, 0 };
  interp_push(&r3, &only);
  CHECK(interp_sample(&r3, 5.05, &out) && out.x == 9 && out.y == 8, "single snapshot holds");
  SnapRing r4 = {0};
  CHECK(!interp_sample(&r4, 1.0, &out), "empty ring returns false");

  printf(fails ? "NET TEST: %d FAILURES\n" : "NET TEST: all passed\n", fails);
  return fails ? 1 : 0;
}
