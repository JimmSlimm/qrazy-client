'use strict';
// Puts a game core library into a package folder under the name the client trusts: core/qrazy_core-<build>-<sha256>.<dll|so>.
// The client accepts a copy only when its content hashes to the name it carries, so the name is made here from the file itself and nothing else has to be kept.
// Usage: node src/bundle-core.cjs <core file> <package dir> --build <number>
// <build> is the core's own build number as its publisher (qlrace2) numbers it: the client takes the highest build it can use, bundled or downloaded.
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');

function bundleCore(file, packageDir, build) {
  if (!/^\d{1,15}$/.test(String(build))) throw new Error('--build must be a whole number');
  const ext = path.extname(file).toLowerCase();
  if (ext !== '.dll' && ext !== '.so') throw new Error('the core must be a .dll or a .so');
  const bytes = fs.readFileSync(file);
  if (bytes.length < 1024 || bytes.length > 64 * 1024 * 1024) throw new Error('the core has an implausible size');
  if (!bytes.includes('qc_interface_version')) throw new Error('this is not a game core (no qc_interface_version export)');
  const sha = crypto.createHash('sha256').update(bytes).digest('hex');
  const dir = path.join(packageDir, 'core');
  fs.mkdirSync(dir, { recursive: true });
  const name = `qrazy_core-${build}-${sha}${ext}`;
  const target = path.join(dir, name);
  const part = target + '.part';
  fs.writeFileSync(part, bytes);
  fs.renameSync(part, target);
  for (const other of fs.readdirSync(dir)) if (other !== name && /^qrazy_core-\d+-[0-9a-f]{64}\.(dll|so)$/.test(other)) fs.rmSync(path.join(dir, other));   // one bundled core per package
  return target;
}

if (require.main === module) {
  const args = process.argv.slice(2);
  const at = args.indexOf('--build');
  if (args.length !== 4 || at !== 2 || at + 1 >= args.length) { console.error('usage: node src/bundle-core.cjs <core file> <package dir> --build <number>'); process.exit(2); }
  try { console.log(bundleCore(args[0], args[1], args[3])); } catch (error) { console.error(error.message); process.exit(1); }
}
module.exports = { bundleCore };
