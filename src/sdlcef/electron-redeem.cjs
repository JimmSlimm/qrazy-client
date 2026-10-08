'use strict';
const crypto=require('node:crypto');
const {ORIGIN,responseURL,redeemResponse}=require('./session-handoff.cjs');
// Temporary authentication copy uses an IN-MEMORY Electron session. Its only
// authentication comes from the connected, pinned source process.
async function redeem(session,bytes) {
  if(!Buffer.isBuffer(bytes)||bytes.length!==43||!/^[A-Za-z0-9_-]{43}$/.test(bytes.toString('ascii')))throw Error('Invalid session transfer');
  const controller=new AbortController(),timer=setTimeout(()=>controller.abort(),8000);
  async function bounded(promise) {
    let abort;
    try{return await Promise.race([promise,new Promise((_,reject)=>{abort=()=>reject(Error('Session transfer timed out'));controller.signal.addEventListener('abort',abort,{once:true});if(controller.signal.aborted)abort();})]);}
    finally{controller.signal.removeEventListener('abort',abort);}
  }
  try {
    const response=await session.fetch(ORIGIN+'/auth/handoff/redeem',{
      method:'POST',redirect:'error',credentials:'include',cache:'no-store',
      headers:{'Content-Type':'application/json','X-Qrazy-Auth':'1','Origin':ORIGIN},
      body:JSON.stringify({code:bytes.toString('ascii')}),signal:controller.signal
    });
    if(response.status!==200||!responseURL(response,'/auth/handoff/redeem')||
      !/^application\/json(?:\s*;|$)/i.test(response.headers.get('content-type')||''))throw Error('Session redemption refused');
    const reader=response.body.getReader(),chunks=[];let size=0;
    let result;
    try {
      for(;;){const {value,done}=await reader.read();if(done)break;size+=value.length;if(size>512)throw Error('Response too large');chunks.push(value);}
      const data=Buffer.concat(chunks);try{result=redeemResponse(JSON.parse(new TextDecoder('utf-8',{fatal:true}).decode(data)));}finally{data.fill(0);}
    }finally{await reader.cancel().catch(()=>{});for(const chunk of chunks)chunk.fill(0);}
    let header=response.headers.get('set-cookie')||'';
    const match=/^qrazy_session=([a-f0-9]{64}); Path=\/; HttpOnly; SameSite=Strict; Secure(?:; Max-Age=([0-9]{1,7}))?$/.exec(header);
    if(!match||Boolean(match[2])!==result.remember||result.remember&&Number(match[2])<1)throw Error('Cookie attributes refused');
    const identity=crypto.createHash('sha256').update(match[1]).digest();header='';match[1]='';
    const cookies=await bounded(session.cookies.get({url:ORIGIN+'/',name:'qrazy_session'}));
    try {
      if(cookies.length!==1)throw Error('Cookie context refused');
      const cookie=cookies[0];
      if(!cookie.secure||!cookie.httpOnly||cookie.sameSite!=='strict'||cookie.domain!=='qrazy-game.onrender.com'||cookie.path!=='/'||
        cookie.session===result.remember||!/^[a-f0-9]{64}$/.test(cookie.value)||
        !crypto.timingSafeEqual(identity,crypto.createHash('sha256').update(cookie.value).digest())||
        result.remember&&(!Number.isFinite(cookie.expirationDate)||cookie.expirationDate*1000>result.expiresAt+1000||cookie.expirationDate*1000<result.expiresAt-10000))throw Error('Cookie context refused');
      await bounded(session.cookies.flushStore());return result;
    }finally{identity.fill(0);for(const cookie of cookies)cookie.value='';}
  }catch{throw Error('Session transfer failed. Electron recovery is retained.');}
  finally{bytes.fill(0);clearTimeout(timer);}
}
module.exports={redeem};
