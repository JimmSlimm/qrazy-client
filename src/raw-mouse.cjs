const path = require('node:path');
function nativeExecutable(app, resourcesPath = process.resourcesPath) {
  if (path.extname(app.getAppPath()).toLowerCase() === '.asar')
    return path.join(path.dirname(app.getAppPath()), 'qrazy-input.node');
  if (app.isPackaged)
    return path.join(resourcesPath, 'qrazy-input.node');
  return require('./native/build.cjs').buildNative();
}
class WindowsMouse {
  constructor(app, win, onMove, onRelease) {
    this.win = win; this.onRelease = onRelease; this.active = false; this.generation = 0;
    this.native = require(nativeExecutable(app));
    win.hookWindowMessage(0x00ff, (_wParam, lParam) => {
      if (!this.active || !win.isFocused() || win.isMinimized()) return;
      const sample = this.native.read(lParam);
      if (sample) onMove({ ...sample, generation: this.generation });
    });
  }
  async capture() {
    if (!this.win.isFocused() || this.win.isMinimized()) return { ok: false };
    this.active = false; clearInterval(this.timer); this.native.release();
    const generation = ++this.generation;
    this.active = this.native.capture(this.win.getNativeWindowHandle());
    if (this.active) this.timer = setInterval(() => {
      if (!this.win.isFocused() || this.win.isMinimized() || !this.native.refresh()) this.release();
    }, 15);
    return { ok: this.active, generation };
  }
  async clock() { return { ok: true, time: this.native.clock() }; }
  release() {
    this.active = false; ++this.generation; clearInterval(this.timer);
    this.native.release(); this.onRelease();
  }
  close() {
    this.release();
    if (!this.win.isDestroyed()) this.win.unhookWindowMessage(0x00ff);
  }
}
class UnsupportedMouse {
  constructor(_app, _win, _move, onRelease) { this.active = false; this.onRelease = onRelease; }
  async capture() { return { ok: false }; }
  async clock() { return { ok: false }; }
  release() { this.onRelease(); }
  close() { this.release(); }
}
const { LinuxMouse, linuxCapability } = require('./linux-input.cjs');
const RawMouse = process.platform === 'win32' ? WindowsMouse :
  process.platform === 'linux' && linuxCapability().supported ? LinuxMouse : UnsupportedMouse;
module.exports = { RawMouse, nativeExecutable };
