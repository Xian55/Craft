// Paints new item icons into items.png tiles 16-23 (grows it to 6 rows / 256x384):
//   16 coal  17 iron ingot  18 cooked pork  19 iron pickaxe  20 iron sword
//   21 iron shovel  22 furnace  23 glass
// Iron tools recolor the wood-tool shapes (tiles 1/2/0) to iron grey; cooked pork
// darkens raw pork (tile 5). Zero-dep PNG decode/paint/encode.
// Run: bun tools/patch_items.mjs
import { readFileSync, writeFileSync } from 'fs';
import zlib from 'node:zlib';
import { dirname, join } from 'path';
import { fileURLToPath } from 'url';

const PATH_ = join(dirname(fileURLToPath(import.meta.url)), '..', 'assets', 'items.png');
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
const sstride = W * 4, src = Buffer.alloc(H * sstride);
for (let y = 0; y < H; y++) {
  const f = rawz[y * (sstride + 1)];
  const row = rawz.subarray(y * (sstride + 1) + 1, (y + 1) * (sstride + 1));
  for (let x = 0; x < sstride; x++) {
    const left = x >= 4 ? src[y * sstride + x - 4] : 0, up = y > 0 ? src[(y - 1) * sstride + x] : 0, ul = (y > 0 && x >= 4) ? src[(y - 1) * sstride + x - 4] : 0;
    let v = row[x];
    if (f === 1) v += left; else if (f === 2) v += up; else if (f === 3) v += (left + up) >> 1; else if (f === 4) v += paeth(left, up, ul);
    src[y * sstride + x] = v & 0xFF;
  }
}

const TILE = 64;
const newH = Math.max(H, 6 * TILE);
const stride = W * 4, img = Buffer.alloc(newH * stride);   // transparent (alpha 0) by default
src.copy(img, 0);                                          // old 16 tiles at the top

const tp = (t) => [(t % 4) * TILE, ((t / 4) | 0) * TILE];
const px = (x, y) => ((y * W) + x) * 4;
const set = (x, y, r, g, b, a = 255) => { const o = px(x, y); img[o] = r; img[o + 1] = g; img[o + 2] = b; img[o + 3] = a; };
const rect = (t, x, y, w, h, r, g, b, a = 255) => { const [ox, oy] = tp(t); for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) set(ox + Math.min(x + i, 63), oy + Math.min(y + j, 63), r, g, b, a); };
function copyTile(sT, dT) { const [sx, sy] = tp(sT), [dx, dy] = tp(dT); for (let j = 0; j < TILE; j++) for (let i = 0; i < TILE; i++) { const s = px(sx + i, sy + j), d = px(dx + i, dy + j); img[d] = img[s]; img[d + 1] = img[s + 1]; img[d + 2] = img[s + 2]; img[d + 3] = img[s + 3]; } }
// recolor opaque pixels of a tile toward a target, keeping per-pixel luminance shading
function tint(t, mulR, mulG, mulB) {
  const [ox, oy] = tp(t);
  for (let j = 0; j < TILE; j++) for (let i = 0; i < TILE; i++) {
    const o = px(ox + i, oy + j); if (img[o + 3] < 40) continue;
    const lum = (img[o] * 0.3 + img[o + 1] * 0.6 + img[o + 2] * 0.1);
    img[o] = Math.min(255, lum * mulR); img[o + 1] = Math.min(255, lum * mulG); img[o + 2] = Math.min(255, lum * mulB);
  }
}

// 16 coal — dark rounded lump
for (let j = 0; j < TILE; j++) for (let i = 0; i < TILE; i++) {
  const dx = i - 32, dy = j - 34, r = Math.sqrt(dx * dx + dy * dy);
  if (r < 20) { const shade = 28 + (20 - r) * 1.4 + ((i * 7 + j * 13) % 11); rect(16, i, j, 1, 1, shade * 0.7 | 0, shade * 0.7 | 0, shade | 0); }
}
rect(16, 22, 24, 6, 4, 90, 90, 96);   // highlight

// 17 iron ingot — a grey trapezoid bar
rect(17, 14, 30, 36, 14, 150, 150, 160);
rect(17, 17, 27, 30, 4, 200, 200, 212);   // top face (bright)
rect(17, 14, 40, 36, 4, 110, 110, 122);   // bottom edge (dark)
rect(17, 20, 31, 10, 3, 230, 230, 240);   // highlight

// 18 cooked pork — darker/browner raw pork
copyTile(5, 18); tint(18, 1.05, 0.72, 0.5);

// 19/20/21 iron tools — recolor wood pickaxe/sword/shovel to iron grey
copyTile(1, 19); tint(19, 0.9, 0.92, 1.02);
copyTile(2, 20); tint(20, 0.9, 0.92, 1.02);
copyTile(0, 21); tint(21, 0.9, 0.92, 1.02);

// 22 furnace — mini stone block with a dark ember mouth
rect(22, 12, 12, 40, 40, 120, 120, 124);
rect(22, 12, 12, 40, 4, 150, 150, 154);
rect(22, 22, 30, 20, 18, 34, 28, 24);     // mouth
rect(22, 24, 40, 16, 5, 255, 140, 30);    // embers

// 23 glass — light frosted square
rect(23, 12, 12, 40, 40, 205, 230, 244);
rect(23, 12, 12, 40, 4, 240, 250, 255);
rect(23, 12, 12, 4, 40, 240, 250, 255);
rect(23, 18, 18, 12, 3, 255, 255, 255);

// --- encode ---
function crc32(b) { let c = ~0; for (let i = 0; i < b.length; i++) { c ^= b[i]; for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (0xEDB88320 & -(c & 1)); } return ~c >>> 0; }
function chunk(type, data) { const t = Buffer.from(type, 'ascii'); const len = Buffer.alloc(4); len.writeUInt32BE(data.length); const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(Buffer.concat([t, data]))); return Buffer.concat([len, t, data, crc]); }
const out = Buffer.alloc(newH * (stride + 1));
for (let y = 0; y < newH; y++) img.copy(out, y * (stride + 1) + 1, y * stride, (y + 1) * stride);
const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(W); ihdr.writeUInt32BE(newH, 4); ihdr[8] = 8; ihdr[9] = 6;
writeFileSync(PATH_, Buffer.concat([Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(out)), chunk('IEND', Buffer.alloc(0))]));
console.log(`items.png: tiles 16-23 painted, ${W}x${newH}`);
