const path = require('node:path');
const fs = require('node:fs/promises');
const { execFileSync } = require('node:child_process');
async function buildInstaller() {
  if (process.platform !== 'win32' || process.arch !== 'x64') throw new Error('The installer build requires Windows x64.');
  const compilerArgument = process.argv.find(arg => arg.startsWith('--nsis='))?.slice(7);
  const candidates = [compilerArgument, process.env.QRAZY_NSIS, path.join(__dirname, '../local/installer-tools/nsis-3.12/makensis.exe'), 'C:/Program Files (x86)/NSIS/makensis.exe'].filter(Boolean);
  let compiler;
  for (const candidate of candidates) { try { await fs.access(candidate); compiler = path.resolve(candidate); break; } catch {} }
  if (!compiler) throw new Error('Provide a portable NSIS 3 compiler with --nsis=PATH or QRAZY_NSIS. No system software is installed by this build.');
  await require('./build.cjs').build();
  const version = require('../package.json').version;
  if (!/^\d+\.\d+\.\d+$/.test(version)) throw new Error('Installer requires a stable three-part version.');
  const root = path.resolve(__dirname, '../dist/Qrazy-win32-x64');
  const output = path.resolve(__dirname, '../dist/Qrazy-Setup.exe');
  execFileSync(compiler, ['/NOCONFIG', '/V2', '/DQRAZY_ROOT=' + root, '/DQRAZY_VERSION=' + version,
    '/DQRAZY_OUTPUT=' + output, path.join(__dirname, 'installer.nsi')], { windowsHide: true, stdio: 'inherit' });
  console.log(output);
}
if (require.main === module) buildInstaller().catch(error => { console.error(error.message); process.exitCode = 1; });
module.exports = { buildInstaller };
