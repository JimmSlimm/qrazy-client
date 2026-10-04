const physicalFiles = require('./update-files.cjs');
const fs = physicalFiles.promises;
const path = require('node:path');
const crypto = require('node:crypto');
const zlib = require('node:zlib');
const { promisify } = require('node:util');
const gunzip = promisify(zlib.gunzip);
const { DATA_START, rangeRequest, readBundleManifest } = require('./update-bundle.cjs');
const MAX_BYTES = 1024 * 1024 * 1024;
const hash = bytes => crypto.createHash('sha256').update(bytes).digest('hex');
async function hashFile(file) {
  const digest = crypto.createHash('sha256');
  for await (const chunk of physicalFiles.createReadStream(file)) digest.update(chunk);
  return digest.digest('hex');
}
async function hashBytes(bytes) {
  const digest = crypto.createHash('sha256');
  for (let offset = 0; offset < bytes.length; offset += 1024 * 1024) {
    digest.update(bytes.subarray(offset, offset + 1024 * 1024));
    await new Promise(resolve => setImmediate(resolve));
  }
  return digest.digest('hex');
}
function versionParts(value) {
  if (typeof value !== 'string' || !/^\d{1,6}\.\d{1,6}\.\d{1,6}$/.test(value)) throw new Error('Invalid stable version');
  return value.split('.').map(Number);
}
function newer(a, b) {
  const aa = versionParts(a), bb = versionParts(b);
  for (let i = 0; i < 3; i++) if (aa[i] !== bb[i]) return aa[i] > bb[i];
  return false;
}
function validPath(value, platform) {
  if (typeof value !== 'string' || value.length > 240 || !/^[a-zA-Z0-9_. /-]+$/.test(value)) return false;
  const parts = value.split('/');
  if (parts.some(p => !p || p === '.' || p === '..' || /[. ]$/.test(p) || /^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\.|$)/i.test(p))) return false;
  if (platform === 'win32-x64') return value === 'Qrazy.exe' || value === 'LICENSE' || value.startsWith('runtime/');
  return ['Qrazy', 'LICENSE', 'LICENSE.electron', 'LICENSES.chromium.html', 'LINUX-SUPPORT.txt', 'SYSTEM-REQUIREMENTS.txt', 'chrome-sandbox', 'chrome_crashpad_handler', 'chrome_100_percent.pak', 'chrome_200_percent.pak', 'icudtl.dat', 'resources.pak', 'snapshot_blob.bin', 'v8_context_snapshot.bin', 'vk_swiftshader_icd.json', 'version'].includes(value) || /^(locales\/[^/]+\.pak|resources\/(app\.asar|qrazy-input-x11)|[^/]+\.so(?:\.\d+)*)$/.test(value);
}
function verifyManifest(envelope, config, platform) {
  if (!config.publicKey) throw new Error('Update signing key is not configured');
  if (!envelope || typeof envelope.payload !== 'string' || typeof envelope.signature !== 'string' || envelope.payload.length > 3 * 1024 * 1024) throw new Error('Invalid update manifest');
  const bytes = Buffer.from(envelope.payload, 'base64');
  if (!crypto.verify(null, bytes, config.publicKey, Buffer.from(envelope.signature, 'base64'))) throw new Error('Update signature is invalid');
  const manifest = JSON.parse(bytes.toString('utf8'));
  versionParts(manifest.version);
  if (![1, 2].includes(manifest.schema) || typeof manifest.notes !== 'string' || manifest.notes.length > 8000) throw new Error('Unsupported update manifest');
  if (manifest.schema === 2 && (!manifest.bundle || manifest.bundle.dataStart !== DATA_START || !Number.isSafeInteger(manifest.bundle.dataSize) || manifest.bundle.dataSize < 1 || manifest.bundle.dataSize > 2 * MAX_BYTES || typeof manifest.bundle.url !== 'string')) throw new Error('Invalid update package inventory');
  const files = manifest.platforms?.[platform];
  if (!Array.isArray(files) || !files.length || files.length > 4096) throw new Error('No update for this platform');
  let total = 0; const names = new Set();
  for (const file of files) {
    if (!validPath(file.path, platform) || names.has(file.path.toLowerCase())) throw new Error('Unsafe or duplicate update path');
    names.add(file.path.toLowerCase());
    for (const field of ['size', 'downloadSize']) if (!Number.isSafeInteger(file[field]) || file[field] < 1 || file[field] > MAX_BYTES) throw new Error('Invalid update size');
    for (const field of ['sha256', 'downloadSha256']) if (!/^[a-f0-9]{64}$/.test(file[field])) throw new Error('Invalid update hash');
    const url = new URL(file.url);
    if (!file.url.startsWith(config.assetPrefix) || url.protocol !== 'https:' || url.username || url.password || url.search || url.hash || !/^[a-zA-Z0-9._/-]+$/.test(url.pathname)) throw new Error('Untrusted update URL');
    if (manifest.schema === 2 && (file.url !== manifest.bundle.url || !Number.isSafeInteger(file.offset) || file.offset < 0 || file.offset + file.downloadSize > manifest.bundle.dataSize)) throw new Error('Invalid update package range');
    if (![0o644, 0o755].includes(file.mode)) throw new Error('Invalid update permissions');
    total += file.size;
  }
  if (total > MAX_BYTES || !names.has(platform === 'win32-x64' ? 'runtime/resources/app.asar' : 'resources/app.asar') || !names.has(platform === 'win32-x64' ? 'qrazy.exe' : 'qrazy')) throw new Error('Incomplete or oversized update');
  const required = platform === 'win32-x64' ? ['runtime/electron.exe', 'runtime/resources/qrazy-input.node'] : ['resources/qrazy-input-x11', 'chrome-sandbox'];
  if (required.some(name => !names.has(name))) throw new Error('Incomplete update runtime');
  if (platform === 'linux-x64' && files.some(file => ['Qrazy', 'chrome-sandbox', 'chrome_crashpad_handler', 'resources/qrazy-input-x11'].includes(file.path) && file.mode !== 0o755)) throw new Error('Linux executables must be executable');
  for (const name of names) if ([...names].some(other => other.startsWith(name + '/'))) throw new Error('Conflicting update paths');
  return { version: manifest.version, notes: manifest.notes, files, ...(manifest.schema === 2 ? { bundle: manifest.bundle } : {}) };
}
async function noLinks(root, relative = '') {
  let current = path.resolve(root);
  // Check ancestors too: an installation redirected through a junction is unsafe.
  const ancestors = []; for (let p = current; ; p = path.dirname(p)) { ancestors.push(p); if (p === path.dirname(p)) break; }
  for (const p of ancestors.reverse()) if ((await fs.lstat(p)).isSymbolicLink()) throw new Error('Update paths cannot contain links');
  for (const part of relative.split('/').filter(Boolean)) {
    current = path.join(current, part);
    try { if ((await fs.lstat(current)).isSymbolicLink()) throw new Error('Update paths cannot contain links'); }
    catch (error) { if (error.code !== 'ENOENT') throw error; }
  }
  return current;
}
async function readBounded(response, limit, progress = () => {}) {
  if (!response.ok) { const error = new Error(`Update server returned HTTP ${response.status}`); error.status = response.status; throw error; }
  const reader = response.body.getReader(); const chunks = []; let size = 0;
  try {
    while (true) {
      const { done, value } = await reader.read(); if (done) break;
      size += value.byteLength; if (size > limit) throw new Error('Update download exceeded its declared size');
      chunks.push(Buffer.from(value)); progress(size);
    }
  } finally { await reader.cancel().catch(() => {}); }
  return Buffer.concat(chunks);
}
class Updater {
  constructor({ root, directory, version, platform, config, fetch, requestRange = rangeRequest, allUsers = false, notify = () => {} }) {
    Object.assign(this, { root, directory, version, platform, config, fetch, requestRange, allUsers, notify });
    this.state = { phase: config.publicKey ? 'idle' : 'unconfigured', installedVersion: version, message: config.publicKey ? '' : 'Updates are being prepared for the first release.' };
    this.busy = false;
  }
  getState() { return { ...this.state, requiresAdmin: this.allUsers }; }
  stagingParent() { return this.allUsers ? this.directory : path.dirname(path.resolve(this.root)); }
  setState(value) { this.state = { installedVersion: this.version, ...value }; this.notify(this.getState()); return this.getState(); }
  async discardStage() {
    if (!this.stage) return;
    const stage = path.resolve(this.stage);
    if (path.dirname(stage) !== path.resolve(this.stagingParent()) || !/^\.qrazy-update-[a-zA-Z0-9]+$/.test(path.basename(stage))) throw new Error('Unsafe staging cleanup path');
    await noLinks(stage);
    await fs.rm(stage, { recursive: true, force: true }); this.stage = null;
  }
  async restorePending() {
    const pendingPath = path.join(this.directory, 'pending.json');
    try {
      if ((await fs.stat(pendingPath)).size > 4 * 1024 * 1024) throw new Error('Invalid pending update');
      const pending = JSON.parse(await fs.readFile(pendingPath, 'utf8'));
      if (path.resolve(pending.root) !== path.resolve(this.root)) return this.getState();
      const release = verifyManifest(pending.envelope, this.config, this.platform);
      if (!newer(release.version, this.version)) { await fs.unlink(pendingPath); return this.getState(); }
      const stage = path.resolve(pending.stage);
      if (path.dirname(stage) !== path.resolve(this.stagingParent()) || !/^\.qrazy-update-[a-zA-Z0-9]+$/.test(path.basename(stage))) throw new Error('Invalid pending update folder');
      await noLinks(stage);
      if (!(await fs.stat(stage)).isDirectory()) throw new Error('Pending update folder is missing');
      this.stage = stage; this.release = release; this.envelope = pending.envelope;
      return this.setState({ phase: 'ready', version: release.version, notes: release.notes,
        downloadBytes: Number.isSafeInteger(pending.downloadBytes) ? pending.downloadBytes : 0,
        message: 'Previously downloaded client update is ready. Files will be verified again before installation.' });
    } catch (error) {
      if (error.code !== 'ENOENT') this.setState({ phase: 'error', message: 'The saved update could not be restored. Check for updates again.' });
      await fs.unlink(pendingPath).catch(() => {}); return this.getState();
    }
  }
  async request(url, limit, progress, timeout = 120000) {
    const signal = AbortSignal.timeout(timeout);
    const response = await this.fetch(url, { cache: 'no-store', credentials: 'omit', signal, redirect: 'follow' });
    return readBounded(response, limit, progress);
  }
  async check() {
    if (this.busy || this.state.phase === 'ready') return this.getState();
    if (!this.config.publicKey) return this.getState();
    this.busy = true; this.setState({ phase: 'checking', message: 'Checking for client updates…' });
    try {
      const bundle = this.config.bundleURL ? await readBundleManifest(this.config.bundleURL, this.requestRange) : null;
      const envelope = bundle ? bundle.envelope : JSON.parse(await this.request(this.config.manifestURL, 4 * 1024 * 1024, undefined, 15000));
      const release = verifyManifest(envelope, this.config, this.platform);
      if (bundle && (!release.bundle || bundle.total !== release.bundle.dataStart + release.bundle.dataSize)) throw new Error('Update package size differs from signed inventory');
      if (!newer(release.version, this.version)) return this.setState({ phase: 'current', message: 'Your client is up to date.' });
      this.release = release;
      this.envelope = envelope;
      const changed = [];
      for (const file of release.files) {
        const target = await noLinks(this.root, file.path);
        let same = false;
        try { const stat = await fs.stat(target); same = stat.isFile() && stat.size === file.size && await hashFile(target) === file.sha256; }
        catch (error) { if (error.code !== 'ENOENT') throw error; }
        if (!same) changed.push(file.path);
      }
      this.changed = new Set(changed);
      return this.setState({ phase: 'available', version: release.version, notes: release.notes, downloadBytes: release.files.filter(f => this.changed.has(f.path)).reduce((s, f) => s + f.downloadSize, 0), message: 'Qrazy client update available' });
    } catch (error) { this.release = null; return this.setState({ phase: error.status === 404 ? 'idle' : 'error', message: error.status === 404 ? 'Client updates are not available yet. You can keep playing.' : 'Could not check for client updates. Please try again later.' }); }
    finally { this.busy = false; }
  }
  async download() {
    if (this.busy || this.state.phase !== 'available' || !this.release) return this.getState();
    this.busy = true;
    const previous = this.getState(); let downloaded = 0, lastProgress = 0;
    this.setState({ ...previous, phase: 'downloading', downloadedBytes: 0, message: 'Downloading client update…' });
    try {
      await noLinks(this.root);
      await this.discardStage();
      if (this.allUsers) await fs.mkdir(this.directory, { recursive: true });
      const parent = this.stagingParent();
      await noLinks(parent);
      const disk = await fs.statfs(parent);
      const needed = this.release.files.reduce((sum, f) => sum + f.size + f.downloadSize, 0) + 1024 * 1024 * 1024;
      if (disk.bavail * disk.bsize < needed) throw new Error('Not enough free space to stage the update safely. Keep at least 1 GiB free.');
      this.stage = await fs.mkdtemp(path.join(parent, '.qrazy-update-'));
      for (const file of this.release.files) {
        const target = path.join(this.stage, ...file.path.split('/'));
        await fs.mkdir(path.dirname(target), { recursive: true });
        if (!this.changed.has(file.path)) {
          const source = await noLinks(this.root, file.path);
          await fs.copyFile(source, target, require('node:fs').constants.COPYFILE_EXCL);
          if ((await fs.stat(target)).size !== file.size || await hashFile(target) !== file.sha256) throw new Error('Existing file changed; check for updates again');
        } else {
          const progress = size => {
            if (Date.now() - lastProgress < 150) return;
            lastProgress = Date.now();
            this.setState({ ...previous, phase: 'downloading', downloadedBytes: downloaded + size, message: 'Downloading client update…' });
          };
          let compressed;
          if (this.release.bundle) {
            const start = this.release.bundle.dataStart + file.offset;
            const result = await this.requestRange(file.url, start, start + file.downloadSize - 1, progress);
            if (result.total !== this.release.bundle.dataStart + this.release.bundle.dataSize) throw new Error('Update package changed during download');
            compressed = result.bytes;
          } else compressed = await this.request(file.url, file.downloadSize, progress);
          if (compressed.length !== file.downloadSize || await hashBytes(compressed) !== file.downloadSha256) throw new Error('Downloaded file failed verification');
          const bytes = await gunzip(compressed, { maxOutputLength: file.size });
          if (bytes.length !== file.size || await hashBytes(bytes) !== file.sha256) throw new Error('Update file failed verification');
          await fs.writeFile(target, bytes, { mode: file.mode, flag: 'wx' });
          downloaded += compressed.length;
        }
        await fs.chmod(target, file.mode);
      }
      await fs.mkdir(this.directory, { recursive: true });
      const pending = path.join(this.directory, 'pending.json');
      await fs.writeFile(pending + '.tmp', JSON.stringify({ root: this.root, stage: this.stage, envelope: this.envelope, downloadBytes: previous.downloadBytes }), { mode: 0o600 });
      await fs.rename(pending + '.tmp', pending);
      return this.setState({ ...previous, phase: 'ready', downloadedBytes: downloaded, message: 'Client update verified and ready to install. Your saved login, settings and downloaded assets will be kept.' });
    } catch (error) {
      if (this.stage) await this.discardStage().catch(() => {});
      this.stage = null;
      const explanation = ['EACCES', 'EPERM'].includes(error.code) ? 'The installation folder is not writable. Move the portable client to a writable folder, or replace it manually.' : error.message;
      return this.setState({ ...previous, phase: 'available', message: `${explanation} You can retry, or use Open downloads for manual replacement.` });
    } finally { this.busy = false; }
  }
  async prepareInstall(pid, parentPid) {
    if (this.busy || this.state.phase !== 'ready' || !this.stage) throw new Error('Download and verify an update first');
    try {
      await noLinks(this.root); await noLinks(this.stage);
      for (const file of this.release.files) {
        const target = await noLinks(this.stage, file.path);
        if ((await fs.stat(target)).size !== file.size || await hashFile(target) !== file.sha256) throw new Error('Staged update changed');
      }
    } catch (error) {
      await fs.unlink(path.join(this.directory, 'pending.json')).catch(() => {});
      const message = 'The staged update changed or could not be read. Check for updates and download it again.';
      this.setState({ phase: 'error', message });
      throw new Error(message, { cause: error });
    }
    await fs.mkdir(this.directory, { recursive: true });
    const token = crypto.randomBytes(12).toString('hex');
    const backup = path.join(path.dirname(this.root), '.qrazy-previous-' + token);
    const plan = { root: this.root, stage: this.stage, backup, version: this.release.version, pid, parentPid, files: this.release.files.map(({ path, sha256 }) => ({ path, sha256 })), result: path.join(this.directory, 'result.json'), ...(this.allUsers ? { envelope: this.envelope, jobToken: token } : {}) };
    const planPath = path.join(this.directory, 'plan-' + token + '.json');
    await fs.writeFile(planPath, JSON.stringify(plan), { mode: 0o600, flag: 'wx' });
    return { plan, planPath };
  }
}
module.exports = { Updater, verifyManifest, validPath, newer, hash, hashFile, noLinks, readBounded };
