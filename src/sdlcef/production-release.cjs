// Production metadata primitives. No feed, channel, installation or launch defaults.
'use strict';
// Signed inventories describe physical archives, including Electron app.asar.
// Electron's virtual fs presents that archive as a directory of size zero.
let fs;
try { fs = require('original-fs'); }
catch (error) { if (error.code !== 'MODULE_NOT_FOUND') throw error; fs = require('node:fs'); }
const path = require('node:path');
const crypto = require('node:crypto');
const schema = 'qrazy-sdlcef-release-v1';
const digest = bytes => crypto.createHash('sha256').update(bytes).digest('hex');
function hashFile(file) {
  const fd=fs.openSync(file,'r'); const hash=crypto.createHash('sha256'), buffer=Buffer.alloc(1024*1024);
  try { let count; while ((count=fs.readSync(fd,buffer,0,buffer.length,null))>0) hash.update(buffer.subarray(0,count)); return hash.digest('hex'); }
  finally { fs.closeSync(fd); }
}
function relative(name) {
  if (typeof name !== 'string' || name.length > 240 || !/^[A-Za-z0-9_. /-]+$/.test(name)) throw Error('Unsafe inventory path');
  for (const part of name.split('/')) {
    if (!part || part === '.' || part === '..' || /[ .]$/.test(part) || /^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\.|$)/i.test(part)) throw Error('Unsafe inventory component');
  }
  if (/^(profile|production-signing|tests|fixtures)([-/ .]|$)/i.test(name) || /(?:private|\.dpapi$|checks\.dll$)/i.test(name)) throw Error('Excluded runtime content');
  return name;
}
function validate(m, policy) {
  if (!policy || !['linux-x64','windows-x64'].includes(policy.platform) || !/^[a-z][a-z0-9-]{0,31}$/.test(policy.channel)) throw Error('Explicit platform and channel required');
  if (m.schema !== schema || m.product !== 'qrazy-sdlcef' || m.platform !== policy.platform || m.channel !== policy.channel) throw Error('Wrong release identity');
  if (!/^(0|[1-9]\d{0,5})\.(0|[1-9]\d{0,5})\.(0|[1-9]\d{0,5})$/.test(m.version) || !Number.isSafeInteger(m.sequence) || m.sequence < 1) throw Error('Invalid release version/sequence');
  if (typeof m.notes !== 'string' || m.notes.length > 8000 || !Array.isArray(m.files) || !m.files.length || m.files.length > 4096) throw Error('Invalid release inventory');
  const names = new Set(); let total = 0;
  for (const file of m.files) {
    const name = relative(file.path).toLowerCase();
    if (names.has(name) || !Number.isSafeInteger(file.size) || file.size < 1 || file.size > 2 * 1024 ** 3 || !/^[a-f0-9]{64}$/.test(file.sha256) || ![420,493].includes(file.mode)) throw Error('Invalid inventory entry');
    names.add(name); total += file.size;
  }
  if (total > 4 * 1024 ** 3 || [...names].some(n => [...names].some(other => other.startsWith(n + '/')))) throw Error('Conflicting or excessive inventory');
  const required = ['bridge.js','icudtl.dat','resources.pak','v8_context_snapshot.bin','locales/en-us.pak','licenses/cef-license.txt','licenses/cef-credits.html','licenses/sdl3-license.txt','licenses/project-license.txt','system-requirements.txt','changes.txt'];
  required.push(...(m.platform === 'windows-x64' ? ['qrazy.exe','qrazy.dll','libcef.dll','sdl3.dll','chrome_elf.dll','d3dcompiler_47.dll','dxcompiler.dll','dxil.dll','chrome_100_percent.pak','chrome_200_percent.pak','vk_swiftshader.dll','vulkan-1.dll','vk_swiftshader_icd.json'] : ['qrazy-sdl-cef','desktop_backend.py','libcef.so','libsdl3.so.0','libvk_swiftshader.so','libvulkan.so.1','chrome_100_percent.pak','chrome_200_percent.pak']));
  if (!required.every(n => names.has(n))) throw Error('Required runtime/licenses missing');
  if (!m.asset || !/^Qrazy-SDLCEF-[A-Za-z0-9_.+-]+\.zip$/.test(m.asset.name) || !m.asset.name.includes(m.platform) || !Number.isSafeInteger(m.asset.size) || m.asset.size < 1 || m.asset.size > 4 * 1024 ** 3 || !/^[a-f0-9]{64}$/.test(m.asset.sha256)) throw Error('Invalid separate payload descriptor');
  return m;
}
function verify(raw, publicKey, policy) {
  if (!Buffer.isBuffer(raw) || raw.length > 3 * 1024 ** 2) throw Error('Metadata limit exceeded');
  const envelope = JSON.parse(raw);
  const decode = value => {
    if (typeof value !== 'string' || !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(value)) throw Error('Invalid base64');
    return Buffer.from(value,'base64');
  };
  const payload = decode(envelope.payload), signature = decode(envelope.signature);
  const key = crypto.createPublicKey(publicKey);
  if (key.asymmetricKeyType !== 'ed25519' || signature.length !== 64 || !crypto.verify(null,payload,key,signature)) throw Error('Invalid release signature');
  return { manifest: validate(JSON.parse(payload),policy), identity: digest(payload) };
}
function inventory(root) {
  root = path.resolve(root); const files = [];
  if (!fs.lstatSync(root).isDirectory() || fs.lstatSync(root).isSymbolicLink()) throw Error('Unsafe inventory root');
  function walk(dir) {
    for (const name of fs.readdirSync(dir).sort()) {
      const file = path.join(dir,name), info = fs.lstatSync(file);
      const rel = relative(path.relative(root,file).split(path.sep).join('/'));
      if (info.isSymbolicLink()) throw Error('Runtime links forbidden');
      if (info.isDirectory()) walk(file);
      else if (info.isFile()) files.push({path:rel,size:info.size,sha256:hashFile(file),mode:info.mode & 0o111 ? 493 : 420});
      else throw Error('Nonregular runtime content');
    }
  }
  walk(root); return files;
}
function verifyTree(root, manifest, policy) {
  validate(manifest,policy);
  const actual=inventory(root), expected=new Map(manifest.files.map(file=>[file.path,file]));
  if(actual.length!==expected.size) throw Error('Unsigned runtime content or missing files');
  for(const file of actual) {
    const signed=expected.get(file.path);
    if(!signed || signed.size!==file.size || signed.sha256!==file.sha256 || policy.platform==='linux-x64' && signed.mode!==file.mode) throw Error('Runtime inventory mismatch');
  }
  return true;
}
module.exports = { schema, relative, validate, verify, inventory, verifyTree };
