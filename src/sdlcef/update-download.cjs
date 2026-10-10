// Signed runtime downloader. The fixed installer still verifies the complete
// staged inventory before replacing anything. ZIP metadata is untrusted; only
// hashes and sizes in the authenticated release inventory authorize files.
'use strict';
const fs=require('node:fs'),path=require('node:path'),https=require('node:https'),crypto=require('node:crypto'),zlib=require('node:zlib');
const {pipeline}=require('node:stream/promises');
const {Transform}=require('node:stream');
const RESERVE=1024**3,METADATA_LIMIT=4*1024**2;
function noLinks(file){for(let p=path.resolve(file);;p=path.dirname(p)){if(fs.existsSync(p)&&fs.lstatSync(p).isSymbolicLink())throw Error('Linked runtime path refused');if(path.dirname(p)===p)break;}}
function hash(file){const fd=fs.openSync(file,'r'),h=crypto.createHash('sha256'),b=Buffer.alloc(1048576);try{let n;while((n=fs.readSync(fd,b,0,b.length,null)))h.update(b.subarray(0,n));return h.digest('hex');}finally{fs.closeSync(fd);}}
async function range(url,start,end,total,destination,deadline=Date.now()+600000,redirects=0){
  if(!Number.isSafeInteger(start)||!Number.isSafeInteger(end)||start<0||end<start||end>=total||!Number.isSafeInteger(total)||total>4*1024**3)throw Error('Invalid ZIP range');
  const u=new URL(url),storage=u.hostname==='release-assets.githubusercontent.com';
  if(u.protocol!=='https:'||u.username||u.password||u.port||u.hash||(!storage&&(u.hostname!=='github.com'||u.search||!/^\/JimmSlimm\/qrazy-client\/releases\/download\/sdlcef-v[0-9.]+\/Qrazy-SDLCEF-[A-Za-z0-9_.+-]+\.zip$/.test(u.pathname))))throw Error('Untrusted update range URL');
  if(Date.now()>=deadline)throw Error('Update deadline exceeded');
  const response=await new Promise((resolve,reject)=>{const req=https.get(u,{headers:{'User-Agent':'Qrazy-SDLCEF-Updater','Accept-Encoding':'identity',...(storage?{Range:`bytes=${start}-${end}`}:{})},timeout:15000},resolve);req.on('error',reject);req.on('timeout',()=>req.destroy(Error('Update connection timeout')));});
  if([301,302,303,307,308].includes(response.statusCode)){
    response.destroy();if(storage||redirects>=5||!response.headers.location)throw Error('Unexpected range redirect');
    const next=new URL(response.headers.location,u);if(next.hostname!=='release-assets.githubusercontent.com')throw Error('Untrusted range redirect');
    return range(next.href,start,end,total,destination,deadline,redirects+1);
  }
  const expected=end-start+1;
  if(response.statusCode!==206||response.headers['content-range']!==`bytes ${start}-${end}/${total}`||response.headers['content-encoding']&&response.headers['content-encoding']!=='identity'||response.headers['content-length']!==undefined&&response.headers['content-length']!==String(expected)){
    response.destroy();throw Error('Update server refused the exact partial download');
  }
  if(!destination&&expected>METADATA_LIMIT){response.destroy();throw Error('ZIP metadata limit exceeded');}
  const timer=setTimeout(()=>response.destroy(Error('Update deadline exceeded')),Math.max(1,deadline-Date.now()));
  let bytes=0;const gate=new Transform({transform(chunk,encoding,done){bytes+=chunk.length;done(bytes>expected?Error('Oversized partial response'):null,chunk);}});
  try{
    if(destination){noLinks(destination);await pipeline(response,gate,fs.createWriteStream(destination,{flags:'wx'}));}
    else{const chunks=[];gate.on('data',chunk=>chunks.push(chunk));await pipeline(response,gate);if(bytes!==expected)throw Error('Truncated partial response');return Buffer.concat(chunks);}
    if(bytes!==expected)throw Error('Truncated partial response');
  }finally{clearTimeout(timer);response.destroy();}
}
async function stage(runtime,candidate,request=range){
  const m=candidate.verified.manifest,total=m.asset.size,url='https://github.com/JimmSlimm/qrazy-client/releases/download/'+candidate.tag+'/'+m.asset.name,deadline=Date.now()+600000;
  const read=(start,length,destination)=>{if(!Number.isSafeInteger(length)||length<1||start+length>total)throw Error('ZIP range invalid');return request(url,start,start+length-1,total,destination,deadline);};
  const tailLength=Math.min(total,65557),tail=await read(total-tailLength,tailLength);let e=-1;
  for(let i=tail.length-22;i>=0;--i)if(tail.readUInt32LE(i)===0x06054b50&&i+22+tail.readUInt16LE(i+20)===tail.length){e=i;break;}
  if(e<0||tail.readUInt16LE(e+4)||tail.readUInt16LE(e+6)||tail.readUInt16LE(e+8)!==tail.readUInt16LE(e+10))throw Error('Unsupported ZIP directory');
  const count=tail.readUInt16LE(e+10),size=tail.readUInt32LE(e+12),offset=tail.readUInt32LE(e+16);
  if(count!==m.files.length||size>METADATA_LIMIT||offset+size!==total-tail.length+e)throw Error('ZIP inventory mismatch');
  const central=await read(offset,size),expected=new Map(m.files.map(f=>[f.path,f])),seen=new Set(),entries=[];let p=0;
  for(let i=0;i<count;i++){
    if(p+46>central.length||central.readUInt32LE(p)!==0x02014b50)throw Error('Invalid ZIP directory');
    const flags=central.readUInt16LE(p+8),method=central.readUInt16LE(p+10),packed=central.readUInt32LE(p+20),plain=central.readUInt32LE(p+24),n=central.readUInt16LE(p+28),extra=central.readUInt16LE(p+30),comment=central.readUInt16LE(p+32),attr=central.readUInt32LE(p+38),local=central.readUInt32LE(p+42);
    if(p+46+n+extra+comment>central.length||flags&~0x808||![0,8].includes(method)||central.readUInt16LE(p+34)||((attr>>>16)&0xf000)&&((attr>>>16)&0xf000)!==0x8000)throw Error('Unsupported ZIP file');
    const name=central.subarray(p+46,p+46+n).toString('utf8'),signed=expected.get(name);
    if(!signed||seen.has(name)||signed.size!==plain||local+30>offset||packed>total)throw Error('Unsigned ZIP entry');
    seen.add(name);entries.push({name,signed,flags,method,packed,local});p+=46+n+extra+comment;
  }
  if(p!==central.length)throw Error('Trailing ZIP directory content');
  const disk=fs.statfsSync(runtime.root),unpacked=m.files.reduce((s,f)=>s+f.size,0),largest=Math.max(...entries.map(f=>f.packed));
  if(disk.bavail*disk.bsize-unpacked-largest<RESERVE)throw Error('Update staging needs 1 GiB free reserve');
  noLinks(runtime.work);fs.mkdirSync(runtime.work);fs.writeFileSync(path.join(runtime.work,'signed.json'),candidate.envelope,{flag:'wx'});fs.mkdirSync(runtime.next);
  const old=new Map(runtime.verified(runtime.current).manifest.files.map(f=>[f.path,f]));let downloaded=tailLength+size,reused=0;
  for(const entry of entries){
    if(Date.now()>=deadline)throw Error('Update deadline exceeded');
    const target=path.join(runtime.next,...entry.name.split('/')),source=path.join(runtime.runtime,...entry.name.split('/')),prior=old.get(entry.name);
    fs.mkdirSync(path.dirname(target),{recursive:true});noLinks(source);
    if(prior&&prior.size===entry.signed.size&&prior.sha256===entry.signed.sha256&&fs.existsSync(source)&&fs.lstatSync(source).isFile()&&fs.statSync(source).size===entry.signed.size&&hash(source)===entry.signed.sha256){
      // Independent copy: never hard-link the live runtime into staging.
      fs.copyFileSync(source,target,fs.constants.COPYFILE_EXCL);reused+=entry.signed.size;
    }else{
      const header=await read(entry.local,30);downloaded+=30;
      if(header.readUInt32LE(0)!==0x04034b50||header.readUInt16LE(6)!==entry.flags||header.readUInt16LE(8)!==entry.method)throw Error('ZIP local header mismatch');
      const nn=header.readUInt16LE(26),xx=header.readUInt16LE(28),start=entry.local+30+nn+xx;
      if(!nn||start+entry.packed>offset||(await read(entry.local+30,nn)).toString('utf8')!==entry.name)throw Error('ZIP file range mismatch');downloaded+=nn;
      if(!(entry.flags&8)&&(header.readUInt32LE(18)!==entry.packed||header.readUInt32LE(22)!==entry.signed.size))throw Error('ZIP local size mismatch');
      if(!entry.packed)throw Error('Empty compressed runtime file');
      const packed=path.join(runtime.work,'entry.partial');await read(start,entry.packed,packed);downloaded+=entry.packed;
      let bytes=0;const h=crypto.createHash('sha256'),gate=new Transform({transform(chunk,encoding,done){bytes+=chunk.length;if(bytes>entry.signed.size)return done(Error('ZIP exceeds signed size'));h.update(chunk);done(null,chunk);}});
      const streams=[fs.createReadStream(packed)];if(entry.method===8)streams.push(zlib.createInflateRaw());streams.push(gate,fs.createWriteStream(target,{flags:'wx',mode:entry.signed.mode}));await pipeline(...streams);
      if(bytes!==entry.signed.size||h.digest('hex')!==entry.signed.sha256)throw Error('ZIP checksum mismatch');fs.unlinkSync(packed);
    }
    const fd=fs.openSync(target,'r+');try{fs.fsyncSync(fd);}finally{fs.closeSync(fd);}if(process.platform==='linux')fs.chmodSync(target,entry.signed.mode);
  }
  // Existing installer authenticates every byte again; archive offsets never
  // authorize output content. Retain no extra files in the staged tree.
  if(process.platform==='linux'){
    const sync=file=>{const fd=fs.openSync(file,'r');try{fs.fsyncSync(fd);}finally{fs.closeSync(fd);}};
    const directories=dir=>{for(const n of fs.readdirSync(dir)){const child=path.join(dir,n);if(fs.lstatSync(child).isDirectory())directories(child);}sync(dir);};
    sync(path.join(runtime.work,'signed.json'));directories(runtime.next);sync(runtime.work);sync(runtime.root);
  }
  const result=runtime.staged();return {...runtime.ready(result),downloadedBytes:downloaded,reusedBytes:reused};
}
async function update(root,options={}){
  const control=path.join(root,'updater'),{Runtime,compare}=require(path.join(control,'production-runtime.cjs')),release=require(path.join(control,'production-release.cjs'));
  const runtime=new Runtime(root,options),current=runtime.verified(runtime.current);
  if(fs.existsSync(runtime.journal))throw Error('Installation recovery required; close and reopen manually');
  if(fs.existsSync(runtime.work)){try{return runtime.ready(runtime.staged());}catch{runtime.remove(runtime.work);}}
  let candidate=null;const assetName='Qrazy-SDLCEF-'+runtime.policy.platform+'-stable.json';
  for(let page=1;page<=5&&!candidate;page++){
    const rows=JSON.parse(await runtime.transport('https://api.github.com/repos/JimmSlimm/qrazy-client/releases?per_page=100&page='+page,3*1024**2,null,null,'api'));if(!Array.isArray(rows))throw Error('Invalid release discovery');
    for(const row of rows){
      if(row.draft||row.prerelease||!/^sdlcef-v(0|[1-9]\d{0,5})\.(0|[1-9]\d{0,5})\.(0|[1-9]\d{0,5})$/.test(row.tag_name)||!Array.isArray(row.assets)||!row.assets.some(a=>a.name===assetName))continue;
      const envelope=await runtime.transport('https://github.com/JimmSlimm/qrazy-client/releases/download/'+row.tag_name+'/'+assetName,3*1024**2),verified=release.verify(envelope,runtime.key,runtime.policy);
      if(row.tag_name!=='sdlcef-v'+verified.manifest.version)throw Error('Release tag and signed version disagree');
      if(!candidate||verified.manifest.sequence>candidate.verified.manifest.sequence)candidate={tag:row.tag_name,envelope,verified};
    }
    if(rows.length<100)break;
  }
  if(!candidate||candidate.verified.manifest.sequence<=current.manifest.sequence||compare(candidate.verified.manifest.version,current.manifest.version)<=0)return {...runtime.state(),phase:'current',message:'No newer signed stable SDL/CEF release is available.'};
  return stage(runtime,candidate,options.range||range);
}
module.exports={range,stage,update};
if(require.main===module)(async()=>{const [op,root]=process.argv.slice(2);if(process.argv.length!==4||op!=='update')throw Error('Invalid download invocation');process.stdout.write(JSON.stringify({ok:true,data:await update(path.resolve(root))}));})().catch(error=>{process.stdout.write(JSON.stringify({ok:false,error:error.message}));process.exitCode=1;});
