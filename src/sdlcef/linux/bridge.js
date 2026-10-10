(function(host) {
  'use strict';
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
    const coloredText = (parent, text) => {
      const parts = text.split(/\^([0-9])/);
      const colors = ['#000','#f00','#0f0','#ff0','#3D65E9','#0ff','#f0f','#fff','#000'];
      let color = '';
      if (parts[0]) add(parent, 'span', parts[0]);
      for (let i = 1; i < parts.length; i += 2) {
        if (parts[i] !== '9') color = colors[Number(parts[i])];
        if (parts[i + 1]) add(parent, 'span', parts[i + 1], `color:${color || 'inherit'}`);
      }
    };
    const coloredName = (parent, row) => {
      if (!row.clanTag) { coloredText(parent, row.coloredName || row.name); return; }
      // One inline run lets layout measure and truncate tag + space + name together.
      const run = add(parent, 'span', '', 'display:block;max-width:100%;overflow:hidden;text-overflow:ellipsis;white-space:nowrap');
      const tag = add(run, 'span', '', `opacity:0.85;${row.clanOfficial ? 'text-decoration-line:underline;text-decoration-color:#eef3f4;text-decoration-thickness:1px;text-underline-offset:2px;' : ''}`);
      const small = add(tag, 'span', '', 'font-size:0.78em;vertical-align:baseline');
      coloredText(small, row.clanTag);
      add(tag, 'span', ' ');
      // Separate color parsing resets the name to its inherited default color.
      coloredText(run, row.coloredBaseName || row.name);
    };
    const comparison = (parent, text) => {
      if (!text) return;
      add(parent, 'div', `${text} vs WR`, `font-size:13px;color:${text.startsWith('-') ? '#86efac' : text.startsWith('+') ? '#fca5a5' : '#c3cfdf'}`);
    };
    add(box, 'div', data.map || 'No current map', 'font-size:28px;font-weight:800;color:#fbbf24;overflow-wrap:anywhere');
    add(box, 'div', `${data.mode || '\u2014'} - Physics: ${data.physics || '\u2014'} - Mode: ${data.category || '\u2014'} - Session: ${data.sessionTime || '\u2014'}`, 'color:#c3cfdf;margin:4px 0 18px');
    const records = add(box, 'div', '', 'display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:10px;margin-bottom:18px');
    const boards = data.profileRecords.length ? data.profileRecords : ['pql','vql'].flatMap(physics => ['w','s'].map(category => ({physics, category})));
    for (const board of boards) {
      const {physics, category} = board;
      const profile = data.profileRecords.length > 0;
      const record = profile ? board : data.records.find(r => r.physics === physics && r.category === category);
      const selection = profile ? data.activeProfileRecord : data.activeRecord;
      const active = profile ? selection?.profileId === board.profileId : selection?.physics === physics && selection?.category === category;
      const title = profile ? board.label : `${physics.toUpperCase()} ${category === 'w' ? 'WEAPONS' : 'STRAFE'}`;
      const card = add(records, 'div', '', `background:${active ? '#34301e' : '#152238'};border:2px solid ${active ? '#fbbf24' : 'transparent'};border-radius:6px;padding:10px 12px;min-width:0;display:flex;flex-direction:column;align-items:flex-start`);
      if (active) {
        const label = selection.source === 'pov' ? 'WATCHING' : 'YOU';
        card.setAttribute('aria-label', `${title} world record \u2014 ${label}`);
      }
      add(card, 'div', `${title} \u00b7 WR`, 'font-size:13px;font-weight:750;color:#fbbf24');
      const recordTime = record?.state === 'ready' ? record.time || 'No record' : record?.state === 'loading' ? 'Loading\u2026' : 'Unavailable';
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
      const name = add(card, 'div', '', 'font-size:14px;color:#b8c8dc;overflow-wrap:anywhere');
      if (record?.state === 'ready') coloredName(name, record); else name.textContent = 'Awaiting game data';
      if (active) {
        const footer = add(card, 'div', '', 'margin-top:auto;padding-top:6px');
        add(footer, 'div', selection.source === 'pov' ? 'WATCHING' : 'YOU', 'font-size:12px;font-weight:900;letter-spacing:1px;color:#0b1424;background:#fbbf24;border-radius:3px;padding:2px 7px;display:inline-block');
      }
    }
    const columns = add(box, 'div', '', 'display:grid;gap:14px');
    function panel(title, rows) {
      const section = add(columns, 'div', '', 'min-width:0');
      add(section, 'div', title, 'font-size:18px;font-weight:750;color:#fbbf24;background:#19263a;padding:8px 12px;border-left:3px solid #fbbf24');
      const table = add(section, 'table', '', 'width:100%;border-collapse:collapse;font-size:17px;table-layout:fixed');
      const head = document.createElement('thead'); table.append(head);
      const tr = document.createElement('tr'); head.append(tr);
      ['Player','PB','Session best','Ping'].forEach((h, i) => add(tr, 'th', h, `width:${[46,22,22,10][i]}%;text-align:${i ? 'right' : 'left'};color:#9eafc5;font-size:14px;padding:6px 12px;border-bottom:1px solid #40516a`));
      const body = document.createElement('tbody'); table.append(body);
      rows.forEach(row => {
        const tr = document.createElement('tr'); body.append(tr);
        const watching = data.mode === 'Spectating' && row.watching;
        const self = ['Multiplayer', 'Solo'].includes(data.mode) && row.you;
        const highlight = watching ? 'WATCHING' : self ? 'YOU' : null;
        if (highlight) {
          tr.style.cssText = 'background:#34301e;box-shadow:inset 3px 0 #fbbf24';
          tr.setAttribute('aria-label', `${row.name} \u2014 ${highlight}`);
        }
        [row.name, row.pb || '\u2014', row.sessionBest || '\u2014', row.ping || '\u2014'].forEach((cell, i) => {
          const td = add(tr, 'td', i === 0 ? '' : cell, `text-align:${i ? 'right' : 'left'};padding:8px 12px;border-bottom:1px solid #26354b;overflow-wrap:anywhere;font-variant-numeric:tabular-nums;${i ? 'white-space:nowrap' : ''}`);
          if (i === 0) {
            coloredName(td, row);
            if (highlight) add(td, 'div', highlight, 'font-size:11px;font-weight:900;letter-spacing:1px;color:#0b1424;background:#fbbf24;border-radius:3px;padding:1px 6px;display:inline-block;margin-top:4px');
          }
          if (i === 1 && row.pb && row.pb !== '\u2014') comparison(td, row.pbWrComparison);
          if (i === 2 && row.sessionBest && row.sessionBest !== '\u2014') comparison(td, row.sessionWrComparison);
        });
      });
    }
    if (data.multiplayer.state === 'disconnected' && data.mode !== 'Solo') add(columns, 'div', 'Not connected to multiplayer.', 'padding:14px;background:#152238;border-radius:6px;color:#cbd7e7');
    else if (!data.groups.some(group => group.rows.length)) add(columns, 'div', 'No active players.', 'color:#cbd7e7');
    for (const group of data.groups) if (group.rows.length) panel(group.label, group.rows);
    const spectators = add(box, 'div', '', 'display:flex;gap:16px;align-items:baseline;border-top:1px solid #40516a;padding-top:12px;margin-top:16px');
    add(spectators, 'div', 'SPECTATORS:', 'font-size:14px;font-weight:800;color:#9eafc5');
    const spectatorNames = add(spectators, 'div', '', 'display:flex;flex-wrap:wrap;gap:8px;font-size:16px;min-width:0');
    if (!data.spectators.length) spectatorNames.textContent = 'None';
    for (const row of data.spectators) {
      const chip = add(spectatorNames, 'span', '', `background:#19263a;border:1px solid #52637b;border-radius:5px;padding:3px 10px;overflow-wrap:anywhere;${row.clanTag ? 'min-width:0;max-width:100%;box-sizing:border-box;' : ''}`);
      coloredName(chip, row);
    }
    const playerCount = data.mode === 'Solo' ? data.groups.reduce((count, group) => count + group.rows.length, 0) + data.spectators.length : data.multiplayer.total;
    add(box, 'div', `${playerCount} ${playerCount === 1 ? 'player' : 'players'} \u00b7 Release Tab to return${data.legacy ? ' \u00b7 Category, PB, session-best and WR data need the updated game adapter' : data.mode === 'Solo' ? ' \u00b7 Session best: local untainted finish, not server-verified' : ' \u00b7 Session best: hub-reported, not replay-verified'}`, 'font-size:13px;color:#9eafc5;margin-top:12px');
    document.body.append(box); informationBox = box;
  }
  const information = {
    version: 1,
    layoutVersion: 3,
    supportsProfileRecords: true,
    report(value) {
      const states = ['loading', 'ready', 'unavailable', 'disconnected'];
      if (!value || !states.includes(value.leaderboard?.state) || !states.includes(value.multiplayer?.state) ||
          !Array.isArray(value.leaderboard.rows) || !Array.isArray(value.multiplayer.rows) ||
          value.leaderboard.rows.length > 10 || value.multiplayer.rows.length > 10) throw new TypeError('Invalid information snapshot');
      const rows = value.leaderboard.rows.map(r => ({rank: infoText(r.rank, 12), name: infoText(r.name, 64), time: infoText(r.time, 48)}));
      const players = value.multiplayer.rows.map(r => ({name: infoText(r.name, 64), status: infoText(r.status, 32), detail: infoText(r.detail, 64), ping: infoText(r.ping, 16)}));
      const clan = r => ({clanTag: infoText(r.clanTag, 192), clanOfficial: r.clanOfficial === true, coloredBaseName: infoText(r.coloredBaseName, 192)});
      const player = r => ({...clan(r), watching: r.watching === true, you: r.you === true, coloredName: infoText(r.coloredName, 192), pbWrComparison: infoText(r.pbWrComparison, 32), sessionWrComparison: infoText(r.sessionWrComparison, 32), name: infoText(r.name, 96), pb: infoText(r.pb, 48), sessionBest: infoText(r.sessionBest, 48), ping: infoText(r.ping, 16)});
      const legacy = !Array.isArray(value.multiplayer.groups);
      let groups, spectators;
      if (legacy) {
        const active = players.filter(r => r.status !== 'Spectating');
        groups = active.length ? [{label: 'Players \u00b7 category data unavailable', rows: active.map(player)}] : [];
        spectators = players.filter(r => r.status === 'Spectating').map(player);
      } else {
        if (value.multiplayer.groups.length > 64 || !Array.isArray(value.multiplayer.spectators) || value.multiplayer.spectators.length > 64) throw new TypeError('Invalid category snapshot');
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
        return {...clan(r), physics:r.physics, category:r.category, state:r.state, name:infoText(r.name, 96), coloredName:infoText(r.coloredName, 192), time:infoText(r.time, 48)};
      });
      if (value.profileRecords !== undefined && (!Array.isArray(value.profileRecords) || value.profileRecords.length > 1)) throw new TypeError('Invalid profile records');
      const profileRecords = (value.profileRecords || []).map(r => {
        if (!r || typeof r.profileId !== 'string' || !r.profileId.trim() || r.profileId.length > 96 ||
            typeof r.label !== 'string' || !r.label.trim() || r.label.length > 64 ||
            !['pql','vql'].includes(r.physics) || !['w','s'].includes(r.category) ||
            !['loading','ready','unavailable'].includes(r.state)) throw new TypeError('Invalid profile record board');
        return {...clan(r), profileId:r.profileId, label:r.label, physics:r.physics, category:r.category, state:r.state,
          name:infoText(r.name, 96), coloredName:infoText(r.coloredName, 192), time:infoText(r.time, 48)};
      });
      const profileActive = value.activeProfileRecord;
      const activeProfileRecord = profileActive && typeof profileActive.profileId === 'string' &&
        profileRecords.some(r => r.profileId === profileActive.profileId) && ['self','pov'].includes(profileActive.source)
        ? {profileId:profileActive.profileId, source:profileActive.source} : null;
      // Only explicit game-owned POV context can select a board. Missing/invalid data clears it.
      const active = value.activeRecord;
      const activeRecord = active && ['pql','vql'].includes(active.physics) && ['w','s'].includes(active.category) && ['self','pov'].includes(active.source)
        ? {physics: active.physics, category: active.category, source: active.source} : null;
      informationSnapshot = {map: infoText(value.map), mode: infoText(value.mode, 32), physics: infoText(value.physics, 32), sessionTime: infoText(value.sessionTime, 32), category: infoText(value.category, 32), activeRecord,
        groups, spectators, records, profileRecords, activeProfileRecord, legacy,
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

  const pending = new Map(), moves = new Set(), states = new Set(), fullscreen = new Set();
  let gameStatusNotice;
  const assetCall = async (op, args) => (await rpc('assets', {op,args})).data;
  function encodeBytes(bytes) {
    if (bytes instanceof ArrayBuffer) bytes = new Uint8Array(bytes);
    if (!(bytes instanceof Uint8Array) || bytes.length < 1 || bytes.length > 1048576) throw new RangeError('Asset chunks must contain 1–1048576 bytes');
    let text = '';
    for (let i=0;i<bytes.length;i+=8192) text += String.fromCharCode(...bytes.subarray(i,i+8192));
    return btoa(text);
  }
  const assets = {
    version: 1, supportedKinds: Object.freeze(['map','sound','shader','texture']),
    has: (kind,key) => assetCall('has',[kind,key]),
    openRead: (kind,key) => assetCall('openRead',[kind,key]),
    async readChunk(token) {
      const value=await assetCall('readChunk',[token]);
      const text=atob(value.base64), bytes=new Uint8Array(text.length);
      // Avoid string iteration and a callback for every byte of cached assets.
      for(let i=0;i<text.length;i++)bytes[i]=text.charCodeAt(i);
      return {bytes,done:value.done};
    },
    closeRead: token => assetCall('closeRead',[token]),
    beginWrite: descriptor => assetCall('beginWrite',[descriptor]),
    writeChunk: (token,bytes) => assetCall('writeChunk',[token,encodeBytes(bytes)]),
    finishWrite: token => assetCall('finishWrite',[token]),
    abortWrite: token => assetCall('abortWrite',[token])
  };
  function showStatus(value) {
    if (value.phase === 'error') information.clear();
    gameStatusNotice?.remove(); gameStatusNotice = null;
    if (!['loading','warning','error'].includes(value.phase) || !document.body) return;
    const box=document.createElement('section'); box.setAttribute('role','status');box.setAttribute('aria-live','polite');
    box.style.cssText='position:fixed;bottom:32px;left:50%;transform:translateX(-50%);width:calc(100% - 48px);max-width:820px;box-sizing:border-box;z-index:2147483647;background:#0b1424;color:white;border:2px solid #fbbf24;border-left:8px solid #fbbf24;border-radius:10px;padding:22px 28px;font:18px/1.45 system-ui;box-shadow:0 8px 40px #000b;pointer-events:'+ (value.phase==='error'?'auto':'none');
    const title=document.createElement('strong');title.style.cssText='display:block;color:#fbbf24;font-size:22px;margin-bottom:6px';title.textContent=value.phase==='loading'?'Loading — please wait':value.phase==='error'?'Game needs recovery':'Notice';
    const text=document.createElement('div');text.textContent=value.message;box.append(title,text);
    if (value.phase==='loading' && ['map','assets','graphics'].includes(value.stage)) { const note=document.createElement('p');note.textContent='Frame rate may drop temporarily while the game prepares these resources.';box.append(note); }
    if (value.phase==='error' && value.recovery==='reload') { const button=document.createElement('button');button.textContent='Reload game';button.onclick=()=>rpc('retry');box.append(button); }
    document.body.append(box);gameStatusNotice=box;
  }
  async function reportStatus(value) {
    if (!value || !['loading','ready','warning','error'].includes(value.phase) || !['server','map','assets','graphics','game'].includes(value.stage||'game') || !['none','reload'].includes(value.recovery||'none') || value.recovery==='reload'&&value.phase!=='error') throw new TypeError('Invalid game status');
    const checked={phase:value.phase,stage:value.stage||'game',recovery:value.recovery||'none'};
    for (const [field,limit] of [['message',500],['code',80],['details',2000]]) {
      const text=value[field] ?? '';if(typeof text!=='string'||text.length>limit||/[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/.test(text))throw new TypeError('Invalid status text');checked[field]=text.trim();
    }
    if (checked.phase!=='ready'&&!checked.message)throw new TypeError('Expected status message');
    await rpc('status',checked);showStatus(checked);
  }

  // Match the game's complete desktop-updates v1 contract. Site deployments
  // are independent of signed native client releases.
  let loadedBuild = null, siteOutdated = false, siteBusy = false, siteDismissed = false, siteNotice;
  const updateListeners = new Set();
  const validBuild = value => typeof value === 'string' && /^[a-f0-9]{7,40}$/.test(value);
  const combinedState = state => ({...state, gameRefreshNeeded:siteOutdated});
  function renderSiteUpdate() {
    siteNotice?.remove();siteNotice=null;
    renderClientNotice();
    const menu=document.getElementById('overlay');if(!menu||!siteOutdated||!siteDismissed)return;
    const reminder=document.createElement('button');reminder.type='button';reminder.textContent='Game refresh needed';reminder.style.cssText='position:fixed;bottom:64px;right:16px;z-index:2147483645;width:auto;background:#0b1424;color:#fbbf24;border:1px solid #fbbf24;padding:10px;font:15px system-ui;letter-spacing:normal';
    reminder.onclick=event=>{event.preventDefault();event.stopPropagation();if(event.isTrusted){siteDismissed=false;renderSiteUpdate();}};menu.append(reminder);siteNotice=reminder;
  }
  async function checkSiteUpdate() {
    if (!loadedBuild || siteBusy || siteOutdated) return;
    siteBusy=true;
    try {
      const response=await fetch('https://qrazy-game.onrender.com/build.json',{cache:'no-store',credentials:'omit',redirect:'error',signal:AbortSignal.timeout(10000)});
      if (!response.ok) return;
      const reader=response.body.getReader();let size=0;const chunks=[];
      try { while(true) { const {done,value}=await reader.read();if(done)break;size+=value.length;if(size>4096)throw new Error('Build response too large');chunks.push(value); } }
      finally { await reader.cancel(); }
      const bytes=new Uint8Array(size);let offset=0;for(const chunk of chunks){bytes.set(chunk,offset);offset+=chunk.length;}
      const build=JSON.parse(new TextDecoder().decode(bytes));
      if(validBuild(build.commit)&&build.commit!==loadedBuild){siteOutdated=true;renderSiteUpdate();updates.getState().then(state=>emit(updateListeners,state)).catch(()=>{});}
    } catch { /* Offline or invalid responses never interrupt gameplay. */ }
    finally { siteBusy=false; }
  }
  const updates=Object.freeze({
    version:1,
    getState:async()=>combinedState((await rpc('update-state')).data),
    check:async()=>{await checkSiteUpdate();const state=combinedState((await rpc('update-check')).data);emit(updateListeners,state);return state;},
    download:async()=>{const state=combinedState((await rpc('update-check')).data);emit(updateListeners,state);return state;},
    install:()=>rpc('update-install'),
    refreshGame:async()=>{release();const result=await rpc('refresh-game');if(!result.ok)throw new Error('Unable to refresh game');return true;},
    reportLoadedBuild:async commit=>{if(validBuild(commit)){if(!loadedBuild)loadedBuild=commit;await checkSiteUpdate();}return {outdated:siteOutdated};},
    onChange:fn=>subscribe(updateListeners,fn)
  });
  const siteTimer=setInterval(checkSiteUpdate,5*60*1000);
  window.addEventListener('focus',checkSiteUpdate);
  window.addEventListener('pagehide',()=>clearInterval(siteTimer));
  document.addEventListener('DOMContentLoaded',()=>{
    const style=document.createElement('style');style.textContent='#overlay #update-popup{display:none!important}';(document.head||document.documentElement).append(style);
    updates.reportLoadedBuild(document.documentElement.dataset.build).catch(()=>{});renderSiteUpdate();
  });


  let clientNoticeState=null, clientNoticeDismissed='', clientCheckBusy=false, clientCheckTime=0;
  async function checkClientNotice() {
    const menu=document.getElementById('overlay');
    if(!menu||!menu.getClientRects().length||document.visibilityState==='hidden'||!document.hasFocus()||clientCheckBusy||Date.now()-clientCheckTime<5*60*1000)return;
    clientCheckBusy=true;
    try { const reply=await rpc('update-notice');if(reply.ok&&reply.data){clientCheckTime=Date.now();clientNoticeState=reply.data;renderClientNotice();} }
    catch { /* Offline checks stay quiet; the manual check remains available. */ }
    finally { clientCheckBusy=false; }
  }
  function renderClientNotice() {
    const menu=document.getElementById('overlay');
    document.getElementById('qrazy-client-update-notice')?.remove();
    const state=clientNoticeState,clientAvailable=state&&['available','ready'].includes(state.phase),showClient=clientAvailable&&clientNoticeDismissed!==state.version,showGame=siteOutdated&&!siteDismissed;
    if(!menu||(!showClient&&!showGame))return;
    const box=document.createElement('section');box.id='qrazy-client-update-notice';box.setAttribute('role','status');
    box.style.cssText='position:fixed;top:50%;left:50%;transform:translate(-50%,-50%);width:min(560px,calc(100% - 64px));max-height:75vh;overflow:auto;z-index:2147483646;background:#0b1424;color:#edf2fa;border:2px solid #fbbf24;border-radius:12px;padding:24px;font:16px/1.5 system-ui;box-shadow:0 12px 60px #000c';
    const add=(tag,text)=>{const el=document.createElement(tag);el.textContent=text;box.append(el);return el;};
    add('h2','Updates available').style.cssText='color:#fbbf24;margin-top:0';
    const action=(label,fn)=>{const b=add('button',label);b.type='button';b.style.cssText='width:auto;margin:8px 12px 0 0;padding:10px 16px;font:16px system-ui;letter-spacing:normal';b.onclick=async event=>{event.preventDefault();event.stopPropagation();if(!event.isTrusted||b.disabled)return;b.disabled=true;try{await fn();}catch(error){add('p',error.message);}finally{b.disabled=false;}};return b;};
    if(showClient){
      add('h3','Client update · v'+state.version);
      add('p','Installed: v'+state.installedVersion+'. Installing closes Qrazy. Reopen it manually afterward; the game will then load the latest site.');
      if(state.notes)add('pre',state.notes).style.cssText='white-space:pre-wrap;overflow-wrap:anywhere;font:inherit';
      action(state.phase==='ready'?'Install client update':'Download client update',async()=>{
        if(state.phase==='ready'){const reply=await rpc('update-install');if(!reply.ok)throw Error(reply.error||'Installation could not start');return;}
        const reply=await rpc('update-check');if(!reply.ok)throw Error(reply.error||'Download failed');clientNoticeState=reply.data;emit(updateListeners,combinedState(reply.data));renderClientNotice();
      });
    }
    if(showGame){
      add('h3','Game update');add('p','Refreshing ends the current run and reconnects multiplayer. Until refreshed, some features may not work and runs may be rejected.');
      action(showClient?'Refresh game only':'Refresh game',()=>updates.refreshGame());
    }
    action('Later',()=>{if(showClient)clientNoticeDismissed=state.version;if(showGame)siteDismissed=true;renderSiteUpdate();});
    menu.append(box);
  }

  function desktopPanel() {
    if (!document.body || document.getElementById('qrazy-prototype-tools')) return;
    // Match the existing Electron menu insertion point; no game data is read.
    const menu=document.getElementById('overlay');if(!menu)return;
    if(!document.getElementById('client-quit')) {
      const quit=document.createElement('button');quit.id='client-quit';quit.type='button';quit.textContent='QUIT';
      quit.style.cssText='position:fixed;bottom:12px;left:16px;z-index:2147483645;background:#0b1424;color:#fbbf24;border:1px solid #fbbf24;padding:5px 12px;border-radius:5px;font:14px system-ui;width:auto;margin:0;letter-spacing:normal;cursor:pointer';
      quit.onclick=event=>{event.preventDefault();event.stopPropagation();if(event.isTrusted)rpc('quit').catch(()=>{});};
      states.add(value=>{quit.hidden=value.captured;});
      menu.append(quit);
    }
    const button=document.createElement('button');button.id='qrazy-prototype-tools';button.textContent='Client';button.style.cssText='position:fixed;bottom:12px;right:16px;z-index:2147483645;background:#0b1424;color:#fbbf24;border:1px solid #fbbf24;padding:5px 12px;border-radius:5px;font:14px system-ui';
    // Hide while captured; the game remains the authority for gameplay/menu state.
    states.add(value=>{button.hidden=value.captured;});
    button.onclick=async()=>{
      if(document.getElementById('qrazy-client-panel'))return;
      const panel=document.createElement('section');panel.id='qrazy-client-panel';panel.style.cssText='position:fixed;inset:10% 15%;overflow:auto;z-index:2147483647;background:#0b1424;color:#edf2fa;border:2px solid #fbbf24;border-radius:12px;padding:24px;font:16px/1.5 system-ui';
      const heading=document.createElement('h2');heading.textContent='Qrazy client';panel.append(heading);
      const versions=document.createElement('p');versions.textContent='Installed version: loading… · Next version: not checked';panel.append(versions);
      const report=document.createElement('pre');report.textContent='Check for updates to find and download a newer verified client, or open Changelog to read the notes.';report.style.cssText='white-space:pre-wrap;overflow-wrap:anywhere';panel.append(report);
      const actions=document.createElement('div');actions.style.cssText='display:flex;flex-wrap:wrap;gap:12px;margin-top:20px';panel.append(actions);
      let installButton=null,checkButton=null;
      const action=(label,fn,parent=actions)=>{const b=document.createElement('button');b.type='button';b.textContent=label;b.style.cssText='width:auto;margin:0;padding:10px 16px;font:15px system-ui;letter-spacing:normal';b.onclick=async event=>{if(!event.isTrusted||b.disabled)return;b.disabled=true;try{await fn();}catch(e){report.textContent=e.message;}finally{b.disabled=b===installButton?b.hidden:false;}};parent.append(b);return b;};
      let installedVersion='loading…';
      const showUpdate=state=>{installedVersion=state.installedVersion||installedVersion;versions.textContent=`Installed version: v${installedVersion} · Next version: ${state.version?'v'+state.version:state.phase==='current'?'none available':'not checked'}`;if(installButton){installButton.hidden=state.phase!=='ready';installButton.disabled=installButton.hidden;if(state.version)installButton.textContent=`Update to v${state.version}`;}if(checkButton)checkButton.hidden=state.phase==='ready';report.textContent=[state.phase?`Status: ${state.phase}`:'',(state.message||'').replace('Click Update to check and download.','Click Check for updates to check and download.'),state.notes||''].filter(Boolean).join('\n\n');};
      checkButton=action('Check for updates',async()=>{report.textContent='Checking regular releases and downloading a verified update, if available…';showUpdate((await rpc('update-check')).data);});
      installButton=action('Install downloaded update',async()=>{showUpdate((await rpc('update-install')).data);});installButton.disabled=true;installButton.hidden=true;
      action('Changelog',async()=>{report.textContent=(await rpc('changelog')).data;});
      action('Close panel',async()=>panel.remove());
      const diagnostics=document.createElement('details');diagnostics.style.cssText='margin-top:20px';
      const summary=document.createElement('summary');summary.textContent='Diagnostics';diagnostics.append(summary);
      const diagnosticActions=document.createElement('div');diagnosticActions.style.cssText='display:flex;flex-wrap:wrap;gap:12px;margin-top:12px';diagnostics.append(diagnosticActions);
      action('Graphics report',async()=>{report.textContent=JSON.stringify((await rpc('diagnostics')).data,null,2);},diagnosticActions);
      action('Copy report',async()=>{if(!report.textContent)return;await rpc('clipboard-write',{text:report.textContent});},diagnosticActions);
      panel.append(diagnostics);menu.append(panel);
      try{showUpdate(clientNoticeState&&['available','ready'].includes(clientNoticeState.phase)?clientNoticeState:(await rpc('update-state')).data);}catch(error){versions.textContent='Installed version unavailable · Next version: not checked';report.textContent=error.message;}
    };
    menu.append(button);
    const observer=new MutationObserver(checkClientNotice);observer.observe(menu,{attributes:true,attributeFilter:['style','class','hidden']});
    states.add(checkClientNotice);const timer=setInterval(checkClientNotice,30000);window.addEventListener('focus',checkClientNotice);
    window.addEventListener('pagehide',()=>{clearInterval(timer);observer.disconnect();},{once:true});checkClientNotice();
    const noticeObserver=new MutationObserver(()=>{checkClientNotice();});noticeObserver.observe(menu,{attributes:true,attributeFilter:['style','class','hidden']});
    states.add(()=>checkClientNotice());
    const noticeTimer=setInterval(checkClientNotice,30000);window.addEventListener('focus',checkClientNotice);
    window.addEventListener('pagehide',()=>{clearInterval(noticeTimer);noticeObserver.disconnect();},{once:true});
    checkClientNotice();
  }
  window.addEventListener('DOMContentLoaded',desktopPanel,{once:true});
  let next = 0, captured = false, generation = 0, sequence = 0, offset = 0, lastTime = -Infinity;
  function rpc(op, payload = null) {
    const id = ++next;
    return new Promise((resolve, reject) => {
      const timeout=op==='config-import'||op==='config-export'?310000:op==='update-notice'||op==='update-check'||op==='update-stage'||op==='update-install'||op==='update-rollback'?910000:30000;
      const timer = setTimeout(() => { pending.delete(id); reject(new Error('Native host reply timed out')); }, timeout);
      pending.set(id, value => { clearTimeout(timer); if (value?.ok === false && value.error) reject(new Error(value.error)); else resolve(value); });
      try { host(op, id, JSON.stringify(payload)); } catch (e) { clearTimeout(timer); pending.delete(id); reject(e); }
    });
  }
  function subscribe(set, fn) {
    if (typeof fn !== 'function') throw new TypeError('Expected callback');
    set.add(fn); return () => set.delete(fn);
  }
  function emit(set, value) { for (const fn of set) { try { fn(value); } catch (e) { console.error('Native bridge callback', e); } } }
  function release() {
    ++sequence; generation = 0;
    host('release', 0, 'null');
    if (captured) { captured = false; emit(states, { captured: false }); }
  }
  // Experimental frame pacing (opt-in, takes effect after a restart). vsync/maxFps exist only while it is active, so the
  // game's r_swapinterval / com_maxfps cvars stay hidden on a normal start.
  const vsyncListeners = new Set(), maxFpsListeners = new Set();
  const experimental = Object.freeze({
    version: 1,
    getState: async () => (await rpc('experimental-get')).data,
    setEnabled: async enabled => { if (typeof enabled !== 'boolean') throw new TypeError('Expected a boolean'); return (await rpc('experimental-set', { enabled })).data; }
  });
  const vsync = Object.freeze({
    getState: async () => (await rpc('vsync-get')).data,
    setEnabled: async enabled => { if (typeof enabled !== 'boolean') throw new TypeError('VSync must be a boolean'); return (await rpc('vsync-set', { enabled })).data; },
    onChange: fn => subscribe(vsyncListeners, fn)
  });
  const maxFps = Object.freeze({
    getState: async () => (await rpc('max-fps-get')).data,
    setValue: async value => { if (!Number.isSafeInteger(value) || value < 30 || value > 10000) throw new TypeError('com_maxfps must be a whole number from 30 to 10000'); return (await rpc('max-fps-set', { value })).data; },
    onChange: fn => subscribe(maxFpsListeners, fn)
  });
  const rawMouse = Object.freeze({
    async capture() {
      const token = ++sequence;
      let best = Infinity;
      for (let i = 0; i < 7; i++) {
        const before = performance.now(), clock = await rpc('clock'), after = performance.now();
        if (!clock.ok || token !== sequence) return false;
        if (after - before < best) { best = after - before; offset = (before + after) / 2 - clock.time; }
      }
      const result = await rpc('capture');
      if (token !== sequence) return false;
      if (!result.ok) { release(); return false; }
      generation = result.generation; captured = true; lastTime = -Infinity;
      emit(states, { captured: true }); return true;
    },
    release,
    onMove: fn => subscribe(moves, fn),
    onCaptureChange: fn => subscribe(states, fn)
  });
  Object.defineProperty(window, 'qrazyDesktop', { configurable: false, writable: false, value: Object.freeze({
    version: 1, prototype: true,
    rawMouseCapability: Object.freeze({ supported: null, backend: 'sdl3-wayland', reason: 'Compositor capture and hardware behaviour require manual verification; capture() reports the SDL request result.' }),
    rawMouse,
    updates,
    updates,
    information: Object.freeze(information),
    assets: Object.freeze(assets),
    status: Object.freeze({ version: 1, report: reportStatus }),
    graphics: Object.freeze({ getState: async () => (await rpc('diagnostics')).data }),
    config: Object.freeze({ version: 1, importFile: async () => (await rpc('config-import')).data, exportFile: async (name, text) => (await rpc('config-export', {name,text})).data }),
    quit: () => rpc('quit'),
    experimental,
    ...(host.pacing === true ? { vsync, maxFps } : {}),
    fullscreen: Object.freeze({ getState: async () => (await rpc('fullscreen-state')).fullscreen, toggle: async () => (await rpc('fullscreen-toggle')).fullscreen, onChange: fn => subscribe(fullscreen, fn) })
  }) });
  window.addEventListener('pagehide', release);
  // Element blur bubbles through capture listeners; it is not window focus loss.
  window.addEventListener('blur', event => { if(event.target===window)release(); });
  return function dispatch(json) {
    const message = JSON.parse(json);
    if (message.type === 'reply') { const fn = pending.get(message.id); pending.delete(message.id); fn?.(message.value); }
    else if (message.type === 'state') {
      // release() already invalidated and notified synchronously. Its host
      // acknowledgement must not cancel a newer console/menu capture request.
      if(message.reason==='release')return;
      // Native release can precede Chromium's asynchronous DOM focus change.
      if (message.reason === 'focus') window.dispatchEvent(new Event('blur'));
      ++sequence; generation = 0;
      if (captured) { captured = false; emit(states, { captured: false }); }
    } else if (message.type === 'focus') {
      window.dispatchEvent(new Event(message.focused ? 'focus' : 'blur'));
    } else if (message.type === 'fullscreen') emit(fullscreen, message.value);
    else if (message.type === 'render-settings') { emit(vsyncListeners, message.vsync); emit(maxFpsListeners, message.maxFps); }
    else if (message.type === 'button') {
      // CEF's native click API covers only three buttons. The existing game accepts
      // document mouse events for side buttons; these events are deliberately synthetic.
      document.dispatchEvent(new MouseEvent(message.down ? 'mousedown' : 'mouseup', { button: message.button, buttons: message.buttons, bubbles: true }));
    } else if (message.type === 'inspect') {
      // Background read-only renderer evidence. No input events or game actions.
      const canvas = document.querySelector('canvas');
      const gl = canvas?.getContext('webgl2') || canvas?.getContext('webgl');
      const extension = gl?.getExtension('WEBGL_debug_renderer_info');
      console.info('PROTOTYPE WebGL renderer=' + (extension ? gl.getParameter(extension.UNMASKED_RENDERER_WEBGL) : gl ? gl.getParameter(gl.RENDERER) : 'unavailable'));
    }
    else if (message.type === 'move' && captured) {
      const valid = message.samples.filter(x => x.generation === generation && Number.isFinite(x.time) && Number.isFinite(x.dx) && Number.isFinite(x.dy));
      if (!valid.length) return;
      offset = Math.min(offset, performance.now() - Math.max(...valid.map(x => x.time)));
      const samples = valid.map(x => ({ movementX: x.dx, movementY: x.dy, timeStamp: (lastTime = Math.max(lastTime, x.time + offset)) }));
      emit(moves, samples);
    }
  };
})
