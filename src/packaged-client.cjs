const path = require('node:path');
function isPackagedClient(app, runtime = process) {
  if (runtime.defaultApp) return false;
  if (app.isPackaged) return true;
  // Our Windows launcher uses Electron's original filename. Electron's
  // isPackaged checks that filename, so verify the loaded application instead.
  if (!runtime.resourcesPath || typeof app.getAppPath !== 'function') return false;
  const normalize = value => runtime.platform === 'win32' ? path.resolve(value).toLowerCase() : path.resolve(value);
  return normalize(app.getAppPath()) === normalize(path.join(runtime.resourcesPath, 'app.asar'));
}
module.exports = { isPackagedClient };
