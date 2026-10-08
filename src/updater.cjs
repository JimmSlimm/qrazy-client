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
const {verifyManifest,validPath,newer}=require('./sdlcef/legacy-release.cjs');
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
  constructor({ root, directory, version, platform, config, fetch, requestRange = rangeRequest, allUsers = false, appImage = null, notify = () => {} }) {
    if (platform === 'linux-appimage-x64' && !appImage) throw new Error('AppImage update location is required');
    if (appImage && (platform !== 'linux-appimage-x64' || !path.isAbsolute(appImage) || path.dirname(appImage) !== path.resolve(root))) throw new Error('Invalid AppImage update location');
    Object.assign(this, { root, directory, version, platform, config, fetch, requestRange, allUsers, appImage, notify });
    this.state = { phase: config.publicKey ? 'idle' : 'unconfigured', installedVersion: version, message: config.publicKey ? '' : 'Updates are being prepared for the first release.' };
    this.busy = false;
  }
  getState() { return { ...this.state, requiresAdmin: this.allUsers }; }
  stagingParent() { return this.appImage ? this.root : this.allUsers ? this.directory : path.dirname(path.resolve(this.root)); }
  installedFile(file) { return this.appImage ? noLinks(this.appImage) : noLinks(this.root, file.path); }
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
      if (path.resolve(pending.root) !== path.resolve(this.root) || (pending.appImage || null) !== this.appImage) return this.getState();
      const release = verifyManifest(pending.envelope, this.config, this.platform);
      if (!newer(release.version, this.version)) {
        if (this.platform === 'win32-x64' && release.version === this.version) {
          // The legacy helper leaves this signed envelope in userData. Preserve
          // only authenticated delivery metadata for the intermediate client's
          // automatic continuation before normal pending-update cleanup.
          try {
            for (const file of release.files) {
              const target = await this.installedFile(file);
              if (!(await fs.stat(target)).isFile() || (await fs.stat(target)).size !== file.size || await hashFile(target) !== file.sha256) throw new Error('Installed delivery changed');
            }
            const record = path.join(this.directory, 'installed-delivery.json');
            await noLinks(this.directory, 'installed-delivery.json');
            await noLinks(this.directory, 'installed-delivery.json.tmp');
            try { if (!(await fs.lstat(record + '.tmp')).isFile()) throw new Error('Unsafe delivery metadata temporary'); await fs.unlink(record + '.tmp'); }
            catch (error) { if (error.code !== 'ENOENT') throw error; }
            const output = await fs.open(record + '.tmp', 'wx', 0o600);
            try {
              await output.writeFile(JSON.stringify({ schema: 'qrazy-electron-installed-delivery-v1', root: this.root,
                envelope: { payload: pending.envelope.payload, signature: pending.envelope.signature } }));
              await output.sync();
            } finally { await output.close(); }
            await fs.rename(record + '.tmp', record);
          } catch {
            return this.setState({ phase: 'error', message: 'The installed Electron delivery could not be verified. Its signed update record is retained for recovery.' });
          }
        }
        await fs.unlink(pendingPath); return this.getState();
      }
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
        const target = await this.installedFile(file);
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
          const source = await this.installedFile(file);
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
      await fs.writeFile(pending + '.tmp', JSON.stringify({ root: this.root, appImage: this.appImage, stage: this.stage, envelope: this.envelope, downloadBytes: previous.downloadBytes }), { mode: 0o600 });
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
      if (this.appImage) {
        await noLinks(this.appImage);
        if (!(await fs.stat(this.appImage)).isFile()) throw new Error('Installed AppImage is missing');
      }
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
    const backup = path.join(this.appImage ? this.root : path.dirname(this.root), '.qrazy-previous-' + token + (this.appImage ? '.AppImage' : ''));
    const plan = { root: this.root, stage: this.stage, backup, ...(this.appImage ? { appImage: this.appImage } : {}), version: this.release.version, pid, parentPid, files: this.release.files.map(({ path, sha256 }) => ({ path, sha256 })), result: path.join(this.directory, 'result.json'), ...(process.platform === 'win32' ? { helperReady: true } : {}), ...(this.allUsers ? { envelope: this.envelope, jobToken: token } : {}) };
    // Applies only to helpers delivered with this source. Published clients
    // still start their replacement launcher; a transition bootstrap must
    // suppress that runtime start independently of this new plan field.
    if (process.platform === 'win32') plan.noRestart = true;
    const planPath = path.join(this.directory, 'plan-' + token + '.json');
    await fs.writeFile(planPath, JSON.stringify(plan), { mode: 0o600, flag: 'wx' });
    return { plan, planPath };
  }
}
module.exports = { Updater, verifyManifest, validPath, newer, hash, hashFile, noLinks, readBounded };
