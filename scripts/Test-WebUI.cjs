// Runs actual application JavaScript with a fake DOM and fetch. No device or network.
const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm'),assert=require('node:assert/strict');
const root=path.resolve(__dirname,'..');
function harness(variant){
 const elements=new Map(),timers=new Map();let next=0;
 function element(id){if(!elements.has(id))elements.set(id,{value:'',textContent:'',className:'',hidden:false,dataset:{},style:{},type:'password',listeners:{},classList:{add(){},remove(){},toggle(){}},addEventListener(e,f){this.listeners[e]=f},setAttribute(){},removeAttribute(){},checkValidity(){return true}});return elements.get(id)}
 const c=vm.createContext({document:{body:{children:[]},documentElement:{dataset:{}},hidden:true,getElementById:element,querySelectorAll:()=>[],querySelector:()=>null},window:{fetch:()=>Promise.reject(Error('unexpected fetch')),scrollTo(){}},sessionStorage:{getItem:()=>null,setItem(){},removeItem(){}},localStorage:{getItem:()=>null,setItem(){}},setInterval(){},setTimeout(fn,ms){let id=++next;timers.set(id,{fn,ms,active:true});return id},clearTimeout(id){if(timers.has(id))timers.get(id).active=false},AbortController,TextEncoder,URL,Blob,confirm:()=>true,navigator:{},console});
 let source=fs.readFileSync(path.join(root,'firmware/controller/main',variant,'app.js'),'utf8');
 source=source.replace(/\}\)\(\);\s*$/,'globalThis.test={request,post,loadSupla,login,addDevice,renderDiscovery,normalizePayload,updatePairingStatus,liveMarkup,loadDeviceTelemetry,controlRelay,pendingCount(){return Object.keys(relayPending).length},setDevices(v){deviceCache=v},setAuthorized(value){authorized=value},scanEpoch(){return discoveryEpoch},setToken(t){panelToken=t},authorized(){return authorized}};})();');
 vm.runInContext(source,c);return {c,elements,timers,element};
}
async function flush(){for(let n=0;n<30;n++)await Promise.resolve()}
function response(status,obj){return {ok:status>=200&&status<300,status,text:()=>Promise.resolve(JSON.stringify(obj))}}
(async()=>{
 let results=[];
 for(const variant of ['www']){
  const html=fs.readFileSync(path.join(root,'firmware/controller/main',variant,'index.html'),'utf8');assert.doesNotMatch(html,/id="live-overview"/);assert.match(html,/grid-template-columns:12px minmax\(0,1fr\) auto/);const {c,elements,timers,element}=harness(variant),token='a'.repeat(64);c.test.setToken(token);
  let sent;
  c.window.fetch=(u,o)=>{sent=o;return Promise.resolve(response(200,{ok:true}))};
  await c.test.post('/api/system/reboot',{});assert.equal(sent.headers.Authorization,'Bearer '+token);
  c.window.fetch=()=>Promise.resolve(response(500,{ok:false,message:'NVS failed'}));
  element('btn-reboot').listeners.click();await flush();assert.equal(element('sys-msg').className,'msg err');assert.equal(element('sys-msg').textContent,'NVS failed');
  c.window.fetch=()=>Promise.resolve(response(200,{ok:false,message:'refused'}));
  element('btn-reboot').listeners.click();await flush();assert.equal(element('sys-msg').className,'msg err');assert.equal(element('sys-msg').textContent,'refused');
  c.window.fetch=()=>Promise.resolve({ok:true,status:200,text:()=>new Promise(()=>{})});
  const pending=c.test.request('/api/status');await flush();
  const active=[...timers.values()].filter(t=>t.ms===12000&&t.active);assert.equal(active.length,1,'Timeout must remain active while body stalls');
  active[0].fn();await assert.rejects(pending,/czas odpowiedzi/);
  c.window.fetch=(u,o)=>{sent=JSON.parse(o.body);return Promise.resolve(response(200,{ok:true}))};
  const ssid=' '+ 'x'.repeat(30)+' ';element('wifi-ssid').value=ssid;element('wifi-pass').value='a'.repeat(64);element('btn-wifi-connect').listeners.click();await flush();assert.equal(sent.ssid,ssid);assert.equal(sent.pass.length,64);
  c.window.fetch=()=>Promise.resolve(response(200,{state:'connected',server:'server',email:'x@y',guid:'f'.repeat(32),authkey_set:true}));
  element('supla-authkey').value='';c.test.loadSupla();await flush();assert.equal(element('supla-authkey').value,'','Polling must not retrieve AuthKey');
  let finish;
  c.window.fetch=()=>new Promise(resolve=>{finish=resolve});
  const oldRequest=c.test.request('/api/supla/reveal_key');
  element('panel-logout').listeners.click();
  finish(response(200,{ok:true,authkey:'secret'}));
  await assert.rejects(oldRequest,/Sesja/);
  c.test.setToken(token);
  c.window.fetch=()=>Promise.resolve(response(401,{message:'unauthorized'}));
  await assert.rejects(c.test.request('/api/status'),/token/);assert.equal(element('panel-login').hidden,false);
  await assert.rejects(c.test.request('/api/status'),/Zaloguj/);
  c.test.setToken('admin');
  c.window.fetch=()=>Promise.resolve(response(200,{setup_required:true}));
  c.test.login();await flush();
  assert.equal(element('panel-setup').hidden,false);
  assert.equal(c.test.authorized(),false,'Bootstrap must not open application');
  element('panel-setup-new').value='newPassword123';
  element('panel-setup-repeat').value='different';
  let calls=0;c.window.fetch=()=>{calls++;return Promise.resolve(response(200,{ok:true}))};
  element('btn-panel-setup').listeners.click();await flush();assert.equal(calls,0);
  element('panel-setup-repeat').value='newPassword123';
  c.window.fetch=()=>Promise.resolve(response(500,{ok:false,message:'NVS failed'}));
  element('btn-panel-setup').listeners.click();await flush();
  assert.equal(element('panel-setup').hidden,false,'NVS failure must keep setup open');
  assert.equal(element('panel-login-msg').textContent,'NVS failed');
  c.window.fetch=(u,o)=>{assert.equal(u,'/api/panel/password');assert.equal(o.headers.Authorization,'Bearer admin');assert.equal(JSON.parse(o.body).password,'newPassword123');return Promise.resolve(response(200,{ok:true}))};
  element('btn-panel-setup').listeners.click();await flush();
  assert.equal(element('panel-login').hidden,false);
  assert.equal(element('panel-setup-new').value,'');
  await assert.rejects(c.test.request('/api/status'),/Zaloguj/);
  c.test.setToken('newPassword123');
  c.window.fetch=()=>Promise.resolve(response(200,{system:{},wifi:{}}));
  c.test.login();await flush();assert.equal(c.test.authorized(),true);
  element('panel-password-new').value='changedPassword123';element('panel-password-repeat').value='changedPassword123';
  c.window.fetch=()=>Promise.resolve(response(500,{ok:false,message:'NVS failed'}));
  element('btn-panel-password').listeners.click();await flush();assert.equal(c.test.authorized(),true);
  c.window.fetch=()=>Promise.resolve(response(200,{ok:true}));
  element('btn-panel-password').listeners.click();await flush();
  assert.equal(c.test.authorized(),false);assert.equal(element('panel-password-new').value,'');
  c.test.setToken(token);
  assert.equal(c.test.normalizePayload('12345-67890-12345-678901'),'123456789012345678901');
  assert.equal(c.test.normalizePayload('12345678901'),'12345678901');
  c.test.setAuthorized(true);
  for (const network of ['thread','wifi','lan']) {
   element('pair-network').value=network;
   element('pair-ssid').value=' network ';
   element('pair-pass').value='testPass123';
   c.window.fetch=(u,o)=>{
    assert.equal(u,'/api/matter/pair');
    sent=JSON.parse(o.body);
    return Promise.resolve(response(409,{message:'Bramka zajęta'}));
   };
   c.test.addDevice('12345678901');
   await flush();
   assert.equal(sent.network,network);
   assert.equal(sent.code,'12345678901');
   assert.equal(sent.ssid,network==='wifi'?' network ':undefined);
   assert.equal(sent.password,network==='wifi'?'testPass123':undefined);
   assert.equal(element('add-msg').textContent,'Bramka zajęta');
   assert.equal(element('btn-add').disabled,false);
  }
  c.window.fetch=(u)=>{
   assert.equal(u,'/api/matter/discovery');
   return Promise.resolve(response(200,{node:7,state:'incomplete',endpoints:[{id:1,servers:[8,1026]}],electrical:[{endpoint:1,active_power_mw:'0',imported_mwh:'4611686018427387904',voltage_mv:null,active_current_ma:false}]}));
  };
  c.test.renderDiscovery(7,c.test.scanEpoch(),0);
  await flush();
  assert.match(element('matter-discovery-result').textContent,/Odczyt niepełny/);
  assert.match(element('matter-discovery-result').textContent,/Moc czynna \[mW\]: 0/);
  assert.match(element('matter-discovery-result').textContent,/Energia pobrana \[mWh\]: 4611686018427387904/);
  assert.match(element('matter-discovery-result').textContent,/Napięcie \[mV\]: brak wartości/);
  assert.match(element('matter-discovery-result').textContent,/Prąd czynny \[mA\]: błąd odczytu/);
  assert.match(element('matter-discovery-result').textContent,/EP 1: Ściemnianie \(0x8\)/);
  const routes=[];
  c.window.fetch=(url,options)=>{
    routes.push(url);
    if(url.endsWith('/discovery')) return Promise.resolve(response(200,{node:7,state:'complete',endpoints:[]}));
    if(url.endsWith('/prepare')) {
      assert.deepEqual(JSON.parse(options.body),{node:7});
      return Promise.resolve(response(200,{ok:true,token:'x'.repeat(32),backup_hex:'1234',backup_sha256:'fake',
        channels:[{number:3,kind:3,sources:[{role:5,ep:1},{role:6,ep:1}]}],relations:[{relay:1,meter:3}]}));
    }
    assert.deepEqual(JSON.parse(options.body),{token:'x'.repeat(32)});
    return Promise.resolve(response(200,{ok:true,message:'Zapisano mapę.'}));
  };
  c.test.renderDiscovery(7,c.test.scanEpoch(),0);await flush();
  assert.equal(element('matter-prepare').disabled,false);
  element('matter-prepare').listeners.click();await flush();
  assert.equal(routes.some(r=>r.endsWith('/commit')),false);
  assert.equal(element('matter-backup').disabled,false);
  assert.match(element('matter-proposal').textContent,/#3 Licznik energii/);
  assert.match(element('matter-proposal').textContent,/#1 z licznikiem #3/);
  element('matter-commit').listeners.click();await flush();
  assert.equal(routes[routes.length-1],'/api/matter/channels/commit');
  assert.equal(element('matter-proposal').textContent,'Zapisano mapę.');
  // Explicit cross-endpoint selection and reviewed retirement, no automatic commit.
  routes.length=0;
  c.window.fetch=(url,options)=>{
    routes.push(url);
    if(url.endsWith('/discovery'))return Promise.resolve(response(200,{node:7,state:'complete',endpoints:[{id:1,servers:[1026]},{id:2,servers:[1029]}]}));
    assert.deepEqual(JSON.parse(options.body),{node:7,thermohygrometer:{temperature_endpoint:1,humidity_endpoint:2}});
    return Promise.resolve(response(200,{ok:true,token:'y'.repeat(32),backup_hex:'00',backup_sha256:'fake',history_note:'Historia zostaje oddzielnie.',channels:[{number:0,kind:2,sources:[{role:1,ep:1},{role:2,ep:2}]},{number:1,kind:2,retired:true,sources:[]}]}));
  };
  c.test.renderDiscovery(7,c.test.scanEpoch(),0);await flush();
  assert.equal(element('matter-merge').checked,true);
  element('matter-prepare').listeners.click();await flush();
  assert.match(element('matter-proposal').textContent,/#0 Temperatura \/ wilgotność/);
  assert.match(element('matter-proposal').textContent,/#1.*archiwalny/);
  assert.match(element('matter-proposal').textContent,/Historia zostaje oddzielnie/);
  assert.equal(routes.some(r=>r.endsWith('/commit')),false);
  // Check does not authorize installation. Only a matching selected candidate may be installed.
  const otaActions=[];
  let otaAuthorized=false;
  c.window.fetch=(url,options)=>{
    if(options && options.method==='POST'){
      const payload=JSON.parse(options.body);otaActions.push(payload);
      if(payload.action==='install'){assert.equal(payload.version,16908290);otaAuthorized=true}
      return Promise.resolve(response(200,{ok:true}));
    }
    return Promise.resolve(response(200,{ok:true,node:7,state:otaAuthorized?'authorized':'available',current_version:16777234,current_version_string:'1.0.18',available_version:16908290,available_version_string:'1.2.2',image_bytes:499004,catalog:'CSA DCL MainNet'}));
  };
  const checkButton=element('matter-ota-check');
  checkButton.listeners.click();checkButton.listeners.click();
  assert.equal(checkButton.disabled,true);
  assert.equal(checkButton.textContent,'Sprawdzam…');
  await flush();
  assert.equal(checkButton.disabled,false);
  assert.deepEqual(otaActions,[{node:7,action:'check'}]);
  assert.equal(element('matter-ota-install').disabled,false);
  assert.match(element('matter-ota-status').textContent,/1.2.2/);
  c.confirm=()=>false;element('matter-ota-install').listeners.click();await flush();
  assert.equal(otaActions.length,1);
  c.confirm=()=>true;element('matter-ota-install').listeners.click();await flush();
  assert.equal(otaActions[1].action,'install');
  assert.equal(element('matter-ota-install').disabled,true);
  assert.match(element('matter-ota-status').textContent,/Zezwolono na jedną aktualizację/);

  const plug=c.test.liveMarkup({values:[{kind:'onoff',endpoint:1,valid:true,value:0},{kind:'voltage',endpoint:2,valid:true,value:230},{kind:'current',endpoint:2,valid:true,value:0},{kind:'active_power',endpoint:2,valid:true,value:-1.25},{kind:'imported_energy',endpoint:2,valid:true,value:4611686018427.388,raw:'4611686018427387904'}],thread:{}});
  assert.match(plug,/Wyłączone/);assert.match(plug,/230 V/);assert.match(plug,/0 A/);assert.match(plug,/-1,25 W/);assert.match(plug,/4\u00a0611\u00a0686\u00a0018\u00a0427,387904 kWh/);assert.doesNotMatch(plug,/Bateria/);
  const nullPower=c.test.liveMarkup({values:[{kind:'active_power',endpoint:2,valid:false,value:0}],thread:{}});assert.doesNotMatch(nullPower,/0 W/);assert.match(nullPower,/—/);
  const payloads=[];
  c.window.fetch=(url,options)=>{
    if(url.endsWith('/discovery'))return Promise.resolve(response(200,{node:10,state:'complete',endpoints:[{id:1,servers:[6]},{id:2,servers:[144,145]}]}));
    payloads.push(JSON.parse(options.body));return Promise.resolve(response(200,{ok:true,token:'z'.repeat(32),channels:[{number:2,kind:1,sources:[{role:0,ep:1}]},{number:3,kind:3,sources:[{role:6,ep:2}]}],relations:[{relay:2,meter:3}]}));
  };
  c.test.renderDiscovery(10,c.test.scanEpoch(),0);await flush();assert.equal(element('matter-meter-box').hidden,false);assert.equal(element('matter-meter-pair').checked,true);assert.equal(element('matter-merge').checked,false);assert.equal(element('matter-relay-ep').value,1);assert.equal(element('matter-meter-ep').value,2);
  element('matter-prepare').listeners.click();await flush();assert.deepEqual(payloads[0],{node:10,electricity_meter:{relay_endpoint:1,meter_endpoint:2}});assert.match(element('matter-proposal').textContent,/#2 z licznikiem #3/);
  c.window.fetch=()=>Promise.resolve(response(200,{node:10,state:'complete',endpoints:[{id:1,servers:[6]},{id:2,servers:[145]},{id:3,servers:[6]},{id:4,servers:[145]}]}));
  c.test.renderDiscovery(10,c.test.scanEpoch(),0);await flush();assert.equal(element('matter-meter-pair').checked,false);

  const zero=c.test.liveMarkup({values:[{kind:'temperature',endpoint:1,valid:true,value:0,received_age_s:2},{kind:'battery',endpoint:0,valid:true,value:0}],thread:{direct_child:false,average_rssi_dbm:-50}});
  assert.match(zero,/0 °C/);assert.match(zero,/0 %/);assert.doesNotMatch(zero,/-50 dBm/);
  const missing=c.test.liveMarkup({values:[{kind:'humidity',endpoint:2,valid:false,value:0}],thread:{}});
  assert.doesNotMatch(missing,/0 %/);assert.match(missing,/Bateria/);
  c.test.setDevices([{node:10,name:'GRILLPLATS <Plug>'}]);
  const router=c.test.liveMarkup({values:[],thread:{role:'router',source:'c6_router',average_rssi_dbm:-42,last_heard_s:2}},10);
  assert.match(router,/Rola: router Thread/);assert.match(router,/Bramka C6.*RSSI -42 dBm/);
  const parent=c.test.liveMarkup({values:[],thread:{role:'sleepy_end_device',source:'matter_parent',parent_node:'10',average_rssi_dbm:-73,last_heard_s:14}},6);
  assert.match(parent,/GRILLPLATS &lt;Plug&gt;.*RSSI -73 dBm/);assert.doesNotMatch(parent,/<Plug>/);
  const unconfirmed=c.test.liveMarkup({values:[],thread:{source:'matter_parent',average_rssi_dbm:-73}},6);
  assert.match(unconfirmed,/Łącze Thread niepotwierdzone/);assert.doesNotMatch(unconfirmed,/-73 dBm/);
  const endpoints=c.test.liveMarkup({values:[{kind:'temperature',endpoint:1,valid:true,value:20},{kind:'temperature',endpoint:3,valid:true,value:21},{kind:'humidity',endpoint:'<img onerror=1>',valid:true,value:60}],thread:{direct_child:true,average_rssi_dbm:-58,quality:3,last_heard_s:2}});
  assert.match(endpoints,/EP 1/);assert.match(endpoints,/EP 3/);assert.match(endpoints,/-58 dBm/);assert.doesNotMatch(endpoints,/<img/);assert.match(endpoints,/&lt;img/);
  c.test.setToken(token);c.test.setAuthorized(true);
  const metric={innerHTML:'old',getAttribute:()=> '7'};
  c.document.querySelectorAll=selector=>selector==='.device-live'?[metric]:[];
  let completeTelemetry,telemetryCalls=0;
  c.window.fetch=()=>{telemetryCalls++;return new Promise(resolve=>{completeTelemetry=resolve})};
  element('draft-name').value='name being edited';
  c.test.loadDeviceTelemetry();c.test.loadDeviceTelemetry();await flush();assert.equal(telemetryCalls,1);
  completeTelemetry(response(200,{devices:[{node:7,name:'<img>',telemetry:{values:[{kind:'temperature',endpoint:1,valid:true,value:22.4}],thread:{}}}]}));await flush();
  assert.match(metric.innerHTML,/22,4/);assert.equal(element('draft-name').value,'name being edited');assert.equal(elements.has('live-overview'),false,'No dashboard copy of device readings');
  c.test.loadDeviceTelemetry();await flush();const prior=metric.innerHTML;
  element('panel-logout').listeners.click();completeTelemetry(response(200,{devices:[{node:7,telemetry:{values:[{kind:'temperature',endpoint:1,valid:true,value:99}],thread:{}}}]}));await flush();
  assert.equal(metric.innerHTML,prior);assert.equal(elements.has('live-overview'),false);
  {
   const h=harness(variant),x=h.c;x.test.setToken(token);x.test.setAuthorized(true);
   const state={node:7,telemetry:{values:[{kind:'onoff',endpoint:1,valid:true,value:1}],thread:{}}};x.test.setDevices([state]);let resolvePost,calls=0;
   x.window.fetch=(url)=>{if(url==='/api/device/control'){calls++;return new Promise(resolve=>{resolvePost=resolve})}return Promise.resolve(response(200,{devices:[state]}))};
   const operation=x.test.controlRelay(7,1,false);await flush();assert.equal(calls,1);assert.equal(x.test.pendingCount(),1);assert.equal(state.telemetry.values[0].value,1,'no optimistic relay state');
   await x.test.controlRelay(7,1,false);assert.equal(calls,1,'single flight per endpoint');
   resolvePost(response(202,{ok:true}));await operation;await flush();assert.equal(x.test.pendingCount(),1,'queue acceptance is not state confirmation');
   state.telemetry.values[0].value=0;x.test.loadDeviceTelemetry();await flush();assert.equal(x.test.pendingCount(),0,'report confirms actual state');
   x.window.fetch=()=>Promise.resolve(response(503,{ok:false,message:'queue full'}));await x.test.controlRelay(7,1,true);assert.equal(x.test.pendingCount(),0);assert.equal(state.telemetry.values[0].value,0);
   x.window.fetch=(url)=>Promise.resolve(response(200,url==='/api/device/control'?{ok:true}:{devices:[state]}));await x.test.controlRelay(7,1,true);await flush();assert.equal(x.test.pendingCount(),1);
   [...h.timers.values()].filter(t=>t.ms===30000).pop().fn();assert.equal(x.test.pendingCount(),0);assert.equal(state.telemetry.values[0].value,0,'timeout is not success');
   let uncertainPosts=0;
   x.window.fetch=(url)=>{if(url==='/api/device/control'){uncertainPosts++;return Promise.reject(Error('TLS timeout'))}return Promise.resolve(response(200,{devices:[state]}))};
   await x.test.controlRelay(7,1,true);await flush();
   assert.equal(x.test.pendingCount(),1,'transport error leaves the physical result unknown');
   assert.match(x.test.liveMarkup(state.telemetry,7),/Sprawdzam stan/);
   await x.test.controlRelay(7,1,true);assert.equal(uncertainPosts,1,'no second command while checking the first');
   state.telemetry.values[0].value=1;x.test.loadDeviceTelemetry();await flush();
   assert.equal(x.test.pendingCount(),0,'later device data resolves uncertain transport');
   const markup=x.test.liveMarkup(state.telemetry,7);assert.match(markup,/data-relay-on="1"/);assert.match(markup,/data-relay-on="0"/);
   const invalid=x.test.liveMarkup({values:[{kind:'onoff',endpoint:'<img>',valid:true,value:1}],thread:{}},7);assert.doesNotMatch(invalid,/data-relay-on/);
   await x.test.controlRelay(7,1,true);await flush();h.element('panel-logout').listeners.click();assert.equal(x.test.pendingCount(),0,'logout clears pending controls');
  }
  {
   const h=harness(variant), x=h.c;
   x.test.setToken(token); x.test.setAuthorized(true);
   x.window.fetch=()=>Promise.resolve(response(200,{ok:true,node:41,message:'Przyjęto kod'}));
   x.test.addDevice('12345678901'); await flush();
   const notice=h.element('add-msg');
   assert.equal(notice.className,'msg');
   assert.equal(h.element('btn-add').disabled,true);
   x.test.updatePairingStatus({node:40,state:'success'});
   assert.equal(notice.textContent,'Przyjęto kod');
   x.test.updatePairingStatus({node:41,state:'commissioning',message:'Trwa próba'});
   assert.equal(notice.textContent,'Trwa próba');
   x.test.updatePairingStatus({node:41,state:'success'});
   assert.match(notice.textContent,/zostało sparowane/);
   assert.equal(h.element('btn-add').disabled,false);
   x.window.fetch=()=>Promise.resolve(response(200,{ok:true,node:42}));
   x.test.addDevice('12345678902'); await flush();
   x.test.updatePairingStatus({node:42,state:'failed',message:'Brak odpowiedzi'});
   assert.equal(notice.className,'msg err');
   assert.match(notice.textContent,/Nie udało się.*Brak odpowiedzi/);
  }
  results.push({variant,checks:['bearer header','HTTP500 reboot failure','ok:false reboot failure','body timeout','SSID spaces + full length','AuthKey not in polling','401 locks and clears token','stale response rejected after logout','admin opens setup only','password confirmation prevents request','setup NVS failure retains setup','password commit clears credentials','custom password login','rotation failure preserves session','rotation success logs out','11/21 digit codes and Thread/Wi-Fi/LAN pairing payloads','partial Descriptor, exact electrical values, null and error','review and explicit commit for the meter proposal','cross-EP selection and reviewed retirement','OTA check never installs','OTA installation cancellation','OTA exact selected version and single authorization','telemetry valid zero versus missing battery','RSSI requires confirmed link and reports router role or named parent','multiple measurement endpoints remain separate','telemetry HTML escaping','single-flight telemetry refresh preserves edited fields','late telemetry rejected and display cleared after logout','electrical units and valid zero / null','exact int64 energy formatting','electrical device has no invented battery','cross-EP electrical request and relation review','multiple circuits require explicit endpoint selection','relay single-flight / no optimistic state / confirmation / timeout / error / logout','pairing tracks the accepted node through success or failure'],result:'PASS'});
 }
 console.log(JSON.stringify({scope:'Host tests of actual app.js; no ESP32 execution',results},null,2));
})().catch(e=>{console.error(e);process.exitCode=1});
