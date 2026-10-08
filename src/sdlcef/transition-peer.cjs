'use strict';
// Nonsecret rendezvous data is only a lookup hint. Authenticate both complete
// distributions on each manual start; the native caller then retains file and
// process handles and validates the connected peer before receiving a ticket.
const fs=require('node:fs'),path=require('node:path');
const {TransitionInstall}=require('./transition-install.cjs');
function peer(root,key,options={}) {
  const engine=new TransitionInstall(root,{...options,key}),record=engine.record();
  engine.verifyInstalled(record);engine.verifyAuth(record);
  const file=path.join(engine.work,'peer.json');
  if(fs.lstatSync(file).isSymbolicLink()||!fs.lstatSync(file).isFile()||fs.statSync(file).size>1024)throw Error('Invalid transition peer');
  const p=JSON.parse(fs.readFileSync(file,'utf8'));
  if(!p||Object.keys(p).sort().join(',')!=='identity,pid,pipe'||p.identity!==record.fresh.identity||
    !Number.isSafeInteger(p.pid)||p.pid<1||p.pid>0xffffffff||
    typeof p.pipe!=='string'||!/^\\\\\.\\pipe\\Qrazy-handoff-[a-f0-9]{32}$/.test(p.pipe))throw Error('Invalid transition peer');
  // App and broker must belong to the signed intermediate delivery.
  if(!record.old.files.some(f=>f.path==='runtime/resources/qrazy-handoff.node'))throw Error('Intermediate broker is missing');
  return {...p,files:record.old.files};
}
module.exports={peer};
function recovery(root,key,options={}) {
  const engine=new TransitionInstall(root,{...options,key}),record=engine.record();engine.verifyAuth(record);
  // The native launcher must validate the live image/user using retained
  // handles. A PID hint by itself cannot defer recovery after PID reuse.
  let pid=0;
  const file=path.join(engine.work,'peer.json');
  if(fs.existsSync(file)) {
    const stat=fs.lstatSync(file);if(stat.isSymbolicLink()||!stat.isFile()||stat.size>1024)throw Error('Invalid recovery peer');
    const p=JSON.parse(fs.readFileSync(file,'utf8'));
    if(p.identity===record.fresh.identity&&Number.isSafeInteger(p.pid)&&p.pid>0&&p.pid<=0x7fffffff) {
      pid=p.pid;
    }
  }
  return {files:record.old.files,pid};
}
module.exports.recovery=recovery;
