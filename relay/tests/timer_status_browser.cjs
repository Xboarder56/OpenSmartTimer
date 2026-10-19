// Isolated timer UI relay-availability regression. Never contacts real hardware.
const assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const root=path.resolve(__dirname,'../..');
const net=require('node:net'),{spawn}=require('node:child_process');
const out=path.join(root,'tmp/relay-tests/timer-relay-status');fs.mkdirSync(out,{recursive:true});
async function preview(){
 const socket=net.createServer();await new Promise(r=>socket.listen(0,'127.0.0.1',r));const port=socket.address().port;await new Promise(r=>socket.close(r));
 const child=spawn(process.env.PYTHON||'python3',[path.join(root,'tools/preview_ui.py'),'--port',String(port),'--v2'],{cwd:root,stdio:'ignore'});
 const origin='http://127.0.0.1:'+port;
 try{for(let i=0;i<150;i++){try{const r=await fetch(origin+'/api/v1/status');if(r.ok)return {child,origin};}catch{}await new Promise(r=>setTimeout(r,100));}throw new Error('isolated preview did not start');}catch(e){child.kill();throw e;}
}
const playwright=require(path.join(root,'relay/tests/node_modules/playwright'));
const engine=process.env.OSTRELAY_BROWSER||'chromium';
(async()=>{
 const {child,origin}=await preview();
 const base=await (await fetch(origin+'/api/v1/relay')).json();
 let fixture={...base,enrolled:true,enabled:true,running:true,state:'connected',sharing_available:true,relay_host:'api.example.invalid',partner_origin:'https://control.example.invalid'};
 let failure=0, posts=0, reads=0, linkReads=0;
  const browser=await playwright[engine].launch({headless:true});
 try{
  const ctx=await browser.newContext({viewport:{width:390,height:950}});
  const page=await ctx.newPage(),errors=[];page.on('pageerror',e=>errors.push(e.message));
  await page.route('**/api/v1/relay',route=>{reads++;return route.fulfill({status:failure||200,contentType:'application/json',body:JSON.stringify(failure?{code:'relay_busy',error:'temporary test failure'}:fixture)});});
  await page.route('**/api/v1/relay/links',route=>{
   if(route.request().method()==='POST')posts++;else linkReads++;
   return route.fulfill({status:200,contentType:'application/json',body:JSON.stringify({links:[]})});
  });
  await page.goto(origin+'/');
  const badge=page.locator('#relayStatusBadge');
  const label=async text=>page.waitForFunction(text=>document.getElementById('relayStatusBadge').textContent===text&&!document.getElementById('relayStatusBadge').classList.contains('hidden'),text,{timeout:15000});
  await label('RELAY ONLINE');
  assert.equal(await page.locator('#settingsPanel').getAttribute('aria-hidden'),'true');
  assert.equal(linkReads,0,'main-page status must not fetch private link URLs/lists');
  fixture={...fixture,state:'backoff',sharing_available:false};
  await label('RELAY OFFLINE');
  assert.equal(await badge.evaluate(el=>{const sample=document.createElement('span');sample.style.color='var(--red)';el.append(sample);const match=getComputedStyle(el).color===getComputedStyle(sample).color;sample.remove();return match;}),true,'offline switches directly to the error color');
  assert.equal(await page.locator('#settingsPanel').getAttribute('aria-hidden'),'true','offline status updates outside Settings');
  for(const palette of ['purple','slate','teal'])for(const theme of ['light','dark']){
   await page.evaluate(({palette,theme})=>{document.documentElement.dataset.palette=palette;document.documentElement.dataset.theme=theme;},{palette,theme});
   assert.equal(await badge.evaluate(el=>{const sample=document.createElement('span');sample.style.color='var(--red)';el.append(sample);const match=getComputedStyle(el).color===getComputedStyle(sample).color;sample.remove();return match;}),true,'offline is red in '+palette+' '+theme);
  }
  await page.evaluate(()=>{document.documentElement.dataset.palette='purple';document.documentElement.dataset.theme='light';});
  for(const width of [320,390,768,1280]){
   await page.setViewportSize({width,height:950});
   assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false,'no horizontal overflow '+width);
   const spacing=await page.locator('#stateBadge').evaluate(el=>{
    const range=document.createRange();range.selectNodeContents(el);
    const text=range.getBoundingClientRect(),badge=el.getBoundingClientRect();
    return {label:el.textContent,left:text.left-badge.left,right:badge.right-text.right,
     top:text.top-badge.top,bottom:badge.bottom-text.bottom};
   });
   assert.equal(spacing.label,'OPEN');
   assert(spacing.left>=15&&spacing.right>=14&&spacing.top>0&&spacing.bottom>0,'Open label has space inside every edge at '+width+'px');
  }
  await page.setViewportSize({width:390,height:950});
  await page.screenshot({path:path.join(out,'relay-offline-main.png'),fullPage:true});
  await badge.click();
  await page.locator('#relaySection').scrollIntoViewIfNeeded();
  assert.equal(await page.locator('#relayCreateButton').isDisabled(),true);
  assert.match(await page.locator('#relayCreateHint').textContent(),/once the relay is connected/);
  await page.locator('#relayCreateForm').evaluate(form=>form.requestSubmit());
  assert.equal(posts,0,'programmatic submit while offline sends no create request');
  await page.screenshot({path:path.join(out,'relay-offline-settings.png'),fullPage:true});
  fixture={...fixture,state:'connected',sharing_available:true};
  await label('RELAY ONLINE');
  await page.waitForFunction(()=>!document.getElementById('relayCreateButton').disabled);
  failure=503;await label('RELAY UNAVAILABLE');
  assert.equal(await badge.evaluate(el=>{const sample=document.createElement('span');sample.style.color='var(--red)';el.append(sample);const match=getComputedStyle(el).color===getComputedStyle(sample).color;sample.remove();return match;}),true,'unavailable uses the error color');
  assert.equal(await page.locator('#relayCreateButton').isDisabled(),true);
  assert.match(await page.locator('#relayCreateHint').textContent(),/status is unavailable/);
  failure=0;await label('RELAY ONLINE');
  await page.waitForFunction(()=>!document.getElementById('relayCreateButton').disabled);
  fixture={...fixture,paused_for_bluetooth:true,sharing_available:false};
  await label('RELAY PAUSED');assert.equal(await page.locator('#relayCreateButton').isDisabled(),true);
  fixture={...fixture,paused_for_bluetooth:false,enabled:false,state:'disabled'};
  await label('RELAY OFF');assert.equal(await page.locator('#relayCreateButton').isDisabled(),true);
  fixture={...fixture,enrolled:false};
  await page.waitForFunction(()=>document.getElementById('relayStatusBadge').classList.contains('hidden'),null,{timeout:15000});
  assert.deepEqual(errors,[]);
  console.log('PASS '+engine+': main-page relay polling, offline/unavailable/recovery/pause/off states, disabled and guarded creation, Open badge padding, 320–1280px layout, no browser errors.');
  fs.writeFileSync(path.join(out,'RESULT-'+engine+'.json'),JSON.stringify({passed:true,engine,relayReads:reads,creationRequests:posts,unexpectedErrors:errors},null,2));
 }finally{await browser.close();child.kill();}
})().catch(e=>{console.error(e);process.exitCode=1;});
