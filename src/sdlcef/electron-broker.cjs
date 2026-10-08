'use strict';
const fs=require('node:fs'),path=require('node:path');
const {performance}=require('node:perf_hooks');
const {TransitionInstall}=require('./transition-install.cjs');
const {issue,NoSession}=require('./session-handoff.cjs');
const delay=()=>new Promise(resolve=>setTimeout(resolve,20));
const fail=()=>Error('The transition could not finish. Electron recovery and both profiles are retained.');
async function wait(predicate,timeout=15000) {
  const deadline=performance.now()+timeout;
  while(!predicate()){if(performance.now()>=deadline)throw fail();await delay();}
}
async function send(native,session,{anonymous=false}={}) {
  await wait(()=>{if(native.poll()===4)throw fail();return native.poll()===2;},300000);
  let ticket=null,bytes=null;
  try {
    if(!anonymous)try{ticket=await issue(session);}catch(e){if(!(e instanceof NoSession))throw e;}
    bytes=ticket?ticket.take():Buffer.alloc(0);
    if(!native.submit(bytes))throw fail();
    await wait(()=>{if(native.poll()===4)throw fail();return native.poll()===3;},20000);
  }finally{bytes?.fill(0);ticket?.dispose();}
}
function pin(native,base,files) {
  for(const file of files)if(!native.pin(path.join(base,...file.path.split('/')),file.sha256))throw fail();
}
function key(){return fs.readFileSync(path.join(__dirname,'release-public.pem'));}
async function runAuthenticationCopy({app,session,args,resourcesPath=process.resourcesPath}) {
  const option=name=>args.find(v=>v.startsWith(name+'='))?.slice(name.length+1);
  const supplied=option('--qrazy-auth-copy'),pipe=option('--qrazy-auth-pipe'),pid=Number(option('--qrazy-auth-source-pid'));
  if(!supplied||!path.isAbsolute(supplied)||!Number.isSafeInteger(pid)||pid<1||pid>0x7fffffff||
    !/^\\\\\.\\pipe\\Qrazy-handoff-[a-f0-9]{32}$/.test(pipe||''))throw fail();
  const root=path.resolve(supplied),engine=new TransitionInstall(root,{key:key()}),record=engine.record();
  const auth=engine.verifyAuth(record);
  if(path.resolve(process.execPath).toLowerCase()!==auth.executable.toLowerCase()||record.old.version!==app.getVersion())throw fail();
  const native=require(path.join(resourcesPath,'qrazy-handoff.node'));
  engine.swapLauncher=(from,to)=>{if(!native.swapLauncher(from,to))throw fail();};
  if(!native.receiver())throw fail();pin(native,root,record.old.files);
  if(!native.connect(pipe,pid,path.join(root,'runtime','electron.exe')))throw fail();
  // This partition is never persisted, even when the source chose Remember me.
  // The final CEF cookie will preserve that choice and the original expiry.
  const memory=session.fromPartition('qrazy-transition-auth');
  memory.setPermissionRequestHandler((_contents,_permission,callback)=>callback(false));
  const deadline=performance.now()+300000;
  try {
    await wait(()=>{if(native.poll()===4)throw fail();return native.poll()===2;});
    const code=native.take();if(!Buffer.isBuffer(code))throw fail();const anonymous=code.length===0;
    if(!anonymous)await require('./electron-redeem.cjs').redeem(memory,code);
    if(!native.acknowledge())throw fail();
    await wait(()=>{if(native.poll()===4)throw fail();return native.poll()===3;});
    await wait(()=>{if(native.poll()===4)throw fail();return native.sourceExited();},Math.max(1,deadline-performance.now()));
    if(!native.sender()||!native.lock(path.join(root,'install.lock')))throw fail();
    // Parent and its launcher have exited; native pins of the source are now
    // released. Do not terminate any client to make replacement possible.
    const profile=path.join(root,'profile-sdlcef-windows');fs.mkdirSync(profile,{recursive:true});
    for(const name of ['host.lock','desktop-worker.lock'])if(!native.lock(path.join(profile,name)))throw fail();
    removePeer(engine);const installed=engine.replace(record.fresh.identity);
    pin(native,root,record.fresh.manifest.files);
    const endpoint=native.start(path.join(root,'runtime','Qrazy.exe'));if(typeof endpoint!=='string')throw fail();
    const peerFile=path.join(engine.work,'peer.json'),temporary=peerFile+'.partial';
    if(fs.existsSync(peerFile)||fs.existsSync(temporary))throw fail();
    const fd=fs.openSync(temporary,'wx',0o600);
    try{fs.writeFileSync(fd,JSON.stringify({identity:installed.identity,pid:process.pid,pipe:endpoint}));fs.fsyncSync(fd);}finally{fs.closeSync(fd);}
    fs.renameSync(temporary,peerFile);native.unlock();
    await send(native,memory,{anonymous});
    // CEF's authenticated acknowledgement proves redeem + exact-context cookie
    // check + flush. Its native caller waits for this process to exit and then
    // removes recovery. A file flag never authorizes that removal.
  }finally{native.stop();native.unlock();}
}
async function runRecovery({app,args,resourcesPath=process.resourcesPath}) {
  const option=name=>args.find(v=>v.startsWith(name+'='))?.slice(name.length+1);
  const root=path.resolve(option('--qrazy-auth-recover')||''),pid=Number(option('--qrazy-recovery-parent'));
  if(pid!==process.ppid||!Number.isSafeInteger(pid)||pid<1)throw fail();
  const engine=new TransitionInstall(root,{key:key()}),record=engine.record(),auth=engine.verifyAuth(record);
  if(path.resolve(process.execPath).toLowerCase()!==auth.executable.toLowerCase()||record.old.version!==app.getVersion())throw fail();
  const native=require(path.join(resourcesPath,'qrazy-handoff.node'));
  engine.swapLauncher=(from,to)=>{if(!native.swapLauncher(from,to))throw fail();};
  const launcher=record.fresh.manifest.files.find(f=>f.path==='Qrazy.exe');
  try {
    if(!launcher||!native.pin(path.join(root,'Qrazy.exe'),launcher.sha256)||!native.track(pid,path.join(root,'Qrazy.exe')))throw fail();
    await wait(()=>native.trackedExited(),300000);
    if(!native.lock(path.join(root,'install.lock')))throw fail();
    const profile=path.join(root,'profile-sdlcef-windows');fs.mkdirSync(profile,{recursive:true});
    for(const name of ['host.lock','desktop-worker.lock'])if(!native.lock(path.join(profile,name)))throw fail();
    engine.restore();removePeer(engine);
  }finally{native.stop();native.unlock();}
}
function removePeer(engine) {
  const file=path.join(engine.work,'peer.json');
  if(fs.existsSync(file)){const stat=fs.lstatSync(file);if(!stat.isFile()||stat.isSymbolicLink()||stat.size>1024)throw fail();fs.unlinkSync(file);}
}
module.exports={runAuthenticationCopy,runRecovery,send,pin,key,wait,removePeer};
