const path = require('node:path');
const fs = require('node:fs/promises');
const { execFileSync } = require('node:child_process');
async function build() {
  const argument = name => process.argv.find(value => value.startsWith(name + '='))?.slice(name.length + 1);
  const platform = process.argv.includes('--linux') ? 'linux' : process.platform;
  const crossNative = argument('--native-input');
  const electronZip = argument('--electron-zip');
  const elfReader = argument('--elf-reader') || 'readelf';
  const python = argument('--python');
  if (!['linux', 'win32'].includes(platform) || process.arch !== 'x64') throw new Error('Build requires Windows x64 or Linux x64.');
  if (platform !== process.platform && !(platform === 'linux' && crossNative && electronZip && python))
    throw new Error('Cross-packaging Linux requires a cross-compiled Linux x64 --native-input, original --electron-zip, --elf-reader and --python for Unix permission preservation. Otherwise build on Linux x64.');
  const nativeExe = crossNative ? path.resolve(crossNative) : require('./native/build.cjs').buildNative();
  if (platform === 'linux') {
    const header = (await fs.readFile(nativeExe)).subarray(0, 20);
    if (header.length !== 20 || !header.subarray(0, 4).equals(Buffer.from([127, 69, 76, 70])) || header[4] !== 2 || header[5] !== 1 || header.readUInt16LE(18) !== 62)
      throw new Error('Native input must be a Linux x86-64 ELF binary, never a Windows addon or source placeholder.');
  }
  const launcher = platform === 'win32' ? require('./native/build.cjs').buildLauncher() : null;
  const { packager } = await import('@electron/packager');
  const { flipFuses, FuseVersion, FuseV1Options: F } = await import('@electron/fuses');
  const outputs = await packager({
    dir: path.resolve(__dirname, '..'), name: 'Qrazy', platform, arch: 'x64',
    out: path.resolve(__dirname, '../dist'), overwrite: true, asar: true, prune: true,
    extraResource: [nativeExe],
    ...(electronZip ? { electronZipDir: path.dirname(path.resolve(electronZip)) } : {}),
    ignore: [/^\/(?!src(?:\/|$)|package\.json$|package-lock\.json$|LICENSE$)/, /^\/src\/native\/bin(?:\/|$)/]
  });
  for (const output of outputs) {
    await flipFuses(path.join(output, platform === 'win32' ? 'Qrazy.exe' : 'Qrazy'), {
      version: FuseVersion.V1,
      [F.RunAsNode]: false,
      [F.EnableCookieEncryption]: true,
      [F.EnableNodeOptionsEnvironmentVariable]: false,
      [F.EnableNodeCliInspectArguments]: false,
      [F.OnlyLoadAppFromAsar]: true,
      [F.GrantFileProtocolExtraPrivileges]: false
    });
    if (platform === 'win32') {
      const files = await fs.readdir(output);
      const runtime = path.join(output, 'runtime');
      await fs.mkdir(runtime);
      for (const file of files) await fs.rename(path.join(output, file), path.join(runtime, file === 'Qrazy.exe' ? 'electron.exe' : file));
      await fs.copyFile(launcher, path.join(output, 'Qrazy.exe'));
    } else {
      // Retain Electron's own license and Chromium third-party notices.
      await fs.rename(path.join(output, 'LICENSE'), path.join(output, 'LICENSE.electron'));
      await fs.chmod(path.join(output, 'Qrazy'), 0o755);
      await fs.chmod(path.join(output, 'resources/qrazy-input-x11'), 0o755);
      await fs.chmod(path.join(output, 'chrome-sandbox'), 0o755);
      await fs.copyFile(path.join(__dirname, 'linux-support.txt'), path.join(output, 'LINUX-SUPPORT.txt'));
      // Record ELF requirements from the actual artifacts instead of guessing
      // a minimum glibc from an Electron version or a compiler on another OS.
      const requirements = new Set();
      const libraries = new Set();
      async function inspect(directory) {
        for (const entry of await fs.readdir(directory, { withFileTypes: true })) {
          const file = path.join(directory, entry.name);
          if (entry.isDirectory()) { await inspect(file); continue; }
          if (!entry.isFile()) continue;
          const handle = await fs.open(file, 'r');
          const magic = Buffer.alloc(4); await handle.read(magic, 0, 4, 0); await handle.close();
          if (!magic.equals(Buffer.from([127, 69, 76, 70]))) continue;
          const llvm = path.basename(elfReader).startsWith('llvm-readobj');
          const common = llvm ? ['--elf-output-style=GNU'] : [];
          const versions = execFileSync(elfReader, [...common, '--version-info', file], { encoding: 'utf8', windowsHide: true });
          // Only version requirements (not definitions provided by this ELF).
          const needs = versions.split(/Version needs section/).slice(1).join('\n');
          for (const match of needs.matchAll(/\bName:\s+([\w.]+)/g)) requirements.add(match[1]);
          const dynamic = execFileSync(elfReader, [...common, '--dynamic', file], { encoding: 'utf8', windowsHide: true });
          for (const match of dynamic.matchAll(/\(NEEDED\).*\[([^\]]+)\]/g)) libraries.add(match[1]);
        }
      }
      await inspect(output);
      await fs.writeFile(path.join(output, 'SYSTEM-REQUIREMENTS.txt'),
        'Linux x64. Required ELF symbol versions (all must be satisfied):\n' + [...requirements].sort().join('\n') +
        '\n\nShared library names (some are bundled; others must be supplied by the distro):\n' + [...libraries].sort().join('\n') +
        '\n\nAlso requires a working desktop, graphics drivers, fonts, and Chromium sandbox support. See LINUX-SUPPORT.txt. No distro has been runtime-certified by this build script.\n');
    }
    await fs.copyFile(path.resolve(__dirname, '../LICENSE'), path.join(output, 'LICENSE'));
    if (platform === 'linux') {
      const archive = path.join(path.dirname(output), 'Qrazy-linux-x64.tar.gz');
      const temporary = archive + '.tmp';
      if (process.platform === 'linux') execFileSync('tar', ['-czf', temporary, '-C', path.dirname(output), path.basename(output)], { stdio: 'inherit' });
      else execFileSync(python, [path.join(__dirname, 'package-linux.py'), output, electronZip, temporary], { stdio: 'inherit', windowsHide: true });
      await fs.rename(temporary, archive);
      console.log(archive);
    }
  }
  console.log(outputs.join('\n'));
}
if (require.main === module) build().catch(error => { console.error(error); process.exitCode = 1; });
module.exports = { build };
