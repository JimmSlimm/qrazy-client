'use strict';
// The Electron update schema cannot authorize these paths. This independent
// envelope is verified with the explicitly bundled SDL/CEF public trust key.
const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const release=require('./production-release.cjs');
const controls=['node.exe','node-license.txt','production-runtime.cjs','production-release.cjs','release-public.pem','current.json',
  'distribution.cjs','transition-install.cjs','transition-peer.cjs','legacy-release.cjs','legacy-config.cjs'];
const fail=()=>Error('Full distribution authentication failed');
function decode(value) {
  if(typeof value!=='string'||value.length>4*1024**2||! /^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(value))throw fail();
  return Buffer.from(value,'base64');
}
function verify(bytes,key,policy={platform:'windows-x64',channel:'stable'}) {
  if(!Buffer.isBuffer(bytes)||bytes.length>3*1024**2)throw fail();
  try {
    const envelope=JSON.parse(bytes),payload=decode(envelope.payload),signature=decode(envelope.signature),publicKey=crypto.createPublicKey(key);
    if(publicKey.asymmetricKeyType!=='ed25519'||signature.length!==64||!crypto.verify(null,payload,publicKey,signature))throw fail();
    const m=JSON.parse(payload);
    const linux=policy.platform==='linux-x64';
    if(!['windows-x64','linux-x64'].includes(policy.platform)||policy.channel!=='stable')throw fail();
    const requiredControls=linux?['node','node-license.txt','production-runtime.cjs','production-release.cjs','release-public.pem','current.json']:controls;
    const rootFiles=linux?['launch.sh','launch.py','system-requirements.txt']:['qrazy.exe'];
    if(m.schema!==(linux?'qrazy-sdlcef-linux-distribution-v1':'qrazy-sdlcef-windows-distribution-v1')||m.product!=='qrazy-sdlcef'||m.platform!==policy.platform||m.channel!=='stable'||
      typeof m.version!=='string'||!/^(0|[1-9]\d{0,5})\.(0|[1-9]\d{0,5})\.(0|[1-9]\d{0,5})$/.test(m.version)||
      !Number.isSafeInteger(m.sequence)||m.sequence<1||!Array.isArray(m.files)||m.files.length>4096||!m.files.length||
      typeof m.runtimeIdentity!=='string'||!/^[a-f0-9]{64}$/.test(m.runtimeIdentity))throw fail();
    if(!m.asset||m.asset.name!=='Qrazy-SDLCEF-'+policy.platform+'-distribution-'+m.version+(linux?'.tar.gz':'.zip')||
      !Number.isSafeInteger(m.asset.size)||m.asset.size<1||m.asset.size>4*1024**3||
      typeof m.asset.sha256!=='string'||!/^[a-f0-9]{64}$/.test(m.asset.sha256))throw fail();
    const names=new Set();let total=0;
    for(const f of m.files) {
      release.relative(f.path);
      if(!rootFiles.includes(f.path.toLowerCase())&&!f.path.startsWith('runtime/')&&!requiredControls.some(n=>f.path.toLowerCase()==='updater/'+n))throw fail();
      const name=f.path.toLowerCase();
      if(names.has(name)||!Number.isSafeInteger(f.size)||f.size<1||f.size>2*1024**3||!/^[a-f0-9]{64}$/.test(f.sha256)||![420,493].includes(f.mode))throw fail();
      names.add(name);total+=f.size;
      if(f.path==='updater/current.json'&&f.size>3*1024**2||f.path==='updater/release-public.pem'&&f.size>65536)throw fail();
    }
    if(total>4*1024**3||rootFiles.some(n=>!names.has(n))||requiredControls.some(n=>!names.has('updater/'+n))||
      [...names].some(n=>[...names].some(other=>other.startsWith(n+'/'))))throw fail();
    return {manifest:m,identity:crypto.createHash('sha256').update(payload).digest('hex')};
  }catch{throw fail();}
}
function verifyTree(root,bytes,key,policy={platform:'windows-x64',channel:'stable'}) {
  // Call only on isolated staging, never an installation containing profiles.
  for(let p=path.resolve(root);;p=path.dirname(p)) {
    if(fs.lstatSync(p).isSymbolicLink())throw fail();if(path.dirname(p)===p)break;
  }
  const result=verify(bytes,key,policy),actual=release.inventory(root),expected=new Map(result.manifest.files.map(f=>[f.path,f]));
  if(actual.length!==expected.size)throw fail();
  for(const f of actual){const e=expected.get(f.path);if(!e||e.size!==f.size||e.sha256!==f.sha256||policy.platform==='linux-x64'&&e.mode!==f.mode)throw fail();}
  const runtimeBytes=fs.readFileSync(path.join(root,'updater','current.json'));
  if(runtimeBytes.length>3*1024**2)throw fail();
  const runtime=release.verify(runtimeBytes,key,policy);
  if(runtime.identity!==result.manifest.runtimeIdentity||runtime.manifest.version!==result.manifest.version||runtime.manifest.sequence!==result.manifest.sequence)throw fail();
  release.verifyTree(path.join(root,'runtime'),runtime.manifest,policy);
  // Bundled future verification must keep the same explicit fixed public key.
  const bundled=crypto.createPublicKey(fs.readFileSync(path.join(root,'updater','release-public.pem'))).export({type:'spki',format:'der'});
  const trusted=crypto.createPublicKey(key).export({type:'spki',format:'der'});
  if(!bundled.equals(trusted))throw fail();
  return result;
}
module.exports={verify,verifyTree};
