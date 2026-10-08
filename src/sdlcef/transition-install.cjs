'use strict';
// Full-distribution transaction engine. The native broker must own installation
// and profile exclusion, retain the authenticated browser process handles, and
// prove orderly shutdown before invoking replace(). No process starts here.
let fs;
try { fs=require('original-fs'); }
catch(error) { if(error.code!=='MODULE_NOT_FOUND')throw error;fs=require('node:fs'); }
const path=require('node:path'),crypto=require('node:crypto');
const distribution=require('./distribution.cjs');
const release=require('./production-release.cjs');
const {extract}=require('./production-runtime.cjs');
const legacy=require('./legacy-release.cjs');
const legacyConfig=require('./legacy-config.cjs');
const GROUPS=['Qrazy.exe','runtime','updater'];
const SCHEMA='qrazy-electron-distribution-transaction-v1';
const fail=()=>Error('Transition verification failed. Preserve the installation and retry recovery.');
function noLinks(file) {
  for(let p=path.resolve(file);;p=path.dirname(p)) {
    try {if(fs.lstatSync(p).isSymbolicLink())throw fail();}catch(e){if(e.code!=='ENOENT')throw e;}
    if(path.dirname(p)===p)break;
  }
}
function read(file,limit=3*1024**2) {
  noLinks(file);const s=fs.lstatSync(file);
  if(!s.isFile()||s.size<1||s.size>limit)throw fail();return fs.readFileSync(file);
}
function atomic(file,value) {
  noLinks(file);noLinks(file+'.partial');
  if(fs.existsSync(file+'.partial')) {
    if(!fs.lstatSync(file+'.partial').isFile())throw fail();fs.unlinkSync(file+'.partial');
  }
  const fd=fs.openSync(file+'.partial','wx',0o600);
  try {fs.writeFileSync(fd,JSON.stringify(value));fs.fsyncSync(fd);}finally{fs.closeSync(fd);}
  fs.renameSync(file+'.partial',file);
}
function envelope(raw,config) {
  if(!Buffer.isBuffer(raw)||raw.length>3*1024**2)throw fail();
  const files=legacy.verifyManifest(JSON.parse(raw),config,'win32-x64');
  // Only immutable application paths belong to recovery. Profiles and unrelated
  // developer artifacts are never selected, traversed or copied.
  if(files.files.some(f=>f.path!=='Qrazy.exe'&&f.path!=='LICENSE'&&!f.path.startsWith('runtime/')))throw fail();
  return files;
}
function groupFiles(root,group) {
  const target=path.join(root,group);noLinks(target);
  if(!fs.existsSync(target))return [];
  if(group==='Qrazy.exe'||group==='LICENSE') {
    const s=fs.lstatSync(target);if(!s.isFile())throw fail();
    const h=crypto.createHash('sha256'),fd=fs.openSync(target,'r'),b=Buffer.alloc(1024**2);
    try {let n;while((n=fs.readSync(fd,b,0,b.length,null)))h.update(b.subarray(0,n));}finally{fs.closeSync(fd);}
    return [{path:group,size:s.size,sha256:h.digest('hex')}];
  }
  return release.inventory(target).map(f=>({...f,path:group+'/'+f.path}));
}
function matches(root,files,group) {
  try {
    const expected=files.filter(f=>f.path===group||f.path.startsWith(group+'/'));
    const actual=groupFiles(root,group),map=new Map(expected.map(f=>[f.path,f]));
    return actual.length===expected.length&&actual.every(f=>{const e=map.get(f.path);return e&&e.size===f.size&&e.sha256===f.sha256;});
  }catch{return false;}
}
class TransitionInstall {
  constructor(root,{key,legacyTrust=legacyConfig,swapLauncher=(from,to)=>fs.renameSync(from,to)}={}) {
    if(!key)throw fail();this.root=path.resolve(root);noLinks(this.root);
    if(!fs.lstatSync(this.root).isDirectory()||this.root===path.parse(this.root).root)throw fail();
    this.key=key;this.legacyTrust=legacyTrust;this.swapLauncher=swapLauncher;
    this.work=path.join(this.root,'.qrazy-transition');this.next=path.join(this.work,'next');
    this.previous=path.join(this.work,'previous');this.journal=path.join(this.work,'transaction.json');
    this.auth=path.join(this.work,'auth');
  }
  record() {
    const r=JSON.parse(read(this.journal,8*1024**2));
    if(!r||Object.keys(r).sort().join(',')!=='distribution,legacy,schema'||r.schema!==SCHEMA||
       typeof r.legacy!=='string'||typeof r.distribution!=='string')throw fail();
    const decode=value=>{
      if(value.length>4*1024**2||!/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(value))throw fail();
      return Buffer.from(value,'base64');
    };
    const old=envelope(decode(r.legacy),this.legacyTrust),fresh=distribution.verify(decode(r.distribution),this.key);
    return {old,fresh,bytes:decode(r.distribution)};
  }
  async stage(archive,signedDistribution,signedElectron) {
    // Download authentication comes from the signed distribution descriptor,
    // not from a transport checksum or mutable pending-progress file.
    noLinks(archive);const fresh=distribution.verify(signedDistribution,this.key);
    const old=envelope(signedElectron,this.legacyTrust);
    if(!GROUPS.every(g=>matches(this.root,old.files,g))||old.files.some(f=>f.path==='LICENSE')&&!matches(this.root,old.files,'LICENSE'))throw fail();
    if(fs.existsSync(this.work))throw Error('Recover the existing transition before staging another package');
    fs.mkdirSync(this.work, {mode:0o700});
    try {
      const descriptor=fresh.manifest.asset;
      if(!descriptor)throw fail();
      const fd=fs.openSync(archive,'r'),h=crypto.createHash('sha256'),buffer=Buffer.alloc(1024**2);let count=0;
      try {let n;while((n=fs.readSync(fd,buffer,0,buffer.length,null))){count+=n;if(count>descriptor.size)throw fail();h.update(buffer.subarray(0,n));}}finally{fs.closeSync(fd);}
      if(count!==descriptor.size||h.digest('hex')!==descriptor.sha256)throw fail();
      await extract(archive,this.next,fresh.manifest);
      distribution.verifyTree(this.next,signedDistribution,this.key);
      atomic(this.journal,{schema:SCHEMA,legacy:signedElectron.toString('base64'),distribution:signedDistribution.toString('base64')});
      // Keep the caller's download untouched; caller may delete its own verified
      // temporary download after this durable staging result.
      return {phase:'ready',identity:fresh.identity};
    }catch(e){this.remove(this.work);throw e;}
  }
  verifyInstalled(record=this.record()) {
    if(!GROUPS.every(g=>matches(this.root,record.fresh.manifest.files,g)))throw fail();
    // Cross-check inner runtime signature and fixed trust material as well as
    // complete distribution hashes, without inventorying either profile.
    const runtimeBytes=read(path.join(this.root,'updater','current.json'));
    const runtime=release.verify(runtimeBytes,this.key,{platform:'windows-x64',channel:'stable'});
    if(runtime.identity!==record.fresh.manifest.runtimeIdentity||
      runtime.manifest.version!==record.fresh.manifest.version||
      runtime.manifest.sequence!==record.fresh.manifest.sequence)throw fail();
    release.verifyTree(path.join(this.root,'runtime'),runtime.manifest,{platform:'windows-x64',channel:'stable'});
    const actual=crypto.createPublicKey(read(path.join(this.root,'updater','release-public.pem'),65536)).export({type:'spki',format:'der'});
    if(!actual.equals(crypto.createPublicKey(this.key).export({type:'spki',format:'der'})))throw fail();
    return {phase:'awaiting-session',identity:record.fresh.identity};
  }
  verifyAuth(record=this.record()) {
    // A running Windows image cannot be renamed out of its runtime directory.
    // Start the intermediate authentication process from this separate verified
    // APPLICATION copy before replacing the original. Both browser profiles
    // remain in their existing locations and are never copied into this tree.
    noLinks(this.auth);
    const actual=release.inventory(this.auth),expected=new Map(record.old.files.map(f=>[f.path,f]));
    if(actual.length!==expected.size||actual.some(f=>{
      const e=expected.get(f.path);return !e||e.size!==f.size||e.sha256!==f.sha256;
    }))throw fail();
    return {phase:'authentication-copy-ready',identity:record.fresh.identity,
      executable:path.join(this.auth,'runtime','electron.exe')};
  }
  prepareAuth() {
    const record=this.record();
    if(!GROUPS.every(g=>matches(this.root,record.old.files,g)))throw fail();
    noLinks(this.auth);
    const existing=fs.existsSync(this.auth)?release.inventory(this.auth):[];
    const expected=new Map(record.old.files.map(f=>[f.path,f]));
    for(const file of existing) {
      const signed=expected.get(file.path);if(!signed||signed.size!==file.size||signed.sha256!==file.sha256)throw fail();
    }
    const names=new Set(existing.map(f=>f.path));
    const total=record.old.files.filter(f=>!names.has(f.path)).reduce((s,f)=>s+f.size,0),disk=fs.statfsSync(this.root);
    if(disk.bavail*disk.bsize-total<1024**3)throw Error('Authentication recovery copy needs 1 GiB free reserve');
    if(!fs.existsSync(this.auth))fs.mkdirSync(this.auth,{mode:0o700});
    const temporary=path.join(this.work,'auth-copy.partial');noLinks(temporary);
    if(fs.existsSync(temporary)){if(!fs.lstatSync(temporary).isFile())throw fail();fs.unlinkSync(temporary);}
    for(const file of record.old.files) {
      if(names.has(file.path))continue;
      const source=path.join(this.root,...file.path.split('/')),destination=path.join(this.auth,...file.path.split('/'));
      noLinks(source);noLinks(destination);
      if(!fs.lstatSync(source).isFile())throw fail();
      fs.mkdirSync(path.dirname(destination),{recursive:true});fs.copyFileSync(source,temporary,fs.constants.COPYFILE_EXCL);
      const fd=fs.openSync(temporary,'r+');try{fs.fsyncSync(fd);}finally{fs.closeSync(fd);}
      fs.renameSync(temporary,destination);
    }
    return this.verifyAuth(record);
  }
  copyLauncher(source,destination,signed) {
    noLinks(source);noLinks(destination);const temporary=destination+'.partial';noLinks(temporary);
    if(fs.existsSync(destination)&&!fs.lstatSync(destination).isFile())throw fail();
    if(fs.existsSync(temporary)){if(!fs.lstatSync(temporary).isFile())throw fail();fs.unlinkSync(temporary);}
    fs.copyFileSync(source,temporary,fs.constants.COPYFILE_EXCL);
    const fd=fs.openSync(temporary,'r+');try{fs.fsyncSync(fd);}finally{fs.closeSync(fd);}
    const bytes=read(temporary,2*1024**3);
    if(bytes.length!==signed.size||crypto.createHash('sha256').update(bytes).digest('hex')!==signed.sha256)throw fail();
    fs.renameSync(temporary,destination);
  }
  replace(identity) {
    // identity is the selection confirmed by native UI, never website input.
    const record=this.record();if(identity!==record.fresh.identity)throw fail();
    this.verifyAuth(record);
    noLinks(this.previous);noLinks(this.next);
    if(!fs.existsSync(this.previous))fs.mkdirSync(this.previous,{mode:0o700});
    // Preflight EVERY group before changing anything. An interrupted transaction
    // is inferred from signed content, not a trusted phase boolean in a file.
    const plans=GROUPS.map(group=>{
      const old=matches(this.root,record.old.files,group),fresh=matches(this.root,record.fresh.manifest.files,group);
      const retained=matches(this.previous,record.old.files,group),staged=matches(this.next,record.fresh.manifest.files,group);
      const exists=fs.existsSync(path.join(this.root,group)),hasOld=record.old.files.some(f=>f.path===group||f.path.startsWith(group+'/'));
      if(fresh&&retained)return {group,done:true};
      if(!staged||(!old&&!(!exists&&retained))||(hasOld&&old&&fs.existsSync(path.join(this.previous,group))&&group!=='Qrazy.exe'))throw fail();
      return {group,done:false,moveOld:hasOld&&old,atomicLauncher:group==='Qrazy.exe'&&old};
    });
    try {
      for(const plan of plans) {
        if(plan.done)continue;
        if(plan.atomicLauncher) {
          const backup=path.join(this.previous,plan.group);
          if(!matches(this.previous,record.old.files,plan.group))this.copyLauncher(path.join(this.root,plan.group),backup,record.old.files.find(f=>f.path===plan.group));
          if(!matches(this.previous,record.old.files,plan.group))throw fail();
          // Never move the only root entrypoint out of the folder. Retain a
          // verified copy first, then atomically replace the existing launcher.
          this.swapLauncher(path.join(this.next,plan.group),path.join(this.root,plan.group));continue;
        }
        if(plan.moveOld)fs.renameSync(path.join(this.root,plan.group),path.join(this.previous,plan.group));
        fs.renameSync(path.join(this.next,plan.group),path.join(this.root,plan.group));
      }
      return this.verifyInstalled(record);
    }catch {
      // Ordinary move failures should not leave the root launcher missing.
      // If recovery itself fails, keep every remaining transaction artifact.
      try {this.restore();}catch{}
      throw fail();
    }
  }
  restore() {
    // Recovery is available until native redemption + cookie-flush success.
    const record=this.record();noLinks(this.next);noLinks(this.previous);
    const plans=GROUPS.map(group=>{
      if(matches(this.root,record.old.files,group))return {group,done:true};
      const retained=matches(this.previous,record.old.files,group),fresh=matches(this.root,record.fresh.manifest.files,group);
      const hasNext=fs.existsSync(path.join(this.next,group));
      if(!retained||(!fresh&&fs.existsSync(path.join(this.root,group)))||
        hasNext&&((fresh&&group!=='Qrazy.exe')||!matches(this.next,record.fresh.manifest.files,group)))throw fail();
      return {group,done:false,fresh,hasOld:record.old.files.some(f=>f.path===group||f.path.startsWith(group+'/'))};
    });
    if(!fs.existsSync(this.next))fs.mkdirSync(this.next,{mode:0o700});
    for(const p of plans) {
      if(p.done)continue;
      if(p.group==='Qrazy.exe'&&p.fresh) {
        const next=path.join(this.next,p.group);
        if(!fs.existsSync(next))this.copyLauncher(path.join(this.root,p.group),next,record.fresh.manifest.files.find(f=>f.path===p.group));
        if(!matches(this.next,record.fresh.manifest.files,p.group))throw fail();
        this.swapLauncher(path.join(this.previous,p.group),path.join(this.root,p.group));continue;
      }
      if(p.fresh)fs.renameSync(path.join(this.root,p.group),path.join(this.next,p.group));
      if(p.hasOld)fs.renameSync(path.join(this.previous,p.group),path.join(this.root,p.group));
    }
    if(!GROUPS.every(g=>matches(this.root,record.old.files,g)))throw fail();
    return {phase:'electron-recovered',identity:record.fresh.identity};
  }
  finish(identity) {
    // Native caller ONLY, immediately after authenticated peer acknowledgement
    // of redemption, exact-context cookie verification AND successful flush.
    // A persisted progress flag must never call this or authorize deletion.
    const installed=this.verifyInstalled();if(installed.identity!==identity)throw fail();
    this.remove(this.work);return {phase:'complete',identity};
  }
  remove(file) {
    if(path.resolve(file)!==this.work)throw fail();noLinks(file);
    // Recursively verify links before removal. Known workspace only; never a
    // recovery artifact outside this transaction or a profile path.
    if(fs.existsSync(file)) {release.inventory(file);fs.rmSync(file,{recursive:true});}
  }
}
module.exports={TransitionInstall};
