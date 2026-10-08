// Shipping helper: standard-library-only transport, ZIP staging and single-copy
// installation. No signing private key, key rotation, automatic check or launch.
'use strict';
const fs=require('node:fs'), path=require('node:path'), crypto=require('node:crypto');
const https=require('node:https'), zlib=require('node:zlib');
const {pipeline}=require('node:stream/promises');
const {Readable,Transform}=require('node:stream');
const release=require('./production-release.cjs');
const LIMIT=4*1024**3, RESERVE=1024**3;
function hash(file) {
  const fd=fs.openSync(file,'r'),h=crypto.createHash('sha256'),b=Buffer.alloc(1048576);
  try{let n;while((n=fs.readSync(fd,b,0,b.length,null)))h.update(b.subarray(0,n));return h.digest('hex');}finally{fs.closeSync(fd);}
}
function bounded(file,limit=3*1024**2) {if(fs.statSync(file).size>limit)throw Error('Metadata limit exceeded');return fs.readFileSync(file);}
function noLinks(file) {
  for(let p=path.resolve(file);;p=path.dirname(p)) {
    if(fs.existsSync(p)&&fs.lstatSync(p).isSymbolicLink())throw Error('Linked runtime path refused');
    if(path.dirname(p)===p)break;
  }
}
function atomic(file,bytes) {
  const temp=file+'.partial';noLinks(file);noLinks(temp);
  // A crash before the atomic rename can leave only this known temporary file.
  // The authenticated record being replaced stays authoritative until rename.
  if(fs.existsSync(temp)){if(!fs.lstatSync(temp).isFile())throw Error('Unsafe metadata temporary');fs.unlinkSync(temp);}
  const fd=fs.openSync(temp,'wx');
  try{fs.writeFileSync(fd,bytes);fs.fsyncSync(fd);}finally{fs.closeSync(fd);}
  fs.renameSync(temp,file);
}
function urlCheck(value,kind='asset') {
  const u=new URL(value);
  if(u.protocol!=='https:'||u.username||u.password||u.port||u.hash)throw Error('Invalid update HTTPS URL');
  if(kind==='api') {
    if(u.hostname!=='api.github.com'||u.pathname!=='/repos/JimmSlimm/qrazy-client/releases'||!/^\?per_page=100&page=[1-9]\d?$/.test(u.search))throw Error('Wrong repository discovery URL');
  }else if(kind==='asset') {
    if(u.hostname!=='github.com'||u.search||!/^\/JimmSlimm\/qrazy-client\/releases\/download\/[A-Za-z0-9_.+-]+\/Qrazy-SDLCEF-[A-Za-z0-9_.+-]+\.(json|zip)$/.test(u.pathname))throw Error('Wrong repository release asset');
  }else if(u.hostname!=='release-assets.githubusercontent.com')throw Error('Wrong release storage host');
  return u;
}
async function fetch(value,maximum,destination,descriptor,kind='asset',redirects=0,deadline=Date.now()+600000) {
  urlCheck(value,kind);
  if(!Number.isSafeInteger(maximum)||maximum<1||maximum>LIMIT)throw Error('Invalid transport bound');
  if(destination&&(!descriptor||!Number.isSafeInteger(descriptor.size)||descriptor.size<1||descriptor.size>maximum||!/^[a-f0-9]{64}$/.test(descriptor.sha256)))throw Error('Authenticated payload descriptor required');
  const response=await new Promise((resolve,reject)=>{
    const req=https.get(value,{headers:{'User-Agent':'Qrazy-SDLCEF-Updater','Accept-Encoding':'identity','Accept':'application/vnd.github+json'},timeout:15000},resolve);
    req.on('timeout',()=>req.destroy(Error('Update connection timeout')));req.on('error',reject);
  });
  if([301,302,303,307,308].includes(response.statusCode)) {
    response.resume();
    if(kind!=='asset'||redirects>=5||!response.headers.location)throw Error('Unexpected update redirect');
    const next=new URL(response.headers.location,value).href;urlCheck(next,'storage');
    return fetch(next,maximum,destination,descriptor,'storage',redirects+1,deadline);
  }
  if(response.statusCode!==200||response.headers['content-encoding']&&response.headers['content-encoding']!=='identity') {response.destroy();throw Error('Update server response refused');}
  const length=response.headers['content-length'];
  if(length!==undefined&&(!/^\d+$/.test(length)||Number(length)>maximum||descriptor&&Number(length)!==descriptor.size)) {response.destroy();throw Error('Wrong response length');}
  const partial=destination&&destination+'.partial';let fd=null,count=0,ownsPartial=false;const h=crypto.createHash('sha256'),chunks=[];
  const timer=setTimeout(()=>response.destroy(Error('Update deadline exceeded')),Math.max(1,deadline-Date.now()));
  try {
    if(destination){noLinks(destination);fd=fs.openSync(partial,'wx');ownsPartial=true;}
    for await(const chunk of response) {
      count+=chunk.length;if(count>maximum||descriptor&&count>descriptor.size)throw Error('Update exceeds signed size');
      h.update(chunk);if(fd===null)chunks.push(chunk);else fs.writeFileSync(fd,chunk);
    }
    if(descriptor&&(count!==descriptor.size||h.digest('hex')!==descriptor.sha256))throw Error('Update payload checksum mismatch');
    if(fd!==null){fs.fsyncSync(fd);fs.closeSync(fd);fd=null;if(fs.existsSync(destination))throw Error('Update destination exists');fs.linkSync(partial,destination);fs.unlinkSync(partial);return destination;}
    return Buffer.concat(chunks);
  }finally{clearTimeout(timer);response.destroy();if(fd!==null)fs.closeSync(fd);if(ownsPartial&&fs.existsSync(partial))fs.unlinkSync(partial);}
}
// ZIP32 only, regular files, store/deflate; no extract-all or executable names
// chosen by the archive. Exact authenticated central and local headers required.
async function extract(archive,destination,manifest) {
  const fd=fs.openSync(archive,'r'),size=fs.fstatSync(fd).size;
  const read=(position,length)=>{if(position<0||length<0||position+length>size)throw Error('ZIP range invalid');const b=Buffer.alloc(length);if(fs.readSync(fd,b,0,length,position)!==length)throw Error('Truncated ZIP');return b;};
  try {
    const tail=read(Math.max(0,size-65557),Math.min(size,65557));let e=-1;
    for(let i=tail.length-22;i>=0;--i)if(tail.readUInt32LE(i)===0x06054b50&&i+22+tail.readUInt16LE(i+20)===tail.length){e=i;break;}
    if(e<0||tail.readUInt16LE(e+4)||tail.readUInt16LE(e+6)||tail.readUInt16LE(e+8)!==tail.readUInt16LE(e+10))throw Error('Unsupported ZIP directory');
    const count=tail.readUInt16LE(e+10),centralSize=tail.readUInt32LE(e+12),offset=tail.readUInt32LE(e+16);
    if(count!==manifest.files.length||centralSize>4*1024**2||offset+centralSize!==size-tail.length+e)throw Error('Unsigned or unsupported ZIP entries');
    const central=read(offset,centralSize),expected=new Map(manifest.files.map(f=>[f.path,f])),entries=[],seen=new Set();let p=0;
    for(let i=0;i<count;++i) {
      if(p+46>central.length||central.readUInt32LE(p)!==0x02014b50)throw Error('Invalid ZIP directory');
      const flags=central.readUInt16LE(p+8),method=central.readUInt16LE(p+10),packed=central.readUInt32LE(p+20),plain=central.readUInt32LE(p+24),n=central.readUInt16LE(p+28),extra=central.readUInt16LE(p+30),comment=central.readUInt16LE(p+32),attr=central.readUInt32LE(p+38),local=central.readUInt32LE(p+42);
      if(p+46+n+extra+comment>central.length||flags&~0x808||![0,8].includes(method)||central.readUInt16LE(p+34)||((attr>>>16)&0xf000)&&((attr>>>16)&0xf000)!==0x8000)throw Error('Unsupported ZIP file');
      const name=central.subarray(p+46,p+46+n).toString('utf8');release.relative(name);
      const signed=expected.get(name);if(!signed||seen.has(name)||signed.size!==plain)throw Error('ZIP inventory mismatch');seen.add(name);
      const header=read(local,30);if(header.readUInt32LE(0)!==0x04034b50||header.readUInt16LE(6)!==flags||header.readUInt16LE(8)!==method)throw Error('ZIP local header mismatch');
      const nn=header.readUInt16LE(26),xx=header.readUInt16LE(28),start=local+30+nn+xx;
      if(read(local+30,nn).toString('utf8')!==name||start+packed>offset)throw Error('ZIP file range mismatch');
      if(!(flags&8)&&(header.readUInt32LE(18)!==packed||header.readUInt32LE(22)!==plain))throw Error('ZIP local size mismatch');
      entries.push({name,signed,method,packed,start});p+=46+n+extra+comment;
    }
    if(p!==central.length)throw Error('Trailing ZIP directory content');
    const total=manifest.files.reduce((sum,f)=>sum+f.size,0),disk=fs.statfsSync(path.dirname(destination));
    if(disk.bavail*disk.bsize-total<RESERVE)throw Error('Update staging needs 1 GiB free reserve');
    noLinks(destination);fs.mkdirSync(destination);
    for(const entry of entries) {
      const target=path.join(destination,...entry.name.split('/'));fs.mkdirSync(path.dirname(target),{recursive:true});let bytes=0;const h=crypto.createHash('sha256');
      const gate=new Transform({transform(chunk,encoding,done){bytes+=chunk.length;if(bytes>entry.signed.size)return done(Error('ZIP exceeds signed size'));h.update(chunk);done(null,chunk);}});
      const src=entry.packed?fs.createReadStream(archive,{start:entry.start,end:entry.start+entry.packed-1}):Readable.from([]);
      const streams=[src];if(entry.method===8)streams.push(zlib.createInflateRaw());streams.push(gate,fs.createWriteStream(target,{flags:'wx',mode:entry.signed.mode}));
      await pipeline(...streams);
      if(bytes!==entry.signed.size||h.digest('hex')!==entry.signed.sha256)throw Error('ZIP checksum mismatch');
      const out=fs.openSync(target,'r+');try{fs.fsyncSync(out);}finally{fs.closeSync(out);}
    }
  }finally{fs.closeSync(fd);}
}
class Runtime {
  constructor(root,options={}) {
    this.root=path.resolve(root);noLinks(this.root);this.control=path.join(this.root,'updater');
    this.policy={platform:process.platform==='win32'?'windows-x64':'linux-x64',channel:'stable',...options.policy};
    this.key=options.key||bounded(path.join(this.control,'release-public.pem'),4096);
    this.transport=options.transport||fetch;
    this.runtime=path.join(this.root,'runtime');this.work=path.join(this.root,'update-work');this.next=path.join(this.work,'next');this.old=path.join(this.root,'update-old');
    this.current=path.join(this.control,'current.json');this.journal=path.join(this.control,'install-pending.json');
  }
  verified(file,tree) {noLinks(file);const result=release.verify(bounded(file),this.key,this.policy);if(tree){noLinks(tree);release.verifyTree(tree,result.manifest,this.policy);}return result;}
  state() {const {manifest}=this.verified(this.current);return {phase:'idle',installedVersion:manifest.version,channel:'stable',message:'Click Update to check and download. Installation asks before closing. Open Qrazy manually afterward.'};}
  selected() {this.recover();return this.verified(this.current,this.runtime);}
  staged() {
    const next=this.verified(path.join(this.work,'signed.json'),this.next),current=this.verified(this.current);
    if(next.manifest.sequence<=current.manifest.sequence||compare(next.manifest.version,current.manifest.version)<=0)throw Error('Staged release is not newer');
    return next;
  }
  async update() {
    const current=this.verified(this.current);
    if(fs.existsSync(this.journal))throw Error('Installation recovery required; close and reopen Qrazy manually');
    if(fs.existsSync(this.work)) {
      try{const staged=this.staged();return this.ready(staged);}catch{this.remove(this.work);}
    }
    const assetName='Qrazy-SDLCEF-'+this.policy.platform+'-stable.json';let candidate=null;
    for(let page=1;page<=5&&!candidate;++page) {
      const raw=await this.transport('https://api.github.com/repos/JimmSlimm/qrazy-client/releases?per_page=100&page='+page,3*1024**2,null,null,'api');
      const rows=JSON.parse(raw);if(!Array.isArray(rows))throw Error('Invalid release discovery response');
      for(const row of rows) {
        if(row.draft||row.prerelease||!/^sdlcef-v(0|[1-9]\d{0,5})\.(0|[1-9]\d{0,5})\.(0|[1-9]\d{0,5})$/.test(row.tag_name)||!Array.isArray(row.assets))continue;
        const asset=row.assets.find(a=>a.name===assetName);if(!asset)continue;
        const url='https://github.com/JimmSlimm/qrazy-client/releases/download/'+row.tag_name+'/'+assetName;
        const envelope=await this.transport(url,3*1024**2),verified=release.verify(envelope,this.key,this.policy);
        if(row.tag_name!=='sdlcef-v'+verified.manifest.version)throw Error('Release tag and signed version disagree');
        if(!candidate||verified.manifest.sequence>candidate.verified.manifest.sequence)candidate={tag:row.tag_name,envelope,verified};
      }
      if(rows.length<100)break;
    }
    if(!candidate||candidate.verified.manifest.sequence<=current.manifest.sequence||compare(candidate.verified.manifest.version,current.manifest.version)<=0)return {...this.state(),phase:'current',message:'No newer signed stable SDL/CEF release is available.'};
    const m=candidate.verified.manifest,disk=fs.statfsSync(this.root),total=m.files.reduce((s,f)=>s+f.size,0);
    if(disk.bavail*disk.bsize-total-m.asset.size<RESERVE)throw Error('Update download and staging need 1 GiB free reserve');
    fs.mkdirSync(this.work);atomic(path.join(this.work,'signed.json'),candidate.envelope);
    const archive=path.join(this.work,'payload.zip');await this.transport('https://github.com/JimmSlimm/qrazy-client/releases/download/'+candidate.tag+'/'+m.asset.name,LIMIT,archive,m.asset);
    await extract(archive,this.next,m);const staged=this.staged();fs.unlinkSync(archive);return this.ready(staged);
  }
  ready(result) {return {...this.state(),phase:'ready',version:result.manifest.version,identity:result.identity,notes:result.manifest.notes,message:'Update downloaded and verified. Choose Install to confirm closing Qrazy. Open it manually afterward.'};}
  prepare() {const result=this.staged();atomic(path.join(this.work,'request.json'),Buffer.from(JSON.stringify({identity:result.identity})));return this.ready(result);}
  install() {
    // Only native launcher calls this while holding installation and host locks,
    // after the real host reports the reserved clean-install exit code.
    if(fs.existsSync(this.journal))throw Error('Pending transaction requires startup recovery');
    const request=JSON.parse(bounded(path.join(this.work,'request.json'),4096)),staged=this.staged();
    if(request.identity!==staged.identity)throw Error('Confirmed update identity changed');
    this.verified(this.current,this.runtime);if(fs.existsSync(this.old))throw Error('Unexpected temporary recovery runtime');
    const current=bounded(this.current),next=bounded(path.join(this.work,'signed.json'));
    atomic(this.journal,Buffer.from(JSON.stringify({schema:'qrazy-single-runtime-transaction-v1',previous:current.toString('base64'),next:next.toString('base64')})));
    fs.renameSync(this.runtime,this.old);fs.renameSync(this.next,this.runtime);
    this.recover();return {phase:'installed',message:'Update installed. Open Qrazy manually.'};
  }
  remove(file) {noLinks(file);const allowed=[this.old,this.work];if(!allowed.includes(path.resolve(file)))throw Error('Unsafe update cleanup');fs.rmSync(file,{recursive:true,force:true});}
  recover() {
    if(!fs.existsSync(this.journal))return;
    noLinks(this.old);noLinks(this.next);noLinks(this.runtime);
    const record=JSON.parse(bounded(this.journal,8*1024**2));
    if(record.schema!=='qrazy-single-runtime-transaction-v1'||typeof record.previous!=='string'||typeof record.next!=='string')throw Error('Invalid update journal');
    const previous=Buffer.from(record.previous,'base64'),next=Buffer.from(record.next,'base64');
    const prev=release.verify(previous,this.key,this.policy),fresh=release.verify(next,this.key,this.policy),current=this.verified(this.current);
    if(fresh.manifest.sequence<=prev.manifest.sequence||compare(fresh.manifest.version,prev.manifest.version)<=0||![prev.identity,fresh.identity].includes(current.identity))throw Error('Invalid transaction history');
    const matches=(tree,result)=>{try{release.verifyTree(tree,result.manifest,this.policy);return true;}catch{return false;}};
    if(matches(this.runtime,fresh)) {
      atomic(this.current,next);this.remove(this.old);this.remove(this.work);fs.unlinkSync(this.journal);return;
    }
    if(current.identity===fresh.identity)throw Error('Installed update was modified; refusing downgrade');
    if(matches(this.runtime,prev)) {
      this.remove(this.work);this.remove(this.old);fs.unlinkSync(this.journal);return;
    }
    if(matches(this.old,prev)) {
      if(fs.existsSync(this.runtime)) {if(fs.existsSync(this.next))throw Error('Ambiguous interrupted update');fs.renameSync(this.runtime,this.next);}
      fs.renameSync(this.old,this.runtime);atomic(this.current,previous);this.remove(this.work);fs.unlinkSync(this.journal);return;
    }
    throw Error('No authenticated runtime available for transaction recovery');
  }
}
function compare(a,b){const aa=a.split('.').map(Number),bb=b.split('.').map(Number);for(let i=0;i<3;++i)if(aa[i]!==bb[i])return aa[i]-bb[i];return 0;}
module.exports={Runtime,fetch,urlCheck,extract,compare};
if(require.main===module) (async()=>{
  const [op,root]=process.argv.slice(2);if(process.argv.length!==4)throw Error('Invalid updater invocation');
  if(op==='transition-recovery') {
    const key=bounded(path.join(__dirname,'release-public.pem'),65536);
    const {files,pid}=require('./transition-peer.cjs').recovery(path.resolve(root),key);
    process.stdout.write((pid?'RESIDENT\t'+pid:'RECOVERY')+'\n'+files.map(f=>f.path+'\t'+f.sha256).join('\n')+'\n');return;
  }
  const runtime=new Runtime(root);let result;
  if(op==='verify')result={phase:'verified',...runtime.selected()};
  else if(op==='state')result=runtime.state();
  else if(op==='update')result=await runtime.update();
  else if(op==='prepare')result=runtime.prepare();
  else if(op==='install')result=runtime.install();
  else if(op==='transition-peer')result=require('./transition-peer.cjs').peer(path.resolve(root),runtime.key);
  else if(op==='transition-finish') {
    const engine=new (require('./transition-install.cjs').TransitionInstall)(root,{key:runtime.key});
    result=engine.finish(engine.record().fresh.identity);
  }
  else throw Error('Unknown updater operation');
  process.stdout.write(JSON.stringify({ok:true,data:result}));
})().catch(()=>{process.stdout.write(JSON.stringify({ok:false,error:'Update verification or installation failed. Preserve the Qrazy folder and retry only after resolving the incomplete update.'}));process.exitCode=1;});
