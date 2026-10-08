'use strict';
// Shared legacy signature/inventory verifier, also usable by isolated Node helpers.
const crypto=require('node:crypto');
const MAX_BYTES=1024*1024*1024,DATA_START=4*1024*1024;
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
  if (platform === 'linux-appimage-x64') return value === 'Qrazy-linux-x64.AppImage';
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
  if (platform === 'linux-appimage-x64' && (files.length !== 1 || files[0].mode !== 0o755)) throw new Error('AppImage update must contain one executable image');
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
  if (total > MAX_BYTES || (platform !== 'linux-appimage-x64' && (!names.has(platform === 'win32-x64' ? 'runtime/resources/app.asar' : 'resources/app.asar') || !names.has(platform === 'win32-x64' ? 'qrazy.exe' : 'qrazy')))) throw new Error('Incomplete or oversized update');
  const required = platform === 'linux-appimage-x64' ? [] : platform === 'win32-x64' ? ['runtime/electron.exe', 'runtime/resources/qrazy-input.node'] : ['resources/qrazy-input-x11', 'chrome-sandbox'];
  if (required.some(name => !names.has(name))) throw new Error('Incomplete update runtime');
  if (platform === 'linux-x64' && files.some(file => ['Qrazy', 'chrome-sandbox', 'chrome_crashpad_handler', 'resources/qrazy-input-x11'].includes(file.path) && file.mode !== 0o755)) throw new Error('Linux executables must be executable');
  for (const name of names) if ([...names].some(other => other.startsWith(name + '/'))) throw new Error('Conflicting update paths');
  return { version: manifest.version, notes: manifest.notes, files, ...(manifest.schema === 2 ? { bundle: manifest.bundle } : {}) };
}
module.exports={verifyManifest,validPath,newer};
