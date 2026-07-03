// Generates .claude/skills/raylib/references/*.md from the PINNED raylib
// headers in the build tree (_deps/raylib-src) — version-exact API listings,
// unlike scraping the website. Rerun after bumping the raylib GIT_TAG.
// Run: bun tools/make_raylib_skill.mjs
import { readFileSync, writeFileSync, mkdirSync } from 'fs';
import { dirname, join } from 'path';
import { fileURLToPath } from 'url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const SRC = join(ROOT, '..', 'craft_raylib_build', 'native', '_deps', 'raylib-src', 'src');
const OUT = join(ROOT, '.claude', 'skills', 'raylib', 'references');
mkdirSync(OUT, { recursive: true });

const rl = readFileSync(join(SRC, 'raylib.h'), 'utf8');

// --- split raylib.h function declarations by "(Module: X)" banners ---
const modules = {};   // name -> [{sub, lines}]
let cur = null, curSub = '';
for (const raw of rl.split('\n')) {
  const line = raw.trimEnd();
  const mod = line.match(/\/\/ (.+) \(Module: (\w+)\)/);
  if (mod) {
    cur = mod[2];
    modules[cur] ??= [];
    curSub = mod[1];
    continue;
  }
  if (!cur) continue;
  const sub = line.match(/^\/\/ ([A-Z][^/]+functions.*)$/i);
  if (sub && !line.includes('---')) { curSub = sub[1].trim(); continue; }
  if (line.startsWith('RLAPI ')) {
    modules[cur].push({ sub: curSub, decl: line.replace(/^RLAPI /, '').replace(/\s+\/\//, '  //') });
  }
}

let total = 0;
for (const [name, fns] of Object.entries(modules)) {
  let md = `# raylib 5.5 — module: ${name}\n\nExact signatures from the pinned raylib.h. \`RLAPI\` prefix stripped.\n`;
  let lastSub = null;
  for (const f of fns) {
    if (f.sub !== lastSub) { md += `\n## ${f.sub}\n\n`; lastSub = f.sub; }
    md += '    ' + f.decl + '\n';
  }
  writeFileSync(join(OUT, `${name}.md`), md);
  console.log(`${name}.md: ${fns.length} functions`);
  total += fns.length;
}

// --- structs, enums and color defines ---
{
  const structs = [...rl.matchAll(/\/\/ (\w[^\n]*)\ntypedef struct (\w+) \{([\s\S]*?)\n\} \w+;/g)]
    .map(m => `## ${m[2]} — ${m[1]}\n\n\`\`\`c\ntypedef struct ${m[2]} {${m[3]}\n} ${m[2]};\n\`\`\`\n`);
  const colors = [...rl.matchAll(/#define (\w+) +CLITERAL\(Color\)\{ ([^}]+) \}[^\n]*/g)]
    .map(m => `    ${m[1].padEnd(12)} { ${m[2]} }`);
  const enums = [...rl.matchAll(/\/\/ (\w[^\n]*)\ntypedef enum \{([\s\S]*?)\n\} (\w+);/g)]
    .map(m => `## ${m[3]} — ${m[1]}\n\n\`\`\`c\ntypedef enum {${m[2]}\n} ${m[3]};\n\`\`\`\n`);
  writeFileSync(join(OUT, 'types.md'),
    `# raylib 5.5 — structs, enums, colors\n\n# Colors\n\n${colors.join('\n')}\n\n# Structs\n\n${structs.join('\n')}\n# Enums\n\n${enums.join('\n')}`);
  console.log(`types.md: ${structs.length} structs, ${enums.length} enums, ${colors.length} colors`);
}

// --- raymath.h ---
{
  const rm = readFileSync(join(SRC, 'raymath.h'), 'utf8');
  const fns = [...rm.matchAll(/^RMAPI ([^\n]+)/gm)].map(m => '    ' + m[1]);
  writeFileSync(join(OUT, 'raymath.md'),
    `# raymath 5.5 — vector/matrix/quaternion helpers\n\nHeader-only (RMAPI = static inline). NOTE the composition convention used all over this codebase: MatrixMultiply(A, B) applies A FIRST, then B (row-vector style) — scale, then rotate, then translate reads left to right.\n\n${fns.join('\n')}\n`);
  console.log(`raymath.md: ${fns.length} functions`);
}

// --- rlgl.h ---
{
  const rg = readFileSync(join(SRC, 'rlgl.h'), 'utf8');
  const fns = [...rg.matchAll(/^RLAPI ([^\n]+)/gm)].map(m => '    ' + m[1]);
  writeFileSync(join(OUT, 'rlgl.md'),
    `# rlgl 5.5 — low-level GL abstraction\n\nImmediate-mode calls (rlBegin/rlVertex3f/...) are BATCHED: state toggles like rlDisableBackfaceCulling/rlDisableDepthTest apply at flush time — call rlDrawRenderBatchActive() before flipping state back (see hand.c, touch draw, draw_world).\n\n${fns.join('\n')}\n`);
  console.log(`rlgl.md: ${fns.length} functions`);
}
console.log(`total raylib.h functions: ${total}`);
