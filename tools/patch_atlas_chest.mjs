// Draws the chest artwork into atlas.png tile 17 (the mesher's T_CHEST slot,
// which shipped empty — chests rendered fully transparent). Zero deps:
// decodes the PNG with node:zlib + unfilters, paints, re-encodes.
// Run: bun tools/patch_atlas_chest.mjs
import { readFileSync, writeFileSync } from 'fs';
import zlib from 'node:zlib';
import { dirname, join } from 'path';
import { fileURLToPath } from 'url';

const PATH_ = join(dirname(fileURLToPath(import.meta.url)), '..', 'assets', 'atlas.png');
const png = readFileSync(PATH_);

// --- decode (8-bit RGBA only, any filter) ---
let W = 0, H = 0;
const idat = [];
for (let o = 8; o < png.length;) {
  const len = png.readUInt32BE(o), type = png.toString('ascii', o + 4, o + 8);
  if (type === 'IHDR') {
    W = png.readUInt32BE(o + 8); H = png.readUInt32BE(o + 12);
    if (png[o + 16] !== 8 || png[o + 17] !== 6) throw new Error('expected 8-bit RGBA');
  }
  if (type === 'IDAT') idat.push(png.subarray(o + 8, o + 8 + len));
  o += 12 + len;
}
const raw = zlib.inflateSync(Buffer.concat(idat));
const stride = W * 4, img = Buffer.alloc(H * stride);
const paeth = (a, b, c) => {
  const p = a + b - c, pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
  return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
};
for (let y = 0; y < H; y++) {
  const f = raw[y * (stride + 1)];
  const row = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
  for (let x = 0; x < stride; x++) {
    const left = x >= 4 ? img[y * stride + x - 4] : 0;
    const up = y > 0 ? img[(y - 1) * stride + x] : 0;
    const ul = (y > 0 && x >= 4) ? img[(y - 1) * stride + x - 4] : 0;
    let v = row[x];
    if (f === 1) v += left; else if (f === 2) v += up;
    else if (f === 3) v += (left + up) >> 1; else if (f === 4) v += paeth(left, up, ul);
    img[y * stride + x] = v & 0xFF;
  }
}

// --- paint tile 17 (col 1, row 4): planky chest with band + latch ---
const TILE = 64, ox = (17 % 4) * TILE, oy = ((17 / 4) | 0) * TILE;
const set = (x, y, hexc) => {
  const o = ((oy + y) * W + ox + x) * 4;
  img[o] = parseInt(hexc.slice(1, 3), 16);
  img[o + 1] = parseInt(hexc.slice(3, 5), 16);
  img[o + 2] = parseInt(hexc.slice(5, 7), 16);
  img[o + 3] = 255;
};
const rect = (x, y, w, h, c) => { for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) set(x + i, y + j, c); };
let seed = 7;
const rnd = () => (seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF) / 0x7FFFFFFF;
rect(0, 0, 64, 64, '#7a5230');                        // wood base
for (let i = 0; i < 90; i++) rect((rnd() * 60) | 0, (rnd() * 60) | 0, 4, 3, rnd() < 0.5 ? '#6a4426' : '#8a5f3a');
for (let gy = 8; gy < 64; gy += 14) rect(0, gy, 64, 2, '#5e3d20');   // plank seams
rect(0, 0, 64, 3, '#4a3218'); rect(0, 61, 64, 3, '#4a3218');         // frame
rect(0, 0, 3, 64, '#4a3218'); rect(61, 0, 3, 64, '#4a3218');
rect(0, 26, 64, 4, '#3a2712');                        // lid band
rect(27, 18, 10, 20, '#3a2712');                      // latch backing
rect(29, 20, 6, 16, '#c9c9c9');                       // metal latch
rect(29, 20, 6, 3, '#e8e8e8');
rect(31, 27, 2, 5, '#5a5a5a');                        // keyhole

// --- encode ---
function crc32(b) {
  let c = ~0;
  for (let i = 0; i < b.length; i++) { c ^= b[i]; for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (0xEDB88320 & -(c & 1)); }
  return ~c >>> 0;
}
function chunk(type, data) {
  const t = Buffer.from(type, 'ascii');
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(Buffer.concat([t, data])));
  return Buffer.concat([len, t, data, crc]);
}
const out = Buffer.alloc(H * (stride + 1));
for (let y = 0; y < H; y++) img.copy(out, y * (stride + 1) + 1, y * stride, (y + 1) * stride);
const ihdr = Buffer.alloc(13);
ihdr.writeUInt32BE(W); ihdr.writeUInt32BE(H, 4); ihdr[8] = 8; ihdr[9] = 6;
writeFileSync(PATH_, Buffer.concat([
  Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]),
  chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(out)), chunk('IEND', Buffer.alloc(0)),
]));
console.log(`atlas.png: chest painted into tile 17 (${W}x${H})`);
