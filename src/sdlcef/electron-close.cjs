'use strict';
// Main process only. A successful cookie flush is a prerequisite, never a
// replacement for native process-exit/profile-lock proof during installation.
async function confirmClose({dialog,window,session,drain,timeout=5000,detail='Your profiles will be preserved. Open Qrazy manually after installation.'}) {
  if(!window||window.isDestroyed()||!window.isFocused()||window.isMinimized()||
     !Number.isSafeInteger(timeout)||timeout<1||timeout>5000||typeof detail!=='string'||detail.length>800)throw Error('Focus Qrazy before confirming installation.');
  const {response}=await dialog.showMessageBox(window,{
    type:'question',title:'Update Qrazy',message:'Close Qrazy and install the verified update?',
    detail,
    buttons:['Cancel','Close and install'],defaultId:0,cancelId:0,noLink:true
  });
  if(response!==1)return false;
  if(window.isDestroyed())throw Error('The client closed before update confirmation completed.');
  let timer;
  try {
    await Promise.race([
      (async()=>{await drain();await session.cookies.flushStore();session.flushStorageData();})(),
      new Promise((_,reject)=>{timer=setTimeout(()=>reject(Error('Storage did not finish flushing. The client remains open; retry installation.')),timeout);})
    ]);
    return true;
  }catch{throw Error('Storage did not finish flushing. The client remains open; retry installation.');}
  finally{clearTimeout(timer);}
}
module.exports={confirmClose};
