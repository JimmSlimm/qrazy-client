const { contextBridge, ipcRenderer } = require('electron');
ipcRenderer.on('client:minimized', (_event, minimized) => {
  contextBridge.executeInMainWorld({ func: value => {
    window.dispatchEvent(new CustomEvent('qrazy-client-minimized', { detail: value }));
  }, args: [minimized === true] });
});

// No generic IPC, file access or Node API is exposed. The main process rechecks origin/focus.
const desktopGameOrigin = process.argv?.includes('--qrazy-dev') ? 'http://localhost:5173' : 'https://qrazy-game.onrender.com';
if (process.isMainFrame && location.origin === desktopGameOrigin) {
  let captured = false, generation = 0, offset = 0, sequence = 0, lastTime = -Infinity;
  const x11Capture = process.argv?.includes('--qrazy-raw-x11');
  const rawUnavailable = process.argv?.includes('--qrazy-raw-unavailable');
  let captureElement;
  const capability = rawUnavailable
    ? { supported: false, backend: null, reason: 'Native Linux input requires an Xorg session with XInput2 2.1 and relative mouse axes; Wayland/XWayland are unsupported.' }
    : { supported: x11Capture ? null : true, backend: x11Capture ? 'x11-xinput2' : 'windows-wm-input',
      reason: x11Capture ? 'X11 candidate: native capture must still probe XI2, window ownership, focus and relative axes. capture() reports success or failure.' : null };
  document.addEventListener('pointerlockchange', () => {
    if (x11Capture && captureElement && document.pointerLockElement !== captureElement) release();
  });
  document.addEventListener('pointerlockerror', () => { if (x11Capture) release(); });
  const moveListeners = new Set(), stateListeners = new Set();
  const fullscreenListeners = new Set();
  const vsyncListeners = new Set();
  const maxFpsListeners = new Set();
  let cursorStyle;
  let gameStatusNotice;
  // Read-only presentation: the game owns data, formatting and Tab eligibility.
  let informationSnapshot = null, informationHeld = false, informationBox;
  const infoText = (value, max = 120) => typeof value === 'string' ? value.slice(0, max) : '';
  function hideInformation() { informationHeld = false; informationBox?.remove(); informationBox = null; }
  function renderInformation() {
    informationBox?.remove(); informationBox = null;
    if (!informationHeld || !informationSnapshot || !document.body) return;
    const data = informationSnapshot;
    const box = document.createElement('section');
    box.setAttribute('aria-label', 'Current map information');
    box.style.cssText = 'position:fixed;top:50%;left:50%;transform:translate(-50%,-50%);width:calc(100% - 48px);max-width:1200px;max-height:90vh;overflow:hidden;box-sizing:border-box;z-index:2147483646;background:#0b1424f5;color:#f4f7fc;border:2px solid #fbbf24;border-radius:12px;box-shadow:0 12px 60px #000c;padding:24px;font:18px/1.45 system-ui,sans-serif;pointer-events:none';
    const add = (parent, tag, text, style = '') => {
      const node = document.createElement(tag); node.textContent = text; node.style.cssText = style; parent.append(node); return node;
    };
    add(box, 'div', data.map || 'No current map', 'font-size:28px;font-weight:800;color:#fbbf24;overflow-wrap:anywhere');
    add(box, 'div', `${data.mode || '—'} - Physics: ${data.physics || '—'} - Mode: ${data.category || '—'} - Session: ${data.sessionTime || '—'}`, 'color:#c3cfdf;margin:4px 0 18px');
    const records = add(box, 'div', '', 'display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:10px;margin-bottom:18px');
    for (const physics of ['pql','vql']) for (const category of ['w','s']) {
      const record = data.records.find(r => r.physics === physics && r.category === category);
      const card = add(records, 'div', '', 'background:#152238;border-radius:6px;padding:10px 12px;min-width:0');
      add(card, 'div', `${physics.toUpperCase()} ${category === 'w' ? 'WEAPONS' : 'STRAFE'} · WR`, 'font-size:13px;font-weight:750;color:#fbbf24');
      const recordTime = record?.state === 'ready' ? record.time || 'No record' : record?.state === 'loading' ? 'Loading…' : 'Unavailable';
      const time = add(card, 'div', '', 'font-size:20px;font-weight:700;font-variant-numeric:tabular-nums');
      // Keep the game's plain-text verification mark, styling only its presentation.
      const question = record?.state === 'ready' ? / \?(?= old$|$)/.exec(recordTime) : null;
      if (question) {
        add(time, 'span', recordTime.slice(0, question.index));
        const mark = add(time, 'span', ' ?', 'font-size:11px;font-weight:400;color:#94a3b8;vertical-align:super');
        mark.setAttribute('aria-label', 'Unverified');
        mark.setAttribute('title', 'Unverified record');
        add(time, 'span', recordTime.slice(question.index + question[0].length));
      } else time.textContent = recordTime;
      add(card, 'div', record?.state === 'ready' ? record.name : 'Awaiting game data', 'font-size:14px;color:#b8c8dc;overflow-wrap:anywhere');
    }
    const columns = add(box, 'div', '', 'display:grid;gap:14px');
    function panel(title, rows) {
      const section = add(columns, 'div', '', 'min-width:0');
      add(section, 'div', title, 'font-size:18px;font-weight:750;color:#fbbf24;background:#19263a;padding:8px 12px;border-left:3px solid #fbbf24');
      const table = add(section, 'table', '', 'width:100%;border-collapse:collapse;font-size:17px;table-layout:fixed');
      const head = document.createElement('thead'); table.append(head);
      const tr = document.createElement('tr'); head.append(tr);
      ['Player','PB','Session best','Ping'].forEach((h, i) => add(tr, 'th', h, `width:${[52,19,19,10][i]}%;text-align:${i ? 'right' : 'left'};color:#9eafc5;font-size:14px;padding:6px 12px;border-bottom:1px solid #40516a`));
      const body = document.createElement('tbody'); table.append(body);
      rows.forEach(row => {
        const tr = document.createElement('tr'); body.append(tr);
        row.forEach((cell, i) => add(tr, 'td', cell, `text-align:${i ? 'right' : 'left'};padding:8px 12px;border-bottom:1px solid #26354b;overflow-wrap:anywhere;font-variant-numeric:tabular-nums;${i ? 'white-space:nowrap' : ''}`));
      });
    }
    if (data.multiplayer.state === 'disconnected' && data.mode !== 'Solo') add(columns, 'div', 'Not connected to multiplayer.', 'padding:14px;background:#152238;border-radius:6px;color:#cbd7e7');
    else if (!data.groups.length) add(columns, 'div', 'No active players.', 'color:#cbd7e7');
    for (const group of data.groups) panel(group.label, group.rows.map(r => [r.name, r.pb || '—', r.sessionBest || '—', r.ping || '—']));
    const spectators = add(box, 'div', '', 'display:flex;gap:16px;align-items:baseline;border-top:1px solid #40516a;padding-top:12px;margin-top:16px');
    add(spectators, 'div', 'SPECTATORS:', 'font-size:14px;font-weight:800;color:#9eafc5');
    add(spectators, 'div', data.spectators.map(r => r.name).join(' · ') || 'None', 'font-size:16px;overflow-wrap:anywhere');
    const playerCount = data.mode === 'Solo' ? data.groups.reduce((count, group) => count + group.rows.length, 0) + data.spectators.length : data.multiplayer.total;
    add(box, 'div', `${playerCount} ${playerCount === 1 ? 'player' : 'players'} · Release Tab to return${data.legacy ? ' · Category, PB, session-best and WR data need the updated game adapter' : data.mode === 'Solo' ? ' · Session best: local untainted finish, not server-verified' : ' · Session best: hub-reported, not replay-verified'}`, 'font-size:13px;color:#9eafc5;margin-top:12px');
    document.body.append(box); informationBox = box;
  }
  const information = {
    version: 1,
    layoutVersion: 2,
    report(value) {
      const states = ['loading', 'ready', 'unavailable', 'disconnected'];
      if (!value || !states.includes(value.leaderboard?.state) || !states.includes(value.multiplayer?.state) ||
          !Array.isArray(value.leaderboard.rows) || !Array.isArray(value.multiplayer.rows) ||
          value.leaderboard.rows.length > 10 || value.multiplayer.rows.length > 10) throw new TypeError('Invalid information snapshot');
      const rows = value.leaderboard.rows.map(r => ({rank: infoText(r.rank, 12), name: infoText(r.name, 64), time: infoText(r.time, 48)}));
      const players = value.multiplayer.rows.map(r => ({name: infoText(r.name, 64), status: infoText(r.status, 32), detail: infoText(r.detail, 64), ping: infoText(r.ping, 16)}));
      const player = r => ({name: infoText(r.name, 96), pb: infoText(r.pb, 48), sessionBest: infoText(r.sessionBest, 48), ping: infoText(r.ping, 16)});
      const legacy = !Array.isArray(value.multiplayer.groups);
      let groups, spectators;
      if (legacy) {
        const active = players.filter(r => r.status !== 'Spectating');
        groups = active.length ? [{label: 'Players · category data unavailable', rows: active.map(player)}] : [];
        spectators = players.filter(r => r.status === 'Spectating').map(player);
      } else {
        if (value.multiplayer.groups.length > 5 || !Array.isArray(value.multiplayer.spectators) || value.multiplayer.spectators.length > 64) throw new TypeError('Invalid category snapshot');
        let count = value.multiplayer.spectators.length;
        groups = value.multiplayer.groups.map(g => {
          if (!Array.isArray(g.rows) || g.rows.length > 64) throw new TypeError('Invalid category rows');
          count += g.rows.length;
          return {label: infoText(g.label, 64), rows: g.rows.map(player)};
        });
        if (count > 64) throw new TypeError('Roster exceeds 64 players');
        spectators = value.multiplayer.spectators.map(player);
      }
      if (value.records !== undefined && (!Array.isArray(value.records) || value.records.length > 4)) throw new TypeError('Invalid records');
      const records = (value.records || []).map(r => {
        if (!['pql','vql'].includes(r.physics) || !['w','s'].includes(r.category) || !states.includes(r.state)) throw new TypeError('Invalid record board');
        return {physics:r.physics, category:r.category, state:r.state, name:infoText(r.name, 96), time:infoText(r.time, 48)};
      });
      informationSnapshot = {map: infoText(value.map), mode: infoText(value.mode, 32), physics: infoText(value.physics, 32), sessionTime: infoText(value.sessionTime, 32), category: infoText(value.category, 32),
        groups, spectators, records, legacy,
        leaderboard: {state: value.leaderboard.state, rows}, multiplayer: {state: value.multiplayer.state, rows: players,
          total: Number.isSafeInteger(value.multiplayer.total) && value.multiplayer.total >= players.length ? value.multiplayer.total : players.length}};
      renderInformation();
    },
    setHeld(value) {
      if (value !== true) { hideInformation(); return; }
      const target = document.activeElement;
      if (!document.hasFocus() || target?.isContentEditable || /^(INPUT|TEXTAREA|SELECT|BUTTON)$/.test(target?.tagName || '')) return;
      informationHeld = true; renderInformation();
    },
    clear() { hideInformation(); informationSnapshot = null; }
  };
  window.addEventListener('keyup', event => { if (event.code === 'Tab') hideInformation(); }, true);
  window.addEventListener('blur', hideInformation);
  window.addEventListener('pagehide', information.clear);
  document.addEventListener('focusin', hideInformation);
  ipcRenderer.on('client:information-hide', hideInformation);
  ipcRenderer.on('client:minimized', (_event, value) => { if (value) hideInformation(); });
  ipcRenderer.on('client:game-status-state', (_event, value) => {
    if (value.phase === 'error') information.clear();
    gameStatusNotice?.remove(); gameStatusNotice = null;
    if (!['loading','warning'].includes(value.phase) || !document.body) return;
    const box = document.createElement('div'); box.setAttribute('role','status');
    box.setAttribute('aria-live','polite');
    box.style.cssText = 'position:fixed;bottom:32px;left:50%;transform:translateX(-50%);width:calc(100% - 48px);max-width:820px;box-sizing:border-box;z-index:2147483647;background:#0b1424;color:#fff;padding:22px 28px;border:2px solid #fbbf24;border-left:8px solid #fbbf24;border-radius:10px;box-shadow:0 8px 40px #000b;font:18px/1.45 system-ui,sans-serif;pointer-events:none;text-align:left';
    const heading = document.createElement('div'), message = document.createElement('div');
    heading.style.cssText = 'font-size:22px;font-weight:800;color:#fbbf24;margin-bottom:6px';
    heading.textContent = value.phase === 'loading' ? 'Loading — please wait' : 'Notice';
    message.textContent = value.message; box.append(heading, message);
    if (value.phase === 'loading' && ['map','assets','graphics'].includes(value.stage)) {
      const explanation = document.createElement('div');
      explanation.style.cssText = 'margin-top:10px;font-size:16px;color:#d5dfed';
      explanation.textContent = 'Frame rate may drop temporarily while the game prepares these resources.';
      box.append(explanation);
    }
    document.body.append(box); gameStatusNotice = box;
  });
  let graphicsWarning, dismissedGraphicsWarning;
  function showGraphicsWarning(value) {
    graphicsWarning?.remove(); graphicsWarning = null;
    if (!value.warning || value.warning === dismissedGraphicsWarning || !document.body) return;
    const box = document.createElement('div'), text = document.createElement('span'), dismiss = document.createElement('button');
    box.setAttribute('role', 'status');
    box.style.cssText = 'position:fixed;top:12px;left:12px;right:12px;z-index:2147483647;padding:12px;background:#362b10;color:#fff;font:14px sans-serif;border:1px solid #dfb34d;display:flex;gap:16px;align-items:center;cursor:auto';
    text.textContent = value.warning; dismiss.textContent = 'Dismiss'; dismiss.style.cssText = 'cursor:pointer;margin-left:auto';
    dismiss.addEventListener('click', () => { dismissedGraphicsWarning = value.warning; box.remove(); });
    box.append(text, dismiss); document.body.append(box); graphicsWarning = box;
  }
  ipcRenderer.on('client:graphics-state', (_event, value) => showGraphicsWarning(value));
  document.addEventListener('DOMContentLoaded', () => {
    ipcRenderer.invoke('client:graphics-get').then(showGraphicsWarning).catch(() => {});
  }, { once: true });
  function state(value) {
    captured = value;
    if (value) {
      cursorStyle ||= document.createElement('style');
      cursorStyle.textContent = '* { cursor: none !important; }';
      (document.head || document.documentElement).append(cursorStyle);
    } else { cursorStyle?.remove(); }
    if (!value && captureElement) {
      const element = captureElement; captureElement = null;
      if (document.pointerLockElement === element) document.exitPointerLock();
    }
    for (const callback of stateListeners) { try { callback({ captured: value }); } catch {} }
  }
  function release() {
    ++sequence; generation = 0; state(false);
    ipcRenderer.send('client:raw-release');
  }
  const updateListeners = new Set();
  let clientUpdateState = { phase: 'idle' }, websiteUpdateState = { outdated: false };
  let updatePanel, updateSummary, updateButton, refreshReminder, updateDialog;
  let updatesOpen = false;
  let dismissedClientVersion = null, dismissedWebsite = false;
  const combinedUpdateState = () => ({ ...clientUpdateState, gameRefreshNeeded: websiteUpdateState.outdated });
  const emitUpdate = () => {
    renderUpdates();
    for (const callback of updateListeners) { try { callback(combinedUpdateState()); } catch {} }
  };
  const updates = {
    version: 1,
    getState: () => ipcRenderer.invoke('client:update-get'),
    check: () => ipcRenderer.invoke('client:update-check'),
    download: () => ipcRenderer.invoke('client:update-download'),
    install: () => ipcRenderer.invoke('client:update-install'),
    refreshGame: () => ipcRenderer.invoke('client:refresh-game'),
    reportLoadedBuild: commit => ipcRenderer.invoke('client:website-build', commit),
    onChange(callback) {
      if (typeof callback !== 'function') throw new TypeError('Update callback must be a function');
      updateListeners.add(callback); return () => updateListeners.delete(callback);
    }
  };
  function updateNode(tag, text, parent) {
    const node = document.createElement(tag); node.textContent = text; parent.append(node); return node;
  }
  function updateAction(parent, label, action) {
    const button = updateNode('button', label, parent); button.type = 'button';
    button.style.cssText = 'font:600 15px system-ui;padding:10px 14px;margin:8px 8px 0 0;border:1px solid #d6a640;border-radius:6px;background:#172237;color:#fff;cursor:pointer;width:auto;letter-spacing:normal';
    button.addEventListener('click', async event => {
      event.preventDefault(); event.stopPropagation(); button.disabled = true;
      try { await action(); } catch (error) { updateSummary.hidden = false; updateSummary.textContent = error.message || 'Unable to complete this action. Try again.'; }
      finally { button.disabled = false; }
    });
    return button;
  }
  function renderUpdates() {
    if (!updatePanel) return;
    const state = clientUpdateState;
    const bytes = state.downloadBytes || 0;
    const size = bytes < 1024 * 1024 ? `${Math.ceil(bytes / 1024)} KB` : `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
    updateButton.textContent = state.phase === 'ready' ? 'Update ready' : ['available', 'downloading'].includes(state.phase) ? 'Update available' : 'Client updates';
    updateButton.style.color = ['available', 'ready', 'downloading'].includes(state.phase) ? '#fbbf24' : '#adbac0';
    updateButton.disabled = false;
    refreshReminder.hidden = !websiteUpdateState.outdated;
    if (!websiteUpdateState.outdated) dismissedWebsite = false;
    updateDialog.replaceChildren();
    updateSummary = updateNode('p', '', updateDialog); updateSummary.hidden = true;
    updateSummary.setAttribute('role', 'status'); updateSummary.setAttribute('aria-live', 'polite');
    if (websiteUpdateState.outdated && !dismissedWebsite) {
      updateDialog.hidden = false;
      updateNode('h2', 'New game version available', updateDialog);
      updateNode('p', 'Refreshing is usually nearly instant. Until you refresh, some features may not work correctly and runs may be rejected. Refreshing ends the current run and reconnects multiplayer.', updateDialog);
      updateAction(updateDialog, 'Refresh game', () => updates.refreshGame());
      updateAction(updateDialog, 'Keep playing', () => { dismissedWebsite = true; updatesOpen = false; renderUpdates(); });
    } else if (['available', 'ready', 'downloading'].includes(state.phase) && dismissedClientVersion !== `${state.version}:${state.phase === 'ready' ? 'ready' : 'available'}`) {
      updateDialog.hidden = false;
      updateNode('h2', 'Qrazy client update available', updateDialog);
      updateNode('p', `Version ${state.version} · Download ${size}. Your saved login, settings and downloaded assets will be kept.`, updateDialog);
      if (state.notes) updateNode('p', state.notes, updateDialog);
      if (state.phase === 'downloading' || (state.phase === 'available' && state.message && state.message !== 'Qrazy client update available')) {
        updateSummary.hidden = false;
        updateSummary.textContent = state.phase === 'downloading' ? `Downloading ${Math.round((state.downloadedBytes || 0) / Math.max(1, bytes) * 100)}% · ${size}` : state.message;
      }
      if (state.phase === 'ready') {
        updateNode('p', 'The download has been verified. Close and update restarts Qrazy and ends your current run.', updateDialog);
        if (state.requiresAdmin) updateNode('p', 'This client is installed for everyone. Windows will ask for administrator approval before Qrazy closes.', updateDialog);
        updateAction(updateDialog, 'Close and update', async () => { const result = await updates.install(); if (result?.message) { updateSummary.hidden = false; updateSummary.textContent = result.message; } });
      } else if (state.phase === 'available') updateAction(updateDialog, 'Download update', async () => { clientUpdateState = await updates.download(); emitUpdate(); });
      if (state.phase === 'available') updateAction(updateDialog, 'View release on GitHub', () => ipcRenderer.invoke('client:update-download-page'));
      updateAction(updateDialog, 'Later', () => { updatesOpen = false; dismissedClientVersion = `${state.version}:${state.phase === 'ready' ? 'ready' : 'available'}`; renderUpdates(); });
    } else if (updatesOpen) {
      updateDialog.hidden = false;
      updateNode('h2', 'Client updates', updateDialog);
      if (state.installedVersion) updateNode('p', `Installed version ${state.installedVersion}`, updateDialog);
      const message = state.phase === 'checking' ? 'Checking for updates…' : state.message || 'Check whether a newer client is available.';
      updateSummary = updateNode('p', message, updateDialog);
      updateSummary.setAttribute('role', 'status'); updateSummary.setAttribute('aria-live', 'polite');
      if (state.phase !== 'checking') updateAction(updateDialog, 'Check again', checkClientUpdates);
      updateAction(updateDialog, 'Close', () => { updatesOpen = false; renderUpdates(); });
    } else updateDialog.hidden = true;
  }
  async function checkClientUpdates() {
    // Show feedback immediately and consume the IPC reply as well as events.
    clientUpdateState = { ...clientUpdateState, phase: 'checking', message: 'Checking for updates…' }; emitUpdate();
    try { clientUpdateState = await updates.check(); }
    catch { clientUpdateState = { ...clientUpdateState, phase: 'error', message: 'Could not check for client updates. Please try again.' }; }
    emitUpdate();
  }
  ipcRenderer.on('client:update-state', (_event, value) => { clientUpdateState = value; emitUpdate(); });
  ipcRenderer.on('client:website-update-state', (_event, value) => { websiteUpdateState = value; emitUpdate(); });
  window.addEventListener('DOMContentLoaded', async () => {
    const menu = document.getElementById('overlay');
    if (menu) {
      updatePanel = document.createElement('section');
      updatePanel.setAttribute('aria-label', 'Qrazy updates');
      updatePanel.style.cssText = 'position:absolute;bottom:20px;right:28px;display:flex;align-items:center;gap:14px;font:14px/1.45 system-ui;z-index:100;letter-spacing:normal';
      menu.append(updatePanel);
      updateButton = updateAction(updatePanel, 'Client updates', () => {
        updatesOpen = true;
        if (['available', 'ready', 'downloading'].includes(clientUpdateState.phase)) { dismissedClientVersion = null; renderUpdates(); return; }
        return checkClientUpdates();
      });
      updateButton.style.cssText = 'font:500 14px system-ui;padding:6px 0;margin:0;border:0;border-radius:0;background:transparent;color:#adbac0;cursor:pointer;width:auto;letter-spacing:normal;text-decoration:underline;text-underline-offset:4px';
      refreshReminder = updateAction(updatePanel, 'Refresh needed', () => updates.refreshGame());
      refreshReminder.title = 'Refresh the game, ending the current run and reconnecting multiplayer.';
      updateSummary = document.createElement('p');
      updateDialog = document.createElement('section');
      updateDialog.setAttribute('role', 'dialog'); updateDialog.setAttribute('aria-label', 'Qrazy update notice');
      updateDialog.style.cssText = 'position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);width:min(560px,calc(100% - 48px));max-height:80vh;overflow:auto;box-sizing:border-box;padding:24px;background:#0b1424;color:#edf2fa;border:2px solid #fbbf24;border-radius:12px;box-shadow:0 16px 64px #000b;font:16px/1.5 system-ui;z-index:101;letter-spacing:normal';
      menu.append(updateDialog);
    }
    try {
      clientUpdateState = await updates.getState();
      websiteUpdateState = await ipcRenderer.invoke('client:website-update-get'); emitUpdate();
      // This is the game's embedded deployment identity, not a fetched guess.
      const commit = document.documentElement.dataset.build;
      if (commit) await updates.reportLoadedBuild(commit);
    } catch { /* An update-service failure must never stop the game. */ }
  });
  contextBridge.exposeInMainWorld('qrazyDesktop', {
    version: 1,
    updates,
    information,
    status: { version: 1, report: value => ipcRenderer.invoke('client:game-status', value) },
    graphics: { getState: () => ipcRenderer.invoke('client:graphics-get') },
    assets: {
      version: 1,
      supportedKinds: ['map', 'sound', 'shader', 'texture'],
      has: (kind, key) => ipcRenderer.invoke('client:asset', 'has', [kind, key]),
      openRead: (kind, key) => ipcRenderer.invoke('client:asset', 'openRead', [kind, key]),
      readChunk: token => ipcRenderer.invoke('client:asset', 'readChunk', [token]),
      closeRead: token => ipcRenderer.invoke('client:asset', 'closeRead', [token]),
      beginWrite: descriptor => ipcRenderer.invoke('client:asset', 'beginWrite', [descriptor]),
      writeChunk(token, bytes) {
        if (!(bytes instanceof Uint8Array || bytes instanceof ArrayBuffer) || bytes.byteLength < 1 || bytes.byteLength > 1024 * 1024)
          return Promise.reject(new RangeError('Asset chunks must contain 1–1048576 bytes'));
        return ipcRenderer.invoke('client:asset', 'writeChunk', [token, bytes]);
      },
      finishWrite: token => ipcRenderer.invoke('client:asset', 'finishWrite', [token]),
      abortWrite: token => ipcRenderer.invoke('client:asset', 'abortWrite', [token])
    },
    maxFps: {
      getState: () => ipcRenderer.invoke('client:max-fps-get'),
      setValue(value) {
        if (!Number.isSafeInteger(value) || value < 30)
          return Promise.reject(new TypeError('com_maxfps must be a whole number of at least 30'));
        return ipcRenderer.invoke('client:max-fps-set', value);
      },
      onChange(callback) {
        if (typeof callback !== 'function') throw new TypeError('Expected callback');
        maxFpsListeners.add(callback); return () => maxFpsListeners.delete(callback);
      }
    },
    vsync: {
      getState: () => ipcRenderer.invoke('client:vsync-get'),
      setEnabled(enabled) {
        if (typeof enabled !== 'boolean') return Promise.reject(new TypeError('VSync preference must be a boolean'));
        return ipcRenderer.invoke('client:vsync-set', enabled);
      },
      onChange(callback) {
        if (typeof callback !== 'function') throw new TypeError('Expected callback');
        vsyncListeners.add(callback); return () => vsyncListeners.delete(callback);
      }
    },
    fullscreen: {
      getState: () => ipcRenderer.invoke('client:fullscreen-get'),
      toggle: () => ipcRenderer.invoke('client:fullscreen-toggle'),
      onChange(callback) {
        if (typeof callback !== 'function') throw new TypeError('Expected callback');
        fullscreenListeners.add(callback); return () => fullscreenListeners.delete(callback);
      }
    },
    rawMouseCapability: capability,
    rawMouse: rawUnavailable ? null : {
      async capture() {
        const token = ++sequence;
        let bestRoundTrip = Infinity;
        for (let i = 0; i < 7; ++i) {
          const before = performance.now();
          const clock = await ipcRenderer.invoke('client:raw-clock');
          const after = performance.now();
          if (!clock.ok || !Number.isFinite(clock.time) || token !== sequence) return false;
          if (after - before < bestRoundTrip) {
            bestRoundTrip = after - before;
            offset = (before + after) / 2 - clock.time;
          }
        }
        if (x11Capture) {
          // Chromium owns the actual game window's grab and normal buttons.
          // Pointer movement is ignored by the game's native adapter; XI2 is
          // the sole source of look deltas. No unadjustedMovement claim here.
          const element = document.documentElement;
          captureElement = element;
          try { await element.requestPointerLock(); }
          catch { release(); return false; }
          if (token !== sequence || document.pointerLockElement !== element) {
            if (document.pointerLockElement === element) document.exitPointerLock();
            release(); return false;
          }
        }
        // Lowest-round-trip midpoint estimate; native receipt, not hardware time.
        let result;
        try { result = await ipcRenderer.invoke('client:raw-capture'); }
        catch { release(); return false; }
        if (token !== sequence) return false;
        if (!result.ok) { release(); return false; }
        generation = result.generation; lastTime = -Infinity; state(true); return true;
      },
      release,
      onMove(callback) {
        if (typeof callback !== 'function') throw new TypeError('Expected callback');
        moveListeners.add(callback); return () => moveListeners.delete(callback);
      },
      onCaptureChange(callback) {
        if (typeof callback !== 'function') throw new TypeError('Expected callback');
        stateListeners.add(callback); return () => stateListeners.delete(callback);
      }
    }
  });
  ipcRenderer.on('client:raw-state', () => { ++sequence; generation = 0; state(false); });
  ipcRenderer.on('client:vsync-state', (_event, state) => {
    for (const callback of vsyncListeners) { try { callback(state); } catch {} }
  });
  ipcRenderer.on('client:max-fps-state', (_event, state) => {
    for (const callback of maxFpsListeners) { try { callback(state); } catch {} }
  });
  ipcRenderer.on('client:fullscreen-state', (_event, fullscreen) => {
    for (const callback of fullscreenListeners) { try { callback(fullscreen === true); } catch {} }
  });
  ipcRenderer.on('client:raw-move', (_event, batch) => {
    if (!captured) return;
    const now = performance.now();
    let latest = -Infinity;
    for (const sample of batch) {
      if (sample.generation === generation && Number.isFinite(sample.time) && Number.isFinite(sample.dx) && Number.isFinite(sample.dy))
        latest = Math.max(latest, sample.time);
    }
    // IPC calibration can be asymmetric. Receipt cannot be after delivery.
    // Shift the common offset, preserving relative sample gaps within the batch.
    if (Number.isFinite(latest)) offset = Math.min(offset, now - latest);
    const samples = [];
    for (const sample of batch) {
      if (sample.generation !== generation || !Number.isFinite(sample.time) || !Number.isFinite(sample.dx) || !Number.isFinite(sample.dy)) continue;
      const timeStamp = Math.max(lastTime, sample.time + offset); lastTime = timeStamp;
      samples.push({ movementX: sample.dx, movementY: sample.dy, timeStamp });
    }
    if (samples.length) for (const callback of moveListeners) { try { callback(samples); } catch {} }
  });
  window.addEventListener('blur', release);
  window.addEventListener('pagehide', release);
}
