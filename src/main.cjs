const { app, BrowserWindow, WebContentsView, Menu, session, ipcMain, dialog, clipboard, screen, net, shell } = require('electron');
const path = require('node:path');
const fs = require('node:fs');
const packagedClient = require('./packaged-client.cjs').isPackagedClient(app, process);
const adminArgument = process.argv.find(argument => argument.startsWith('--qrazy-admin-update='));
if (adminArgument) {
  // No game window, profile stores or instance lock in the elevated worker.
  app.disableHardwareAcceleration();
  app.whenReady().then(async () => {
    if (process.platform !== 'win32' || !packagedClient) throw new Error('Packaged Windows updater only');
    await require('./admin-update.cjs').runAdminUpdate(adminArgument.slice('--qrazy-admin-update='.length), path.dirname(path.dirname(process.execPath)), app.getVersion());
    app.exit(0);
  }).catch(() => app.exit(1));
} else {
const { DEV_MODE, GAME_URL, isGameURL, permissionAllowed, isFullscreenShortcut } = require('./policy.cjs');
const { RawMouse } = require('./raw-mouse.cjs');
const { VSyncPreference } = require('./vsync.cjs');
const { MaxFpsPreference } = require('./max-fps.cjs');
const { AssetStore } = require('./asset-store.cjs');
const { graphicsState } = require('./graphics.cjs');
const { validateGameStatus } = require('./game-status.cjs');
const { WindowState } = require('./window-state.cjs');
const { linuxCapability } = require('./linux-input.cjs');
const { Updater } = require('./updater.cjs');
const { WebsiteUpdates } = require('./website-updates.cjs');
const { startInstaller } = require('./update-install.cjs');
const updateConfig = require('./update-config.cjs');
// A second packaged instance must not keep files/profile open during replacement.
const ownsClientInstance = !packagedClient || app.requestSingleInstanceLock();
if (!ownsClientInstance) app.quit();
// This backend attaches to the public X11 Window handle. Wayland sessions use
// XWayland browser fallback; they are never advertised as native XI2 capture.
if (process.platform === 'linux') app.commandLine.appendSwitch('ozone-platform', 'x11');
// Prefer the discrete GPU without bypassing Chromium's driver safety checks.
app.commandLine.appendSwitch('force_high_performance_gpu');
const vsync = new VSyncPreference(app.getPath('userData'), app.commandLine);
const maxFps = new MaxFpsPreference(app.getPath('userData'));
const assets = new AssetStore(path.join(app.getPath('userData'), 'assets-v1'));
app.enableSandbox();
let win, game, status, rawMouse, loading = false, attempt = 0, timeout;
let gameErrorShowing = false, gameErrorDetails = '';
let graphics = graphicsState();
let updater, websiteUpdates, updateTimer, websiteTimer;
let installingUpdate = false;
app.on('gpu-info-update', () => {
  graphics = graphicsState(app.getGPUFeatureStatus());
  if (game && !game.isDestroyed() && isGameURL(game.webContents.getURL()))
    game.webContents.send('client:graphics-state', graphics);
});
function trustedGame(event) {
  return game && !game.isDestroyed() && event.sender === game.webContents &&
    event.senderFrame === game.webContents.mainFrame && isGameURL(event.senderFrame.url);
}
function showStatus(message, retry, details = '', retryLabel = 'Retry') {
  if (!win || win.isDestroyed()) return;
  rawMouse?.release();
  win.contentView.addChildView(status);
  syncBounds();
  status.webContents.send('client:status', { message, retry, details, retryLabel });
}
function syncBounds() {
  const [width, height] = win.getContentSize();
  status.setBounds({ x: 0, y: 0, width, height });
}
function fail(message) {
  loading = false; clearTimeout(timeout); showStatus(message, true);
}
async function loadGame() {
  if (loading) return;
  loading = true;
  gameErrorShowing = false; gameErrorDetails = '';
  const token = ++attempt;
  showStatus(DEV_MODE ? 'Connecting to local development server (localhost:5173)…' : 'Connecting to Qrazy…', false);
  timeout = setTimeout(() => {
    if (token !== attempt || !loading) return;
    ++attempt; game.webContents.stop();
    fail(DEV_MODE ? 'Local development server is taking too long to respond. Start it on localhost:5173 and retry.' : 'Qrazy is taking too long to respond. Check your internet connection and retry.');
  }, 15000);
  try {
    // HTTP cache only: keep cookies, local storage, profiles and disk assets.
    await game.webContents.session.clearCache();
    await game.webContents.loadURL(GAME_URL);
    if (token !== attempt || win.isDestroyed()) return;
    loading = false; clearTimeout(timeout);
    if (!gameErrorShowing) win.contentView.removeChildView(status);
    game.webContents.send('client:minimized', win.isMinimized());
    game.webContents.focus();
  } catch {
    if (token === attempt) fail(DEV_MODE ? 'Unable to load the local development server. Start it on localhost:5173 and retry.' : 'Unable to load Qrazy. Check your internet connection and retry.');
  }
}
function secure(contents, status = false) {
  contents.setWindowOpenHandler(() => ({ action: 'deny' }));
  contents.on('will-attach-webview', event => event.preventDefault());
  contents.on('will-navigate', (event, url) => { if (status || !isGameURL(url)) event.preventDefault(); });
  contents.on('will-redirect', (event, url) => { if (status || !isGameURL(url)) event.preventDefault(); });
  contents.on('will-frame-navigate', event => {
    if (status || !isGameURL(event.url)) event.preventDefault();
  });
  contents.on('before-input-event', (event, input) => {
    contents.setIgnoreMenuShortcuts(true);
    if (isFullscreenShortcut(input)) {
      event.preventDefault();
      if (input.type === 'keyDown' && !input.isAutoRepeat) win.setFullScreen(!win.isFullScreen());
    }
  });
}
app.whenReady().then(async () => {
  if (!ownsClientInstance) return;
  Menu.setApplicationMenu(null);
  const gameSession = session.fromPartition(DEV_MODE ? 'persist:qrazy-dev' : 'persist:qrazy-local');
  gameSession.setPermissionCheckHandler((_contents, permission, origin, details) =>
    permissionAllowed(permission, details?.requestingUrl || origin));
  gameSession.setPermissionRequestHandler((_contents, permission, callback, details) =>
    callback(permissionAllowed(permission, details.requestingUrl)));
  gameSession.on('will-download', event => event.preventDefault());
  const windowState = new WindowState(app.getPath('userData'), screen.getAllDisplays());
  win = new BrowserWindow({ title: DEV_MODE ? 'Qrazy — Local development' : 'Qrazy', ...windowState.bounds,
    minWidth: 640, minHeight: 480, backgroundColor: '#111827', autoHideMenuBar: true,
    icon: path.join(__dirname, process.platform === 'linux' ? 'assets/qrazy.png' : 'assets/qrazy.ico'),
    webPreferences: {
    sandbox: true, contextIsolation: true, nodeIntegration: false,
    nodeIntegrationInWorker: false, nodeIntegrationInSubFrames: false,
    webSecurity: true, devTools: false, backgroundThrottling: false,
    session: gameSession, preload: path.join(__dirname, 'preload.cjs'),
    additionalArguments: [...(DEV_MODE ? ['--qrazy-dev'] : []),
      ...(process.platform === 'linux' ? [linuxCapability().supported ? '--qrazy-raw-x11' : '--qrazy-raw-unavailable'] : [])]
  } });
  win.setMenu(null);
  if (windowState.maximized) win.maximize();
  if (windowState.fullscreen) win.setFullScreen(true);
  windowState.track(win);
  if (DEV_MODE) win.webContents.on('page-title-updated', event => {
    event.preventDefault(); win.setTitle('Qrazy — Local development (localhost:5173)');
  });
  game = win;
  const updateRoot = process.platform === 'win32' ? path.dirname(path.dirname(process.execPath)) : path.dirname(process.execPath);
  const updateDirectory = path.join(app.getPath('userData'), 'updates-v1');
  const sendUpdate = (channel, state) => {
    if (!win.isDestroyed() && isGameURL(game.webContents.getURL())) game.webContents.send(channel, state);
  };
  updater = new Updater({ root: updateRoot, directory: updateDirectory, version: app.getVersion(),
    allUsers: require('./installation.cjs').allUsersInstallation(updateRoot),
    platform: process.platform + '-' + process.arch, config: updateConfig,
    fetch: (url, options) => net.fetch(url, options), notify: state => sendUpdate('client:update-state', state) });
  if (packagedClient && !DEV_MODE) await updater.restorePending();
  if (!packagedClient || DEV_MODE) updater.setState({ phase: 'unconfigured', message: 'Client updates are available in the packaged live client.' });
  websiteUpdates = new WebsiteUpdates({ url: GAME_URL, fetch: (url, options) => net.fetch(url, options),
    notify: state => sendUpdate('client:website-update-state', state) });
  const requireUpdateSender = (event, focused = false) => {
    if (!trustedGame(event) || (focused && (!win.isFocused() || win.isMinimized()))) throw new Error('Updates require the official game in the current client window');
  };
  ipcMain.handle('client:update-get', event => { requireUpdateSender(event); return { ...updater.getState(), gameRefreshNeeded: websiteUpdates.getState().outdated }; });
  ipcMain.handle('client:update-check', event => {
    requireUpdateSender(event);
    if (!packagedClient || DEV_MODE) return updater.getState();
    return updater.check();
  });
  ipcMain.handle('client:update-download', event => { requireUpdateSender(event, true); return updater.download(); });
  ipcMain.handle('client:update-download-page', event => {
    requireUpdateSender(event, true);
    return shell.openExternal('https://github.com/JimmSlimm/qrazy-client/releases/latest');
  });
  ipcMain.handle('client:update-install', async event => {
    requireUpdateSender(event, true);
    if (!packagedClient || DEV_MODE) throw new Error('Only a packaged live client can update itself');
    if (installingUpdate) throw new Error('The update is already starting');
    installingUpdate = true;
    // Ensure open asset writes and window settings finish during normal shutdown.
    try { await startInstaller(updater); } catch (error) { installingUpdate = false; return { ...updater.getState(), message: error.message }; }
    app.quit(); return { phase: 'installing' };
  });
  ipcMain.handle('client:website-update-get', event => { requireUpdateSender(event); return websiteUpdates.getState(); });
  ipcMain.handle('client:website-build', (event, commit) => { requireUpdateSender(event); return websiteUpdates.reportLoadedBuild(commit); });
  ipcMain.handle('client:refresh-game', async event => {
    requireUpdateSender(event, true);
    if (loading) return false;
    rawMouse?.release();
    await loadGame(); return true;
  });
  updateTimer = setTimeout(() => { if (packagedClient && !DEV_MODE) updater.check(); }, 10000);
  websiteTimer = setInterval(() => websiteUpdates.check(), 5 * 60 * 1000);
  win.on('focus', () => websiteUpdates.check());
  // Installation failures remain visible after the helper restarts the old client.
  try {
    const resultPath = path.join(updateDirectory, 'result.json');
    const result = JSON.parse(fs.readFileSync(resultPath, 'utf8').replace(/^\uFEFF/, ''));
    if (result.root && path.resolve(result.root) === path.resolve(updateRoot)) {
      updater.setState({ ...updater.getState(), phase: result.phase === 'error' ? (updater.getState().phase === 'ready' ? 'ready' : 'error') : 'current', message: String(result.message).slice(0, 1000) });
      fs.unlinkSync(resultPath);
    }
  } catch { /* First launch has no helper result. */ }
  const muteAudio = muted => {
    if (!game.webContents.isDestroyed()) game.webContents.setAudioMuted(muted);
  };
  muteAudio(!win.isFocused() || win.isMinimized());
  win.on('blur', () => muteAudio(true));
  win.on('focus', () => muteAudio(win.isMinimized()));
  win.on('minimize', () => muteAudio(true));
  win.on('restore', () => muteAudio(!win.isFocused()));
  let samples = [], scheduled = false;
  rawMouse = new RawMouse(app, win, sample => {
    samples.push(sample);
    if (samples.length > 8192) { samples = []; rawMouse.release(); return; }
    if (scheduled) return;
    scheduled = true;
    setImmediate(() => {
      scheduled = false;
      const batch = samples; samples = [];
      if (rawMouse.active && !win.isDestroyed() && win.isFocused() && isGameURL(game.webContents.getURL()))
        game.webContents.send('client:raw-move', batch);
    });
  }, () => {
    samples = [];
    if (!win.isDestroyed()) game.webContents.send('client:raw-state', { captured: false });
  });
  ipcMain.handle('client:raw-capture', async event => {
    if (!trustedGame(event) || !win.isFocused() || win.isMinimized() || loading || win.contentView.children.includes(status)) return { ok: false };
    try { return await rawMouse.capture(); } catch { return { ok: false }; }
  });
  ipcMain.handle('client:raw-clock', async event => {
    if (!trustedGame(event)) return { ok: false };
    try { return await rawMouse.clock(); } catch { return { ok: false }; }
  });
  ipcMain.on('client:raw-release', event => { if (trustedGame(event)) rawMouse.release(); });
  ipcMain.handle('client:asset', (event, operation, args) => {
    if (!trustedGame(event)) throw new Error('Asset storage is available only to the official game');
    return assets.call(operation, args);
  });
  ipcMain.handle('client:graphics-get', event => {
    if (!trustedGame(event)) throw new Error('Graphics status is available only to the official game');
    return graphics;
  });
  ipcMain.handle('client:game-status', (event, value) => {
    if (!trustedGame(event)) throw new Error('Game status is available only to the official game');
    const state = validateGameStatus(value);
    if (state.phase === 'error') {
      gameErrorShowing = true;
      gameErrorDetails = [state.stage, state.code, state.message, state.details].filter(Boolean).join('\n');
      showStatus(state.message, state.recovery === 'reload', gameErrorDetails, 'Reload game');
    } else if (state.phase === 'ready' && gameErrorShowing) {
      gameErrorShowing = false; gameErrorDetails = '';
      win.contentView.removeChildView(status);
    }
    game.webContents.send('client:game-status-state', state);
    return state;
  });
  ipcMain.on('client:copy-error', event => {
    if (event.sender === status.webContents && event.senderFrame === status.webContents.mainFrame && gameErrorShowing)
      clipboard.writeText(gameErrorDetails);
  });
  ipcMain.handle('client:vsync-get', event => {
    if (!trustedGame(event)) throw new Error('VSync is available only to the official game');
    return vsync.getState();
  });
  ipcMain.handle('client:max-fps-get', event => {
    if (!trustedGame(event)) throw new Error('FPS settings are available only to the official game');
    return maxFps.getState();
  });
  ipcMain.handle('client:max-fps-set', (event, value) => {
    if (!trustedGame(event)) throw new Error('FPS settings are available only to the official game');
    const state = maxFps.setValue(value);
    game.webContents.send('client:max-fps-state', state);
    return state;
  });
  ipcMain.handle('client:vsync-set', (event, enabled) => {
    if (!trustedGame(event)) throw new Error('VSync is available only to the official game');
    const state = vsync.setEnabled(enabled);
    game.webContents.send('client:vsync-state', state);
    return state;
  });
  ipcMain.handle('client:fullscreen-get', event => trustedGame(event) ? win.isFullScreen() : false);
  ipcMain.handle('client:fullscreen-toggle', event => {
    if (!trustedGame(event) || !win.isFocused()) return false;
    win.setFullScreen(!win.isFullScreen()); return win.isFullScreen();
  });
  const notifyFullscreen = () => {
    if (!win.isDestroyed() && isGameURL(game.webContents.getURL()))
      game.webContents.send('client:fullscreen-state', win.isFullScreen());
  };
  win.on('enter-full-screen', notifyFullscreen);
  win.on('leave-full-screen', notifyFullscreen);
  win.on('blur', () => { rawMouse.release(); game.webContents.send('client:information-hide'); });
  win.on('minimize', () => rawMouse.release());
  game.webContents.on('did-start-navigation', (_event, _url, inPlace, mainFrame) => {
    if (mainFrame && !inPlace) { rawMouse.release(); assets.reset().catch(() => {}); websiteUpdates.reset(); }
  });
  game.webContents.on('render-process-gone', () => { rawMouse.release(); assets.reset().catch(() => {}); });
  status = new WebContentsView({ webPreferences: {
    sandbox: true, contextIsolation: true, nodeIntegration: false, devTools: false,
    preload: path.join(__dirname, 'status-preload.cjs')
  } });
  win.contentView.addChildView(status); syncBounds();
  secure(status.webContents, true); secure(game.webContents);
  game.webContents.on('did-navigate', (_event, _url, responseCode) => {
    if (responseCode >= 400) {
      ++attempt;
      fail(`The Qrazy server returned HTTP ${responseCode}. Please try again shortly.`);
    }
  });
  win.on('resize', syncBounds);
  win.on('minimize', () => {
    game.webContents.setBackgroundThrottling(true);
    game.webContents.send('client:minimized', true);
  });
  win.on('restore', () => {
    game.webContents.setBackgroundThrottling(false);
    game.webContents.send('client:minimized', false);
  });
  game.webContents.on('render-process-gone', () => fail('The game stopped unexpectedly. Retry to reload it.'));
  game.webContents.on('did-fail-load', (_event, code, _description, _url, mainFrame) => {
    if (mainFrame && !loading && code !== -3) fail('The game page could not load. Check your internet connection and retry.');
  });
  ipcMain.on('client:retry', event => {
    if (event.sender === status.webContents && event.senderFrame === status.webContents.mainFrame) loadGame();
  });
  win.on('closed', () => {
    rawMouse.close();
    assets.reset().catch(() => {});
    clearTimeout(timeout); ++attempt;
    clearTimeout(updateTimer); clearInterval(websiteTimer);
    if (!status.webContents.isDestroyed()) status.webContents.close();
  });
  // Read through Node's ASAR support instead of resolving an installation path
  // through Chromium's file URL loader (paths may contain # or other escapes).
  const statusHTML = fs.readFileSync(path.join(__dirname, 'status.html'), 'utf8');
  await status.webContents.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(statusHTML));
  loadGame();
}).catch(error => {
  dialog.showErrorBox('Unable to start Qrazy', error.message || String(error));
  app.quit();
});
app.on('window-all-closed', () => app.quit());
}
