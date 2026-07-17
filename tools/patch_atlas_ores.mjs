// Paints coal ore (tile 19) + iron ore (tile 20) into atlas.png. Grows the atlas
// to a 6th row if needed (mesh.c auto-detects ATLAS_ROWS from the PNG height).
// Each ore = the stone tile (3) copied as a base + scattered mineral specks.
// Zero deps: decode with node:zlib + unfilter, paint, re-encode.
// Run: bun tools/patch_atlas_ores.mjs
import { readFileSync, writeFileSync } from 'fs';
import zlib from 'node:zlib';
import { dirname, join } from 'path';
import { fileURLToPath } from 'url';

const PATH_ = join(dirname(fileURLToPath(import.meta.url)), '..', 'assets', 'atlas.png');
const png = readFileSync(PATH_);

// --- decode (8-bit RGBA, any filter) ---
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
const rawz = zlib.inflateSync(Buffer.concat(idat));
const paeth = (a, b, c) => {
  const p = a + b - c, pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
  return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
};
const srcStride = W * 4, src = Buffer.alloc(H * srcStride);
for (let y = 0; y < H; y++) {
  const f = rawz[y * (srcStride + 1)];
  const row = rawz.subarray(y * (srcStride + 1) + 1, (y + 1) * (srcStride + 1));
  for (let x = 0; x < srcStride; x++) {
    const left = x >= 4 ? src[y * srcStride + x - 4] : 0;
    const up = y > 0 ? src[(y - 1) * srcStride + x] : 0;
    const ul = (y > 0 && x >= 4) ? src[(y - 1) * srcStride + x - 4] : 0;
    let v = row[x];
    if (f === 1) v += left; else if (f === 2) v += up;
    else if (f === 3) v += (left + up) >> 1; else if (f === 4) v += paeth(left, up, ul);
    src[y * srcStride + x] = v & 0xFF;
  }
}

// --- grow to 6 rows (384px) so tile 20 (row 5) fits; copy old rows in ---
const TILE = 64;
const newH = Math.max(H, 6 * TILE);
const stride = W * 4, img = Buffer.alloc(newH * stride);
src.copy(img, 0);   // old pixels at the top; new rows stay transparent (0)

const px = (x, y) => ((y * W) + x) * 4;
const setrgb = (x, y, r, g, b) => { const o = px(x, y); img[o] = r; img[o + 1] = g; img[o + 2] = b; img[o + 3] = 255; };
function copyTile(srcTid, dstTid) {
  const sx = (srcTid % 4) * TILE, sy = ((srcTid / 4) | 0) * TILE;
  const dx = (dstTid % 4) * TILE, dy = ((dstTid / 4) | 0) * TILE;
  for (let j = 0; j < TILE; j++) for (let i = 0; i < TILE; i++) {
    const s = px(sx + i, sy + j), d = px(dx + i, dy + j);
    img[d] = img[s]; img[d + 1] = img[s + 1]; img[d + 2] = img[s + 2]; img[d + 3] = 255;
  }
}
// deterministic per-ore rng
function paintOre(tid, cols, seed0) {
  copyTile(3, tid);                                   // stone base (tile 3)
  const ox = (tid % 4) * TILE, oy = ((tid / 4) | 0) * TILE;
  let seed = seed0;
  const rnd = () => (seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF) / 0x7FFFFFFF;
  for (let n = 0; n < 9; n++) {                        // 9 mineral blobs
    const bx = 4 + ((rnd() * 54) | 0), by = 4 + ((rnd() * 54) | 0);
    const rw = 3 + ((rnd() * 4) | 0), rh = 3 + ((rnd() * 4) | 0);
    for (let j = 0; j < rh; j++) for (let i = 0; i < rw; i++) {
      if (rnd() < 0.25) continue;                      // ragged edge
      const c = cols[(rnd() * cols.length) | 0];
      setrgb(ox + Math.min(bx + i, 63), oy + Math.min(by + j, 63),
             (c >> 16) & 255, (c >> 8) & 255, c & 255);
    }
  }
}
paintOre(19, [0x1c1c1c, 0x2a2a2a, 0x111111], 101);     // coal: near-black
paintOre(20, [0xc89060, 0xd8a878, 0xb07a4a], 202);     // iron: tan/ochre

// --- encode (filter 0 rows) ---
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
const out = Buffer.alloc(newH * (stride + 1));
for (let y = 0; y < newH; y++) img.copy(out, y * (stride + 1) + 1, y * stride, (y + 1) * stride);
const ihdr = Buffer.alloc(13);
ihdr.writeUInt32BE(W); ihdr.writeUInt32BE(newH, 4); ihdr[8] = 8; ihdr[9] = 6;
writeFileSync(PATH_, Buffer.concat([
  Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]),
  chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(out)), chunk('IEND', Buffer.alloc(0)),
]));
console.log(`atlas.png: coal(19) + iron(20) painted, ${W}x${newH}`);
