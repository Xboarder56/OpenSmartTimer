const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const vm=require('node:vm');
const {webcrypto}=require('node:crypto');
class Element {
  constructor(){this.disabled=false;this.value='';this.dataset={};this.children=[];this.listeners={};}
  addEventListener(type,fn){this.listeners[type]=fn;}
  replaceChildren(...children){this.children=children;}
  append(...children){this.children.push(...children);}
  setAttribute(name,value){this[name]=value;}
}
async function harness() {
  const elements=new Map(), requests=[], storage=new Map(), intervals=[];
  let now=1000;
  const get=id=>{if(!elements.has(id))elements.set(id,new Element());return elements.get(id);};
  get('lifetime').value='60';get('label').value='Partner';
  get('shareControls').disabled=true;get('generate').disabled=true;
  const context=vm.createContext({document:{getElementById:get,documentElement:new Element(),createElement:()=>new Element()},
    location:{hash:'#test',pathname:'/'},history:{replaceState(){}},crypto:webcrypto,AbortController,
    Date:class extends Date{static now(){return now;}},
    sessionStorage:{getItem:k=>storage.get(k)||null,setItem:(k,v)=>storage.set(k,v),removeItem:k=>storage.delete(k)},
    localStorage:{getItem:()=>null,setItem(){}},
    setTimeout:()=>1,clearTimeout(){},setInterval:fn=>intervals.push(fn),
    fetch:async(url,opts)=>{requests.push({url,opts});return {ok:true,status:200,json:async()=>({connected:false,can_share:false,links:[]})};}});
  vm.runInContext(fs.readFileSync(path.join(__dirname,'../mock-timer/creator/owner.js'),'utf8'),context);
  await new Promise(setImmediate);
  const api=vm.runInContext('({render,refresh,sharingAvailability,get canShare(){return canShare;}})',context);
  return {context,get,requests,api,intervals,advance:ms=>now+=ms};
}
test('creator starts disabled, never submits offline, and enables after relay confirmation',async()=>{
  const h=await harness();
  assert(h.get('shareControls').disabled&&h.get('generate').disabled);
  const before=h.requests.length;
  await h.get('createForm').listeners.submit({preventDefault(){}});
  assert.equal(h.requests.length,before);
  h.api.render({connected:true,can_share:true,links:[]});
  assert(!h.get('generate').disabled&&!h.get('shareControls').disabled);
  h.api.render({connected:false,can_share:false,links:[]});
  assert(h.get('generate').disabled);
  assert.match(h.get('sharingUnavailable').textContent,/Connect the timer/);
});
test('creator disables after failed or stale availability and hides copy while offline',async()=>{
  const h=await harness();
  const link={state:'ready',url:'https://relay.example/s/#test',label:'Partner',remaining_seconds:60};
  h.api.render({connected:true,can_share:true,links:[link]});
  const onlineChildren=h.get('links').children[0].children;
  assert(onlineChildren.some(e=>e.value===link.url));
  h.api.render({connected:false,can_share:false,links:[link]});
  assert(!h.get('links').children[0].children.some(e=>e.value===link.url));
  h.api.render({connected:true,can_share:true,links:[link]});h.advance(5001);h.intervals[0]();
  assert(h.get('generate').disabled&&h.get('links').children.length===0);
  h.api.render({connected:true,can_share:true,links:[]});
  h.context.fetch=async()=>{throw new Error('offline');};await h.api.refresh();
  assert(h.get('generate').disabled);
});
test('a disconnect at submit preserves request identity and keeps sharing disabled',async()=>{
  const h=await harness();h.api.render({connected:true,can_share:true,links:[]});
  h.context.fetch=async()=>({ok:false,status:503,json:async()=>({detail:'Connect the timer'})});
  await h.get('createForm').listeners.submit({preventDefault(){}});
  assert(h.get('generate').disabled);
  assert.match(h.get('message').textContent,/Connect the timer/);
});
