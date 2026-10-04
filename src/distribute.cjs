const fs = require('node:fs/promises');
const path = require('node:path');
const crypto = require('node:crypto');
const { createReadStream } = require('node:fs');
const { execFileSync } = require('node:child_process');
const { createInterface } = require('node:readline/promises');
const { prepare } = require('./release.cjs');
const { verifyManifest, newer, hashFile, readBounded } = require('./updater.cjs');
const updateConfig = require('./update-config.cjs');
const root = path.resolve(__dirname, '..');
const repository = 'JimmSlimm/qrazy-client';
const sourceRoots = ['.gitignore', 'push.bat', 'package.json', 'package-lock.json', 'LICENSE', 'src'];
function sourceBytes(name, bytes) {
  // Git on Windows normalizes text line endings; compare the same source
  // content across the working tree, index and commit without changing files.
  return /\.(png|ico)$/i.test(name) ? bytes : Buffer.from(bytes.toString('utf8').replace(/\r\n/g, '\n'));
}
function run(executable, args, options = {}) {
  return execFileSync(executable, args, { cwd: root, windowsHide: true, stdio: 'inherit', ...options });
}
function git(args) { return run('git', args, { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] }).trim(); }
function sourceAllowed(name) {
  if (sourceRoots.slice(0, -1).includes(name)) return true;
  return name.startsWith('src/') && !name.startsWith('src/native/bin/') &&
    !/(?:^|\/)(?:local|tests?|__tests__|screenshots|dependencies)(?:\/|$)/i.test(name) &&
    !/(?:\.test\.|\.spec\.|private|credentials|secret)/i.test(name) &&
    /\.(?:cjs|js|json|cpp|h|rc|html|css|png|ico|svg|py|ps1|txt|nsi)$/i.test(name);
}
async function sourceSnapshot() {
  const files = [];
  async function walk(name) {
    const full = path.join(root, name), stat = await fs.lstat(full);
    if (stat.isSymbolicLink()) throw new Error('Source cannot contain links: ' + name);
    if (name === 'src/native/bin') return;
    if (stat.isDirectory()) { for (const entry of await fs.readdir(full)) await walk(name + '/' + entry); return; }
    if (!sourceAllowed(name)) throw new Error('Move non-publishable files out of source: ' + name);
    files.push(name);
  }
  for (const name of sourceRoots) await walk(name);
  files.sort();
  const digest = crypto.createHash('sha256');
  for (const name of files) digest.update(name + '\0').update(sourceBytes(name, await fs.readFile(path.join(root, name)))).update('\0');
  return { files, hash: digest.digest('hex') };
}
function inspectGit() {
  const origin = git(['remote', 'get-url', 'origin']);
  if (![...['https://github.com/', 'git@github.com:'].map(prefix => prefix + repository), ...['https://github.com/', 'git@github.com:'].map(prefix => prefix + repository + '.git')].includes(origin)) throw new Error('Origin must match the updater repository: ' + repository);
  const branch = git(['symbolic-ref', '--short', 'HEAD']);
  for (const name of git(['ls-files', '-z']).split('\0').filter(Boolean)) if (!sourceAllowed(name)) throw new Error('Non-publishable file in the Git index: ' + name);
  return branch;
}
async function authentication() {
  if (process.env.GH_TOKEN || process.env.GITHUB_TOKEN) return process.env.GH_TOKEN || process.env.GITHUB_TOKEN;
  try { const token = run('gh', ['auth', 'token', '--hostname', 'github.com'], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] }).trim(); if (token) return token; } catch {}
  try {
    const credential = run('git', ['credential', 'fill'], { input: 'protocol=https\nhost=github.com\n\n', encoding: 'utf8', stdio: ['pipe', 'pipe', 'ignore'], env: { ...process.env, GIT_TERMINAL_PROMPT: '0', GCM_INTERACTIVE: 'never' } });
    const token = credential.match(/^password=(.+)$/m)?.[1].trim(); if (token) return token;
  } catch {}
  throw new Error('GitHub release authentication is missing. Sign in with GitHub CLI (gh auth login), or provide GH_TOKEN with Contents read/write access. Credentials are never stored in the release settings.');
}
class GitHub {
  constructor(token, request = fetch) { this.token = token; this.request = request; }
  async api(endpoint, method = 'GET', body, allow404 = false) {
    const response = await this.request('https://api.github.com/repos/' + repository + endpoint, { method, headers: { Authorization: 'Bearer ' + this.token, Accept: 'application/vnd.github+json', 'X-GitHub-Api-Version': '2022-11-28', ...(body ? { 'Content-Type': 'application/json' } : {}) }, ...(body ? { body: JSON.stringify(body) } : {}), signal: AbortSignal.timeout(60000), redirect: 'error' });
    if (allow404 && response.status === 404) return null;
    if (!response.ok) throw new Error('GitHub ' + method + ' failed (HTTP ' + response.status + '). Any unfinished release remains a draft.');
    return response.status === 204 ? null : response.json();
  }
  async upload(release, asset) {
    const url = new URL(release.upload_url.replace(/\{.*$/, ''));
    if (url.origin !== 'https://uploads.github.com') throw new Error('Unexpected GitHub upload host');
    url.searchParams.set('name', asset.name);
    const response = await this.request(url.href, { method: 'POST', headers: { Authorization: 'Bearer ' + this.token, 'Content-Type': 'application/octet-stream', 'Content-Length': String(asset.size), 'X-GitHub-Api-Version': '2022-11-28' }, body: createReadStream(asset.path), duplex: 'half', signal: AbortSignal.timeout(15 * 60000), redirect: 'error' });
    if (!response.ok) throw new Error('Upload failed for ' + asset.name + ' (HTTP ' + response.status + '). The release remains a draft; rerun to retry.');
    return response.json();
  }
  async readManifest(asset) {
    if (!Number.isSafeInteger(asset.id) || asset.id < 1 || asset.size > 4 * 1024 * 1024) throw new Error('Invalid manifest asset');
    let response = await this.request('https://api.github.com/repos/' + repository + '/releases/assets/' + asset.id, { headers: { Authorization: 'Bearer ' + this.token, Accept: 'application/octet-stream', 'X-GitHub-Api-Version': '2022-11-28' }, signal: AbortSignal.timeout(15000), redirect: 'manual' });
    if ([301, 302, 303, 307, 308].includes(response.status)) {
      const location = new URL(response.headers.get('location'));
      if (location.protocol !== 'https:' || !['release-assets.githubusercontent.com', 'objects.githubusercontent.com'].includes(location.hostname) || location.username || location.password) throw new Error('Unexpected manifest download host');
      // A signed CDN URL needs no GitHub authorization header.
      response = await this.request(location.href, { signal: AbortSignal.timeout(15000), redirect: 'error' });
    }
    return readBounded(response, 4 * 1024 * 1024);
  }
}
async function publishAssets(api, version, commit, notes, assets) {
  const tag = 'v' + version;
  let release = await api.api('/releases/tags/' + tag, 'GET', undefined, true);
  if (release && !release.draft) throw new Error(tag + ' is already published. Use a new version.');
  if (release && release.target_commitish !== commit) throw new Error('Existing draft targets another commit. Resolve that draft before reusing this version.');
  const ref = await api.api('/git/ref/tags/' + tag, 'GET', undefined, true);
  if (ref) {
    let object = ref.object;
    for (let count = 0; object.type === 'tag' && count < 10; count++) object = (await api.api('/git/tags/' + object.sha)).object;
    if (object.type !== 'commit' || object.sha !== commit) throw new Error('Existing tag targets another commit. Use a new release version.');
  }
  if (!release) release = await api.api('/releases', 'POST', { tag_name: tag, target_commitish: commit, name: 'Qrazy ' + version, body: notes, draft: true, prerelease: false });
  console.log('Draft: ' + release.html_url);
  // Delete only stale assets in this unpublished draft, never published assets.
  const expected = new Map(assets.map(asset => [asset.name, asset]));
  for (const existing of await listAssets(api, release.id)) {
    const wanted = expected.get(existing.name);
    if (!wanted || existing.digest !== 'sha256:' + wanted.sha256 || existing.size !== wanted.size || existing.state !== 'uploaded') await api.api('/releases/assets/' + existing.id, 'DELETE');
    else expected.delete(existing.name);
  }
  let count = 0;
  for (const asset of expected.values()) {
    console.log('Uploading ' + (++count) + '/' + expected.size + ': ' + asset.name);
    const uploaded = await api.upload(release, asset);
    if (uploaded.size !== asset.size || uploaded.digest !== 'sha256:' + asset.sha256 || uploaded.state !== 'uploaded') throw new Error('GitHub asset verification failed: ' + asset.name + '. The release remains a draft.');
  }
  const complete = await listAssets(api, release.id);
  if (complete.length !== assets.length || assets.some(asset => !complete.some(remote => remote.name === asset.name && remote.size === asset.size && remote.digest === 'sha256:' + asset.sha256 && remote.state === 'uploaded'))) throw new Error('Draft is incomplete; it has not been published.');
  return api.api('/releases/' + release.id, 'PATCH', { draft: false, prerelease: false, make_latest: 'true', body: notes });
}
async function listAssets(api, id) {
  const assets = [];
  for (let page = 1; page <= 100; page++) {
    const batch = await api.api('/releases/' + id + '/assets?per_page=100&page=' + page);
    assets.push(...batch);
    if (batch.length < 100) return assets;
  }
  throw new Error('Too many release assets');
}
async function settings() {
  const file = path.join(root, 'local/release-settings.json');
  let value; try { value = JSON.parse(await fs.readFile(file, 'utf8')); } catch { throw new Error('Configure ignored local/release-settings.json with python, clang, elfReader, sysroot and signingKey paths.'); }
  for (const name of ['python', 'clang', 'elfReader', 'sysroot', 'signingKey']) {
    if (typeof value[name] !== 'string' || !value[name]) throw new Error('Missing release setting: ' + name);
    value[name] = path.resolve(root, value[name]); await fs.access(value[name]);
  }
  if (value.nsis) { value.nsis = path.resolve(root, value.nsis); await fs.access(value.nsis); }
  const key = crypto.createPrivateKey(await fs.readFile(value.signingKey));
  if (crypto.createPublicKey(key).export({ type: 'spki', format: 'pem' }) !== updateConfig.publicKey) throw new Error('Wrong update signing key');
  return value;
}
function checkBuildLocks() {
  const targets = ['dist/Qrazy-win32-x64/Qrazy.exe', 'dist/Qrazy-win32-x64/runtime/electron.exe', 'dist/Qrazy-Setup.exe'].map(name => "'" + path.join(root, name).replace(/'/g, "''") + "'");
  const script = '$targets=@(' + targets.join(',') + '); Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -in $targets } | Select-Object -ExpandProperty Id';
  const result = run(path.join(process.env.SystemRoot || 'C:\\Windows', 'System32/WindowsPowerShell/v1.0/powershell.exe'), ['-NoProfile', '-NonInteractive', '-EncodedCommand', Buffer.from(script, 'utf16le').toString('base64')], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
  if (result.trim()) throw new Error('Close Qrazy running from dist and Qrazy Setup, then retry. No process has been terminated.');
}
async function verifyPackage(folder, snapshot, version) {
  const { extractFile, listPackage, uncache } = await import('@electron/asar');
  const asar = path.join(folder, folder.endsWith('win32-x64') ? 'runtime/resources/app.asar' : 'resources/app.asar');
  uncache(asar);
  const expected = new Set(snapshot.files.filter(name => !['.gitignore', 'push.bat', 'package-lock.json'].includes(name)));
  for (const name of listPackage(asar).map(name => name.replace(/^[\\/]+/, '').replace(/\\/g, '/'))) {
    if (!path.posix.extname(name)) continue;
    if (!expected.has(name)) throw new Error('Unexpected packaged file: ' + name);
  }
  const packaged = JSON.parse(extractFile(asar, 'package.json'));
  const metadata = JSON.parse(await fs.readFile(path.join(root, 'package.json')));
  // Electron Packager removes development-only metadata from production.
  delete metadata.devDependencies; delete metadata.scripts; delete metadata.private;
  if (packaged.version !== version || !require('node:util').isDeepStrictEqual(packaged, metadata)) throw new Error('Wrong packaged application metadata');
  for (const name of expected) if (name !== 'package.json' && !extractFile(asar, path.normalize(name)).equals(await fs.readFile(path.join(root, name)))) throw new Error('Packaged source differs: ' + name);
}
async function buildRelease(version, notes = '', baseline) {
  if (process.platform !== 'win32' || process.arch !== 'x64') throw new Error('This combined workflow builds on Windows x64.');
  if (!/^\d{1,6}\.\d{1,6}\.\d{1,6}$/.test(version)) throw new Error('Use a stable version such as 0.3.1.');
  if (require('./client-mode.cjs').DEV_MODE) throw new Error('Select live client mode before preparing a release.');
  const config = await settings(); checkBuildLocks();
  const packagePath = path.join(root, 'package.json'), lockPath = path.join(root, 'package-lock.json');
  const pkg = JSON.parse(await fs.readFile(packagePath)), lock = JSON.parse(await fs.readFile(lockPath));
  if (version !== pkg.version && !newer(version, pkg.version)) throw new Error('Do not decrease the client version.');
  pkg.version = version; lock.version = version; lock.packages[''].version = version;
  await fs.writeFile(packagePath, JSON.stringify(pkg, null, 2) + '\n');
  await fs.writeFile(lockPath, JSON.stringify(lock, null, 2) + '\n');
  const snapshot = await sourceSnapshot();
  console.log('Building Windows client and installer...');
  run(process.execPath, [path.join(__dirname, 'build-installer.cjs'), ...(config.nsis ? ['--nsis=' + config.nsis] : [])]);
  run(config.python, [path.join(__dirname, 'package-windows.py'), '--root', path.join(root, 'dist/Qrazy-win32-x64'), '--out', path.join(root, 'dist/Qrazy-win32-x64.zip')]);
  console.log('Cross-compiling Linux input helper...');
  const helper = path.join(root, 'src/native/bin/linux-x64/qrazy-input-x11');
  await fs.mkdir(path.dirname(helper), { recursive: true });
  run(config.clang, ['--target=x86_64-linux-gnu', '--sysroot=' + config.sysroot, '--gcc-toolchain=' + path.join(config.sysroot, 'usr'), '-fuse-ld=lld', '-std=c++17', '-O2', '-Wall', '-Wextra', '-I' + path.join(config.sysroot, 'usr/include/x86_64-linux-gnu'), path.join(__dirname, 'native/raw-mouse-x11.cpp'), '-o', helper, '-lX11', '-lXi']);
  const electronVersion = pkg.devDependencies.electron;
  if (!/^\d+\.\d+\.\d+$/.test(electronVersion)) throw new Error('Pin an exact Electron version.');
  const zip = path.join(root, 'local/linux-build/electron', 'electron-v' + electronVersion + '-linux-x64.zip');
  try { await fs.access(zip); } catch {
    const { downloadArtifact } = await import('@electron/get');
    const downloaded = await downloadArtifact({ version: electronVersion, artifactName: 'electron', platform: 'linux', arch: 'x64' });
    await fs.mkdir(path.dirname(zip), { recursive: true }); await fs.copyFile(downloaded, zip);
  }
  console.log('Building Linux archive...');
  run(process.execPath, [path.join(__dirname, 'build.cjs'), '--linux', '--native-input=' + helper, '--electron-zip=' + zip, '--elf-reader=' + config.elfReader, '--python=' + config.python]);
  for (const platform of ['win32-x64', 'linux-x64']) await verifyPackage(path.join(root, 'dist/Qrazy-' + platform), snapshot, version);
  if ((await sourceSnapshot()).hash !== snapshot.hash) throw new Error('Source changed during the build. Rebuild before publishing.');
  const out = path.join(root, 'local/release-' + version), manifest = path.join(out, 'qrazy-update.json');
  await prepare({ root: path.join(root, 'dist/Qrazy-win32-x64'), platform: 'win32-x64', key: config.signingKey, version, out, notes, baseline });
  await prepare({ root: path.join(root, 'dist/Qrazy-linux-x64'), platform: 'linux-x64', key: config.signingKey, version, out, notes, previous: manifest, baseline });
  const envelope = JSON.parse(await fs.readFile(manifest));
  const payloads = new Map();
  for (const platform of ['win32-x64', 'linux-x64']) for (const file of verifyManifest(envelope, updateConfig, platform).files) {
    if (await hashFile(path.join(root, 'dist/Qrazy-' + platform, file.path)) !== file.sha256) throw new Error('Release inventory differs from package');
    if (file.url.startsWith(updateConfig.assetPrefix + 'v' + version + '/')) payloads.set(path.posix.basename(new URL(file.url).pathname), file.downloadSha256);
  }
  const paths = ['Qrazy-win32-x64.zip', 'Qrazy-linux-x64.tar.gz', 'Qrazy-Setup.exe'].map(name => path.join(root, 'dist', name));
  paths.push(manifest, ...[...payloads.keys()].map(name => path.join(out, name)));
  const assets = [];
  for (const file of paths) {
    const sha256 = await hashFile(file), name = path.basename(file);
    if (payloads.has(name) && payloads.get(name) !== sha256) throw new Error('Compressed update hash mismatch');
    assets.push({ name, path: file, size: (await fs.stat(file)).size, sha256 });
  }
  const sums = path.join(out, 'SHA256SUMS.txt');
  await fs.writeFile(sums, assets.map(asset => asset.sha256 + '  ' + asset.name).join('\n') + '\n');
  assets.push({ name: 'SHA256SUMS.txt', path: sums, size: (await fs.stat(sums)).size, sha256: await hashFile(sums) });
  const plan = { version, notes, sourceHash: snapshot.hash, assets };
  await fs.writeFile(path.join(out, 'release-plan.json'), JSON.stringify(plan, null, 2) + '\n');
  console.log('Verified: all three packages, both signed inventories and ' + payloads.size + ' update payloads.');
  return plan;
}
function committedSnapshot(revision) {
  const names = git(['ls-files', '-z']).split('\0').filter(Boolean).sort();
  const digest = crypto.createHash('sha256');
  for (const name of names) {
    if (!sourceAllowed(name)) throw new Error('Non-publishable file in the index: ' + name);
    const bytes = run('git', ['show', revision + ':' + name], { stdio: ['ignore', 'pipe', 'pipe'], maxBuffer: 16 * 1024 * 1024 });
    digest.update(name + '\0').update(sourceBytes(name, bytes)).update('\0');
  }
  return digest.digest('hex');
}
async function commitAndPush(message, branch, expectedHash) {
  inspectGit();
  run('git', ['add', '--', ...sourceRoots]); inspectGit();
  if (expectedHash && committedSnapshot('') !== expectedHash) throw new Error('Staged source differs from the packages. Rebuild before publishing.');
  if (git(['diff', '--cached', '--name-only'])) {
    const messageFile = path.join(root, 'local/distribution-commit-message.txt');
    await fs.mkdir(path.dirname(messageFile), { recursive: true }); await fs.writeFile(messageFile, message + '\n');
    run('git', ['commit', '--file=' + messageFile]);
  }
  if (expectedHash && committedSnapshot('HEAD') !== expectedHash) throw new Error('Committed source differs from the packages. Nothing was pushed.');
  run('git', ['push', '--set-upstream', 'origin', branch]);
  return git(['rev-parse', 'HEAD']);
}
async function main() {
  const argument = name => process.argv.find(arg => arg.startsWith('--' + name + '='))?.slice(name.length + 3);
  if (process.argv.includes('--prepare')) {
    const version = argument('version') || require('../package.json').version;
    const notes = argument('notes-file') ? await fs.readFile(path.resolve(argument('notes-file')), 'utf8') : '';
    await buildRelease(version, notes, argument('baseline')); console.log('Prepared locally. Nothing committed, pushed or published.'); return;
  }
  const input = createInterface({ input: process.stdin, output: process.stdout });
  try {
    console.log('\nQrazy distribution\n1. Push source only\n2. Publish new release (ZIP + Linux archive + installer + small updates)\n3. Exit\n');
    const choice = (await input.question('Choose [1/2/3]: ')).trim();
    if (!['1', '2'].includes(choice)) return;
    const branch = inspectGit(); await sourceSnapshot();
    if (choice === '1') {
      const message = (await input.question('Commit message: ')).trim() || 'Update Qrazy source';
      run('git', ['status', '--short']);
      if ((await input.question('Type PUSH to commit/push source to ' + repository + ' (' + branch + '): ')).trim() !== 'PUSH') return;
      await commitAndPush(message, branch); console.log('Source pushed. No release created.'); return;
    }
    const api = new GitHub(await authentication());
    const info = await api.api('');
    const latest = await api.api('/releases/latest', 'GET', undefined, true);
    const current = require('../package.json').version;
    const latestVersion = latest?.tag_name?.replace(/^v/, '');
    const suggested = latestVersion && !newer(current, latestVersion) ? latestVersion.replace(/\d+$/, value => String(Number(value) + 1)) : current;
    const version = (await input.question('Release version [' + suggested + ']: ')).trim() || suggested;
    if (!/^\d{1,6}\.\d{1,6}\.\d{1,6}$/.test(version) || (latestVersion && !newer(version, latestVersion))) throw new Error('Choose a stable version newer than the latest release.');
    const existing = await api.api('/releases/tags/v' + version, 'GET', undefined, true);
    if (existing && !existing.draft) throw new Error('That version is already published.');
    const notes = (await input.question('Release notes (one line): ')).trim() || 'Qrazy client update';
    let baseline;
    if (latest) {
      const asset = (await listAssets(api, latest.id)).find(asset => asset.name === 'qrazy-update.json');
      if (!asset) throw new Error('The latest release has no signed update manifest. Resolve it before releasing a new version.');
      const bytes = await api.readManifest(asset);
      const envelope = JSON.parse(bytes);
      const payload = JSON.parse(Buffer.from(envelope.payload, 'base64'));
      verifyManifest(envelope, updateConfig, Object.keys(payload.platforms || {})[0]);
      if (payload.version !== latestVersion) throw new Error('Latest release and signed manifest versions disagree');
      baseline = path.join(root, 'local/distribution-baseline.json'); await fs.mkdir(path.dirname(baseline), { recursive: true }); await fs.writeFile(baseline, bytes);
    }
    const plan = await buildRelease(version, notes, baseline);
    console.log('\nReady: Qrazy ' + version + ' -> ' + repository + ' (' + branch + ')');
    for (const asset of plan.assets.filter(asset => !asset.name.endsWith('.gz') || asset.name.endsWith('.tar.gz'))) console.log('  ' + asset.name + ' (' + (asset.size / 1024 / 1024).toFixed(1) + ' MiB)');
    console.log('  ' + plan.assets.filter(asset => asset.name.endsWith('.gz') && !asset.name.endsWith('.tar.gz')).length + ' compressed update files');
    if (info.private) console.log('Repository is PRIVATE: players cannot download this release or its updates until the repository is public.');
    console.log('Keep older releases available: unchanged update files may reference them. Keep a secure backup of your private signing key.');
    run('git', ['status', '--short']);
    if ((await input.question('Type PUBLISH ' + version + ' to commit, push and publish: ')).trim() !== 'PUBLISH ' + version) { console.log('Packages retained locally. Nothing published.'); return; }
    if ((await sourceSnapshot()).hash !== plan.sourceHash) throw new Error('Source changed after building. Rebuild before publishing.');
    for (const asset of plan.assets) if (await hashFile(asset.path) !== asset.sha256) throw new Error('Prepared asset changed: ' + asset.name);
    const commit = await commitAndPush('Release Qrazy ' + version, branch, plan.sourceHash);
    const release = await publishAssets(api, version, commit, notes, plan.assets);
    console.log('Published: ' + release.html_url);
  } finally { input.close(); }
}
if (require.main === module) main().catch(error => { console.error('\n' + error.message); process.exitCode = 1; });
module.exports = { sourceAllowed, sourceBytes, sourceSnapshot, GitHub, publishAssets, verifyPackage, buildRelease };
