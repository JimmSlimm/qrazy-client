// Read-only discovery shipped inside the signed runtime; pinned updater remains unchanged.
'use strict';
const fs=require('node:fs'),path=require('node:path');
async function check(root){
  const control=path.join(root,'updater'),release=require(path.join(control,'production-release.cjs'));
  const {Runtime,compare}=require(path.join(control,'production-runtime.cjs'));
  const runtime=new Runtime(root);const current=runtime.verified(runtime.current);
  if(fs.existsSync(runtime.journal))throw Error('Installation recovery required');
  if(fs.existsSync(runtime.work)){try{return runtime.ready(runtime.staged());}catch{/* Never remove staged files during discovery. */}}
  const context={policy:runtime.policy,key:runtime.key,transport:runtime.transport,state:()=>runtime.state()};
  return await (async function(){
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

    return {...this.state(),phase:'available',version:candidate.verified.manifest.version,notes:candidate.verified.manifest.notes,message:'A newer verified client is available. Download it to continue.'};
  }).call(context);
}
module.exports={check};
if(require.main===module)(async()=>{const [op,root]=process.argv.slice(2);if(process.argv.length!==4||op!=='check')throw Error('Invalid notification invocation');process.stdout.write(JSON.stringify({ok:true,data:await check(path.resolve(root))}));})().catch(()=>{process.stdout.write(JSON.stringify({ok:false,error:'Client update check unavailable'}));process.exitCode=1;});
