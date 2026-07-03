// Generates assets/zombie.png and assets/skeleton.png — classic 64x32
// Minecraft humanoid skin unwraps for the boxel mobs (entities.c mob_box).
// Zero deps: hand-rolled PNG encoder (stored zlib blocks + CRC32), like the
// craft_js asset tools. Run: bun tools/make_mobs.mjs
import { writeFileSync } from 'fs';
import { dirname, join } from 'path';
import { fileURLToPath } from 'url';

const W = 64, H = 32;

// --- tiny PNG writer (RGBA, zlib "stored" blocks) ---
function crc32(buf) {
  let c, table = [];
  for (let n = 0; n < 256; n++) {
    c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
    table[n] = c >>> 0;
  }
  let crc = 0xFFFFFFFF;
  for (const b of buf) crc = table[(crc ^ b) & 0xFF] ^ (crc >>> 8);
  return (crc ^ 0xFFFFFFFF) >>> 0;
}
function adler32(buf) {
  let a = 1, b = 0;
  for (const x of buf) { a = (a + x) % 65521; b = (b + a) % 65521; }
  return ((b << 16) | a) >>> 0;
}
function chunk(type, data) {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type), data]);
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td));
  return Buffer.concat([len, td, crc]);
}
function png(rgba) {
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(W, 0); ihdr.writeUInt32BE(H, 4);
  ihdr[8] = 8; ihdr[9] = 6;                       // 8-bit RGBA
  const raw = Buffer.alloc(H * (1 + W * 4));      // filter byte 0 per row
  for (let y = 0; y < H; y++) rgba.copy(raw, y * (1 + W * 4) + 1, y * W * 4, (y + 1) * W * 4);
  // zlib: header + single stored deflate block + adler
  const zhdr = Buffer.from([0x78, 0x01]);
  const parts = [zhdr];
  for (let o = 0; o < raw.length; o += 65535) {
    const n = Math.min(65535, raw.length - o);
    const bh = Buffer.alloc(5);
    bh[0] = o + n >= raw.length ? 1 : 0;
    bh.writeUInt16LE(n, 1); bh.writeUInt16LE(n ^ 0xFFFF, 3);
    parts.push(bh, raw.subarray(o, o + n));
  }
  const ad = Buffer.alloc(4); ad.writeUInt32BE(adler32(raw));
  parts.push(ad);
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]),
    chunk('IHDR', ihdr), chunk('IDAT', Buffer.concat(parts)), chunk('IEND', Buffer.alloc(0)),
  ]);
}

// --- painting helpers (deterministic noise so builds are reproducible) ---
let seed = 12345;
const rnd = () => (seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF) / 0x7FFFFFFF;
function makeCanvas() { return Buffer.alloc(W * H * 4); }
function px(img, x, y, [r, g, b]) {
  const o = (y * W + x) * 4;
  img[o] = r; img[o + 1] = g; img[o + 2] = b; img[o + 3] = 255;
}
function fill(img, x0, y0, x1, y1, base, vary) {
  for (let y = y0; y < y1; y++)
    for (let x = x0; x < x1; x++) {
      const v = (rnd() - 0.5) * 2 * vary;
      px(img, x, y, [base[0] + v, base[1] + v, base[2] + v].map(c => Math.max(0, Math.min(255, c | 0))));
    }
}
// full unwrap of one box part: (w,h,d) px at (ox,oy) — mirrors mob_box UVs
function box(img, w, h, d, ox, oy, base, vary) {
  fill(img, ox, oy + d, ox + 2 * d + 2 * w, oy + d + h, base, vary);  // side strip
  fill(img, ox + d, oy, ox + d + 2 * w, oy + d, base, vary);          // up+down
}

// classic humanoid layout: head 8^3 @(0,0), body 8x12x4 @(16,16),
// arm 4x12x4 @(40,16), leg 4x12x4 @(0,16). Face on the -z region (8,8)-(16,16).
function humanoid(img, skin, skinVar, shirt, shirtVar, pants, pantsVar) {
  box(img, 8, 8, 8, 0, 0, skin, skinVar);        // head
  box(img, 8, 12, 4, 16, 16, shirt, shirtVar);   // body
  box(img, 4, 12, 4, 40, 16, skin, skinVar);     // arm
  box(img, 4, 12, 4, 0, 16, pants, pantsVar);    // leg
}

// --- zombie: green skin, teal shirt, purple-blue pants ---
{
  const img = makeCanvas();
  const SKIN = [70, 140, 70], SHIRT = [0, 145, 145], PANTS = [70, 60, 140];
  humanoid(img, SKIN, 14, SHIRT, 12, PANTS, 12);
  // face (-z region x 8..16, y 8..16): heavy brow, black eyes, grim mouth
  fill(img, 8, 9, 16, 10, [50, 105, 50], 6);                  // brow shadow
  px(img, 10, 11, [15, 15, 15]); px(img, 11, 11, [15, 15, 15]);
  px(img, 13, 11, [15, 15, 15]); px(img, 12, 11, [40, 90, 40]);
  fill(img, 10, 13, 14, 14, [25, 60, 25], 4);                 // mouth
  px(img, 11, 14, [25, 60, 25]); px(img, 12, 14, [25, 60, 25]);
  // shirt: torn hem (skin shows through at the bottom row of the front)
  for (let x = 28; x < 36; x++) if (rnd() < 0.4) px(img, x, 31, SKIN);
  writeFileSync(join(dirname(fileURLToPath(import.meta.url)), '..', 'assets', 'zombie.png'), png(img));
  console.log('assets/zombie.png');
}

// --- creeper: green camo noise, the iconic drooping face; no arms ---
{
  seed = 424242;
  const img = makeCanvas();
  const CREEP = [70, 180, 70];
  box(img, 8, 8, 8, 0, 0, CREEP, 30);            // head (heavy camo variance)
  box(img, 8, 12, 4, 16, 16, CREEP, 30);         // body
  box(img, 4, 6, 4, 0, 16, [60, 150, 60], 25);   // leg
  // face (-z region 8..16 x 8..16): black droopy eyes + open mouth
  fill(img, 9, 10, 11, 12, [20, 20, 20], 3);     // left eye
  fill(img, 13, 10, 15, 12, [20, 20, 20], 3);    // right eye
  fill(img, 11, 12, 13, 14, [20, 20, 20], 3);    // nose/mouth top
  fill(img, 10, 14, 11, 16, [20, 20, 20], 3);    // mouth droop left
  fill(img, 13, 14, 14, 16, [20, 20, 20], 3);    // mouth droop right
  fill(img, 11, 14, 13, 15, [20, 20, 20], 3);
  writeFileSync(join(dirname(fileURLToPath(import.meta.url)), '..', 'assets', 'creeper.png'), png(img));
  console.log('assets/creeper.png');
}

// --- skeleton: bone everything, dark sockets, ribs on the torso ---
{
  seed = 99991;
  const img = makeCanvas();
  const BONE = [200, 200, 195], DARKB = [160, 160, 152];
  humanoid(img, BONE, 10, BONE, 10, DARKB, 8);
  // face: deep sockets, nose hole, toothy mouth line
  fill(img, 9, 11, 11, 13, [35, 35, 35], 3);                  // left socket
  fill(img, 13, 11, 15, 13, [35, 35, 35], 3);                 // right socket
  px(img, 12, 13, [60, 60, 60]);                              // nose
  for (let x = 9; x < 15; x++) px(img, x, 15, x % 2 ? [90, 90, 88] : [230, 230, 225]); // teeth
  // ribs: darker horizontal bands across the whole torso strip (y 20..32)
  for (let y = 21; y < 31; y += 3)
    fill(img, 16, y, 44, y + 1, [120, 120, 114], 6);
  writeFileSync(join(dirname(fileURLToPath(import.meta.url)), '..', 'assets', 'skeleton.png'), png(img));
  console.log('assets/skeleton.png');
}
