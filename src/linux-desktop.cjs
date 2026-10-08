const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');

// Recent desktop portals require the app ID to resolve to a desktop entry.
// The AppImage's embedded entry is outside the portal service's search path.
function registerLinuxDesktop(app, runtime = process) {
  if (runtime.platform !== 'linux') return;
  app.setDesktopName('qrazy.desktop');
  const executable = runtime.env.APPIMAGE || runtime.execPath;
  if (!path.isAbsolute(executable) || /[\r\n\0]/.test(executable)) return;
  const dataHome = runtime.env.XDG_DATA_HOME;
  const directory = path.join(dataHome && path.isAbsolute(dataHome) ? dataHome : path.join(os.homedir(), '.local', 'share'), 'applications');
  const filename = path.join(directory, 'qrazy.desktop');
  const marker = 'X-Qrazy-Managed=portal-identification-v1';
  const quoted = executable.replace(/\\/g, '\\\\\\\\').replace(/["`$]/g, '\\$&').replace(/%/g, '%%');
  const content = `[Desktop Entry]\nType=Application\nName=Qrazy\nExec="${quoted}"\nNoDisplay=true\nTerminal=false\n${marker}\n`;
  try {
    fs.mkdirSync(directory, { recursive: true });
    try {
      // Preserve desktop entries installed or customized by the user.
      const existing = fs.lstatSync(filename);
      if (!existing.isFile() || !fs.readFileSync(filename, 'utf8').split('\n').includes(marker)) return;
      if (fs.readFileSync(filename, 'utf8') === content) return;
      // Replace our metadata atomically; never follow a desktop-entry symlink.
      const temporary = `${filename}.${runtime.pid}.tmp`;
      fs.writeFileSync(temporary, content, { flag: 'wx', mode: 0o644 });
      try { fs.renameSync(temporary, filename); } finally { if (fs.existsSync(temporary)) fs.unlinkSync(temporary); }
    } catch (error) {
      if (error.code !== 'ENOENT') throw error;
      fs.writeFileSync(filename, content, { flag: 'wx', mode: 0o644 });
    }
  } catch (error) {
    // Metadata failure must not prevent the game from starting.
    console.warn(`Qrazy desktop identification unavailable: ${error.message}`);
  }
}

module.exports = { registerLinuxDesktop };
