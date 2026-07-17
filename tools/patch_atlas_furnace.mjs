// Paints furnace (front tile 21, top tile 22) + glass (tile 23) into atlas.png.
// Furnace tiles use the stone tile (3) as a base; glass is a light frosted pane.
// Zero-dep PNG decode/paint/encode (assumes atlas already 6 rows / 256x384 from
// patch_atlas_ores.mjs). Run: bun tools/patch_atlas_furnace.mjs
import { readFileSync, writeFileSync } from 'fs';
import zlib from 'node:zlib';
import { dirname, join } from 'path';
import { fileURLToPath } from 'url';

const PATH_ = join(dirname(fileURLToPath(import.meta.url)), '..', 'assets', 'atlas.png');
const png = readFileSync(PATH_);
let W = 0, H = 0; const idat = [];
for (let o = 8; o < png.length;) {
  const len = png.readUInt32BE(o), type = png.toString('ascii', o + 4, o + 8);
  if (type === 'IHDR') { W = png.readUInt32BE(o + 8); H = png.readUInt32BE(o + 12); if (png[o + 16] !== 8 || png[o + 17] !== 6) throw new Error('expected 8-bit RGBA'); }
  if (type === 'IDAT') idat.push(png.subarray(o + 8, o + 8 + len));
  o += 12 + len;
}
const rawz = zlib.inflateSync(Buffer.concat(idat));
const paeth = (a, b, c) => { const p = a + b - c, pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c); return pa <= pb && pa <= pc ? a : pb <= pc ? b : c; };
const stride = W * 4, img = Buffer.alloc(H * stride);
for (let y = 0; y < H; y++) {
  const f = rawz[y * (stride + 1)];
  const row = rawz.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
  for (let x = 0; x < stride; x++) {
    const left = x >= 4 ? img[y * stride + x - 4] : 0, up = y > 0 ? img[(y - 1) * stride + x] : 0, ul = (y > 0 && x >= 4) ? img[(y - 1) * stride + x - 4] : 0;
    let v = row[x];
    if (f === 1) v += left; else if (f === 2) v += up; else if (f === 3) v += (left + up) >> 1; else if (f === 4) v += paeth(left, up, ul);
    img[y * stride + x] = v & 0xFF;
  }
}
if (H < 384) throw new Error('run patch_atlas_ores.mjs first (need a 6-row atlas)');

const TILE = 64;
const px = (x, y) => ((y * W) + x) * 4;
const setrgb = (x, y, r, g, b, a = 255) => { const o = px(x, y); img[o] = r; img[o + 1] = g; img[o + 2] = b; img[o + 3] = a; };
function tilePos(t) { return [(t % 4) * TILE, ((t / 4) | 0) * TILE]; }
function copyTile(srcT, dstT) {
  const [sx, sy] = tilePos(srcT), [dx, dy] = tilePos(dstT);
  for (let j = 0; j < TILE; j++) for (let i = 0; i < TILE; i++) { const s = px(sx + i, sy + j), d = px(dx + i, dy + j); img[d] = img[s]; img[d + 1] = img[s + 1]; img[d + 2] = img[s + 2]; img[d + 3] = 255; }
}
const rect = (t, x, y, w, h, r, g, b) => { const [ox, oy] = tilePos(t); for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) setrgb(ox + Math.min(x + i, 63), oy + Math.min(y + j, 63), r, g, b); };

// --- tile 21: furnace FRONT (stone base + dark arched opening + embers) ---
copyTile(3, 21);
rect(21, 3, 3, 58, 58, 90, 90, 92);              // lighter stone frame face
copyTile(3, 21);                                  // (keep stone; frame drawn below)
rect(21, 2, 2, 60, 3, 120, 120, 122);            // top bevel
rect(21, 2, 2, 3, 60, 110, 110, 112);            // left bevel
rect(21, 14, 20, 36, 34, 30, 24, 20);            // furnace mouth (dark)
rect(21, 14, 18, 36, 4, 60, 50, 44);             // mouth lintel
for (let i = 0; i < 36; i++) { const t = i / 35, r = 255, g = 150 - t * 90, b = 20; rect(21, 14 + i, 50, 1, 4, r, g | 0, b); }  // ember glow at base
rect(21, 20, 44, 24, 6, 255, 120, 20);           // brighter ember bar
rect(21, 24, 40, 16, 4, 255, 200, 90);           // hot core

// --- tile 22: furnace TOP (stone with a small dark hole) ---
copyTile(3, 22);
rect(22, 2, 2, 60, 3, 130, 130, 132);            // top bevel
rect(22, 24, 24, 16, 16, 66, 62, 58);            // rim (small)
rect(22, 27, 27, 10, 10, 34, 30, 28);            // hole

// --- tile 23: GLASS (light frosted pane with a bright frame) ---
rect(23, 0, 0, 64, 64, 200, 226, 240);           // pale glass fill
rect(23, 0, 0, 64, 4, 236, 248, 255);            // frame (bright)
rect(23, 0, 60, 64, 4, 200, 220, 232);
rect(23, 0, 0, 4, 64, 236, 248, 255);
rect(23, 60, 0, 4, 64, 200, 220, 232);
rect(23, 10, 10, 22, 4, 255, 255, 255);          // highlight streak
rect(23, 10, 14, 4, 16, 255, 255, 255);

// --- encode ---
function crc32(b) { let c = ~0; for (let i = 0; i < b.length; i++) { c ^= b[i]; for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (0xEDB88320 & -(c & 1)); } return ~c >>> 0; }
function chunk(type, data) { const t = Buffer.from(type, 'ascii'); const len = Buffer.alloc(4); len.writeUInt32BE(data.length); const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(Buffer.concat([t, data]))); return Buffer.concat([len, t, data, crc]); }
const out = Buffer.alloc(H * (stride + 1));
for (let y = 0; y < H; y++) img.copy(out, y * (stride + 1) + 1, y * stride, (y + 1) * stride);
const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(W); ihdr.writeUInt32BE(H, 4); ihdr[8] = 8; ihdr[9] = 6;
writeFileSync(PATH_, Buffer.concat([Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(out)), chunk('IEND', Buffer.alloc(0))]));
console.log(`atlas.png: furnace(21,22) + glass(23) painted, ${W}x${H}`);
