const fs = require('node:fs');
const path = require('node:path');
function fitBounds(value, displays) {
  const valid = value && ['x', 'y', 'width', 'height'].every(key => Number.isSafeInteger(value[key]));
  const initial = valid ? value : { ...displays[0].workArea, width: 1280, height: 800 };
  const area = (displays.find(({ workArea: a }) => initial.x < a.x + a.width && initial.x + initial.width > a.x && initial.y < a.y + a.height && initial.y + initial.height > a.y) || displays[0]).workArea;
  const width = Math.min(area.width, Math.max(640, initial.width));
  const height = Math.min(area.height, Math.max(480, initial.height));
  return { x: Math.max(area.x, Math.min(initial.x, area.x + area.width - width)), y: Math.max(area.y, Math.min(initial.y, area.y + area.height - height)), width, height };
}
class WindowState {
  constructor(directory, displays) {
    this.file = path.join(directory, 'window.json');
    let saved;
    try { saved = JSON.parse(fs.readFileSync(this.file, 'utf8')); } catch {}
    this.bounds = fitBounds(saved?.bounds, displays);
    this.maximized = saved?.maximized === true;
    this.fullscreen = saved?.fullscreen === true;
  }
  track(win, screen) {
    let timer;
    // Windows can restore fullscreen windows onto the primary monitor. Keep
    // the last visible placement independently of minimized native bounds.
    let placement, minimizedPlacement;
    const rememberPlacement = () => {
      if (!screen || win.isDestroyed() || win.isMinimized() || minimizedPlacement) return;
      const bounds = win.getBounds();
      placement = { bounds, normal: win.getNormalBounds(),
        displayId: screen.getDisplayMatching(bounds).id,
        fullscreen: win.isFullScreen(), maximized: win.isFullScreen() ? this.maximized : win.isMaximized() };
    };
    rememberPlacement();
    for (const event of ['move', 'resize', 'maximize', 'unmaximize', 'enter-full-screen', 'leave-full-screen'])
      win.on(event, rememberPlacement);
    win.on('minimize', () => { minimizedPlacement = placement; });
    win.on('restore', () => {
      const previous = minimizedPlacement;
      if (!previous) return;
      // Let the native restore finish before correcting its monitor choice.
      setImmediate(() => {
        if (win.isDestroyed() || win.isMinimized()) return;
        const displays = screen.getAllDisplays();
        const target = displays.find(display => display.id === previous.displayId);
        if (target && screen.getDisplayMatching(win.getBounds()).id !== target.id) {
          if (win.isFullScreen()) win.setFullScreen(false);
          if (win.isMaximized()) win.unmaximize();
          const bounds = previous.fullscreen || previous.maximized ? previous.normal : previous.bounds;
          win.setBounds(fitBounds(bounds, [target]));
          if (previous.maximized) win.maximize();
          if (previous.fullscreen) win.setFullScreen(true);
        }
        minimizedPlacement = undefined;
        rememberPlacement();
      });
    });
    win.on('maximize', () => { if (!win.isFullScreen()) this.maximized = true; });
    win.on('unmaximize', () => { if (!win.isFullScreen()) this.maximized = false; });
    const save = () => {
      clearTimeout(timer);
      if (win.isDestroyed()) return;
      // Minimizing must not replace the last usable mode. Fullscreen retains
      // the underlying maximized state so leaving fullscreen restores it.
      if (!win.isMinimized()) {
        this.fullscreen = win.isFullScreen();
        if (!this.fullscreen) this.maximized = win.isMaximized();
        if (!this.fullscreen && !this.maximized) this.bounds = win.getNormalBounds();
      }
      try {
        fs.mkdirSync(path.dirname(this.file), { recursive: true });
        fs.writeFileSync(this.file + '.tmp', JSON.stringify({ bounds: this.bounds, maximized: this.maximized, fullscreen: this.fullscreen }) + '\n');
        fs.renameSync(this.file + '.tmp', this.file);
      } catch (error) { console.warn('Unable to save window preference:', error.code || 'write failed'); }
    };
    const schedule = () => { clearTimeout(timer); timer = setTimeout(save, 250); };
    for (const event of ['resize', 'move', 'maximize', 'unmaximize', 'enter-full-screen', 'leave-full-screen', 'restore', 'minimize']) win.on(event, schedule);
    win.on('close', save);
    win.on('closed', () => clearTimeout(timer));
  }
}
module.exports = { WindowState, fitBounds };
