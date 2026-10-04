const fs = require('node:fs');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
function compile(sourceName, executableName, windows = false, addon = false) {
  if (process.platform !== 'win32' || process.arch !== 'x64') throw new Error('Native input currently requires Windows x64.');
  const output = path.join(__dirname, 'bin');
  const source = path.join(__dirname, sourceName);
  const exe = path.join(output, executableName);
  const resource = path.join(__dirname, 'launcher.rc');
  const icon = path.join(__dirname, '../assets/qrazy.ico');
  const inputs = [source, __filename, ...(windows ? [resource, icon] : [])];
  if (fs.existsSync(exe) && fs.statSync(exe).mtimeMs >= Math.max(...inputs.map(file => fs.statSync(file).mtimeMs))) return exe;
  const vswhere = path.join(process.env['ProgramFiles(x86)'] || 'C:\\Program Files (x86)', 'Microsoft Visual Studio/Installer/vswhere.exe');
  let installation;
  try { installation = execFileSync(vswhere, ['-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], { encoding: 'utf8', windowsHide: true }).trim(); } catch {}
  if (!installation) throw new Error('Install Microsoft Visual Studio Build Tools with Desktop development with C++ and a Windows SDK, then rerun npm install.');
  const setup = path.join(installation, 'VC/Auxiliary/Build/vcvars64.bat');
  fs.mkdirSync(output, { recursive: true });
  const batch = path.join(output, 'compile.cmd');
  const resourceOutput = path.join(output, 'launcher.res');
  const resourceCommand = windows ? `pushd "${__dirname}"\r\nrc /nologo /fo "${resourceOutput}" "${resource}"\r\nif errorlevel 1 exit /b 1\r\npopd\r\n` : '';
  fs.writeFileSync(batch, `@echo off\r\ncall "${setup}" >nul\r\nif errorlevel 1 exit /b 1\r\n${resourceCommand}cl /nologo /std:c++17 /EHsc /O2 /MT /W4 ${addon ? '/LD' : ''} "${source}" ${windows ? `"${resourceOutput}"` : ''} /Fo"${path.join(output, sourceName + '.obj')}" /Fe"${exe}" /link user32.lib ${windows ? '/SUBSYSTEM:WINDOWS' : ''}\r\n`);
  execFileSync(process.env.ComSpec || 'cmd.exe', ['/d', '/c', 'compile.cmd'], { cwd: output, stdio: 'inherit', windowsHide: true });
  return exe;
}
function buildNative() {
  if (process.platform === 'win32') return compile('raw-mouse.cpp', 'qrazy-input.node', false, true);
  if (process.platform !== 'linux' || process.arch !== 'x64') throw new Error('Supported build targets are Windows x64 and Linux x64.');
  const source = path.join(__dirname, 'raw-mouse-x11.cpp');
  const output = path.join(__dirname, 'bin/linux-x64/qrazy-input-x11');
  if (fs.existsSync(output) && fs.statSync(output).mtimeMs >= Math.max(fs.statSync(source).mtimeMs, fs.statSync(__filename).mtimeMs)) return output;
  fs.mkdirSync(path.dirname(output), { recursive: true });
  let flags;
  try { flags = execFileSync('pkg-config', ['--cflags', '--libs', 'x11', 'xi'], { encoding: 'utf8' }).trim().split(/\s+/); }
  catch { throw new Error('Linux source builds require a C++17 compiler, pkg-config, libX11 and libXi development headers. No system packages were installed.'); }
  const temporary = output + '.tmp';
  execFileSync(process.env.CXX || 'c++', ['-std=c++17', '-O2', '-Wall', '-Wextra', source, '-o', temporary, ...flags], { stdio: 'inherit' });
  fs.chmodSync(temporary, 0o755); fs.renameSync(temporary, output);
  return output;
}
function buildLauncher() { return compile('launcher.cpp', 'Qrazy.exe', true); }
module.exports = { buildNative, buildLauncher };
if (require.main === module) buildNative();
