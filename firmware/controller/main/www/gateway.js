(function(){"use strict";
const encoder=new TextEncoder(),decoder=new TextDecoder();
const b64=a=>{let s="";for(const n of new Uint8Array(a))s+=String.fromCharCode(n);return btoa(s);};
const bytes=s=>{if(typeof s!=="string"||s.length>160000)throw Error("Niepoprawny plik kopii.");return Uint8Array.from(atob(s),c=>c.charCodeAt(0));};
async function key(password,salt,usage){if(typeof password!=="string"||password.length<12||password.length>256)throw Error("Hasło kopii: od 12 do 256 znaków.");const seed=await crypto.subtle.importKey("raw",encoder.encode(password),"PBKDF2",false,["deriveKey"]);return crypto.subtle.deriveKey({name:"PBKDF2",hash:"SHA-256",salt,iterations:300000},seed,{name:"AES-GCM",length:256},false,usage);}
const aad=encoder.encode("Matter2Supla encrypted backup v1");
async function encrypt(snapshot,password){const salt=crypto.getRandomValues(new Uint8Array(16)),iv=crypto.getRandomValues(new Uint8Array(12));const k=await key(password,salt,["encrypt"]);const plain=encoder.encode(JSON.stringify(snapshot));try{const ciphertext=await crypto.subtle.encrypt({name:"AES-GCM",iv,additionalData:aad},k,plain);return {format:"Matter2Supla encrypted backup",version:1,kdf:"PBKDF2-SHA256",iterations:300000,salt:b64(salt),iv:b64(iv),data:b64(ciphertext)};}finally{plain.fill(0);}}
async function decrypt(file,password){if(!file||file.format!=="Matter2Supla encrypted backup"||file.version!==1||file.kdf!=="PBKDF2-SHA256"||file.iterations!==300000)throw Error("Nieobsługiwany format kopii.");const salt=bytes(file.salt),iv=bytes(file.iv),ciphertext=bytes(file.data);if(salt.length!==16||iv.length!==12||ciphertext.length>100000)throw Error("Uszkodzona kopia.");const k=await key(password,salt,["decrypt"]);let plain;try{plain=new Uint8Array(await crypto.subtle.decrypt({name:"AES-GCM",iv,additionalData:aad},k,ciphertext));const snapshot=JSON.parse(decoder.decode(plain));if(!snapshot||snapshot.format!==3||typeof snapshot.image!=="string")throw Error("Niepoprawna zawartość kopii.");return snapshot;}catch(e){throw Error("Błędne hasło albo uszkodzona kopia.");}finally{if(plain)plain.fill(0);}}
window.GatewayBackup={encrypt,decrypt};
window.initGatewayPanel=function(api){const $=id=>document.getElementById(id);let restoreData=null,lastBackup=false,backupRequired=false,active=false,tab=null;
const clear=()=>{restoreData=null;lastBackup=false;backupRequired=false;$("update-status").textContent="";$("update-version").textContent="";$("backup-password").value="";$("mqtt-password").value="";$("gateway-restore").disabled=true;$("backup-preview").textContent="";$("gateway-diagnostics").textContent="Zaloguj się, aby zobaczyć diagnostykę.";$("mqtt-status").textContent="";
 nameEdited=false;settingsReady=false;currentChannel=null;
 $("gateway-name").value="";$("gateway-label").textContent="Matter2Supla /";
 $("gateway-sidebar-label").textContent="Centrum bramki";
 $("gateway-name-msg").textContent="";$("thread-channel-msg").textContent="";
 $("thread-channel").disabled=true;$("thread-channel-save").disabled=true;
 $("thread-channel-state").textContent="Odczytuję kanał…";};
function get(path){return api.request(path,{cache:"no-store"}).then(r=>r.json());}
async function action(button,output,fn){if(!api.authorized()||active)return;active=true;const epoch=api.epoch();button.disabled=true;try{await fn(epoch);}catch(e){if(epoch===api.epoch())$(output).textContent=e.message;}finally{active=false;if(epoch===api.epoch())button.disabled=(button.id==="gateway-restore"&&!restoreData)||(button.id==="thread-channel-save"&&$("thread-channel").disabled);}}

function renderDiagnostics(d){
 const r=d.s3_c6||{},m=d.memory||{};
 const value=v=>v==null?"Brak danych":String(v);
 const bytes=v=>typeof v==="number"&&Number.isFinite(v)&&v>=0?(v/1024).toLocaleString("pl-PL",{minimumFractionDigits:1,maximumFractionDigits:1})+" KiB":"Brak danych";
 const memrow=(label,v)=>[label,bytes(v),null,typeof v==="number"?v.toLocaleString("pl-PL")+" B":null];
 const f=d.flash||{},cpu=d.cpu||{};
 const percent=v=>Number.isFinite(v)&&v>=0&&v<=100?v.toLocaleString("pl-PL",{maximumFractionDigits:1})+"%":null;
 const cpuRows=[["Obciążenie CPU S3",percent(cpu.load_pct)]];
 if(Array.isArray(cpu.core_load_pct))cpu.core_load_pct.forEach((load,core)=>cpuRows.push(["Rdzeń "+core,percent(load)]));
 const age=r.last_success_age_s==null?"Brak odpowiedzi":r.last_success_age_s+" s temu";
 const sections=[
  ["System",[...cpuRows,["Wersja S3",d.firmware],["Wersja C6",r.rcp_version],["Czas działania",d.uptime_s==null?null:d.uptime_s+" s"],["Powód restartu (ESP-IDF)",d.reset_reason]]],
 ["Flash S3",[memrow("Pamięć flash",f.physical_bytes),memrow("Obraz uruchomionej aplikacji",f.app_image_bytes),memrow("Partycja aplikacji",f.app_partition_bytes),memrow("Zapas w partycji aplikacji",f.app_partition_unused_bytes),memrow("Nieprzydzielone partycjom",f.unassigned_bytes),["Uruchomiona partycja",f.running_partition]]],
  ["RAM wewnętrzny — sterta",[memrow("Całkowita pojemność sterty",m.internal_total),memrow("Zajęta sterta",m.internal_used),memrow("Wolna sterta",m.internal_free),memrow("Minimum wolnego RAM",m.internal_minimum),memrow("Największy wolny blok",m.internal_largest)]],
  ["PSRAM — sterta",[memrow("Całkowita pojemność sterty",m.psram_total),memrow("Zajęta sterta",m.psram_used),memrow("Wolna sterta",m.psram_free)]],
  ["Połączenie S3 ↔ C6",[["Stan połączenia",r.connected?"Połączono":"Brak świeżej odpowiedzi",r.connected?"diagnostic-ok":"diagnostic-warn"],["Ostatnia odpowiedź",age],["Nieudane zapytania",r.probe_failures],["Transport",r.transport]]],
  ...(d.wifi_health ? [["Wi-Fi bramki",[
    ["Test bramy sieciowej",!d.wifi_health.gateway_probe_active?"Nieaktywny":d.wifi_health.gateway_seen_reply?"Odpowiedź potwierdzona":"Oczekiwanie na pierwszą odpowiedź"],
    ["Kolejne braki odpowiedzi",d.wifi_health.gateway_misses],
    ["Rozłączenia Wi-Fi",d.wifi_health.disconnects],
    ["Utraty IPv4",d.wifi_health.ip_losses],
    ["Ostatni kod rozłączenia",d.wifi_health.last_disconnect_reason||null],
    ["Próby ponownego połączenia",d.wifi_health.reconnects],
    ["Odzyskiwanie Wi-Fi",d.wifi_health.recoveries]
  ]]] : []),
  ["Radio Thread",[["Dostęp do radia",r.radio_access_blocked==null?null:r.radio_access_blocked?"Wszystkie próby zablokowane w ostatnim oknie":"Brak pełnej blokady w ostatnim oknie"],["Próby nadania od kontroli",r.radio_tx_since_last_probe],["Zajęty kanał — łącznie",r.radio_cca_total],["Zajęty kanał — ostatnie okno",r.radio_cca_since_last_probe]]]
 ];
 const nodes=sections.map(([title,rows])=>{const section=document.createElement("section");section.className="diagnostic-section";const h=document.createElement("h2");h.textContent=title;section.append(h);const dl=document.createElement("dl");for(const [label,v,state,hint] of rows){const row=document.createElement("div");row.className="diagnostic-row";const dt=document.createElement("dt"),dd=document.createElement("dd");dt.textContent=label;dd.textContent=value(v);if(hint)dd.title=hint;if(state)dd.className=state;row.append(dt,dd);dl.append(row);}section.append(dl);return section;});
 $("gateway-diagnostics").replaceChildren(...nodes);
}

const nameInput=$("gateway-name"),channelSelect=$("thread-channel"),channelSave=$("thread-channel-save");
let settingsReady=false,nameEdited=false,currentChannel=null;
nameInput.addEventListener("input",()=>{nameEdited=true;});
function renderSettings(data){
 const t=data.thread||{};
 if(!nameEdited)nameInput.value=data.gateway_name||"Matter Gateway";
 const options=Array.isArray(t.channels)?t.channels:[];
 const selected=settingsReady&&currentChannel===t.channel?Number(channelSelect.value):t.channel;
 currentChannel=t.channel;
 const choice=options.includes(selected)?selected:options.includes(t.channel)?t.channel:options[0];
 channelSelect.replaceChildren(...options.map(ch=>{const opt=document.createElement("option");opt.value=ch;opt.textContent=String(ch);return opt;}));
 if(choice!=null)channelSelect.value=String(choice);
 const busy=!!t.updating;
 channelSelect.disabled=!t.can_change||busy||!options.length;
 channelSave.disabled=channelSelect.disabled;
 let state=t.channel==null?"Aktywny kanał niedostępny":"Aktywny kanał: "+t.channel;
 if(t.pending_channel!=null)state+=" · docelowy: "+t.pending_channel+(t.remaining_s==null?"":" · zostało "+t.remaining_s+" s");
 else if(busy)state+=" · trwa uzgadnianie zmiany sieci";
 else if(t.last_error)state+=" · ostatnia zmiana nie powiodła się (OpenThread "+t.last_error+")";
 $("thread-channel-state").textContent=state;
 settingsReady=true;
}
async function refreshSettings(epoch){
 try{const data=await get("/api/gateway/settings");if(epoch===api.epoch())renderSettings(data);}
 catch(e){if(epoch===api.epoch())$("thread-channel-state").textContent=e.message;}
}
$("gateway-name-save").addEventListener("click",function(){action(this,"gateway-name-msg",async epoch=>{
 const name=nameInput.value.trim();
 if(!name||encoder.encode(name).length>48)throw Error("Wpisz nazwę o długości od 1 do 48 bajtów UTF-8.");
 const result=await api.post("/api/gateway/name",{name});
 if(epoch!==api.epoch())return;
 nameEdited=false;
 $("gateway-label").textContent=name+" /";
 $("gateway-sidebar-label").textContent=name==="Matter Gateway"?"Centrum bramki":name;
 $("gateway-name-msg").textContent=result.message;
 await refreshSettings(epoch);
 });});
channelSave.addEventListener("click",function(){action(this,"thread-channel-msg",async epoch=>{
 const channel=Number(channelSelect.value);
 if(channel===currentChannel){$("thread-channel-msg").textContent="Ten kanał jest już aktywny.";return;}
 if(!confirm("Zaplanować zmianę kanału całej sieci Thread na "+channel+"? Urządzenia mogą chwilowo stracić łączność."))return;
 const result=await api.post("/api/gateway/thread-channel",{channel,confirm:true});
 if(epoch===api.epoch()){
  $("thread-channel-msg").textContent=result.message;
  await refreshSettings(epoch);
 }
 });});
async function refresh(name){if(!api.authorized())return;tab=name;const epoch=api.epoch();if(name==="settings")return refreshSettings(epoch);try{if(name==="diagnostics"){const d=await get("/api/gateway/diagnostics");if(epoch===api.epoch()){renderDiagnostics(d);}}if(name==="maintenance"){const d=await get("/api/gateway/diagnostics");if(epoch===api.epoch())$("update-version").textContent="Wersja S3: "+d.firmware;}if(name==="integrations"){const d=await get("/api/mqtt");if(epoch!==api.epoch())return;$("mqtt-enabled").checked=!!d.enabled;$("mqtt-uri").value=d.uri||"";$("mqtt-user").value=d.user||"";$("mqtt-ca").value=d.ca||"";$("mqtt-status").textContent=d.connected?"Połączono z brokerem":"Nie połączono";}}catch(e){if(epoch===api.epoch())$(name==="diagnostics"?"gateway-diagnostics":name==="maintenance"?"update-version":"mqtt-status").textContent=e.message;}}
$("gateway-backup").addEventListener("click",function(){action(this,"backup-result",async epoch=>{const password=$("backup-password").value;if(password.length<12)throw Error("Wpisz hasło kopii: minimum12 znaków.");const snapshot=await api.post("/api/gateway/backup",{});const encrypted=await encrypt(snapshot,password);if(epoch!==api.epoch())return;api.download("matter2supla-backup-encrypted.json",JSON.stringify(encrypted),"application/json");lastBackup=true;if(backupRequired){backupRequired=false;$("update-status").textContent="Kopia gotowa. Możesz rozpocząć aktualizację S3.";}$("backup-result").textContent="Pobrano szyfrowaną kopię. Zachowaj hasło w bezpiecznym miejscu.";});});
$("gateway-restore-preview").addEventListener("click",function(){action(this,"backup-result",async epoch=>{restoreData=null;$("gateway-restore").disabled=true;const file=$("backup-file").files[0];if(!file||file.size>160000)throw Error("Wybierz poprawny plik kopii.");const snapshot=await decrypt(JSON.parse(await file.text()),$("backup-password").value);if(epoch!==api.epoch())return;restoreData=snapshot.simulated?null:snapshot;$("backup-preview").textContent="Kopia NVS tej samej płytki i tożsamości SUPLA.\nLiczba urządzeń: "+snapshot.devices+"\nOdtworzenie przywróci zapisany stan urządzeń, kanałów, sieci i haseł; późniejsze zmiany zostaną zastąpione. Historia w Cloud nie jest przenoszona ani usuwana. Bramka zrestartuje się.";$("gateway-restore").disabled=!!snapshot.simulated;if(snapshot.simulated)$("backup-preview").textContent="DEMO: to podgląd szyfrowania, nie prawdziwa kopia urządzenia.";});});
$("gateway-restore").addEventListener("click",function(){action(this,"backup-result",async epoch=>{if(!restoreData||!confirm("Odtworzyć sprawdzoną kopię i zrestartować bramkę? Bieżąca konfiguracja zostanie zastąpiona kopią."))return;const data=restoreData;restoreData=null;const result=await api.post("/api/gateway/restore",data);if(epoch===api.epoch())$("backup-result").textContent=result.message;});});
async function verifyUpdateBoot(version,epoch,accepted){
  const deadline=Date.now()+90000;
  let lastVersion="";
  while(epoch===api.epoch() && Date.now()<deadline){
    // Give the server time to restart, then query the running application.
    await new Promise(resolve=>setTimeout(resolve,2500));
    if(epoch!==api.epoch())return;
    try{
      const response=await api.request("/api/gateway/diagnostics",{cache:"no-store",timeoutMs:3500});
      const status=await response.json();
      if(epoch!==api.epoch())return;
      lastVersion=status.firmware||"";
      if(lastVersion===version){
        $("update-version").textContent="Wersja S3: "+lastVersion;
        $("update-status").textContent="Bramka uruchomiła wersję "+version+". Kontrola startu może jeszcze trwać.";
        return;
      }
    }catch(_){/* HTTPS can be unavailable during restart. */}
  }
  if(epoch===api.epoch()){
    $("update-status").textContent=(accepted?"Bramka przyjęła obraz. ":"Utracono odpowiedź podczas aktualizacji. ")+
      "Nie potwierdzono uruchomienia "+version+"."+(lastVersion?" Ostatnia odczytana wersja: "+lastVersion+".":"");
  }
}
$("gateway-update").addEventListener("click",function(){return action(this,"update-status",async epoch=>{
  const status=$("update-status");
  if(!lastBackup){backupRequired=true;throw Error("Najpierw pobierz szyfrowaną kopię bramki.");}
  const file=$("update-file").files[0];
  if(!file||file.size<171||file.size>4194474)throw Error("Wybierz pakiet aktualizacji .m2s.");
  status.textContent="Sprawdzam plik aktualizacji…";
  const image=await file.arrayBuffer(),v=new DataView(image);
  if(epoch!==api.epoch())return;
  if(image.byteLength<171||decoder.decode(new Uint8Array(image,0,7))!=="M2SOTA3"||
     v.getUint32(8,true)!==3||v.getUint32(12,true)!==9||v.getUint32(16,true)!==image.byteLength-170)
    throw Error("Pakiet nie jest przeznaczony dla tej bramki S3.");
  const version=decoder.decode(new Uint8Array(image,24,32)).replace(/\0.*$/,"");
  if(!confirm("Zainstalować podpisaną wersję "+version+" na S3? Bramka uruchomi się ponownie.")){
    status.textContent="Anulowano aktualizację.";
    return;
  }
  status.textContent="Rozpoczęto aktualizację "+version+". Przesyłam obraz i czekam na weryfikację przez bramkę…";
  try{
    await api.request("/api/gateway/update",{method:"POST",headers:{"Content-Type":"application/octet-stream"},
      body:image,timeoutMs:180000}).then(response=>response.json());
  }catch(error){
    if(epoch!==api.epoch())return;
    if(error.responseReceived)throw error;
    status.textContent="Utracono odpowiedź podczas aktualizacji. Sprawdzam, jaka wersja działa…";
    await verifyUpdateBoot(version,epoch,false);
    return;
  }
  if(epoch!==api.epoch())return;
  status.textContent="Obraz zweryfikowany. Czekam na restart i odpowiedź bramki…";
  await verifyUpdateBoot(version,epoch,true);
});});
$("gateway-rollback").addEventListener("click",function(){action(this,"update-status",async epoch=>{if(!confirm("Uruchomić poprzednią potwierdzoną aplikację lub obraz odzyskiwania? Ustawienia NVS pozostaną bez zmian."))return;const result=await api.post("/api/gateway/rollback",{confirm:"ROLLBACK"});if(epoch===api.epoch())$("update-status").textContent=result.message;});});
$("cloud-map-download").addEventListener("click",function(){action(this,"cloud-map-result",async epoch=>{const map=await get("/api/gateway/cloud-map");if(epoch!==api.epoch())return;api.download("matter2supla-cloud-map.json",JSON.stringify(map,null,2),"application/json");$("cloud-map-result").textContent="Pobrano mapę numerów kanałów i podurządzeń, bez AuthKey i haseł. To nie zmienia publicznego Cloud.";});});
$("mqtt-save").addEventListener("click",function(){action(this,"mqtt-status",async epoch=>{const uri=$("mqtt-uri").value.trim();if($("mqtt-enabled").checked&&uri.startsWith("mqtt://")&&!confirm("Wybrano MQTT bez szyfrowania. Kontynuować dla tej sieci lokalnej?"))return;const result=await api.post("/api/mqtt",{enabled:$("mqtt-enabled").checked,uri,user:$("mqtt-user").value,password:$("mqtt-password").value,ca:$("mqtt-ca").value});$("mqtt-password").value="";if(epoch===api.epoch())$("mqtt-status").textContent=result.message;});});
setInterval(()=>{
 const current=api.activeTab?api.activeTab():tab;
 if(!document.hidden&&!active&&(current==="diagnostics"||current==="settings")&&api.authorized())refresh(current);
},5000);
return {refresh,clear};
};
})();
