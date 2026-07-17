// STALE / RETIRED FROM THE GATE. craft's gen has forked from the legacy JS (JS
// cross-play was dropped); the determinism gate is now a committed C golden
// (tests/gen_golden.txt) vs gen_test.exe, NOT this file. Kept only as a snapshot
// of the original JS terrain. Do not treat a diff against this as authoritative.
//
// Reference terrain dump from the ORIGINAL JS implementation (game.js copy).
// Functions below are copied verbatim from craft_js/game.js (post pow->sqrt patch).

const CHUNK = 16, WORLD_H = 64, SEA_Y = 16;
const CHUNK_VOL = CHUNK * CHUNK * WORLD_H;
const AIR = 0, GRASS = 1, DIRT = 2, STONE = 3, WOOD = 4, LEAVES = 5, SAND = 6;
const li = (lx, y, lz) => lx + lz * CHUNK + y * CHUNK * CHUNK;

// --- verbatim from game.js ---
function hash2(x, z) {
  let h = ((x | 0) * 374761393 + (z | 0) * 668265263) | 0;
  h = Math.imul(h ^ (h >>> 13), 1274126177);
  return ((h ^ (h >>> 16)) >>> 0) / 4294967296;
}
function valueNoise(x, z) {
  const xi = Math.floor(x), zi = Math.floor(z), xf = x - xi, zf = z - zi;
  const u = xf * xf * (3 - 2 * xf), v = zf * zf * (3 - 2 * zf);
  const a = hash2(xi, zi), b = hash2(xi + 1, zi), c = hash2(xi, zi + 1), d = hash2(xi + 1, zi + 1);
  return a * (1 - u) * (1 - v) + b * u * (1 - v) + c * (1 - u) * v + d * u * v;
}
function fbm(x, z, oct) {
  let sum = 0, amp = 1, freq = 1, norm = 0;
  for (let i = 0; i < oct; i++) { sum += amp * valueNoise(x * freq, z * freq); norm += amp; amp *= 0.5; freq *= 2; }
  return sum / norm;
}
function terrainHeight(x, z) {
  const cont = fbm(x * 0.004, z * 0.004, 4);
  if (cont < 0.42)
    return Math.max(1, SEA_Y - 2 - Math.round((0.42 - cont) / 0.42 * 12));
  const land = (cont - 0.42) / 0.58;
  const coast = Math.min(1, land * 3);
  const hills = fbm(x * 0.018, z * 0.018, 5);
  const relief = hills * hills * Math.sqrt(hills) * 55;
  const h = SEA_Y - 1 + Math.round(land * 4 + relief * coast);
  return Math.max(1, Math.min(WORLD_H - 4, h));
}
function genChunk(cx, cz) {
  const blocks = new Uint8Array(CHUNK_VOL), water = new Uint8Array(CHUNK_VOL);
  for (let lx = 0; lx < CHUNK; lx++)
    for (let lz = 0; lz < CHUNK; lz++) {
      const wx = cx * CHUNK + lx, wz = cz * CHUNK + lz;
      const h = terrainHeight(wx, wz);
      for (let y = 0; y <= h; y++) {
        blocks[li(lx, y, lz)] =
          y === 0 ? STONE :
          y === h ? (h <= SEA_Y + 1 ? SAND : GRASS) :
          y > h - 3 ? DIRT : STONE;
      }
      if (h < SEA_Y) for (let y = h + 1; y <= SEA_Y; y++) water[li(lx, y, lz)] = 9;
      const forest = fbm(wx * 0.012 + 500, wz * 0.012 + 500, 3);
      const treeChance = forest > 0.6 ? 0.09 : forest > 0.45 ? 0.02 : 0;
      if (h > SEA_Y + 1 && lx >= 2 && lx <= CHUNK - 3 && lz >= 2 && lz <= CHUNK - 3
          && treeChance > 0 && hash2(wx * 131 + 7, wz * 131 + 13) < treeChance) {
        const base = h + 1;
        for (let i = 0; i < 4; i++) blocks[li(lx, base + i, lz)] = WOOD;
        const top = base + 4;
        for (let dx = -2; dx <= 2; dx++)
          for (let dz = -2; dz <= 2; dz++)
            for (let dy = -1; dy <= 1; dy++) {
              if (Math.abs(dx) === 2 && Math.abs(dz) === 2) continue;
              const k = li(lx + dx, top + dy, lz + dz);
              if (blocks[k] === AIR) blocks[k] = LEAVES;
            }
      }
    }
  return { blocks, water };
}
// --- end verbatim ---

const out = [];
// 1) height samples
for (let x = -512; x <= 512; x += 7)
  for (let z = -512; z <= 512; z += 7)
    out.push(`h ${x} ${z} ${terrainHeight(x, z)}`);
// 2) hash2 direct samples (catches ToInt32/imul bugs even if heights agree)
for (let x = -3000000000; x <= 3000000000; x += 700000001)
  for (let z = -3000000000; z <= 3000000000; z += 900000007)
    out.push(`n ${x} ${z} ${hash2(x, z).toFixed(17)}`);
// 3) FNV-1a of full chunk contents for chunks (-3..3)^2
function fnv1a(h, arr) { for (let i = 0; i < arr.length; i++) { h ^= arr[i]; h = Math.imul(h, 16777619) >>> 0; } return h; }
for (let cx = -3; cx <= 3; cx++)
  for (let cz = -3; cz <= 3; cz++) {
    const { blocks, water } = genChunk(cx, cz);
    let h = fnv1a(2166136261 >>> 0, blocks); h = fnv1a(h, water);
    out.push(`c ${cx} ${cz} ${h.toString(16)}`);
  }
console.log(out.join('\n'));
