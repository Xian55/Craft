// v2 binary protocol e2e test: the C server (craft --server) + fake clients,
// over real sockets. Run: bun tools/e2e_net.mjs   (from the project directory)
// Uses its own temporary port and world dir; does not touch your world.
import { spawn } from 'child_process';
import { mkdtempSync, rmSync } from 'fs';
import { tmpdir } from 'os';
import { join, dirname } from 'path';
import { fileURLToPath } from 'url';

const ROOT = dirname(dirname(fileURLToPath(import.meta.url)));
const PORT = 18942;
const PROTO = 2;
const CL = { HELLO: 0x01, SETTIME: 0x04, POS: 0x10, EDIT: 0x12, HELD: 0x16, CHAT: 0x18, MOBS: 0x1A, BOOM: 0x1C, MOB_HIT: 0x1E, SAVE: 0x20, PING: 0x40 };
const SV = { WELCOME: 0x02, TIME: 0x03, REJECT: 0x0F, POS: 0x11, EDIT: 0x13, EDITS: 0x14, HELD: 0x17, CHAT: 0x19, MOBS: 0x1B, BOOM: 0x1D, MOB_HIT: 0x1F, RESTORE: 0x21, LEAVE: 0x30, MASTER: 0x32, PONG: 0x41 };

let fails = 0;
const check = (c, msg) => { console.log((c ? 'ok: ' : 'FAIL: ') + msg); if (!c) fails++; };
const sleep = (ms) => new Promise(r => setTimeout(r, ms));

function hello(uid, ver = PROTO) {
  const u = Buffer.from(uid);
  return Buffer.concat([Buffer.from([CL.HELLO, ver, u.length]), u]);
}
function pos(x, y, z, yaw = 0, pitch = 0, vx = 0, vy = 0, vz = 0) {
  const b = Buffer.alloc(33); b[0] = CL.POS;
  [x, y, z, yaw, pitch, vx, vy, vz].forEach((v, i) => b.writeFloatLE(v, 1 + i * 4));
  return b;
}
function edit(x, y, z, bl, w) {
  const b = Buffer.alloc(12); b[0] = CL.EDIT;
  b.writeInt32LE(x, 1); b.writeInt32LE(z, 5); b[9] = y; b[10] = bl; b[11] = w;
  return b;
}
function ping() { const b = Buffer.alloc(9); b[0] = CL.PING; b.writeDoubleLE(Date.now(), 1); return b; }

// Fake client: WebSocket + message collector, by type.
function client(name) {
  const ws = new WebSocket(`ws://localhost:${PORT}`);
  ws.binaryType = 'arraybuffer';
  const c = { ws, name, msgs: [], open: false, closed: false, pinger: null };
  ws.onopen = () => { c.open = true; };
  ws.onclose = () => { c.closed = true; if (c.pinger) clearInterval(c.pinger); };
  ws.onmessage = (ev) => { c.msgs.push(Buffer.from(ev.data)); };
  c.send = (b) => ws.send(b);
  c.of = (type) => c.msgs.filter(m => m[0] === type);
  c.startPing = () => { c.pinger = setInterval(() => { if (c.open && !c.closed) c.send(ping()); }, 2000); };
  return c;
}
async function waitFor(fn, ms, step = 50) {
  const t0 = Date.now();
  while (Date.now() - t0 < ms) { if (fn()) return true; await sleep(step); }
  return fn();
}

// --- starting the C server with a temporary data directory ---
const dir = mkdtempSync(join(tmpdir(), 'craft-e2e-'));
const exe = join(ROOT, '..', 'craft_raylib_build', 'native', process.platform === 'win32' ? 'craft.exe' : 'craft');
const server = spawn(exe, ['--server'], { env: { ...process.env, PORT: String(PORT), CRAFT_DATA: dir }, stdio: 'pipe' });
await sleep(800);

try {
  // 1) join order: WELCOME, TIME (empty log: no EDITS), no RESTORE
  const A = client('A');
  await waitFor(() => A.open, 3000);
  A.send(hello('uid-A'));
  await waitFor(() => A.of(SV.TIME).length > 0, 2000);
  check(A.msgs[0][0] === SV.WELCOME, 'join: first frame is WELCOME');
  check(A.msgs[0][1] === PROTO, 'welcome carries protocol version');
  const aId = A.msgs[0].readUInt32LE(2);
  check(A.msgs[1][0] === SV.TIME, 'join: second frame is TIME');
  A.startPing();

  // 2) wrong version -> REJECT
  const OLD = client('old');
  await waitFor(() => OLD.open, 3000);
  OLD.send(hello('uid-old', 1));
  await waitFor(() => OLD.of(SV.REJECT).length > 0 || OLD.closed, 2000);
  check(OLD.of(SV.REJECT).length === 1, 'wrong version gets REJECT');

  // 3) B joins, A moves at 20 Hz for ~1.5 s -> B receives POS frames with A's id
  const B = client('B');
  await waitFor(() => B.open, 3000);
  B.send(hello('uid-B'));
  await waitFor(() => B.of(SV.WELCOME).length > 0, 2000);
  B.startPing();
  const t0 = Date.now();
  let sent = 0;
  while (Date.now() - t0 < 1500) { A.send(pos(10 + sent * 0.1, 20, 30, 0.5, -0.2, 2, 0, 0)); sent++; await sleep(50); }
  await sleep(300);
  const bPos = B.of(SV.POS);
  const hz = bPos.length / 1.5;
  check(bPos.length > 0 && bPos.every(m => m.readUInt32LE(1) === aId), 'B receives POS frames with A\'s id');
  check(hz >= 15, `pos relay rate >= 15 Hz (got ${hz.toFixed(1)})`);
  const lastP = bPos[bPos.length - 1];
  check(Math.abs(lastP.readFloatLE(25) - 2) < 1e-6, 'pos velocity field survives the relay');

  // 3b) A announces held item -> B receives it with A's id
  const held = Buffer.alloc(3); held[0] = CL.HELD; held.writeUInt16LE(11, 1);
  A.send(held);
  await waitFor(() => B.of(SV.HELD).length > 0, 2000);
  const bh = B.of(SV.HELD)[0];
  check(bh.readUInt32LE(1) === aId && bh.readUInt16LE(5) === 11, 'held item relayed to B with A\'s id');

  // 3b2) mob authority: A (oldest) is master, B is not; only A's snapshots relay
  await waitFor(() => A.of(SV.MASTER).length > 0 && B.of(SV.MASTER).length > 0, 2000);
  check(A.of(SV.MASTER).some(m => m[1] === 1), 'oldest client elected mob master');
  check(B.of(SV.MASTER).every(m => m[1] === 0), 'second client is not master');
  const mobSnap = Buffer.alloc(2 + 16 + 1);   // 1 zombie, 0 arrows
  mobSnap[0] = CL.MOBS; mobSnap[1] = 1;
  mobSnap[2] = 5; mobSnap[3] = 0; mobSnap[4] = 0; mobSnap[5] = 64;
  mobSnap.writeFloatLE(10, 6); mobSnap.writeFloatLE(20, 10); mobSnap.writeFloatLE(30, 14);
  mobSnap[18] = 0;
  B.send(mobSnap);                            // non-master: must be dropped
  A.send(mobSnap);
  await waitFor(() => B.of(SV.MOBS).length > 0, 2000);
  const ms = B.of(SV.MOBS)[0];
  check(ms.readUInt32LE(1) === aId && ms[5] === 1 && ms[6] === 5 && ms.readFloatLE(10) === 10,
    'master mob snapshot relayed to B');
  check(A.of(SV.MOBS).length === 0, 'non-master mob snapshot dropped');
  const boom = Buffer.alloc(10); boom[0] = CL.BOOM;
  boom.writeInt32LE(-5, 1); boom.writeInt32LE(9, 5); boom[9] = 30;
  A.send(boom);
  await waitFor(() => B.of(SV.BOOM).length > 0, 2000);
  const bb = B.of(SV.BOOM)[0];
  check(bb.readUInt32LE(1) === aId && bb.readInt32LE(5) === -5 && bb[13] === 30, 'boom relayed to B');
  const hit = Buffer.alloc(11); hit[0] = CL.MOB_HIT; hit[1] = 5; hit[2] = 4;
  hit.writeFloatLE(1, 3); hit.writeFloatLE(0, 7);
  B.send(hit);                                // non-master reports a hit
  await waitFor(() => A.of(SV.MOB_HIT).length > 0, 2000);
  check(A.of(SV.MOB_HIT)[0][5] === 5 && A.of(SV.MOB_HIT)[0][6] === 4, 'mob hit relayed to the master');

  // 3c) A chats -> B receives the text with A's id
  const txt = Buffer.from('szia bela');
  A.send(Buffer.concat([Buffer.from([CL.CHAT, txt.length]), txt]));
  await waitFor(() => B.of(SV.CHAT).length > 0, 2000);
  const bc = B.of(SV.CHAT)[0];
  check(bc.readUInt32LE(1) === aId && bc[5] === txt.length
    && bc.slice(6, 6 + txt.length).toString() === 'szia bela', 'chat relayed to B with A\'s id');

  // 4) A edits -> B receives it; C joins later -> it's in the log replay
  A.send(edit(-42, 33, 77, 8, 0));
  await waitFor(() => B.of(SV.EDIT).length > 0, 2000);
  const be = B.of(SV.EDIT)[0];
  check(be.readInt32LE(5) === -42 && be[13] === 33 && be[14] === 8, 'edit relayed to B with correct fields');
  const C = client('C');
  await waitFor(() => C.open, 3000);
  C.send(hello('uid-C'));
  await waitFor(() => C.of(SV.EDITS).length > 0, 2000);
  const ce = C.of(SV.EDITS)[0];
  check(ce.readUInt16LE(1) === 1 && ce.readInt32LE(3) === -42 && ce[3 + 8] === 33, 'late joiner gets the edit in replay');
  C.ws.close();

  // 4b) chest contents: A writes, B fetches; break returns + clears them
  const CH = { GET: 0x22, SET: 0x24, BREAK: 0x26, SV: 0x23 };
  const cpos = (t) => { const b = Buffer.alloc(10); b[0] = t; b.writeInt32LE(7, 1); b.writeInt32LE(-3, 5); b[9] = 20; return b; };
  const cset = Buffer.alloc(118); cset[0] = CH.SET;
  cset.writeInt32LE(7, 1); cset.writeInt32LE(-3, 5); cset[9] = 20;
  cset.writeUInt16LE(8, 10); cset.writeUInt16LE(64, 12);       // slot0: 64 cobble
  cset.writeUInt16LE(22, 10 + 26 * 4); cset.writeUInt16LE(1, 12 + 26 * 4);  // slot26: sword
  A.send(cset);                               // note: SET also broadcasts to B
  await sleep(200);
  let cbase = B.of(CH.SV).length;
  B.send(cpos(CH.GET));
  await waitFor(() => B.of(CH.SV).length > cbase, 2000);
  const cv = B.of(CH.SV)[cbase];
  check(cv.readInt32LE(1) === 7 && cv[9] === 20 && cv[10] === 0
    && cv.readUInt16LE(11) === 8 && cv.readUInt16LE(13) === 64
    && cv.readUInt16LE(11 + 26 * 4) === 22, 'chest contents stored and fetched by another player');
  cbase = B.of(CH.SV).length;
  B.send(cpos(CH.BREAK));
  await waitFor(() => B.of(CH.SV).length > cbase, 2000);
  const cb2 = B.of(CH.SV)[cbase];
  check(cb2[10] === 1 && cb2.readUInt16LE(11) === 8, 'chest break returns contents with the broken flag');
  cbase = B.of(CH.SV).length;
  B.send(cpos(CH.GET));
  await waitFor(() => B.of(CH.SV).length > cbase, 2000);
  check(B.of(CH.SV)[cbase].readUInt16LE(11) === 0, 'broken chest is forgotten by the server');

  // 5) SAVE -> reconnect -> RESTORE with the same state
  const save = Buffer.alloc(164); save[0] = CL.SAVE;
  save.writeFloatLE(11, 1); save.writeFloatLE(22, 5); save.writeFloatLE(33, 9); save.writeFloatLE(0.7, 13);
  save[17] = 13; save[18] = 17; save[19] = 2;
  save.writeUInt16LE(25, 20); save.writeUInt16LE(3, 22);   // slot0: pork x3
  B.send(save);
  await sleep(300);
  B.ws.close();
  await sleep(300);
  const B2 = client('B2');
  await waitFor(() => B2.open, 3000);
  B2.send(hello('uid-B'));
  await waitFor(() => B2.of(SV.RESTORE).length > 0, 2000);
  const rs = B2.of(SV.RESTORE)[0];
  check(rs[17] === 13 && rs[19] === 2 && rs.readUInt16LE(20) === 25 && rs.readUInt16LE(22) === 3,
    'reconnect restores the saved state');
  B2.startPing();

  // 6) heartbeat: A stops pinging -> the server closes it, B2 gets LEAVE
  if (A.pinger) clearInterval(A.pinger);
  const gotLeave = await waitFor(() =>
    B2.of(SV.LEAVE).some(m => m.readUInt32LE(1) === aId), 16000, 250);
  check(gotLeave, 'silent client is closed and LEAVE broadcast (heartbeat)');

  // 7) ping-pong echo
  const before = B2.of(SV.PONG).length;
  B2.send(ping());
  await waitFor(() => B2.of(SV.PONG).length > before, 2000);
  check(B2.of(SV.PONG).length > before, 'ping echoes pong');

  B2.ws.close();
} finally {
  server.kill();
  await sleep(200);
  rmSync(dir, { recursive: true, force: true });
}

console.log(fails ? `E2E NET: ${fails} FAILURES` : 'E2E NET: all passed');
process.exit(fails ? 1 : 0);
