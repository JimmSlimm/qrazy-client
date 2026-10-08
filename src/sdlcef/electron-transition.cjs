'use strict';
const fs=require('node:fs'),path=require('node:path'),{spawn}=require('node:child_process');
const {TransitionInstall}=require('./transition-install.cjs');
const distribution=require('./distribution.cjs');
const {fetch}=require('./production-runtime.cjs');
const {send,pin,key}=require('./electron-broker.cjs');
async function continueTransition({app,dialog,window,session,directory,root,drain,show}) {
  let stage='delivery';
  try {
  const resource=path.join(process.resourcesPath,'qrazy-transition.json');
  if(!fs.existsSync(resource))return false;
  // Only the signed intermediate delivery contains this resource. Ordinary
  // launches do not interpret generic website data as transition instructions.
  const deliveryFile=path.join(directory,'installed-delivery.json');
  if(fs.lstatSync(deliveryFile).isSymbolicLink()||fs.statSync(deliveryFile).size>4*1024**2)throw Error('The signed intermediate update record is missing.');
  const delivery=JSON.parse(fs.readFileSync(deliveryFile,'utf8'));
  if(delivery.schema!=='qrazy-electron-installed-delivery-v1'||path.resolve(delivery.root)!==path.resolve(root))throw Error('The installed delivery does not match this folder.');
  const signed=fs.readFileSync(resource),selected=distribution.verify(signed,key());
  const engine=new TransitionInstall(root,{key:key()});
  show('Preparing the SDL/CEF update…');
  stage='staging';
  if(!fs.existsSync(engine.work)) {
    const archive=path.join(directory,'sdlcef-distribution.zip');
    if(fs.existsSync(archive))throw Error('A previous transition download needs recovery before retrying.');
    const url='https://github.com/JimmSlimm/qrazy-client/releases/download/sdlcef-v'+selected.manifest.version+'/'+selected.manifest.asset.name;
    await fetch(url,selected.manifest.asset.size,archive,selected.manifest.asset);
    try{await engine.stage(archive,signed,Buffer.from(JSON.stringify(delivery.envelope)));}finally{fs.unlinkSync(archive);}
  }
  if(engine.record().fresh.identity!==selected.identity)throw Error('The pending transition selected a different signed release.');
  stage='authentication-copy';
  const auth=engine.prepareAuth(),native=require(path.join(process.resourcesPath,'qrazy-handoff.node'));
  let child;
  try {
    stage='asset-copy';
    const profile=path.join(root,'profile-sdlcef-windows');fs.mkdirSync(profile,{recursive:true});
    for(const name of ['host.lock','desktop-worker.lock'])if(!native.lock(path.join(profile,name)))throw Error('Close the other Qrazy client before transferring saved downloads.');
    let downloads;
    try{downloads=await require('./asset-transition.cjs').migrate(path.join(app.getPath('userData'),'assets-v1'),path.join(profile,'desktop-assets'),{
      commit:async(from,to)=>{if(!native.assetMove(from,to))throw Error('Saved download activation failed. Electron originals are kept.');}
    });}
    finally{native.unlock();}
    stage='broker-start';
    pin(native,engine.auth,engine.record().old.files);
    const pipe=native.start(auth.executable);if(typeof pipe!=='string')throw Error('Authentication broker could not start.');
    // Hidden authentication process only; it never creates a BrowserWindow.
    // No cookie, token or handoff code is placed in arguments or environment.
    child=spawn(auth.executable,[`--qrazy-auth-copy=${root}`,`--qrazy-auth-source-pid=${process.pid}`,`--qrazy-auth-pipe=${pipe}`],{
      // Windows otherwise puts the spawned broker in libuv's parent-lifetime
      // job. It must survive the source's orderly exit to install and hand off.
      cwd:path.dirname(auth.executable),stdio:'ignore',windowsHide:true,detached:true,
      env:{...process.env,NODE_OPTIONS:'',NODE_PATH:'',ELECTRON_RUN_AS_NODE:''}
    });
    let spawnError=false;child.on('error',()=>{spawnError=true;});
    stage='first-handoff';
    // Installation transfers consent and process ownership only. The user has
    // accepted signing in again; never issue or persist a session handoff code.
    await send(native,session,{anonymous:true});if(spawnError)throw Error('Authentication process did not start.');
    const skipped=downloads.conflicts+downloads.invalid+downloads.skippedForSpace;
    show(skipped?'The update is ready. Some saved downloads could not be copied; their Electron originals are kept. Confirm installation in Qrazy.':'The update is ready. Saved downloads are available for SDL/CEF. Confirm installation in Qrazy.');
    if(!window.isFocused()||window.isMinimized())await new Promise(resolve=>window.once('focus',resolve));
    const detail=skipped?'Your profiles will be kept. Some saved downloads could not be copied; their Electron originals remain intact. Those assets may need downloading again in SDL/CEF. Open Qrazy manually after installation and sign in again.':'Your profiles and completed saved downloads will be kept. Open Qrazy manually after installation and sign in again.';
    stage='confirmation';
    if(!await require('./electron-close.cjs').confirmClose({dialog,window,session,drain,detail}))return false;
    stage='authorization';
    if(!native.authorize())throw Error('The authentication process timed out; no installation was authorized.');
    // Normal shutdown releases the source profile and launcher lock. The
    // authentication copy waits on retained native process handles before move.
    app.quit();return true;
  }finally{native.stop();child?.unref();}
  }catch(error){error.transitionStage=stage;throw error;}
}
module.exports={continueTransition};
