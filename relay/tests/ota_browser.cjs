// Real Wi-Fi/Bluetooth HTML, styles, updater and existing dialog callbacks.
// Disposable signed feed and device adapter; never accesses or flashes hardware.
'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),http=require('node:http');
const {createHash,generateKeyPairSync,sign}=require('node:crypto'),{spawn}=require('node:child_process');
const {chromium}=require('playwright'),root=path.resolve(__dirname,'../..');
const out=path.join(root,'tmp/relay-tests/ota-dialog');fs.mkdirSync(out,{recursive:true});
const image=Buffer.alloc(24,0xe9),{privateKey,publicKey}=generateKeyPairSync('ec',{namedCurve:'prime256v1'});
const jwk=publicKey.export({format:'jwk'}),publicHex=Buffer.concat([Buffer.from([4]),Buffer.from(jwk.x,'base64url'),Buffer.from(jwk.y,'base64url')]).toString('hex');
const record={target:'magbound_v2_esp32c3',release_version:'1.1.0',image_size:24,image_crc32:'',image_sha256:createHash('sha256').update(image).digest('hex'),file:'OpenSmartTimer-1.1.0-MagBoundV2-ESP32C3.bin',release_notes:'Changes\n• Hardened relay communication.\n• Improved firmware updates.\n<script>window.notesExecuted=true</script>\n\n'+('More changes with a long line for layout validation. '.repeat(100))};
let crc=0xffffffff;for(const byte of image){crc^=byte;for(let b=0;b<8;b++)crc=(crc>>>1)^(0xedb88320&-(crc&1));}record.image_crc32=((crc^0xffffffff)>>>0).toString(16).padStart(8,'0');
const canonical='OpenSmartTimer-Update-v1\n'+record.target+'\n'+record.release_version+'\n'+record.image_size+'\n'+record.image_crc32+'\n'+record.image_sha256+'\n';
record.release_signature=sign('sha256',Buffer.from(canonical),{key:privateKey,dsaEncoding:'ieee-p1363'}).toString('hex');
const device={version:'1.0.0',armed:false,state:'open',updates:{configured:true,target:record.target,base_url:'https://example-owner.github.io/OpenSmartTimer/',public_key:publicHex}};
let preview,previewPort;
const server=http.createServer((req,res)=>{
 const pathname=new URL(req.url,'http://localhost').pathname;
 const file=path.resolve(root,'.'+pathname);
 if(!file.startsWith(root+'/')||!/^\/(web|bluetooth-app)\//.test(pathname)){res.writeHead(404);res.end();return;}
 const target=pathname.endsWith('/')?path.join(file,'index.html'):file;
 try{res.writeHead(200,{'Content-Type':target.endsWith('.js')||target.endsWith('.mjs')?'text/javascript':target.endsWith('.css')?'text/css':'text/html'});res.end(fs.readFileSync(target));}catch{res.writeHead(404);res.end();}
});
(async()=>{
 await new Promise(r=>server.listen(0,'127.0.0.1',r));
 // Let the preview pick a separate ephemeral port by first reserving one.
 const probe=http.createServer();await new Promise(r=>probe.listen(0,'127.0.0.1',r));previewPort=probe.address().port;await new Promise(r=>probe.close(r));
 preview=spawn(process.env.PYTHON||'python3',[path.join(root,'tools/preview_ui.py'),'--port',String(previewPort),'--v2'],{cwd:root,stdio:'ignore'});
 const wifi='http://127.0.0.1:'+previewPort,ble='http://127.0.0.1:'+server.address().port;
 let ready=false;for(let i=0;i<100;i++){try{if((await fetch(wifi+'/api/v1/status')).ok){ready=true;break;}}catch{}await new Promise(r=>setTimeout(r,50));}assert(ready);
 const browser=await chromium.launch({headless:true}),checks=[];
 try{
  for(const [name,url,dialog,cancel,accept,message] of [
   ['wifi',wifi+'/', '#confirmDialog','#confirmCancel','#confirmAccept','#confirmMessage'],
   ['bluetooth',ble+'/bluetooth-app/','#mgmtConfirm','#mgmtConfirmCancel','#mgmtConfirmAccept','#mgmtConfirmMessage']
  ]){
   const ctx=await browser.newContext({viewport:{width:390,height:844}}),page=await ctx.newPage(),errors=[];let feedStatus=404,downloads=0;
   page.on('pageerror',e=>errors.push(e.message));
   await page.route('**/web/updates.js',inject);await page.route('**/updates.js',inject);
   async function inject(route){
    const response=await route.fetch();const source=await response.text();
    await route.fulfill({response,body:source+'\nconst realPanel=SmartTimerUpdates.panel;SmartTimerUpdates.panel=options=>realPanel({...options,device:async()=>window.otaTestDevice,install:async(file,meta,report)=>{window.otaInstalled={size:file.size,meta,reports:typeof report==="function"};if(report)report("Update installed. The device is restarting…","ok");}});'});
   }
   await page.addInitScript(d=>{window.otaTestDevice=d;},device);
   if(name==='bluetooth')await page.route('**/bluetooth-app/app.js',async route=>{const response=await route.fetch();await route.fulfill({response,body:(await response.text())+'\nrefreshSystemInfo=async()=>{};maintainLiveStatus=async()=>{};window.showTestBLESettings=()=>{app.server={connected:true,disconnect(){}};app.authenticated=true;app.live={state:"open",controls_locked:false};renderManagementAvailability();};'});});
   await page.route('https://example-owner.github.io/OpenSmartTimer/updates/**',route=>{
    if(route.request().url().endsWith('.json'))return route.fulfill({status:feedStatus,contentType:'application/json',body:JSON.stringify(record)});
    downloads++;return route.fulfill({status:200,contentType:'application/octet-stream',body:image});
   });
   await page.route('https://api.github.com/repos/example-owner/OpenSmartTimer/releases?per_page=1',route=>route.fulfill({status:name==='wifi'?404:200,contentType:'application/json',body:name==='wifi'?'{"message":"Not Found"}':'[]'}));
   await page.goto(url);await page.locator('#settingsOpen').click();
   if(name==='bluetooth')await page.evaluate(()=>window.showTestBLESettings());
   await page.locator('[data-update-check]').scrollIntoViewIfNeeded();
   await page.locator('[data-update-check]').click();
   await page.waitForFunction(()=>document.querySelector('[data-update-message]').textContent==='No updates available.');
   assert.equal(await page.locator(dialog).getAttribute('aria-hidden'),'true');assert.equal(downloads,0);
   assert.equal(await page.locator('[data-update-channel]').count(),0);assert.equal(await page.locator('[data-update-install]').count(),0);
   feedStatus=200;await page.locator('[data-update-check]').click();await page.locator(dialog).waitFor({state:'visible'});
   await page.waitForFunction(selector=>getComputedStyle(document.querySelector(selector)).opacity==='1',dialog);
   assert.match(await page.locator(message).textContent(),/<script>/);assert.equal(await page.evaluate(()=>window.notesExecuted),undefined);
   assert.equal(await page.locator(accept).textContent(),'Install');assert.equal(await page.locator(cancel).textContent(),'Cancel');
   // A stable version is not labelled a pre-release; the Reset reminder has its own line.
   assert.equal(await page.locator(name==='wifi'?'#confirmEyebrow':'#mgmtConfirmEyebrow').textContent(),'Firmware update');
   assert.match(await page.locator(name==='wifi'?'#confirmSafety':'#mgmtConfirmSafety').textContent(),/^Provided as is, without warranty\. Open the timer and briefly press Reset/);
   await page.waitForFunction(selector=>document.activeElement.id===document.querySelector(selector).id,cancel,{timeout:2000});
   for(const width of [320,390,1280]){
    await page.setViewportSize({width,height:844});
    const box=await page.locator(dialog).boundingBox();assert(box.x>=0&&box.y>=0&&box.x+box.width<=width+1&&box.y+box.height<=845,'dialog fits '+name+' '+width);
    assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);
    await page.screenshot({path:path.join(out,name+'-'+width+'.png'),fullPage:true});
   }
   await page.locator(cancel).click();await page.waitForFunction(()=>document.querySelector('[data-update-message]').textContent==='Update cancelled.');
   assert.equal(downloads,0);assert.equal(await page.evaluate(()=>window.otaInstalled),undefined);
   await page.locator('[data-update-check]').click();await page.locator(accept).click();
   await page.waitForFunction(()=>document.querySelector('[data-update-message]').textContent.includes('briefly press Reset'));
   assert.equal(downloads,0);
   await page.evaluate(()=>window.otaTestDevice.armed=true);
   await page.locator('[data-update-check]').click();await page.locator(accept).click();await page.waitForFunction(()=>window.otaInstalled);
   assert.equal(downloads,1);assert.equal(await page.evaluate(()=>window.otaInstalled.size),image.length);
   assert.equal(await page.evaluate(()=>window.otaInstalled.meta.release_signature),record.release_signature);
   // An online update reports its whole progress on the Online update line.
   assert.equal(await page.evaluate(()=>window.otaInstalled.reports),true,'the panel passes its own progress line to the install');
   assert.equal(await page.locator('[data-update-message]').textContent(),'Update installed. The device is restarting…');
   if(name==='wifi'){
    assert.equal(await page.locator('#otaMessage').textContent(),'','file-install line stays empty during an online update');
    assert.equal(await page.locator('#otaButton').isDisabled(),true,'Install update needs a chosen file');
   }
   assert.deepEqual(errors,[]);checks.push(name+': no releases, plain notes, responsive modal, Cancel, physical arm, signed Install');await ctx.close();
  }
  fs.writeFileSync(path.join(out,'RESULT.json'),JSON.stringify({passed:true,checks,scope:'synthetic signed browser fixtures; no hardware flash'},null,2)+'\n');console.log('PASS: '+checks.join('; '));
 }finally{await browser.close();preview.kill();server.closeAllConnections();await new Promise(r=>server.close(r));}
})().catch(e=>{console.error(e.stack);preview?.kill();server.closeAllConnections();server.close();process.exitCode=1;});
