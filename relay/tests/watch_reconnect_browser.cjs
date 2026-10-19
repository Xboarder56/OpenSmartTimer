// Actual trusted SPA + native EventSource, with a synthetic endpoint performing
// real authenticated ECDH/GCM. HTTPS relay and localhost page have distinct origins.
'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const http=require('node:http'),https=require('node:https'),{execFileSync}=require('node:child_process');
const {createECDH,createPrivateKey,sign,randomBytes,hkdfSync,createHmac,createHash,createCipheriv,createDecipheriv}=require('node:crypto');
const playwright=require('playwright');
// An explicit selector also works in shells without POSIX ENV assignments.
const args=process.argv.slice(2);
if(args.length>1 || (args.length && !/^--browser=(chromium|webkit)$/.test(args[0]))) {
 throw Error('Usage: node relay/tests/watch_reconnect_browser.cjs [--browser=chromium|webkit]');
}
const engine=args.length?args[0].slice('--browser='.length):(process.env.OSTRELAY_BROWSER||'chromium');
if(!['chromium','webkit'].includes(engine))throw Error('Unsupported browser: '+engine);
const root=path.resolve(__dirname,'../..'),spa=path.join(root,'relay/spa');
const output=path.join(root,'tmp/relay-tests/watch-reconnect',engine);fs.mkdirSync(output,{recursive:true});
const cert=path.join(output,'cert.pem'),tlsKey=path.join(output,'key.pem');
execFileSync('openssl',['req','-x509','-newkey','rsa:2048','-nodes','-days','1','-subj','/CN=localhost','-keyout',tlsKey,'-out',cert],{stdio:'ignore'});
fs.chmodSync(tlsKey,0o600);
const order=0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551n;
const signing=createECDH('prime256v1');signing.generateKeys();const point=signing.getPublicKey();
const key=createPrivateKey({format:'jwk',key:{kty:'EC',crv:'P-256',d:signing.getPrivateKey().toString('base64url'),x:point.subarray(1,33).toString('base64url'),y:point.subarray(33).toString('base64url')}});
const fp=randomBytes(16),hash=createHash('sha256').update(fp).digest().subarray(0,16),tid='synthetic-timer',boot='aabbccdd';
const token='ost-cap.v1.'+Buffer.from(JSON.stringify({aud:'ost-relay/1',tid,fp:fp.toString('base64url'),boot,vk:signing.getPublicKey(null,'compressed').toString('base64url'),sc:['view','start','release','adjust','pause_resume','lock','identify'],exp_up_ms:86400000,mint_up_ms:0})).toString('base64url')+'.'+randomBytes(16).toString('base64url');
const auth=Buffer.from(hkdfSync('sha256',fp,Buffer.from('OST-RELAY-E2E-AUTH-v1'),Buffer.alloc(0),32));
const mac=data=>createHmac('sha256',auth).update(data).digest();
const be=n=>{const b=Buffer.alloc(8);b.writeBigUInt64BE(BigInt(n));return b;};
let sequence=0,channel,phase='online',watches=0,unavailable=0,mutations=0,syncs=0,opens=0,trustedOrigin,sseFirstStatuses=0;
const streams=new Set(),started=Date.now(),handle=randomBytes(16).toString('base64url');
let timerState='open',session=null,remaining=0,duration=0,sealed=false,dropNextResult=false;
const savedResults=new Map(),commands=[];let lookups=0;
const uptime=()=>Date.now()-started;
const snapshot=()=>({boot_id:boot,session_id:session,state:timerState,uptime_ms:uptime(),remaining_ms:remaining,session_duration_ms:duration,max_duration_ms:86400000,sealed,emergency_input_active:false,state_revision:1,share_link_expires_up_ms:86400000});
function identitySignature(message){
 let signature=sign('sha256',message,{key,dsaEncoding:'ieee-p1363'});
 const s=BigInt('0x'+signature.subarray(32).toString('hex'));
 if(s>order>>1n)signature=Buffer.concat([signature.subarray(0,32),Buffer.from((order-s).toString(16).padStart(64,'0'),'hex')]);
 return signature.toString('base64url');
}
function responseFrame(type,body){
 return {type,upstream_seq:++sequence,body_json:JSON.stringify(body)};
}
const statusResponse=()=>responseFrame('status',snapshot());
function execute(body){
 commands.push(body);
 let code='applied',changed=true;
 if(body.expiry_uptime_ms<uptime())code='expired';
 else if(body.if_boot_id!==boot||body.if_session_id!==session)code='precondition_failed';
 else if(sealed&&body.op!=='identify')code='sealed';
 else if(body.op==='start'&&timerState==='open'){
  remaining=duration=body.params.duration_ms;timerState='running';session='synthetic-session-'+commands.length;
 }else if(body.op==='adjust'&&timerState!=='open'){
  remaining=Math.max(0,remaining+body.params.delta_ms);if(!remaining){timerState='open';session=null;}
 }else if(body.op==='pause'&&timerState==='running')timerState='paused';
 else if(body.op==='resume'&&timerState==='paused')timerState='running';
 else if(body.op==='lock_controls'&&timerState==='running')sealed=true;
 else if(body.op==='release'){timerState='open';session=null;remaining=0;sealed=false;}
 else code='invalid_state';
 if(code!=='applied')changed=false;
 const result={op:body.op,op_id:body.op_id,code,mutation:{action:body.op,operation_id:body.op_id,changed,duplicate:false},status:snapshot()};
 savedResults.set(body.op_id,result);return responseFrame('result',result);
}
function aad(direction,n,request){return Buffer.concat([Buffer.from('OST-RELAY-E2E-DATA-v1'),Buffer.from([direction]),hash,channel.client,channel.server,be(n),Buffer.from(request)]);}
function encrypt(inner,request=''){
 const seq=++channel.tx,nonce=Buffer.concat([Buffer.alloc(4),be(seq)]),cipher=createCipheriv('aes-256-gcm',channel.keys.subarray(32),nonce);
 cipher.setAAD(aad(2,seq,request));const data=Buffer.concat([cipher.update(JSON.stringify(inner)),cipher.final(),cipher.getAuthTag()]);
 return {type:'e2e',upstream_seq:++sequence,fp_hash:hash.toString('base64url'),client_id:channel.client.toString('base64url'),server_id:channel.server.toString('base64url'),seq,request_id:request,ciphertext:data.toString('base64url')};
}
function decrypt(frame){
 assert(frame.seq>channel.rx);const data=Buffer.from(frame.ciphertext,'base64url'),nonce=Buffer.concat([Buffer.alloc(4),be(frame.seq)]),cipher=createDecipheriv('aes-256-gcm',channel.keys.subarray(0,32),nonce);
 cipher.setAAD(aad(1,frame.seq,frame.request_id));cipher.setAuthTag(data.subarray(-16));const plain=Buffer.concat([cipher.update(data.subarray(0,-16)),cipher.final()]);channel.rx=frame.seq;
 const body=JSON.parse(plain);assert.equal(body.cap_token,token);return body;
}
function open(body){
 opens++;
 const client=Buffer.from(body.client_id,'base64url'),peer=Buffer.from(body.client_key,'base64url');
 const transcript=Buffer.concat([Buffer.from('OST-RELAY-E2E-OPEN-v1'+tid+'\0'+boot+'\0'),hash,client,peer]);
 assert.equal(body.boot_id,boot);assert.equal(body.fp_hash,hash.toString('base64url'));assert.deepEqual(Buffer.from(body.tag,'base64url'),mac(transcript));
 const ephemeral=createECDH('prime256v1');ephemeral.generateKeys();const server=randomBytes(16),serverKey=ephemeral.getPublicKey();
 const accepted=Buffer.concat([Buffer.from('OST-RELAY-E2E-ACCEPT-v1'),transcript,server,serverKey]),info=Buffer.concat([Buffer.from('OST-RELAY-E2E-KEYS-v1'),createHash('sha256').update(accepted).digest()]);
 channel={client,server,tx:0,rx:0,keys:Buffer.from(hkdfSync('sha256',ephemeral.computeSecret(peer),auth,info,64))};
 return {session_handle:handle,timer_id:tid,boot_id:boot,frame:{type:'e2e_accept',upstream_seq:++sequence,fp_hash:body.fp_hash,client_id:body.client_id,server_id:server.toString('base64url'),server_key:serverKey.toString('base64url'),tag:mac(accepted).toString('base64url'),sig:identitySignature(accepted)}};
}
async function jsonBody(req){const parts=[];for await(const b of req)parts.push(b);return JSON.parse(Buffer.concat(parts));}
const relay=https.createServer({key:fs.readFileSync(tlsKey),cert:fs.readFileSync(cert)},async(req,res)=>{
 res.setHeader('Access-Control-Allow-Origin',trustedOrigin);res.setHeader('Access-Control-Allow-Methods','GET,POST,DELETE,OPTIONS');res.setHeader('Access-Control-Allow-Headers','content-type,x-ost-session');
 if(req.method==='OPTIONS'){res.writeHead(204);res.end();return;}
 const pathname=new URL(req.url,'https://localhost').pathname;
 const reply=(code,data)=>{res.writeHead(code,{'Content-Type':'application/json'});res.end(JSON.stringify(data));};
 try{
  if(pathname==='/api/v1/watch'){
   watches++;if(phase!=='online'){unavailable++;res.writeHead(503);res.end();return;}
   // Match a proxy/browser that waits for the first body: no immediate
   // header flush or periodic data to bootstrap the client's first challenge.
   res.writeHead(200,{'Content-Type':'text/event-stream','Cache-Control':'no-store'});
   streams.add(res);res.on('close',()=>streams.delete(res));return;
  }
  if(pathname==='/api/v1/session'){
   if(phase==='revoked')return reply(403,{detail:'invalid_or_expired'});
   if(phase==='expired')return reply(401,{detail:'invalid_or_expired'});
   if(req.method==='POST')return phase==='online'?reply(200,open(await jsonBody(req))):reply(503,{detail:'timer_not_connected'});
   return reply(200,{session_handle:handle,timer_id:tid,boot_id:boot});
  }
  if(pathname==='/api/v1/command'){
   if(phase!=='online')return reply(503,{detail:'timer_not_connected'});
   // An idle device erases its traffic keys while the routing admission and
   // owner-issued capability remain valid. Opaque traffic then gets no reply.
   if(!channel)return reply(504,{detail:'timer_reply_timeout'});
   const frame=await jsonBody(req),inner=decrypt(frame);
   if(inner.type==='sync'){
    // Exercise SSE-first deterministically: the page may send its first sync
    // before its stream connects, so give the stream up to 2 s to arrive.
    for(let i=0;i<40&&streams.size===0;i++)await new Promise(r=>setTimeout(r,50));
    syncs++;const status=encrypt(statusResponse(),frame.request_id);
    // The identical encrypted response reaches SSE first, then HTTP. The
    // duplicate must not negate an already verified nonce-bound challenge.
    for(const stream of streams){stream.write('event: e2e\ndata: '+JSON.stringify(status)+'\n\n');sseFirstStatuses++;}
    return setTimeout(()=>reply(200,status),150);
   }
   if(inner.type==='lookup'){
    lookups++;const saved=savedResults.get(frame.request_id);
    return saved?reply(200,encrypt(responseFrame('result',saved),frame.request_id)):reply(404,{});
   }
   assert.equal(inner.type,'command');mutations++;
   const result=execute(inner);
   if(dropNextResult){dropNextResult=false;res.destroy();return;}
   return setTimeout(()=>reply(200,encrypt(result,frame.request_id)),400);
  }
  reply(404,{});
 }catch(e){reply(500,{detail:'synthetic_endpoint_error'});}
});
const pageServer=http.createServer((req,res)=>{
 const pathname=new URL(req.url,'http://localhost').pathname;
 if(pathname==='/s/'){res.writeHead(200,{'Content-Type':'text/html'});res.end(fs.readFileSync(path.join(spa,'index.html')));return;}
 const name=pathname.startsWith('/s/assets/')?pathname.slice(10):'';
 if(/^[a-z0-9-]+\.(js|css)$/.test(name)&&fs.existsSync(path.join(spa,name))){res.writeHead(200,{'Content-Type':name.endsWith('.js')?'text/javascript':'text/css'});res.end(fs.readFileSync(path.join(spa,name)));return;}
 res.writeHead(404);res.end();
});
(async()=>{
 await new Promise(r=>pageServer.listen(0,'127.0.0.1',r));trustedOrigin='http://127.0.0.1:'+pageServer.address().port;
 await new Promise(r=>relay.listen(0,'127.0.0.1',r));const relayOrigin='https://127.0.0.1:'+relay.address().port;
 let browser;try{
  browser=await playwright[engine].launch({headless:true});const context=await browser.newContext({ignoreHTTPSErrors:true,hasTouch:true});const page=await context.newPage(),errors=[];
  page.on('pageerror',e=>errors.push(e.message));
  await page.goto(trustedOrigin+'/s/?relay='+Buffer.from(relayOrigin).toString('base64url')+'#'+token);
  const online=()=>page.waitForFunction(()=>document.getElementById('connectionState').textContent==='online'&&state.fresh,null,{timeout:25000});
  await online();assert(syncs>0);assert(sseFirstStatuses>0,'initial status bootstraps unflushed stream and accepts SSE-first proof');
  // Real pointer capture and native scrolling edit drafts without control traffic.
  const drag = async (id, pixels) => {
   const input=page.locator('#'+id);await input.scrollIntoViewIfNeeded();const box=await input.boundingBox();
   const x=box.x+box.width/2,y=box.y+box.height/2;
   await page.mouse.move(x,y);await page.mouse.down();await page.mouse.move(x,y-pixels,{steps:4});await page.mouse.up();
  };
  const wheel = async (id, pixels) => {
   await page.locator('#'+id).hover();await page.mouse.wheel(0,pixels);
  };
  const beforeDraft=mutations;
  await drag('minutes',36);assert.equal(await page.locator('#minutes').inputValue(),'17','swipe up adds two minutes');
  await drag('minutes',-18);assert.equal(await page.locator('#minutes').inputValue(),'16','swipe down subtracts one minute');
  await wheel('minutes',-84);await page.waitForFunction(()=>document.getElementById('minutes').value==='18');
  await page.locator('#minutes').fill('0');await page.locator('#seconds').fill('59');await drag('seconds',18);
  assert.equal(await page.locator('#minutes').inputValue(),'1');assert.equal(await page.locator('#seconds').inputValue(),'0','seconds carry to minutes');
  await page.locator('#hours').fill('23');await page.locator('#minutes').fill('59');await page.locator('#seconds').fill('59');await drag('seconds',36);
  assert.equal(await page.locator('#hours').inputValue(),'24');assert.equal(await page.locator('#minutes').inputValue(),'0');assert.equal(await page.locator('#seconds').inputValue(),'0','draft clamps to authenticated timer limit');
  await page.locator('#hours').fill('0');await page.locator('#minutes').fill('0');await drag('seconds',-36);
  assert.equal(await page.locator('#seconds').inputValue(),'0','draft cannot become negative');
  await page.locator('#randomMode').check();await drag('randomMin',36);
  assert.equal(await page.locator('#randomMin').inputValue(),'12');await wheel('randomMax',84);
  await page.waitForFunction(()=>document.getElementById('randomMax').value==='28');
  await page.locator('#randomMin').fill('28');await drag('randomMin',36);
  assert.equal(await page.locator('#randomMin').inputValue(),'28','random minimum never crosses maximum');
  await page.locator('#randomMax').fill('1440');await drag('randomMax',36);
  assert.equal(await page.locator('#randomMax').inputValue(),'1440','random maximum respects timer limit');
  await page.evaluate(()=>{state.fresh=false;updateButtonsForState(state.status);});
  await page.locator('#randomMax').dispatchEvent('wheel',{deltaY:84,deltaMode:0});
  assert.equal(await page.locator('#randomMax').inputValue(),'1440','stale draft ignores even programmatic wheel events');
  await page.evaluate(()=>syncStatus());await page.locator('#randomMode').uncheck();
  await page.locator('#hours').fill('0');await page.locator('#minutes').fill('15');await page.locator('#seconds').fill('0');
  if(engine==='chromium'){
   const touch=await context.newCDPSession(page),box=await page.locator('#minutes').boundingBox();
   const x=box.x+box.width/2,y=box.y+box.height/2;
   await touch.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[{x,y}]});
   await touch.send('Input.dispatchTouchEvent',{type:'touchMove',touchPoints:[{x,y:y-36}]});
   await touch.send('Input.dispatchTouchEvent',{type:'touchEnd',touchPoints:[]});await touch.detach();
   assert.equal(await page.locator('#minutes').inputValue(),'17','native touchscreen swipe changes the same draft');
   await page.locator('#minutes').fill('15');
  }
  assert.equal(mutations,beforeDraft,'all gestures edit drafts without sending controls');
  assert.equal(await page.locator('#durationGestureHint').isVisible(),true);
  // Reproduce a real operator click late in the ten-second heartbeat cycle.
  await page.waitForTimeout(6500);
  await page.locator('[data-op="start"]').click();
  await page.waitForFunction(()=>state.awaitingCommand!==null);
  assert.equal(await page.locator('#pendingCard').isVisible(),false,'normal requests must not flash an unknown-outcome card');
  await page.waitForFunction(()=>state.status?.state==='running'&&!state.awaitingCommand);
  assert.equal(commands.at(-1).params.duration_ms,900000);
  await page.waitForTimeout(6500);
  await page.locator('[data-adjust="-1800000"]').click();
  await page.waitForFunction(()=>state.status?.state==='open'&&!state.awaitingCommand);
  assert.equal(savedResults.get(commands.at(-1).op_id).code,'applied','deduction past zero opens instead of rejecting an expired click');
  assert.equal(remaining,0);

  // Language changes retain the draft and never issue a control command.
  await page.locator('#randomMode').check();
  await page.locator('#randomMin').fill('1');await page.locator('#randomMax').fill('2');
  const beforeLanguage=mutations;
  await page.locator('#languageButton').click();
  assert.equal(await page.locator('html').getAttribute('lang'),'zh-Hans');
  assert.equal(await page.locator('[data-op="start"]').textContent(),'开始计时');
  assert.equal(await page.locator('#randomMin').inputValue(),'1');
  assert.equal(await page.locator('#randomMax').inputValue(),'2');
  assert.equal(await page.locator('#fixedDuration').isVisible(),false);
  assert.equal(mutations,beforeLanguage);
  assert.match(await page.locator('#expiryCountdown').textContent(),/小时.*分钟/,'expiry units change with the language');
  for(const width of [320,390,768,1280]){
   await page.setViewportSize({width,height:950});
   assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false,'Chinese random-duration layout fits '+width+'px');
   if(width!==768)await page.screenshot({path:path.join(output,'random-chinese-'+width+'.png'),fullPage:true});
  }
  dropNextResult=true;
  await page.locator('[data-op="start"]').click();
  await page.waitForFunction(()=>state.pendingIntents.length===0&&!state.awaitingCommand&&state.status?.state==='running');
  assert(lookups>0,'lost response is resolved by automatic read-only lookup');
  assert.equal(mutations,beforeLanguage+1,'lost random start is never replayed');
  assert(commands.at(-1).params.duration_ms>=60000&&commands.at(-1).params.duration_ms<=120000);
  assert.match(await page.locator('#lastResult').textContent(),/已执行/);
  await page.locator('#languageButton').click();
  assert.match(await page.locator('#lastResult').textContent(),/Start · applied/,'dynamic action labels return to English');

  const beforeLock=mutations;
  await page.locator('#sealButton').click();
  assert.equal(await page.locator('#confirmDialog').getAttribute('aria-hidden'),'false');
  assert.equal(await page.locator('main').evaluate(el=>el.inert),true);
  assert.equal(await page.locator('#confirmCancel').evaluate(el=>el===document.activeElement),true);
  await page.setViewportSize({width:390,height:950});
  await page.screenshot({path:path.join(output,'lock-confirm-mobile.png'),fullPage:true});
  await page.setViewportSize({width:1280,height:950});
  assert.equal(mutations,beforeLock,'opening confirmation does not send a lock');
  await page.locator('#confirmCancel').click();
  await page.waitForFunction(()=>!state.preparingCommand);
  assert.equal(mutations,beforeLock,'Cancel sends no command');
  // Controls the owner switched off on the timer are hidden from the partner; Unlock stays.
  await page.evaluate(()=>updateButtonsForState({...state.status,pause_allowed:false,control_lock_allowed:false}));
  assert.equal(await page.locator('[data-op="pause"]').isVisible(),false,'pause is hidden when switched off');
  assert.equal(await page.locator('#sealButton').isVisible(),false,'control lock is hidden when switched off');
  assert.equal(await page.locator('[data-op="release"]').isVisible(),true,'unlock stays available');
  await page.evaluate(()=>updateButtonsForState(state.status));
  assert.equal(await page.locator('[data-op="pause"]').isVisible(),true);
  await page.locator('[data-op="pause"]').click();
  await page.waitForFunction(()=>state.status?.state==='paused'&&!state.awaitingCommand);
  assert.equal(await page.locator('#sealButton').isVisible(),false,'paused timers cannot lock controls');
  await page.locator('[data-op="resume"]').click();
  await page.waitForFunction(()=>state.status?.state==='running'&&!state.awaitingCommand);
  await page.locator('#sealButton').click();await page.keyboard.press('Escape');
  await page.waitForFunction(()=>!state.preparingCommand);
  assert.equal(await page.locator('#confirmDialog').getAttribute('aria-hidden'),'true');
  await page.locator('#sealButton').click();
  await page.keyboard.press('Tab');assert.equal(await page.locator('#confirmAccept').evaluate(el=>el===document.activeElement),true);
  await page.keyboard.press('Tab');assert.equal(await page.locator('#confirmCancel').evaluate(el=>el===document.activeElement),true);
  await page.locator('#confirmAccept').click();
  await page.waitForFunction(()=>state.status?.sealed&&!state.awaitingCommand);
  assert.equal(await page.locator('#sealedNotice').isVisible(),true);
  assert.equal(await page.locator('[data-op="release"]').isVisible(),false);
  assert.equal(await page.locator('#leftRail').isVisible(),false);
  timerState='open';session=null;remaining=0;sealed=false;await page.evaluate(()=>syncStatus());
  const controlsCompleted=mutations;

  const beforeIdleRecovery=opens;
  channel=null;
  await page.evaluate(()=>{
   state.lastVerifiedWallMs=Date.now()-1229000;
   markStale('Timer status is stale · last verified snapshot',true);
  });
  assert.match(await page.locator('#connectionWarningMessage').textContent(),/1229 s ago/);
  await page.locator('#connectionRetry').click();
  await online();
  assert(opens>beforeIdleRecovery,'Retry replaces the device channel after idle expiry');
  assert.equal(mutations,controlsCompleted,'recovery never replays a control');

  const beforeTabRecovery=opens;
  const savedDraft=await page.locator('#randomMin').inputValue();
  channel=null;
  await page.evaluate(()=>{
   state.lastVerifiedWallMs=Date.now()-125000;
   state.channelCreatedWallMs=state.lastVerifiedWallMs;
   document.dispatchEvent(new Event('visibilitychange'));
  });
  await online();
  assert(opens>beforeTabRecovery,'returning to an idle tab replaces its discarded channel');
  assert.equal(await page.locator('#randomMin').inputValue(),savedDraft,'recovery preserves the duration draft');
  assert.equal(mutations,controlsCompleted);

  const beforeAutomaticRecovery=opens;
  channel=null;
  await page.evaluate(()=>{
   state.lastVerifiedWallMs=Date.now()-125000;
   state.channelCreatedWallMs=state.lastVerifiedWallMs;
  });
  // Wait for the next heartbeat; the current fresh flag predates key erasure.
  await page.waitForFunction(n=>state.channelCreatedWallMs>n,Date.now()-1000,{timeout:25000});
  await online();
  assert(opens>beforeAutomaticRecovery,'heartbeat restores a discarded channel without user interaction');
  assert.equal(mutations,controlsCompleted);

  phase='offline';
  await page.evaluate(async()=>{
   state.lastVerifiedWallMs=Date.now()-20000;
   state.channelCreatedWallMs=state.lastVerifiedWallMs;
   await syncStatus();
  });
  assert.equal(await page.evaluate(()=>state.channel===null&&!state.closed&&!state.fresh&&Boolean(state.syncTimer)),true,
   'a temporary failed handshake keeps read-only polling active');
  const beforeOfflineRecovery=opens;
  phase='online';await online();
  assert(opens>beforeOfflineRecovery,'the same valid link recovers after a failed handshake');
  assert.equal(mutations,controlsCompleted);

  phase='offline';for(const res of streams){res.write('event: timer_gone\ndata: \n\n');res.end();}
  await page.waitForFunction(()=>state.eventSource?.readyState===EventSource.CLOSED,null,{timeout:15000});
  assert(unavailable>0);assert.equal(await page.evaluate(()=>state.fresh),false);assert(await page.locator('[data-op="start"]').isDisabled());
  const before=watches;phase='online';
  await page.waitForFunction(()=>state.eventSource?.readyState===EventSource.OPEN,null,{timeout:25000});
  await online();assert(watches>before);assert.equal(mutations,controlsCompleted);
  phase='expired';
  await page.evaluate(()=>{state.tokenExpUpMs=0;state.expired=true;showLinkExpired();});
  assert.equal(await page.evaluate(()=>state.closed),true);
  const beforeReactivation=opens;phase='online';await online();
  assert(opens>beforeReactivation);assert.equal(await page.evaluate(()=>state.closed||state.expired),false);
  assert.equal(await page.evaluate(()=>state.tokenExpUpMs),86400000);assert.equal(mutations,controlsCompleted);
  phase='revoked';await page.evaluate(()=>{state.lastBindingCheckMs=0;});for(const res of streams)res.end();
  await page.waitForFunction(()=>state.closed,null,{timeout:15000});const stopped=watches;await page.waitForTimeout(4000);
  assert.equal(watches,stopped);assert.equal(mutations,controlsCompleted);assert.deepEqual(errors,[]);
  fs.writeFileSync(path.join(output,'RESULT-'+engine+'.json'),JSON.stringify({passed:true,engine,commands:commands.length,lookups,watches,unavailable,syncs,opens,mutations,checks:['native-draft-swipe-and-wheel','duration-carry-and-authenticated-limit','random-range-gestures','stale-gestures-send-no-controls','fresh-click-deadline-after-six-seconds','deduct-past-zero-opens','no-unconfirmed-flash-in-flight','automatic-read-only-reconciliation','random-start-picked-once','chinese-draft-and-dynamic-result-preserved','lock-confirm-cancel-and-escape','paused-lock-hidden','lock-confirm-focus-trap-and-accept','real-ecdh-gcm','native-http-503-closes-stream','stale-controls-disabled','idle-channel-retry-replaces-keys','idle-tab-recovers-with-draft-preserved','automatic-idle-channel-recovery','automatic-reconnect-without-refresh','fresh-challenge-restores-controls','reactivation-refreshes-expired-deadline-before-reopening','no-control-replay','terminal-auth-cancels-watch-retry'],page_errors:0},null,2)+'\n');
  console.log('PASS '+engine+': control UI and authenticated status; idle keys recover by Retry, tab return and heartbeat; failed handshake/outage and expiry recover without control replay; terminal watch retries stop.');
 }finally{if(browser)await browser.close();for(const res of streams)res.destroy();relay.closeAllConnections();pageServer.closeAllConnections();await Promise.all([new Promise(r=>relay.close(r)),new Promise(r=>pageServer.close(r))]);}
})().catch(e=>{console.error(e.stack);process.exitCode=1;});
