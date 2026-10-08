'use strict';
// QAS1 completed-file migration only. No Chromium profile/cache or credentials.
const fs=require('node:fs/promises'),path=require('node:path'),crypto=require('node:crypto');
const HEADER=4096,CHUNK=1024**2,TOTAL=16*1024**3,RESERVE=1024**3;
const LIMITS={map:512*1024**2,sound:64*1024**2,shader:512*1024**2,texture:512*1024**2};
async function noLinks(file) {
  for(let p=path.resolve(file);;p=path.dirname(p)) {
    try{if((await fs.lstat(p)).isSymbolicLink())throw Error('Linked asset path refused');}catch(e){if(e.code!=='ENOENT')throw e;}
    if(path.dirname(p)===p)break;
  }
}
async function inspect(file,name) {
  await noLinks(file);if(!(await fs.lstat(file)).isFile())throw Error('Regular asset required');
  const fd=await fs.open(file,'r');
  try {
    const header=Buffer.alloc(HEADER);if((await fd.read(header,0,HEADER,0)).bytesRead!==HEADER||header.toString('ascii',0,4)!=='QAS1')throw Error('Invalid asset header');
    const length=header.readUInt32LE(4);if(!length||length>HEADER-8)throw Error('Invalid asset metadata');
    const m=JSON.parse(new TextDecoder('utf-8',{fatal:true}).decode(header.subarray(8,8+length)));
    if(!m||Object.keys(m).sort().join(',')!=='key,kind,sha256,size'||!Object.hasOwn(LIMITS,m.kind)||typeof m.key!=='string'||
      !m.key.length||Buffer.byteLength(m.key)>2048||/[\x00-\x1f\x7f]/.test(m.key)||!Number.isSafeInteger(m.size)||m.size<1||m.size>LIMITS[m.kind]||
      typeof m.sha256!=='string'||!/^[a-f0-9]{64}$/.test(m.sha256)||
      crypto.createHash('sha256').update(m.kind+'\0'+m.key).digest('hex')+'.asset'!==name||(await fd.stat()).size!==HEADER+m.size)throw Error('Invalid asset identity');
    const hash=crypto.createHash('sha256'),buffer=Buffer.alloc(CHUNK);let offset=0;
    while(offset<m.size){const n=Math.min(buffer.length,m.size-offset),read=await fd.read(buffer,0,n,HEADER+offset);if(read.bytesRead!==n)throw Error('Incomplete asset');hash.update(buffer.subarray(0,n));offset+=n;}
    if(hash.digest('hex')!==m.sha256)throw Error('Asset checksum mismatch');return m;
  }finally{await fd.close();}
}
async function migrate(source,destination,{maxBytes=TOTAL,minFreeBytes=RESERVE,commit=async(from,to)=>{try{await fs.lstat(to);throw Error('Destination appeared during migration');}catch(e){if(e.code!=='ENOENT')throw e;}await fs.rename(from,to);}}={}) {
  if(!Number.isSafeInteger(maxBytes)||maxBytes<1||maxBytes>TOTAL||!Number.isSafeInteger(minFreeBytes)||minFreeBytes<0||minFreeBytes>RESERVE)throw Error('Invalid storage bounds');
  if(path.resolve(source)===path.resolve(destination))throw Error('Separate asset stores required');
  await noLinks(source);await noLinks(destination);
  const result={copied:0,alreadyPresent:0,conflicts:0,invalid:0,skippedForSpace:0};
  let names;try{names=await fs.readdir(source);}catch(e){if(e.code==='ENOENT')return result;throw e;}
  await fs.mkdir(destination,{recursive:true});let used=0;
  for(const name of await fs.readdir(destination)) {
    const file=path.join(destination,name);await noLinks(file);const stat=await fs.lstat(file);if(!stat.isFile())throw Error('Unexpected asset store entry');
    // Only this migration's explicitly named incomplete writes may be removed.
    if(/^migration-[a-f0-9]{32}\.partial$/.test(name)){await fs.unlink(file);continue;}
    used+=stat.size;
  }
  for(const name of names.sort()) {
    if(!/^[a-f0-9]{64}\.asset$/.test(name))continue;
    const file=path.join(source,name),target=path.join(destination,name);let m;
    try{m=await inspect(file,name);}catch{++result.invalid;continue;}
    try{
      const existing=await inspect(target,name);
      if(existing.sha256===m.sha256&&existing.size===m.size)++result.alreadyPresent;else ++result.conflicts;
      continue;
    }catch(e){if(e.code!=='ENOENT'){++result.conflicts;continue;}}
    const required=HEADER+m.size,disk=await fs.statfs(destination);
    if(used+required>maxBytes||disk.bavail*disk.bsize-required<minFreeBytes){++result.skippedForSpace;continue;}
    const temporary=path.join(destination,'migration-'+crypto.randomBytes(16).toString('hex')+'.partial');let owned=false;
    try {
      await fs.copyFile(file,temporary,fs.constants.COPYFILE_EXCL);owned=true;
      const copied=await inspect(temporary,name);
      if(copied.kind!==m.kind||copied.key!==m.key||copied.size!==m.size||copied.sha256!==m.sha256)throw Error('Source asset changed during migration');
      const fd=await fs.open(temporary,'r+');try{await fd.sync();}finally{await fd.close();}
      await noLinks(target);await commit(temporary,target);owned=false;
      used+=required;++result.copied;
    }finally{if(owned)await fs.unlink(temporary).catch(()=>{});}
  }
  return result;
}
module.exports={migrate,inspect};
