const vm=require('node:vm'),fs=require('node:fs'),assert=require('node:assert/strict');
const path=require('node:path');
const firmware=fs.readFileSync(path.join(__dirname,'../sketch_sep23a2.ino'),'utf8');
const page=firmware.split('R"HTML(')[1].split(')HTML"')[0];
const script=page.split('<script>')[1].split('</script>')[0];
const ids=new Set([...page.matchAll(/id="([^"]+)"/g)].map(m=>m[1]));
const elements=new Map([...ids].map(id=>[id,{textContent:'',disabled:true,checked:false}]));
const el=id=>{assert.ok(elements.has(id),'Unknown element '+id);return elements.get(id)};
let data={time:'12:00:00 AM',ip:'192.168.0.180',wifi:true,ntp:true,hardware:true,
 ntpTarget:'--',ntpSource:'SIRIM',ntpServer:'ntp1.sirim.my',ntpMsg:'NTP synced: SIRIM',ntpEvent:1,
 age:0,manual:false,started:0,aborted:0,saved:true,auto:false,slotMask:7,shutdown:'04:10 MY'};
let posts=[],offline=false,pendingStatus=null;
const context={document:{getElementById:el},AbortController,setTimeout,clearTimeout,
 URLSearchParams,setInterval:()=>{},fetch:async(url,options)=>{
  if(offline)throw Error('offline');
  if(url==='/api/status'){
   if(pendingStatus)return pendingStatus;
   return {ok:true,json:async()=>({...data})};
  }
  assert.equal(url,'/api/control');assert.equal(options.method,'POST');
  assert.equal(options.headers['X-MB6-Token'],'__TOKEN__');
  const args=new URLSearchParams(options.body);posts.push(Object.fromEntries(args));
  if(args.get('cmd')==='schedule'){
   data.slotMask=Number(args.get('mask'));if(!data.slotMask)data.auto=false;
  }else if(args.get('cmd')==='auto_on'){assert.notEqual(data.slotMask,0);data.auto=true}
  else if(args.get('cmd')==='auto_off')data.auto=false;
  return {ok:true,text:async()=> 'Saved'};
 }};
vm.createContext(context);vm.runInContext(script,context);
const settle=()=>new Promise(r=>setImmediate(r));
(async()=>{
 await settle();
 assert.equal(el('ntpSource').textContent,'SIRIM (ntp1.sirim.my)');
 assert.equal(el('shutdown').textContent,'04:10 MY');
 for(let mask=0;mask<8;mask++){
  data={...data,slotMask:mask,auto:mask!==0};
  await vm.runInContext('refresh()',context);
  for(let i=0;i<3;i++){assert.equal(el('slot'+i).checked,!!(mask&(1<<i)));assert.equal(el('slot'+i).disabled,false)}
  assert.equal(el('auto').disabled,mask===0);assert.equal(el('auto').checked,mask!==0);
  assert.equal(el('start').disabled,false);
 }
 data={...data,slotMask:1,auto:true};await vm.runInContext('refresh()',context);
 el('slot0').checked=false;await el('slot0').onchange();
 assert.deepEqual(posts.at(-1),{cmd:'schedule',mask:'0'});
 assert.equal(el('auto').disabled,true);assert.equal(el('auto').checked,false);
 assert.equal(el('start').disabled,false);
 el('slot2').checked=true;await el('slot2').onchange();
 assert.deepEqual(posts.at(-1),{cmd:'schedule',mask:'4'});
 assert.equal(el('auto').disabled,false);assert.equal(el('auto').checked,false);
 el('auto').checked=true;await el('auto').onchange();assert.equal(data.auto,true);
 // A poll already in flight must not restore stale choices during a save.
 let resolvePoll;pendingStatus=new Promise(r=>{resolvePoll=r});
 const poll=vm.runInContext('refresh()',context);
 await settle();vm.runInContext('busy=true',context);el('slot0').checked=true;
 resolvePoll({ok:true,json:async()=>({...data,slotMask:0,auto:false})});
 await poll;assert.equal(el('slot0').checked,true);
 vm.runInContext('busy=false',context);pendingStatus=null;
 // The same stale response is ignored even if it arrives after save completes.
 pendingStatus=new Promise(r=>{resolvePoll=r});
 const latePoll=vm.runInContext('refresh()',context);await settle();
 el('slot2').checked=false;await el('slot2').onchange();
 assert.deepEqual(posts.at(-1),{cmd:'schedule',mask:'1'});
 resolvePoll({ok:true,json:async()=>({...data,slotMask:4,auto:true})});
 await latePoll;assert.equal(el('slot0').checked,true);assert.equal(el('slot2').checked,false);
 pendingStatus=null;await vm.runInContext('refresh()',context);
 offline=true;await vm.runInContext('refresh()',context);
 for(const id of ['auto','start','stop','ntp','slot0','slot1','slot2'])assert.equal(el(id).disabled,true);
 offline=false;await vm.runInContext('refresh()',context);assert.equal(el('slot0').disabled,false);
 data.saved=false;await vm.runInContext('refresh()',context);assert.equal(el('saved').textContent,'SAVE ERROR');
 console.log('PASS: actual Web UI, all checkbox combinations, empty/reselect flow, authenticated POST, stale-poll race, offline recovery and save warning.');
})().catch(e=>{console.error(e);process.exit(1)});
