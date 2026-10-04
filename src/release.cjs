// Local release preparation only: never uploads, commits, or publishes.
// node src/release.cjs --root=dist/Qrazy-win32-x64 --platform=win32-x64
//   --key=local/update-signing/private.pem --version=0.3.0 --out=local/release-0.3.0
const fs = require('node:fs/promises');
const path = require('node:path');
const crypto = require('node:crypto');
const zlib = require('node:zlib');
const { promisify } = require('node:util');
const gzip = promisify(zlib.gzip);
const { hash, validPath, newer, verifyManifest } = require('./updater.cjs');
async function prepare({ root, platform, key, version, out, notes = '', previous, baseline }) {
  newer(version, '0.0.0');
  const privateKey = crypto.createPrivateKey(await fs.readFile(key));
  const publicKey = crypto.createPublicKey(privateKey).export({ type: 'spki', format: 'pem' });
  await fs.mkdir(out, { recursive: true });
  const config = require('./update-config.cjs');
  if (publicKey !== config.publicKey) throw new Error('Signing key does not match the client public key');
  const { extractFile } = await import('@electron/asar');
  const asar = path.join(root, platform === 'win32-x64' ? 'runtime/resources/app.asar' : 'resources/app.asar');
  const packagedVersion = JSON.parse(extractFile(asar, 'package.json')).version;
  if (packagedVersion !== version) throw new Error('Release version must match the packaged application version');
  let platforms = {};
  let oldFiles = new Map();
  if (baseline) {
    const envelope = JSON.parse(await fs.readFile(baseline));
    const payload = JSON.parse(Buffer.from(envelope.payload, 'base64'));
    const firstPlatform = Object.keys(payload.platforms || {})[0];
    verifyManifest(envelope, config, firstPlatform);
    if (!newer(version, payload.version)) throw new Error('The release must be newer than its update baseline');
    if (payload.platforms[platform]) oldFiles = new Map(verifyManifest(envelope, config, platform).files.map(file => [file.path, file]));
  }
  if (previous) {
    const envelope = JSON.parse(await fs.readFile(previous));
    verifyManifest(envelope, config, Object.keys(JSON.parse(Buffer.from(envelope.payload, 'base64')).platforms)[0]);
    const existing = JSON.parse(Buffer.from(envelope.payload, 'base64'));
    if (existing.version !== version) throw new Error('Merged platform manifests must have the same version');
    platforms = existing.platforms;
  }
  const files = [];
  async function walk(directory, prefix = '') {
    for (const entry of await fs.readdir(directory, { withFileTypes: true })) {
      const relative = prefix + entry.name, full = path.join(directory, entry.name);
      if (entry.isSymbolicLink()) throw new Error('Release cannot contain links');
      if (entry.isDirectory()) { await walk(full, relative + '/'); continue; }
      if (!entry.isFile() || !validPath(relative, platform)) throw new Error('Unexpected release file: ' + relative);
      const bytes = await fs.readFile(full), sha256 = hash(bytes);
      const executable = platform === 'linux-x64' && ['Qrazy', 'chrome-sandbox', 'chrome_crashpad_handler', 'resources/qrazy-input-x11'].includes(relative);
      const mode = executable ? 0o755 : 0o644;
      const old = oldFiles.get(relative);
      if (old && old.sha256 === sha256 && old.size === bytes.length && old.mode === mode) {
        files.push({ ...old }); continue;
      }
      const compressed = await gzip(bytes, { level: 9 });
      const name = platform + '-' + sha256 + '.gz';
      await fs.writeFile(path.join(out, name), compressed);
      files.push({ path: relative, size: bytes.length, sha256, downloadSize: compressed.length, downloadSha256: hash(compressed), mode, url: config.assetPrefix + 'v' + version + '/' + name });
    }
  }
  await walk(root); platforms[platform] = files.sort((a, b) => a.path.localeCompare(b.path));
  const payload = Buffer.from(JSON.stringify({ schema: 1, version, notes, platforms }));
  const envelope = { payload: payload.toString('base64'), signature: crypto.sign(null, payload, privateKey).toString('base64') };
  verifyManifest(envelope, config, platform);
  await fs.writeFile(path.join(out, 'qrazy-update.json'), JSON.stringify(envelope));
  return { version, platform, files: files.length, downloadBytes: files.reduce((sum, file) => sum + file.downloadSize, 0) };
}
if (require.main === module) {
  const arg = name => process.argv.find(a => a.startsWith('--' + name + '='))?.slice(name.length + 3);
  prepare({ root: arg('root'), platform: arg('platform'), key: arg('key'), version: arg('version'), out: arg('out'), notes: arg('notes') || '', previous: arg('merge'), baseline: arg('baseline') }).then(result => console.log(JSON.stringify(result))).catch(error => { console.error(error.message); process.exitCode = 1; });
}
module.exports = { prepare };
