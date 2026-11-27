'use strict';
// Checks a third_party folder (miniz/, stb/, glslang/) against deps.lock.json: single-file libraries by content hash, glslang by its git commit.
// Usage: node src/nativeclient/check-deps.cjs <third_party dir>
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const cp = require('node:child_process');

const dir = process.argv[2];
if (!dir) { console.error('usage: node src/nativeclient/check-deps.cjs <third_party dir>'); process.exit(2); }
const lock = JSON.parse(fs.readFileSync(path.join(__dirname, 'deps.lock.json'), 'utf8'));
const bad = [];
for (const lib of ['miniz', 'stb']) for (const [file, want] of Object.entries(lock[lib].files)) {
  const p = path.join(dir, lib, file);
  if (!fs.existsSync(p)) bad.push(`${lib}/${file}: missing`); else if (crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex') !== want) bad.push(`${lib}/${file}: differs from the lock`);
}
try { const head = cp.execFileSync('git', ['-C', path.join(dir, 'glslang'), 'rev-parse', 'HEAD'], { encoding: 'utf8' }).trim(); if (head !== lock.glslang.commit) bad.push(`glslang: at ${head.slice(0, 12)}, lock says ${lock.glslang.commit.slice(0, 12)}`); }
catch { bad.push('glslang: not a git checkout'); }
if (bad.length) { console.error('dependencies differ from deps.lock.json:\n  ' + bad.join('\n  ')); process.exit(1); }
console.log('dependencies match deps.lock.json');
