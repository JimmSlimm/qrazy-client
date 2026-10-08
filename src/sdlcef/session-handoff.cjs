'use strict';
// Main-process-only handoff contract. Never expose this module to website IPC.
const {performance}=require('node:perf_hooks');
const ORIGIN='https://qrazy-game.onrender.com';
const fail=()=>Error('Session transfer failed. Keep the Electron installation and retry.');
class NoSession extends Error {constructor(){super('No authenticated session is available.');}}
// Electron Session.fetch leaves Response.url empty. Requests below use a fixed
// HTTPS endpoint and redirect:error; a populated foreign URL is still refused.
function responseURL(response,endpoint){return !response.redirected&&(response.url===''||response.url===ORIGIN+endpoint);}
function object(value,keys) {
  return value && typeof value==='object' && !Array.isArray(value) &&
    Object.keys(value).sort().join(',')===keys.slice().sort().join(',');
}
function issueResponse(value) {
  if(!object(value,['code','expiresIn']) || typeof value.code!=='string' || !/^[A-Za-z0-9_-]{43}$/.test(value.code) || value.expiresIn!==30)throw fail();
  return value;
}
function redeemResponse(value,now=Date.now()) {
  if(!object(value,['expiresAt','remember']) || !Number.isSafeInteger(value.expiresAt) ||
    value.expiresAt<=now || typeof value.remember!=='boolean')throw fail();
  return {expiresAt:value.expiresAt,remember:value.remember};
}
async function json(response,limit=512) {
  if(response.status!==200 || !responseURL(response,'/auth/handoff/issue') ||
    !/^application\/json(?:\s*;|$)/i.test(response.headers.get('content-type')||''))throw fail();
  const reader=response.body.getReader();let length=0;const chunks=[];
  try {
    for(;;){const {done,value}=await reader.read();if(done)break;length+=value.byteLength;if(length>limit)throw fail();chunks.push(value);}
    const bytes=Buffer.concat(chunks.map(v=>Buffer.from(v)));
    try{return JSON.parse(new TextDecoder('utf-8',{fatal:true}).decode(bytes));}finally{bytes.fill(0);}
  }finally{await reader.cancel().catch(()=>{});for(const chunk of chunks)chunk.fill(0);}
}
async function issue(session,{signal,timeout=5000}={}) {
  // Session.fetch uses the existing Electron Chromium cookie context. Cookies
  // are never enumerated or manually copied into request headers.
  if(!Number.isSafeInteger(timeout)||timeout<1||timeout>5000)throw fail();
  const controller=new AbortController(),abort=()=>controller.abort();
  signal?.addEventListener('abort',abort,{once:true});if(signal?.aborted)abort();
  const started=performance.now(),timer=setTimeout(abort,timeout);
  try {
    const response=await session.fetch(ORIGIN+'/auth/handoff/issue',{
      method:'POST',redirect:'error',credentials:'include',cache:'no-store',
      headers:{'Content-Type':'application/json','X-Qrazy-Auth':'1','Origin':ORIGIN},
      body:'{}',signal:controller.signal});
    if(response.status===401&&responseURL(response,'/auth/handoff/issue')){await response.body?.cancel();throw new NoSession();}
    const result=issueResponse(await json(response));
    // Use request start, not receipt, so transport can never extend the lifetime.
    const remaining=30000-(performance.now()-started);
    if(remaining<5000)throw fail();
    return new Ticket(result.code,performance.now()+remaining);
  }catch(e){if(e instanceof NoSession)throw e;throw fail();}finally{clearTimeout(timer);signal?.removeEventListener('abort',abort);}
}
class Ticket {
  #bytes;#deadline;#used=false;
  constructor(code,deadline){issueResponse({code,expiresIn:30});const now=performance.now();if(!Number.isFinite(deadline)||deadline<=now||deadline>now+30000)throw fail();this.#bytes=Buffer.from(code,'ascii');this.#deadline=deadline;}
  take(now=performance.now()) {
    if(this.#used||!Number.isFinite(now)||now+3000>=this.#deadline){this.dispose();throw fail();}
    this.#used=true;const bytes=Buffer.from(this.#bytes);this.dispose();return bytes;
  }
  dispose(){this.#used=true;this.#bytes?.fill(0);this.#bytes=null;}
}
module.exports={ORIGIN,responseURL,issue,issueResponse,redeemResponse,Ticket,NoSession};
