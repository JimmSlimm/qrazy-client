const path = require('node:path');
const { execFileSync } = require('node:child_process');
function allUsersInstallation(root, platform = process.platform) {
  if (platform !== 'win32') return false;
  try {
    const output = execFileSync(path.join(process.env.SystemRoot || 'C:\\Windows', 'System32/reg.exe'), ['query', 'HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Qrazy', '/v', 'InstallLocation', '/reg:64'], { encoding: 'utf8', windowsHide: true, stdio: ['ignore', 'pipe', 'ignore'] });
    const registered = output.match(/InstallLocation\s+REG_SZ\s+([^\r\n]+)/)?.[1].trim();
    return !!registered && path.resolve(registered).toLowerCase() === path.resolve(root).toLowerCase();
  } catch { return false; }
}
module.exports = { allUsersInstallation };
