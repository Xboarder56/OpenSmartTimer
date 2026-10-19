import {test} from 'node:test';
import assert from 'node:assert/strict';
import vm from 'node:vm';
import {readFileSync} from 'node:fs';

const source = readFileSync(new URL('../../bluetooth-app/service-worker.js', import.meta.url), 'utf8');
function harness() {
  const handlers = {};
  const records = new Map();
  const origin = 'https://example.github.io';
  const scope = origin + '/timer/bluetooth-app/';
  let failInstall = false;
  const cache = {match: async url => records.get(url), addAll: async requests => {
    if (failInstall) throw Error('interrupted download');
    for (const req of requests) records.set(req.url, new Response('CURRENT ' + req.url));
  }};
  const context = vm.createContext({URL, Request, Response,
    self: {registration:{scope}, location:{origin}, addEventListener:(name,fn)=>handlers[name]=fn},
    caches:{open:async()=>cache,keys:async()=>[],delete:async()=>true},
    fetch:()=>{throw Error('must not fetch a different shell version');},
  });
  vm.runInContext(source, context);
  const install = () => {let promise; handlers.install({waitUntil:p=>promise=p});return promise;};
  const get = (path, mode='cors') => {
    let promise;
    handlers.fetch({request:{method:'GET',url:new URL(path,scope).href,mode},respondWith:p=>promise=p});
    return promise;
  };
  return {install,get,records,fail:()=>failInstall=true,scope};
}
test('active offline shell never mixes new network HTML with old cached JavaScript', async()=>{
  const h=harness(); await h.install();
  for(const [path,mode] of [['./','navigate'],['app.js','cors'],['../web/styles.css','cors'],['../web/appearance.js','cors']]) {
    const response=await h.get(path,mode); assert.equal(response.status,200);
    assert.match(await response.text(),/^CURRENT /);
  }
  assert.equal(h.get('not-an-app-asset.txt'),undefined);
  assert.doesNotMatch(source,/self\.skipWaiting\(/);
});
test('failed install cannot promote files; a missing current asset fails closed',async()=>{
  const h=harness(); h.fail(); await assert.rejects(h.install(),/interrupted/);
  assert.equal(h.records.size,0);
  assert.equal((await h.get('app.js')).status,503);
});

test('offline-ready label requires an active shell, not an in-progress first install', async()=>{
  const app = readFileSync(new URL('../../bluetooth-app/app.js', import.meta.url), 'utf8');
  const fn = app.match(/^async function registerOfflineApp\(\) \{[\s\S]*?^\}/m)[0];
  const handlers = {};
  let label = '', finishReady;
  const worker = {state:'installing', addEventListener:(name,callback)=>handlers[name]=callback};
  const registration = {active:null, waiting:null, installing:worker,
    addEventListener:(name,callback)=>handlers[name]=callback};
  const context = vm.createContext({navigator:{serviceWorker:{
    register:async()=>registration, ready:new Promise(resolve=>{finishReady=resolve;})}},
    t:text=>text, setText:(id,text)=>{label=text;}, log:()=>{},
  });
  vm.runInContext(fn,context);
  const pending = context.registerOfflineApp();
  await new Promise(setImmediate);
  handlers.updatefound(); handlers.statechange();
  assert.equal(label,'Preparing cache…');
  worker.state='redundant'; handlers.statechange();
  assert.equal(label,'Cache unavailable');
  registration.active={}; finishReady(registration); await pending;
  assert.equal(label,'Ready offline');
  registration.waiting={}; handlers.statechange();
  assert.equal(label,'Update ready. Close all app windows and reopen.');
});
