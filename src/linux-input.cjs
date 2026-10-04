const path = require('node:path');
const { spawn } = require('node:child_process');
function linuxCapability(env = process.env) {
  if (env.WAYLAND_DISPLAY || env.WAYLAND_SOCKET || env.XDG_SESSION_TYPE === 'wayland')
    return { supported: false, backend: null, reason: 'Native Wayland and XWayland raw capture are unsupported. Use an Xorg session for native input.' };
  if (!env.DISPLAY) return { supported: false, backend: null, reason: 'No X11 display is available.' };
  return { supported: true, backend: 'x11-xinput2', reason: null };
}
class LinuxMouse {
  constructor(app, win, onMove, onRelease, launch = spawn) {
    Object.assign(this, { app, win, onMove, onRelease, launch, active: false, generation: 0, nextId: 0 });
    this.pending = new Map();
  }
  start() {
    if (this.child) return;
    if (!linuxCapability().supported) throw new Error(linuxCapability().reason);
    const file = this.app.isPackaged
      ? path.join(path.dirname(this.app.getAppPath()), 'qrazy-input-x11')
      : require('./native/build.cjs').buildNative();
    const handle = this.win.getNativeWindowHandle();
    if (handle.length !== 8) throw new Error('Expected Linux x64 X11 Window handle');
    const child = this.launch(file, [handle.readBigUInt64LE().toString(), String(process.pid)], { stdio: ['pipe', 'pipe', 'ignore'], windowsHide: true });
    this.child = child;
    let buffer = '';
    const failed = () => { if (this.child === child) this.release(); };
    child.on('error', failed); child.on('exit', failed); child.stdin.on('error', failed);
    child.stdout.on('data', bytes => {
      if (this.child !== child) return;
      buffer += bytes.toString();
      if (buffer.length > 1024 * 1024) return failed();
      let end;
      while ((end = buffer.indexOf('\n')) !== -1) {
        const line = buffer.slice(0, end); buffer = buffer.slice(end + 1);
        let value; try { value = JSON.parse(line); } catch { return failed(); }
        if (value.release) return failed();
        if (value.id) {
          const request = this.pending.get(value.id);
          if (request) { this.pending.delete(value.id); clearTimeout(request.timer); request.resolve(value); }
        } else if (value.generation === this.generation &&
          Number.isFinite(value.dx) && Number.isFinite(value.dy) && Number.isFinite(value.time)) {
          // A capture reply and first samples can share one pipe read. Let
          // capture's promise continuation activate before dispatching them.
          queueMicrotask(() => {
            if (this.child !== child || !this.active || value.generation !== this.generation) return;
            if (!this.win.isFocused() || this.win.isMinimized()) return failed();
            this.onMove(value);
          });
        }
      }
    });
  }
  request(command) {
    this.start();
    const id = ++this.nextId;
    return new Promise(resolve => {
      const timer = setTimeout(() => this.release(), 2000);
      this.pending.set(id, { resolve, timer });
      this.child.stdin.write(`${command} ${id}\n`);
    });
  }
  async clock() { return this.request('T'); }
  async capture() {
    if (!this.win.isFocused() || this.win.isMinimized() || this.win.isDestroyed()) return { ok: false };
    const generation = ++this.generation;
    this.active = false;
    const result = await this.request(`C ${generation}`);
    if (generation !== this.generation || !this.win.isFocused() || this.win.isMinimized()) return { ok: false };
    this.active = result.ok === true;
    if (!this.active) { this.release(); return { ok: false }; }
    clearInterval(this.timer);
    this.timer = setInterval(() => {
      if (this.win.isDestroyed() || !this.win.isFocused() || this.win.isMinimized()) this.release();
    }, 15);
    return { ok: true, generation };
  }
  release() {
    this.active = false; ++this.generation; clearInterval(this.timer);
    const child = this.child; this.child = null;
    // Only terminate our own input helper. Closing its X connection removes all selections.
    child?.kill();
    for (const request of this.pending.values()) { clearTimeout(request.timer); request.resolve({ ok: false }); }
    this.pending.clear(); this.onRelease();
  }
  close() { this.release(); }
}
module.exports = { LinuxMouse, linuxCapability };
