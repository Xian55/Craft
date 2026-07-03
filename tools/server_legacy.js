// RETIRED: the server now lives in C inside the game binary (src/server.c,
// `craft --server`). Kept as a reference implementation and because this
// version can still migrate a pre-split legacy world.json (the C server
// only reads the split world.edits / players.json / world.meta.json).
//
// Craft Survival – multiplayer server (BINARY v2 protocol).
//
// What does it do? A very simple "mailman": whatever one player does
// (breaking/placing a block, moving) gets forwarded to the other players.
// It does NOT store the world – every machine regenerates it identically
// on its own (the terrain is deterministic). Only CHANGES and movement
// are distributed.
//
// The v2 protocol uses BINARY websocket frames (little-endian, first byte
// is the type) – small messages, 20 Hz movement. The old JSON client
// (game.js) does NOT understand this; the C/wasm client (craft_raylib)
// is the current one.
//
// Start:  bun server.js   (or: node server.js)
// Serving the wasm build on the same port: STATIC=<dir> bun server.js
// Self-test: node server.js test  /  bun server.js test
//
// Persistence is split by concern:
//   world.edits      binary block-edit log ("KEDT" v1: magic, u8 version,
//                    u32le count, then count 11-byte records — same record
//                    layout as the SV_EDITS wire batches)
//   players.json     per-uid saved player states (kept as JSON on purpose:
//                    handy to inspect/fix a player's bag by hand)
//   world.meta.json  world-level state (time of day)
// A legacy single-file world.json is migrated on first load and kept as
// world.json.bak.

const http = require('http');
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

const PORT = process.env.PORT || 8080;
// Where the world files live. CRAFT_DATA env wins (docker volume); in a
// compiled executable (bun build --compile) __dirname is the baked-in
// compile-time source dir, not where the exe lives — persist next to the real
// exe instead. Bun.main carries the embedded-bundle marker (/$bunfs/ or /~BUN/).
const BUNDLED = typeof Bun !== 'undefined' && (Bun.main.includes('/$bunfs/') || Bun.main.includes('/~BUN/'));
const DATA_DIR = process.env.CRAFT_DATA || (BUNDLED ? path.dirname(process.execPath) : __dirname);
const EDITS_FILE   = DATA_DIR + '/world.edits';
const PLAYERS_FILE = DATA_DIR + '/players.json';
const META_FILE    = DATA_DIR + '/world.meta.json';
const LEGACY_FILE  = DATA_DIR + '/world.json';
const clients = new Map();          // socket -> id
const socketUid = new Map();        // socket -> uid (persistent player identifier)
const lastSeen = new Map();         // socket -> time of last message (heartbeat)
let nextId = 1;
let dirty = false;                  // any unsaved changes?
// Log of block edits per cell; late joiners get it replayed.
const editLog = new Map();          // "x,y,z" -> {t:'edit',x,y,z,b,w,id}
const players = new Map();          // uid -> {x,y,z,yaw,health,hunger,selSlot,slots}

// --- Shared time of day: the server is the authoritative clock. (Same cycle as game.js.) ---
const DAY_LENGTH = 300;
let timeBase = 0.12, timeBaseAt = Date.now();
function serverTime() { return (timeBase + (Date.now() - timeBaseAt) / 1000 / DAY_LENGTH) % 1; }
function setServerTime(v) { timeBase = ((v % 1) + 1) % 1; timeBaseAt = Date.now(); }

// --- Save/load to disk: world.edits (binary) + players.json + world.meta.json ---
const EDITS_MAGIC = 'KEDT', EDITS_VER = 1;

function encodeEditsFile() {
  const all = [...editLog.values()];
  const b = Buffer.alloc(9 + all.length * 11);
  b.write(EDITS_MAGIC, 0, 'ascii'); b[4] = EDITS_VER; b.writeUInt32LE(all.length, 5);
  for (let k = 0; k < all.length; k++) {
    const e = all[k], o = 9 + k * 11;
    b.writeInt32LE(e.x, o); b.writeInt32LE(e.z, o + 4);
    b[o + 8] = e.y; b[o + 9] = e.b; b[o + 10] = e.w;
  }
  return b;
}
function decodeEditsFile(b) {
  if (b.length < 9 || b.toString('ascii', 0, 4) !== EDITS_MAGIC || b[4] !== EDITS_VER) return false;
  const count = b.readUInt32LE(5);
  if (b.length < 9 + count * 11) return false;
  for (let k = 0; k < count; k++) {
    const o = 9 + k * 11;
    const x = b.readInt32LE(o), z = b.readInt32LE(o + 4), y = b[o + 8], bl = b[o + 9], w = b[o + 10];
    editLog.set(x + ',' + y + ',' + z, { t: 'edit', x, y, z, b: bl, w, id: 0 });
  }
  return true;
}

function writeAtomic(file, data) {
  const tmp = file + '.tmp';
  fs.writeFileSync(tmp, data);
  fs.renameSync(tmp, file);
}

function loadWorld() {
  if (fs.existsSync(EDITS_FILE)) {
    try {
      if (!decodeEditsFile(fs.readFileSync(EDITS_FILE))) console.error('world.edits: bad magic/version or truncated — ignored');
    } catch (e) { console.error('world.edits unreadable:', e.message); }
    try { for (const [uid, st] of Object.entries(JSON.parse(fs.readFileSync(PLAYERS_FILE, 'utf8')))) players.set(uid, st); } catch (e) {}
    try { const m = JSON.parse(fs.readFileSync(META_FILE, 'utf8')); if (typeof m.time === 'number') setServerTime(m.time); } catch (e) {}
    console.log('World loaded: %d edits, %d players.', editLog.size, players.size);
    return;
  }
  // legacy single-file world.json -> migrate to the split format once
  try {
    const d = JSON.parse(fs.readFileSync(LEGACY_FILE, 'utf8'));
    for (const e of d.edits || []) editLog.set(e.x + ',' + e.y + ',' + e.z, e);
    for (const [uid, st] of Object.entries(d.players || {})) players.set(uid, st);
    if (typeof d.time === 'number') setServerTime(d.time);
    dirty = true;
    saveWorld();
    fs.renameSync(LEGACY_FILE, LEGACY_FILE + '.bak');
    console.log('Migrated world.json -> world.edits + players.json + world.meta.json (backup: world.json.bak)');
  } catch (e) { /* no save yet – fresh world */ }
}
function saveWorld() {
  if (!dirty) return;
  dirty = false;
  try {
    writeAtomic(EDITS_FILE, encodeEditsFile());
    writeAtomic(PLAYERS_FILE, JSON.stringify(Object.fromEntries(players)));
    writeAtomic(META_FILE, JSON.stringify({ time: serverTime() }));
  } catch (e) { console.error('Save failed:', e.message); dirty = true; }
}

// ============================================================================
//  BINARY v2 PROTOCOL (little-endian, buf[0] = type)
// ============================================================================
const PROTO = 2;
// client -> server
const CL_HELLO = 0x01, CL_SETTIME = 0x04, CL_POS = 0x10, CL_EDIT = 0x12, CL_HELD = 0x16,
      CL_CHAT = 0x18, CL_SAVE = 0x20, CL_PING = 0x40;
// server -> client
const SV_WELCOME = 0x02, SV_TIME = 0x03, SV_REJECT = 0x0F, SV_POS = 0x11, SV_EDIT = 0x13,
      SV_EDITS = 0x14, SV_HELD = 0x17, SV_CHAT = 0x19, SV_RESTORE = 0x21, SV_LEAVE = 0x30, SV_PONG = 0x41;

const INV_SIZE = 36;
const STATE_SIZE = 1 + 16 + 3 + INV_SIZE * 4;   // type + 4×f32 + 3×u8 + 36×(u16,u16) = 164

function encWelcome(id, time) {
  const b = Buffer.alloc(10);
  b[0] = SV_WELCOME; b[1] = PROTO;
  b.writeUInt32LE(id, 2); b.writeFloatLE(time, 6);
  return b;
}
function encTime(v) { const b = Buffer.alloc(5); b[0] = SV_TIME; b.writeFloatLE(v, 1); return b; }
function encReject() { const b = Buffer.alloc(2); b[0] = SV_REJECT; b[1] = PROTO; return b; }
function encLeave(id) { const b = Buffer.alloc(5); b[0] = SV_LEAVE; b.writeUInt32LE(id, 1); return b; }
function encPong(t) { const b = Buffer.alloc(9); b[0] = SV_PONG; b.writeDoubleLE(t, 1); return b; }
// POS/EDIT relay: SV type + sender id + the client message body unchanged.
function encRelay(svType, senderId, clBody) {
  const b = Buffer.alloc(5 + clBody.length);
  b[0] = svType; b.writeUInt32LE(senderId, 1);
  clBody.copy(b, 5);
  return b;
}
// Player state (SAVE/RESTORE body) <-> the object stored in world.json.
function encodeState(st, type) {
  const b = Buffer.alloc(STATE_SIZE);
  b[0] = type;
  b.writeFloatLE(st.x || 0, 1); b.writeFloatLE(st.y || 0, 5);
  b.writeFloatLE(st.z || 0, 9); b.writeFloatLE(st.yaw || 0, 13);
  b[17] = st.health ?? 20; b[18] = st.hunger ?? 20; b[19] = st.selSlot || 0;
  const slots = st.slots || [];
  for (let i = 0; i < INV_SIZE; i++) {
    const s = slots[i];
    b.writeUInt16LE(s ? s.id : 0, 20 + i * 4);
    b.writeUInt16LE(s ? s.count : 0, 22 + i * 4);
  }
  return b;
}
function decodeState(buf) {
  const slots = [];
  for (let i = 0; i < INV_SIZE; i++) {
    const id = buf.readUInt16LE(20 + i * 4), count = buf.readUInt16LE(22 + i * 4);
    slots.push(id ? { id, count } : null);
  }
  return {
    x: buf.readFloatLE(1), y: buf.readFloatLE(5), z: buf.readFloatLE(9), yaw: buf.readFloatLE(13),
    health: buf[17], hunger: buf[18], selSlot: buf[19], slots,
  };
}
// Join replay: the log in 11-byte records, in large batches.
const JOIN_BATCH = 16384;           // 3 + 16384*11 ≈ 180 KB / frame
function editBatches() {
  const all = [...editLog.values()], out = [];
  for (let i = 0; i < all.length; i += JOIN_BATCH) {
    const n = Math.min(JOIN_BATCH, all.length - i);
    const b = Buffer.alloc(3 + n * 11);
    b[0] = SV_EDITS; b.writeUInt16LE(n, 1);
    for (let k = 0; k < n; k++) {
      const e = all[i + k], o = 3 + k * 11;
      b.writeInt32LE(e.x, o); b.writeInt32LE(e.z, o + 4);
      b[o + 8] = e.y; b[o + 9] = e.b; b[o + 10] = e.w;
    }
    out.push(b);
  }
  return out;
}

// --- Writing OUT one WebSocket frame (v2: BINARY frames, opcode 0x2) ---
// Under Bun sock is a ServerWebSocket (has .send), under node a raw TCP socket.
function send(sock, buf) {
  if (typeof sock.send === 'function') { try { sock.send(buf); } catch (e) {} return; }
  const len = buf.length;
  let header;
  if (len < 126) {
    header = Buffer.from([0x82, len]);
  } else if (len < 65536) {
    header = Buffer.alloc(4);
    header[0] = 0x82; header[1] = 126; header.writeUInt16BE(len, 2);
  } else {
    header = Buffer.alloc(10);
    header[0] = 0x82; header[1] = 127;
    header.writeUInt32BE(0, 2); header.writeUInt32BE(len, 6);
  }
  try { sock.write(Buffer.concat([header, buf])); } catch (e) { /* closed connection */ }
}

function broadcast(exceptSock, buf) {
  for (const sock of clients.keys()) if (sock !== exceptSock) send(sock, buf);
}

// --- Processing incoming frames (the client ALWAYS masks) ---
function drain(sock, id, buf) {
  while (buf.length >= 2) {
    const opcode = buf[0] & 0x0f;
    const masked = (buf[1] & 0x80) !== 0;
    let len = buf[1] & 0x7f;
    let off = 2;
    if (len === 126) { if (buf.length < 4) break; len = buf.readUInt16BE(2); off = 4; }
    else if (len === 127) { if (buf.length < 10) break; len = Number(buf.readBigUInt64BE(2)); off = 10; }
    const maskLen = masked ? 4 : 0;
    if (buf.length < off + maskLen + len) break;
    let payload = buf.slice(off + maskLen, off + maskLen + len);
    if (masked) {
      const mask = buf.slice(off, off + 4);
      const out = Buffer.alloc(len);
      for (let i = 0; i < len; i++) out[i] = payload[i] ^ mask[i & 3];
      payload = out;
    }
    buf = buf.slice(off + maskLen + len);
    if (opcode === 0x8) { try { sock.end(); } catch (e) {} break; }   // close
    if (opcode === 0x2) handleMsg(sock, id, payload);                 // v2: binary
    // text (0x1) frames – legacy clients – are ignored
  }
  return buf;
}

function handleMsg(sock, id, buf) {
  if (buf.length < 1) return;
  lastSeen.set(sock, Date.now());
  switch (buf[0]) {
    case CL_HELLO: {
      if (buf.length < 3) return;
      if (buf[1] !== PROTO) { send(sock, encReject()); try { sock.end ? sock.end() : sock.close(); } catch (e) {} return; }
      const uidLen = buf[2];
      if (uidLen < 1 || uidLen > 63 || buf.length < 3 + uidLen) return;
      const uid = buf.slice(3, 3 + uidLen).toString('utf8');
      socketUid.set(sock, uid);
      // welcome packet: WELCOME, TIME, log batches, finally the saved state
      send(sock, encWelcome(id, serverTime()));
      send(sock, encTime(serverTime()));
      for (const b of editBatches()) send(sock, b);
      const st = players.get(uid);
      if (st) send(sock, encodeState(st, SV_RESTORE));
      return;
    }
    case CL_SETTIME: {
      if (buf.length < 5) return;
      setServerTime(buf.readFloatLE(1)); dirty = true;
      broadcast(null, encTime(serverTime()));
      return;
    }
    case CL_SAVE: {
      if (buf.length < STATE_SIZE) return;
      const uid = socketUid.get(sock);
      if (uid) { players.set(uid, decodeState(buf)); dirty = true; }
      return;
    }
    case CL_POS: {
      if (buf.length < 33) return;
      broadcast(sock, encRelay(SV_POS, id, buf.slice(1)));
      return;
    }
    case CL_HELD: {
      if (buf.length < 3) return;
      broadcast(sock, encRelay(SV_HELD, id, buf.slice(1)));
      return;
    }
    case CL_CHAT: {
      if (buf.length < 2 || buf.length < 2 + buf[1]) return;
      broadcast(sock, encRelay(SV_CHAT, id, buf.slice(1)));
      return;
    }
    case CL_EDIT: {
      if (buf.length < 12) return;
      const x = buf.readInt32LE(1), z = buf.readInt32LE(5), y = buf[9], b = buf[10], w = buf[11];
      editLog.set(x + ',' + y + ',' + z, { t: 'edit', x, y, z, b, w, id });   // disk format unchanged
      dirty = true;
      broadcast(sock, encRelay(SV_EDIT, id, buf.slice(1)));
      return;
    }
    case CL_PING: {
      if (buf.length < 9) return;
      send(sock, encPong(buf.readDoubleLE(1)));
      return;
    }
  }
}

// --- Join / leave (in v2 the HELLO triggers the welcome packet) ---
function onJoin(sock, id) {
  clients.set(sock, id);
  lastSeen.set(sock, Date.now());
  console.log('Player %d joined (%d online now)', id, clients.size);
}
function onLeave(sock) {
  if (!clients.has(sock)) return;
  const id = clients.get(sock);
  clients.delete(sock);
  socketUid.delete(sock);
  lastSeen.delete(sock);
  console.log('Player %d left (%d online now)', id, clients.size);
  broadcast(sock, encLeave(id));
}

// Heartbeat check: anyone silent for 10 s gets closed (the client pings every
// 2 s and moves at 20 Hz, so this only signals a real death).
function sweepDead() {
  const now = Date.now();
  for (const [sock, seen] of lastSeen) {
    if (now - seen > 10000) {
      try { sock.close ? sock.close() : sock.destroy(); } catch (e) {}
      onLeave(sock);
    }
  }
}

// --- Serving static files (Bun only) ---
const STATIC_DIR = process.env.STATIC || '';

async function serveStatic(req) {
  let p;
  try { p = decodeURIComponent(new URL(req.url).pathname); } catch (e) { return null; }
  if (p.includes('..')) return null;
  // no-cache: the browser revalidates on every load, so a fresh build shows
  // up on plain reload instead of hiding behind a cached craft.wasm/js/data
  const headers = { 'Cache-Control': 'no-cache' };
  if (p === '/' || p === '') {
    for (const name of ['/craft.html', '/index.html']) {
      const f = Bun.file(STATIC_DIR + name);
      if (await f.exists()) return new Response(f, { headers });
    }
    return null;
  }
  const f = Bun.file(STATIC_DIR + p);
  if (await f.exists()) return new Response(f, { headers });
  return null;
}

// --- Two runtimes, one logic. ---
function startBun() {
  Bun.serve({
    port: PORT,
    async fetch(req, srv) {
      if (srv.upgrade(req, { data: { id: nextId++ } })) return;
      if (STATIC_DIR) {
        const res = await serveStatic(req);
        if (res) return res;
      }
      return new Response('Craft Survival server is running. Open the game in a browser!');
    },
    websocket: {
      open(ws)        { onJoin(ws, ws.data.id); },
      message(ws, m)  {
        if (typeof m === 'string') return;   // v2: binary only
        handleMsg(ws, ws.data.id, Buffer.isBuffer(m) ? m : Buffer.from(m));
      },
      close(ws)       { onLeave(ws); },
    },
  });
  console.log('Craft Survival server running on port %d (Bun, v2 binary). Stop: Ctrl+C', PORT);
  if (STATIC_DIR) console.log('Static dir served: %s -> http://localhost:%d/', STATIC_DIR, PORT);
}

function startNode() {
  const server = http.createServer((req, res) => {
    res.writeHead(200, { 'Content-Type': 'text/plain; charset=utf-8' });
    res.end('Craft Survival server is running. Open the game in a browser!');
  });

  server.on('upgrade', (req, socket) => {
    const key = req.headers['sec-websocket-key'];
    if (!key) { socket.end(); return; }
    const accept = crypto.createHash('sha1')
      .update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
    socket.write(
      'HTTP/1.1 101 Switching Protocols\r\n' +
      'Upgrade: websocket\r\n' +
      'Connection: Upgrade\r\n' +
      'Sec-WebSocket-Accept: ' + accept + '\r\n\r\n');

    const id = nextId++;
    onJoin(socket, id);

    let buf = Buffer.alloc(0);
    socket.on('data', d => { buf = drain(socket, id, Buffer.concat([buf, d])); });
    socket.on('close', () => onLeave(socket));
    socket.on('error', () => onLeave(socket));
  });

  server.listen(PORT, () => {
    console.log('Craft Survival server running on port %d (node, v2 binary). Stop: Ctrl+C', PORT);
  });
}

// ============================================================================
//  SELF-TEST: node server.js test  /  bun server.js test
// ============================================================================
if (process.argv[2] === 'test') {
  const nul = { end() {}, write() {} };
  // Building a masked BINARY client frame, the way the browser/C client sends it.
  const buildFrame = (payload) => {
    const mask = Buffer.from([11, 22, 33, 44]);
    const body = Buffer.alloc(payload.length);
    for (let i = 0; i < payload.length; i++) body[i] = payload[i] ^ mask[i & 3];
    let header;
    if (payload.length < 126) header = Buffer.from([0x82, 0x80 | payload.length]);
    else { header = Buffer.alloc(4); header[0] = 0x82; header[1] = 0x80 | 126; header.writeUInt16BE(payload.length, 2); }
    return Buffer.concat([header, mask, body]);
  };
  const clEdit = (x, y, z, b, w) => {
    const p = Buffer.alloc(12);
    p[0] = CL_EDIT; p.writeInt32LE(x, 1); p.writeInt32LE(z, 5); p[9] = y; p[10] = b; p[11] = w;
    return p;
  };
  const clHello = (ver, uid) => {
    const u = Buffer.from(uid, 'utf8');
    return Buffer.concat([Buffer.from([CL_HELLO, ver, u.length]), u]);
  };

  // GOLDEN FIXTURES: byte-exact encodings – the C test (net_test.c) expects
  // these same bytes. If this changes, the C side MUST be updated too.
  console.assert(clEdit(5, 6, 7, 3, 0).toString('hex') === '120500000007000000060300',
    'GOLDEN: edit(5,6,7,b3,w0) bytes');
  const gp = Buffer.alloc(33); gp[0] = CL_POS;
  gp.writeFloatLE(1.5, 1); gp.writeFloatLE(20, 5); gp.writeFloatLE(-3.25, 9);
  gp.writeFloatLE(0.5, 13); gp.writeFloatLE(-0.25, 17);
  gp.writeFloatLE(1, 21); gp.writeFloatLE(0, 25); gp.writeFloatLE(-1, 29);
  console.assert(gp.toString('hex') ===
    '10' + '0000c03f' + '0000a041' + '000050c0' + '0000003f' + '000080be' + '0000803f' + '00000000' + '000080bf',
    'GOLDEN: pos(1.5,20,-3.25,yaw.5,pitch-.25,v 1,0,-1) bytes');
  console.assert(encWelcome(7, 0.5).toString('hex') === '02' + '02' + '07000000' + '0000003f',
    'GOLDEN: welcome(id7,time.5) bytes');
  console.assert(encLeave(9).toString('hex') === '3009000000', 'GOLDEN: leave(9) bytes');
  console.assert(encRelay(SV_HELD, 3, Buffer.from([0x07, 0x00])).toString('hex') === '17' + '03000000' + '0700',
    'GOLDEN: held relay (sender 3, item 7) bytes');
  console.assert(encRelay(SV_CHAT, 3, Buffer.concat([Buffer.from([2]), Buffer.from('hi')])).toString('hex')
    === '19' + '03000000' + '02' + '6869', 'GOLDEN: chat relay (sender 3, "hi") bytes');

  // Frame parsing: a masked binary edit frame ends up in the log.
  const rest = drain(nul, 1, buildFrame(clEdit(5, 6, 7, 3, 0)));
  console.assert(rest.length === 0, 'the whole frame is consumed');
  const e = editLog.get('5,6,7');
  console.assert(e && e.b === 3 && e.id === 1, 'the log stores the right block and sender');
  // Two frames in one data chunk.
  const two = Buffer.concat([buildFrame(clEdit(0, 0, 0, 1, 0)), buildFrame(clEdit(1, 0, 0, 2, 0))]);
  drain(nul, 2, two);
  console.assert(editLog.get('0,0,0') && editLog.get('1,0,0'), 'two glued frames are split apart');
  // A half frame stays in the buffer.
  const half = buildFrame(clEdit(9, 1, 2, 3, 0)).slice(0, 3);
  console.assert(drain(nul, 3, half).length === 3, 'the half-done frame stays in the buffer');
  // Negative coordinate round-trip.
  drain(nul, 4, buildFrame(clEdit(-100, 63, -200, 8, 9)));
  const ne = editLog.get('-100,63,-200');
  console.assert(ne && ne.b === 8 && ne.w === 9, 'negative coords encode correctly');

  // HELLO with wrong version -> REJECT, no WELCOME.
  const rejBox = [];
  const rejSock = { end() {}, write(b) { rejBox.push(b); } };
  clients.set(rejSock, 90);
  handleMsg(rejSock, 90, clHello(1, 'old-client'));
  console.assert(rejBox.length === 1 && rejBox[0][2 + 0] === SV_REJECT || rejBox[0].includes(SV_REJECT),
    'wrong version -> REJECT frame');
  console.assert(![...rejBox].some(b => b.includes && b[2] === SV_WELCOME), 'no WELCOME after wrong version');
  clients.delete(rejSock);

  // HELLO with correct version -> WELCOME + TIME (+ log) in order.
  const okBox = [];
  const okSock = { end() {}, write(b) { okBox.push(b); } };
  clients.set(okSock, 91);
  handleMsg(okSock, 91, clHello(PROTO, 'uid-A'));
  // the 1st frame's payload starts after the 2-byte header
  console.assert(okBox[0][2] === SV_WELCOME, 'first frame: WELCOME');
  console.assert(okBox[1][2] === SV_TIME, 'second frame: TIME');

  // SAVE -> stored state -> RESTORE round-trips (36 slots, including empties).
  const st = { x: 1, y: 2, z: 3, yaw: 0.5, health: 15, hunger: 18, selSlot: 4,
               slots: [{ id: 7, count: 4 }, null, { id: 300, count: 64 }] };
  const saveBuf = encodeState(st, CL_SAVE);
  handleMsg(okSock, 91, saveBuf);
  const stored = players.get('uid-A');
  console.assert(stored.health === 15 && stored.selSlot === 4, 'SAVE stores the state');
  console.assert(stored.slots[0].id === 7 && stored.slots[1] === null && stored.slots[2].id === 300,
    'slots (including empties) round-trip');
  console.assert(dirty === true, 'SAVE sets the disk dirty flag');
  const restored = decodeState(encodeState(stored, SV_RESTORE));
  console.assert(restored.slots[2].count === 64 && Math.abs(restored.yaw - 0.5) < 1e-6,
    'encode->decode state is identical');
  clients.delete(okSock);

  // Batching: 40,000 records -> 16384 + 16384 + 7232.
  editLog.clear();
  for (let i = 0; i < 40000; i++) editLog.set(i + ',0,0', { t: 'edit', x: i, y: 0, z: 0, b: 1, w: 0, id: 1 });
  const batches = editBatches();
  console.assert(batches.length === 3, '40k records split into 3 batches');
  const counts = batches.map(b => b.readUInt16LE(1));
  console.assert(counts[0] === 16384 && counts[1] === 16384 && counts[2] === 7232, 'batch sizes are correct');
  console.assert(counts.reduce((a, c) => a + c, 0) === 40000, 'the batches carry the whole log');
  console.assert(batches[0].readInt32LE(3) === 0 && batches[0][3 + 9] === 1, 'first record fields are right');

  // Disk-format guard: world.edits binary codec (golden bytes + round-trip).
  editLog.clear();
  editLog.set('5,6,7', { t: 'edit', x: 5, y: 6, z: 7, b: 3, w: 0, id: 1 });
  const ef = encodeEditsFile();
  console.assert(ef.toString('hex') === '4b454454' + '01' + '01000000' + '0500000007000000060300',
    'GOLDEN: world.edits magic+version+count+record bytes');
  editLog.clear();
  console.assert(decodeEditsFile(ef) && editLog.get('5,6,7').b === 3, 'world.edits round-trips');
  editLog.set('-100,63,-200', { t: 'edit', x: -100, y: 63, z: -200, b: 8, w: 9, id: 0 });
  const ef2 = encodeEditsFile();
  editLog.clear();
  console.assert(decodeEditsFile(ef2) && editLog.size === 2 && editLog.get('-100,63,-200').w === 9,
    'negative coords survive the file round-trip');
  console.assert(!decodeEditsFile(Buffer.from('junk')), 'garbage edits file rejected');
  console.assert(!decodeEditsFile(ef2.slice(0, ef2.length - 3)), 'truncated edits file rejected');
  // players.json stays human-readable JSON and round-trips.
  const snap = JSON.parse(JSON.stringify(Object.fromEntries(players)));
  console.assert(snap['uid-A'].health === 15, 'players snapshot intact');

  // Time of day: settime via binary message.
  const stBuf = Buffer.alloc(5); stBuf[0] = CL_SETTIME; stBuf.writeFloatLE(0.75, 1);
  handleMsg(nul, 99, stBuf);
  console.assert(Math.abs(serverTime() - 0.75) < 0.01, 'settime adjusts the server clock');

  // Ping -> pong echo.
  const pingBox = [];
  const pingSock = { end() {}, write(b) { pingBox.push(b); } };
  const pg = Buffer.alloc(9); pg[0] = CL_PING; pg.writeDoubleLE(12345.5, 1);
  handleMsg(pingSock, 98, pg);
  console.assert(pingBox.length === 1 && pingBox[0][2] === SV_PONG
    && pingBox[0].readDoubleLE(3) === 12345.5, 'ping echoes back a pong');

  console.log('Self-test OK: v2 binary framing, codecs, persistence and day cycle are correct.');
  process.exit(0);
}

loadWorld();
setInterval(saveWorld, 10000).unref();
setInterval(() => broadcast(null, encTime(serverTime())), 10000).unref();
setInterval(sweepDead, 5000).unref();
process.on('SIGINT', () => { saveWorld(); process.exit(0); });

if (typeof Bun !== 'undefined') startBun(); else startNode();
