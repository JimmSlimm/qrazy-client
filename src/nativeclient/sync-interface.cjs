'use strict';
// The contract headers the client builds against live in qlrace2 (the one source). This keeps a pinned snapshot in ./interface so the client builds without a qlrace2 checkout.
//   node src/nativeclient/sync-interface.cjs <qlrace2 interface dir>           copy the headers + manifest.json, after checking each header against that manifest
//   node src/nativeclient/sync-interface.cjs                                    check the snapshot against its own manifest.json (what a build without qlrace2 can do)
//   node src/nativeclient/sync-interface.cjs <qlrace2 interface dir> --check    report whether the snapshot is the same as qlrace2's current headers
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');

const here = path.join(__dirname, 'interface');
const sha = (file) => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');

function verify(dir) {
  const manifest = JSON.parse(fs.readFileSync(path.join(dir, 'manifest.json'), 'utf8'));
  const bad = [];
  for (const [name, info] of Object.entries(manifest.files)) {
    const file = path.join(dir, name);
    if (!fs.existsSync(file)) bad.push(`${name}: missing`); else if (sha(file) !== info.sha256) bad.push(`${name}: does not match manifest.json`);
  }
  return { manifest, bad };
}

const args = process.argv.slice(2), check = args.includes('--check'), from = args.find((a) => !a.startsWith('--'));
try {
  if (!from) {
    const { manifest, bad } = verify(here);
    if (bad.length) { console.error('snapshot is damaged:\n  ' + bad.join('\n  ')); process.exit(1); }
    console.log(`snapshot ok: interface version ${manifest.interfaceVersion}, ${Object.keys(manifest.files).length} headers`); process.exit(0);
  }
  const { manifest, bad } = verify(from);
  if (bad.length) { console.error("qlrace2's headers do not match their own manifest.json:\n  " + bad.join('\n  ')); process.exit(1); }
  const names = [...Object.keys(manifest.files), 'manifest.json'];
  if (check) {
    const diff = names.filter((n) => !fs.existsSync(path.join(here, n)) || sha(path.join(here, n)) !== sha(path.join(from, n)));
    console.log(diff.length ? 'OUT OF DATE: ' + diff.join(', ') : `up to date (interface version ${manifest.interfaceVersion})`); process.exit(diff.length ? 1 : 0);
  }
  fs.mkdirSync(here, { recursive: true });
  for (const n of names) fs.copyFileSync(path.join(from, n), path.join(here, n));
  const again = verify(here); if (again.bad.length) throw new Error('copy failed: ' + again.bad.join(', '));
  console.log(`snapshot written: interface version ${manifest.interfaceVersion}, ${names.length - 1} headers. Run gen-core-table.cjs ./interface if the version changed.`);
} catch (e) { console.error(e.message); process.exit(1); }
