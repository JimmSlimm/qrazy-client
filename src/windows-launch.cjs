const path = require('node:path');
const fs = require('node:fs');

function redirectThroughLauncher(app, context = process, packaged = false) {
  if (!packaged || context.platform !== 'win32') return false;
  const runtime = path.resolve(context.execPath);
  const marker = context.env?.QRAZY_CLIENT_RUNTIME;
  if (marker) {
    try {
      if (fs.realpathSync.native(marker).toLowerCase() === fs.realpathSync.native(runtime).toLowerCase()) return false;
    } catch { /* A stale marker must not bypass this installation's launcher. */ }
  }
  const launcher = path.join(path.dirname(path.dirname(runtime)), 'Qrazy.exe');
  if (!fs.existsSync(launcher)) return false;
  // A direct runtime/taskbar launch must receive the same pre-process Windows
  // GPU setup as the Start menu launcher. No game window has been created yet.
  app.relaunch({ execPath: launcher, args: [] });
  app.exit(0);
  return true;
}

module.exports = { redirectThroughLauncher };
