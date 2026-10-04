const fs = require('node:fs/promises');
const path = require('node:path');
const { createHash, randomUUID } = require('node:crypto');
const HEADER = 4096, CHUNK = 1024 * 1024;
const LIMITS = { map: 512 * 1024 * 1024, sound: 64 * 1024 * 1024, shader: 512 * 1024 * 1024, texture: 512 * 1024 * 1024 };
const OPERATIONS = new Set(['has', 'openRead', 'readChunk', 'closeRead', 'beginWrite', 'writeChunk', 'finishWrite', 'abortWrite']);
function identity(kind, key) {
  if (typeof kind !== 'string' || !Object.hasOwn(LIMITS, kind) || typeof key !== 'string' || !key.length || Buffer.byteLength(key) > 2048 || /[\u0000-\u001f\u007f]/.test(key))
    throw new TypeError('Expected a map/sound/shader/texture kind and an asset key of 1–2048 UTF-8 bytes');
  return createHash('sha256').update(kind + '\0' + key).digest('hex');
}
async function writeAll(file, bytes, position) {
  let offset = 0;
  while (offset < bytes.length) {
    const { bytesWritten } = await file.write(bytes, offset, bytes.length - offset, position + offset);
    if (!bytesWritten) throw new Error('Asset write made no progress');
    offset += bytesWritten;
  }
}
async function readAll(file, bytes, position) {
  let offset = 0;
  while (offset < bytes.length) {
    const { bytesRead } = await file.read(bytes, offset, bytes.length - offset, position + offset);
    if (!bytesRead) throw new Error('Asset is truncated');
    offset += bytesRead;
  }
}
class AssetStore {
  constructor(directory, { maxBytes = 16 * 1024 ** 3, minFreeBytes = 1024 ** 3, maxPending = 16 } = {}) {
    this.directory = directory; this.transfers = new Map(); this.queue = Promise.resolve(); this.epoch = 0;
    this.maxBytes = maxBytes; this.minFreeBytes = minFreeBytes; this.maxPending = maxPending; this.pending = 0;
  }
  call(operation, args) {
    if (!OPERATIONS.has(operation) || !Array.isArray(args)) return Promise.reject(new TypeError('Unknown asset operation'));
    if (this.pending >= this.maxPending) return Promise.reject(new Error('Too many queued asset operations'));
    ++this.pending;
    const epoch = this.epoch;
    const work = this.queue.then(() => {
      if (epoch !== this.epoch) throw new Error('Asset operation cancelled by navigation');
      return this[operation](...args);
    });
    const result = work.finally(() => { --this.pending; });
    this.queue = result.catch(() => {}); return result;
  }
  reset() {
    ++this.epoch;
    const work = this.queue.then(async () => {
      for (const [token, transfer] of this.transfers) await this.dispose(token, transfer);
    });
    this.queue = work.catch(() => {}); return work;
  }
  filename(kind, key) { return path.join(this.directory, identity(kind, key) + '.asset'); }
  room() { if (this.transfers.size >= 4) throw new Error('At most four asset transfers may be open'); }
  async storageRoom(size) {
    let used = 0;
    for (const name of await fs.readdir(this.directory)) {
      if (!/^(?:[a-f0-9]{64}\.asset|[a-f0-9-]{36}\.partial)$/.test(name)) continue;
      used += (await fs.stat(path.join(this.directory, name))).size;
    }
    let reserved = 0;
    for (const transfer of this.transfers.values()) if (transfer.mode === 'write') reserved += transfer.size - transfer.offset;
    const required = size + HEADER;
    if (used + reserved + required > this.maxBytes) throw new Error('Desktop asset storage limit reached (16 GiB). Existing downloads are preserved.');
    const disk = await fs.statfs(this.directory);
    if (disk.bavail * disk.bsize - reserved - required < this.minFreeBytes) throw new Error('Not enough free disk space to save desktop assets (1 GiB reserve).');
  }
  transfer(token, mode) {
    if (typeof token !== 'string') throw new TypeError('Expected an asset transfer token');
    const value = this.transfers.get(token);
    if (!value || value.mode !== mode) throw new Error('Unknown or expired asset transfer');
    return value;
  }
  async dispose(token, value) {
    this.transfers.delete(token);
    try { await value.file.close(); }
    finally { if (value.temporary) await fs.unlink(value.temporary).catch(error => { if (error.code !== 'ENOENT') throw error; }); }
  }
  async metadata(kind, key) {
    const filename = this.filename(kind, key);
    let file;
    try { file = await fs.open(filename, 'r'); }
    catch (error) { if (error.code === 'ENOENT') return null; throw error; }
    try {
      const header = Buffer.alloc(HEADER); await readAll(file, header, 0);
      const length = header.readUInt32LE(4);
      if (header.toString('ascii', 0, 4) !== 'QAS1' || length < 1 || length > HEADER - 8) throw new Error('Invalid asset header');
      const data = JSON.parse(header.toString('utf8', 8, 8 + length));
      if (data.kind !== kind || data.key !== key || !Number.isSafeInteger(data.size) || data.size < 1 || data.size > LIMITS[kind] ||
          typeof data.sha256 !== 'string' || !/^[a-f0-9]{64}$/.test(data.sha256) || (await file.stat()).size !== HEADER + data.size)
        throw new Error('Invalid or incomplete asset');
      return { file, kind, key, size: data.size, sha256: data.sha256 };
    } catch (error) { await file.close(); throw error; }
  }
  async has(kind, key) {
    const data = await this.metadata(kind, key);
    if (!data) return false;
    await data.file.close(); return true;
  }
  async openRead(kind, key) {
    this.room(); const data = await this.metadata(kind, key); if (!data) return null;
    const token = randomUUID();
    this.transfers.set(token, { ...data, mode: 'read', offset: 0, hash: createHash('sha256') });
    return { token, size: data.size, sha256: data.sha256 };
  }
  async readChunk(token) {
    const value = this.transfer(token, 'read');
    try {
      const bytes = Buffer.alloc(Math.min(CHUNK, value.size - value.offset));
      await readAll(value.file, bytes, HEADER + value.offset);
      value.offset += bytes.length; value.hash.update(bytes);
      const done = value.offset === value.size;
      if (done) {
        if (value.hash.digest('hex') !== value.sha256) throw new Error('Asset checksum mismatch; download it again');
        await this.dispose(token, value);
      }
      return { bytes: new Uint8Array(bytes), done };
    } catch (error) { if (this.transfers.has(token)) await this.dispose(token, value); throw error; }
  }
  async closeRead(token) { await this.dispose(token, this.transfer(token, 'read')); }
  async beginWrite(descriptor) {
    if (!descriptor || typeof descriptor !== 'object') throw new TypeError('Expected an asset descriptor');
    const { kind, key, size, sha256 } = descriptor;
    const destination = this.filename(kind, key);
    if (!Number.isSafeInteger(size) || size < 1 || size > LIMITS[kind]) throw new RangeError('Invalid asset size');
    if (sha256 !== undefined && (typeof sha256 !== 'string' || !/^[a-f0-9]{64}$/.test(sha256))) throw new TypeError('Expected lowercase SHA-256');
    if (Buffer.byteLength(JSON.stringify({ kind, key, size, sha256: '0'.repeat(64) })) > HEADER - 8) throw new TypeError('Asset key requires too much metadata');
    this.room(); await fs.mkdir(this.directory, { recursive: true });
    await this.storageRoom(size);
    const token = randomUUID(), temporary = path.join(this.directory, token + '.partial');
    const file = await fs.open(temporary, 'wx');
    const value = { mode: 'write', file, temporary, destination, kind, key, size, sha256, offset: 0, hash: createHash('sha256') };
    this.transfers.set(token, value);
    try { await writeAll(file, Buffer.alloc(HEADER), 0); }
    catch (error) { await this.dispose(token, value); throw error; }
    return { token, maxChunkBytes: CHUNK };
  }
  async writeChunk(token, data) {
    const value = this.transfer(token, 'write');
    try {
      let bytes;
      if (data instanceof ArrayBuffer) bytes = Buffer.from(data);
      else if (data instanceof Uint8Array) bytes = Buffer.from(data.buffer, data.byteOffset, data.byteLength);
      else throw new TypeError('Expected Uint8Array or ArrayBuffer');
      if (!bytes.length || bytes.length > CHUNK || value.offset + bytes.length > value.size) throw new RangeError('Invalid asset chunk size');
      await writeAll(value.file, bytes, HEADER + value.offset);
      value.offset += bytes.length; value.hash.update(bytes);
    } catch (error) { await this.dispose(token, value); throw error; }
  }
  async finishWrite(token) {
    const value = this.transfer(token, 'write');
    try {
      if (value.offset !== value.size) throw new Error('Asset download is incomplete');
      const sha256 = value.hash.digest('hex');
      if (value.sha256 !== undefined && value.sha256 !== sha256) throw new Error('Downloaded asset checksum mismatch');
      const metadata = { kind: value.kind, key: value.key, size: value.size, sha256 };
      const json = Buffer.from(JSON.stringify(metadata));
      if (json.length > HEADER - 8) throw new Error('Asset metadata is too large');
      const header = Buffer.alloc(HEADER); header.write('QAS1'); header.writeUInt32LE(json.length, 4); json.copy(header, 8);
      await writeAll(value.file, header, 0); await value.file.sync(); await value.file.close();
      // Rename only after all bytes, metadata and checksum have been completed.
      await fs.rename(value.temporary, value.destination);
      this.transfers.delete(token); return { size: value.size, sha256 };
    } catch (error) { await this.dispose(token, value); throw error; }
  }
  async abortWrite(token) { await this.dispose(token, this.transfer(token, 'write')); }
}
module.exports = { AssetStore, CHUNK };
