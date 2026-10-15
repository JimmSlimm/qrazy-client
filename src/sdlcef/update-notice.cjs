// Read-only discovery shipped inside the signed runtime; pinned updater remains unchanged.
'use strict';
const fs=require('node:fs'),path=require('node:path');
// A release description may carry a message for installed clients, as an invisible HTML comment:
//   <!-- qrazy-notice {"id":"reinstall-1","below":"0.5.1","title":"...","message":"..."} -->
// "below" limits it to clients older than that version. Shown as plain text only.
function parseNotice(body,installed,compare){
  if(typeof body!=='string'||body.length>60000)return null;
  const match=/<!--\s*qrazy-notice\s+(\{[\s\S]*?\})\s*-->/.exec(body);if(!match)return null;
  try{
    const n=JSON.parse(match[1]),clean=(v,max)=>typeof v==='string'?v.replace(/[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/g,'').trim().slice(0,max):'';
    const id=clean(n.id,64),message=clean(n.message,1500),title=clean(n.title,80)||'Message from Qrazy';
    if(!/^[A-Za-z0-9._-]{1,64}$/.test(id)||!message)return null;
    if(n.below!==undefined&&(typeof n.below!=='string'||!/^\d+\.\d+\.\d+$/.test(n.below)||compare(installed,n.below)>=0))return null;
    return {id,title,message};
  }catch{return null;}
}
async function check(root){
  const control=path.join(root,'updater'),release=require(path.join(control,'production-release.cjs'));
  const {Runtime,compare}=require(path.join(control,'production-runtime.cjs'));
  const runtime=new Runtime(root);const current=runtime.verified(runtime.current);
  if(fs.existsSync(runtime.journal))throw Error('Installation recovery required');
  if(fs.existsSync(runtime.work)){try{return runtime.ready(runtime.staged());}catch{/* Never remove staged files during discovery. */}}
  const context={policy:runtime.policy,key:runtime.key,transport:runtime.transport,state:()=>runtime.state()};
  return await (async function(){
    const assetName='Qrazy-SDLCEF-'+this.policy.platform+'-stable.json';let candidate=null,notice=null,noticeSeen=false;
    for(let page=1;page<=5&&!candidate;++page) {
      const raw=await this.transport('https://api.github.com/repos/JimmSlimm/qrazy-client/releases?per_page=100&page='+page,3*1024**2,null,null,'api');
      const rows=JSON.parse(raw);if(!Array.isArray(rows))throw Error('Invalid release discovery response');
      for(const row of rows) {
        if(row.draft||row.prerelease||!/^sdlcef-v(0|[1-9]\d{0,5})\.(0|[1-9]\d{0,5})\.(0|[1-9]\d{0,5})$/.test(row.tag_name)||!Array.isArray(row.assets))continue;
        if(!noticeSeen){noticeSeen=true;notice=parseNotice(row.body,current.manifest.version,compare);}
        const asset=row.assets.find(a=>a.name===assetName);if(!asset)continue;
        const url='https://github.com/JimmSlimm/qrazy-client/releases/download/'+row.tag_name+'/'+assetName;
        const envelope=await this.transport(url,3*1024**2),verified=release.verify(envelope,this.key,this.policy);
        if(row.tag_name!=='sdlcef-v'+verified.manifest.version)throw Error('Release tag and signed version disagree');
        if(!candidate||verified.manifest.sequence>candidate.verified.manifest.sequence)candidate={tag:row.tag_name,envelope,verified};
      }
      if(rows.length<100)break;
    }
    if(!candidate||candidate.verified.manifest.sequence<=current.manifest.sequence||compare(candidate.verified.manifest.version,current.manifest.version)<=0)return {...this.state(),phase:'current',message:'No newer signed stable SDL/CEF release is available.',...(notice?{notice}:{})};

    return {...this.state(),phase:'available',version:candidate.verified.manifest.version,notes:candidate.verified.manifest.notes,message:'A newer verified client is available. Download it to continue.',...(notice?{notice}:{})};
  }).call(context);
}
module.exports={check,parseNotice};
if(require.main===module)(async()=>{const [op,root]=process.argv.slice(2);if(process.argv.length!==4||op!=='check')throw Error('Invalid notification invocation');process.stdout.write(JSON.stringify({ok:true,data:await check(path.resolve(root))}));})().catch(()=>{process.stdout.write(JSON.stringify({ok:false,error:'Client update check unavailable'}));process.exitCode=1;});
