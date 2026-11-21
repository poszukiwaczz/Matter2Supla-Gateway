/* Bramka Matter - logika panelu (bez zewnetrznych zaleznosci oprocz jsQR). */
(function () {
  "use strict";
  var $ = function (id) { return document.getElementById(id); };
  var busy = false, pairingNode = null, pairingAcceptedAt = 0;
  var latestStatus = null, deviceCache = [], rawLogs = "", logsPaused = false, statusPending = false;
  var suplaDirty = false, toastTimer;
  function toast(message) {
    $("toast").textContent = message; $("toast").hidden = false;
    clearTimeout(toastTimer); toastTimer = setTimeout(function () { $("toast").hidden = true; }, 4000);
  }
  var panelToken = window.M2S_DEMO_TOKEN || "", authorized = false, keyTimer, keyDirty = false, authEpoch = 0;
  try { panelToken = panelToken || sessionStorage.getItem("m2s-panel-token") || ""; } catch (_) {}
  function lockPanel() {refreshDeviceList=false;Object.values(relayPending).forEach(function(p){p.cancelTimer();});relayPending={};busy=false;pairingNode=null;
    setMobileMenu(false,false);
    if(window.gatewayPanel)window.gatewayPanel.clear();
    ++discoveryEpoch;
    clearProposal();
    authorized = false; panelToken = ""; authEpoch++;
    $("panel-login-msg").textContent = window.M2S_DEMO_TOKEN ? "DEMO: hasło startowe to admin." : "Wpisz hasło panelu. Pierwsze wejście: admin.";
    try { sessionStorage.removeItem("m2s-panel-token"); } catch (_) {}
    $("panel-login").hidden = false;
    $("panel-token").type="password";
    $("panel-token-reveal").textContent="Pokaż";
    $("panel-token-reveal").setAttribute("aria-pressed","false");
    Array.prototype.forEach.call(document.body.children, function(el) { if (el.id !== "panel-login" && el.tagName !== "SCRIPT") el.inert = true; });
    $("supla-authkey").value = ""; $("wifi-pass").value = ""; $("code").value = ""; $("pair-pass").value = "";
    ["panel-password-new","panel-password-repeat","panel-setup-new","panel-setup-repeat"].forEach(function(id){$(id).value="";});
    $("panel-setup").hidden=true;
    deviceCache=[]; $("devices").textContent="Zaloguj się, aby zobaczyć urządzenia.";
    latestStatus = null; rawLogs = ""; $("logs").textContent = "Zaloguj się, aby odczytać log.";
  }
  function request(url, options) {
    if (!panelToken) return Promise.reject(new Error("Zaloguj się do panelu."));
    var controller = new AbortController(), timer, requestEpoch = authEpoch;
    var timeout=(options && options.timeoutMs)||12000;if(options)delete options.timeoutMs;
    options = options || {}; options.signal = controller.signal;
    options.headers = Object.assign({}, options.headers || {}, {Authorization:"Bearer " + panelToken});
    var operation = window.fetch(url, options).then(function(r) {
      return r.text().then(function(text) {
        if (requestEpoch !== authEpoch) throw new Error("Sesja została zamknięta.");
        if (r.status === 401) { lockPanel(); throw new Error("Nieprawidłowe hasło lub token panelu."); }
        if (!r.ok) {
          var message = "HTTP " + r.status;
          try { message = JSON.parse(text).message || message; } catch (_) {}
          var rejection = new Error(message);
          rejection.responseReceived = true;
          throw rejection;
        }
        return {ok:true,status:r.status,text:function(){return Promise.resolve(text);},json:function(){return Promise.resolve().then(function(){return JSON.parse(text);});}};
      });
    });
    var deadline = new Promise(function(_,reject) { timer=setTimeout(function(){controller.abort();reject(new Error("Przekroczono czas odpowiedzi bramki."));},timeout); });
    return Promise.race([operation,deadline]).finally(function(){clearTimeout(timer);});
  }
  function download(name, contents, type) {
    var url = URL.createObjectURL(new Blob([contents], { type:type }));
    var a = document.createElement("a"); a.href = url; a.download = name;
    document.body.appendChild(a); a.click(); a.remove(); setTimeout(function () { URL.revokeObjectURL(url); }, 1000);
  }

  function esc(s) {
    return String(s == null ? "" : s).replace(/[<>&"]/g, function (c) {
      return { "<": "&lt;", ">": "&gt;", "&": "&amp;", '"': "&quot;" }[c];
    });
  }
  function fmtUptime(sec) {
    sec = Math.floor(sec || 0);
    var d = Math.floor(sec / 86400), h = Math.floor((sec % 86400) / 3600),
        m = Math.floor((sec % 3600) / 60), s = sec % 60;
    if (d) return d + " d " + h + " h";
    if (h) return h + " h " + m + " min";
    if (m) return m + " min " + s + " s";
    return s + " s";
  }
  function post(url, body, opts) {
    opts = opts || {};
    return request(url, {
      method: "POST",
      timeoutMs: opts.timeoutMs,
      headers: { "Content-Type": opts.contentType || "application/json" },
      body: opts.contentType === "text/plain" ? body : JSON.stringify(body || {}),
      cache: "no-store"
    }).then(function (r) { return r.json(); }).then(function(j) { if (!j || j.ok !== true) {
      var rejection = new Error(j && j.message || "Bramka odrzuciła polecenie.");
      rejection.responseReceived = true;
      throw rejection;
    }
    return j; });
  }

  /* ---- zakladki ---------------------------------------------------------- */
  // Mobile navigation is a left drawer; desktop keeps its permanent sidebar.
  var menuMedia = window.matchMedia ? window.matchMedia("(max-width:760px)") : {matches:false};
  var menuOpen = false;
  function setMobileMenu(open,returnFocus) {
    var wasOpen=menuOpen;
    menuOpen=!!open && menuMedia.matches;
    document.documentElement.dataset.navOpen=menuOpen?"true":"false";
    $("mobile-menu-toggle").setAttribute("aria-expanded",menuOpen?"true":"false");
    $("mobile-menu-backdrop").hidden=!menuOpen;
    $("main-navigation").inert=menuMedia.matches&&!menuOpen;
    if(menuMedia.matches)$("main-navigation").setAttribute("aria-hidden",menuOpen?"false":"true");
    else $("main-navigation").removeAttribute("aria-hidden");
    var target=menuOpen?$("mobile-menu-close"):(wasOpen&&returnFocus?$("mobile-menu-toggle"):null);
    if(target&&typeof target.focus==="function")target.focus({preventScroll:true});
  }
  $("mobile-menu-toggle").addEventListener("click",function(){setMobileMenu(!menuOpen,true);});
  $("mobile-menu-close").addEventListener("click",function(){setMobileMenu(false,true);});
  $("mobile-menu-backdrop").addEventListener("click",function(){setMobileMenu(false,true);});
  if(menuMedia.addEventListener)menuMedia.addEventListener("change",function(){setMobileMenu(false,false);});
  else if(menuMedia.addListener)menuMedia.addListener(function(){setMobileMenu(false,false);});
  if(document.addEventListener)document.addEventListener("keydown",function(e){
    if(!menuOpen)return;
    if(e.key==="Escape"){e.preventDefault();setMobileMenu(false,true);return;}
    if(e.key!=="Tab")return;
    var controls=Array.prototype.filter.call($("main-navigation").querySelectorAll("button:not([disabled]),a[href],[tabindex=\"0\"]"),function(el){return el.offsetParent!==null;});
    if(!controls.length)return;
    var first=controls[0],last=controls[controls.length-1];
    if(e.shiftKey&&document.activeElement===first){e.preventDefault();last.focus();}
    else if(!e.shiftKey&&document.activeElement===last){e.preventDefault();first.focus();}
  });
  setMobileMenu(false,false);

  var tabs = document.querySelectorAll("#tabs button");
  function openTab(name) {
    setMobileMenu(false,true);
    Array.prototype.forEach.call(tabs, function (b) {
      var selected = b.dataset.tab === name;
      b.classList.toggle("active", selected);
      if (selected) { b.setAttribute("aria-current", "page"); $("breadcrumb-page").textContent = name === "add" ? "Dodaj urządzenie" : b.textContent.trim(); }
      else b.removeAttribute("aria-current");
    });
    Array.prototype.forEach.call(document.querySelectorAll(".tab"), function (t) { t.classList.toggle("active", t.id === "tab-" + name); });
    if (name === "devices") loadDevices();
    if (name === "settings") loadWifiInfo();
    if (name === "diagnostics") loadLogs();
    if(window.gatewayPanel && ["diagnostics","maintenance","integrations","settings"].indexOf(name)>=0)window.gatewayPanel.refresh(name);
    if (name === "supla") loadSupla();
    window.scrollTo(0, 0);
  }
  Array.prototype.forEach.call(tabs, function (b) {
    b.addEventListener("click", function () {
      openTab(b.dataset.tab);
    });
  });
  function activeTab() {
    var b = document.querySelector("#tabs button.active");
    return b ? b.dataset.tab : "status";
  }

  /* ---- status ------------------------------------------------------------- */
  function tick() {
    if (!authorized || statusPending || document.hidden) return;
    if (activeTab()==="devices") {
      if (relayRequestsInFlight || telemetryPending) return;
      if (refreshDeviceList || Object.keys(relayPending).length || Date.now()<nextDevicesStatusAt) {
        if(refreshDeviceList || Object.keys(relayPending).length || document.querySelector("details.device[open]"))loadDeviceTelemetry();
        return;
      }
      nextDevicesStatusAt=Date.now()+6000;
    }
    statusPending = true;
    request("/api/status", { cache: "no-store" }).then(function (r) { if (!r.ok) throw new Error("HTTP " + r.status); return r.json(); }).then(function (j) {
      if(j.setup_required) {
        $("panel-setup").hidden=false;
        $("panel-login-msg").textContent="Ustaw własne hasło, aby otworzyć panel.";
        return;
      }
      $("panel-setup").hidden=true;
      if (!j.system || !j.wifi) throw new Error("Niepełny status");
      latestStatus = j;
      var name = j.system && j.system.gateway_name;
      $("gateway-label").textContent = (name || "Matter2Supla") + " /";
      $("gateway-sidebar-label").textContent = name && name !== "Matter Gateway" ? name : "Centrum bramki";
      var w = j.wifi || {};
      if (w.mode === "ap") {
        $("st-wifi").textContent = "tryb konfiguracji (AP: " + (w.ssid || "?") + ")";
        $("hdr-info").textContent = "AP: " + (w.ssid || "");
      } else if (w.connected) {
        $("st-wifi").textContent = (w.ssid || "?") + " - " + w.ip + " (" + w.rssi + " dBm)";
        $("hdr-info").textContent = w.ip || "";
      } else {
        $("st-wifi").textContent = "brak połączenia";
        $("hdr-info").textContent = "";
      }
      if(w.config_error){$("wifi-msg").textContent="Błąd konfiguracji: " + w.config_error + ". Sprawdź konsolę; dane mogły nie zostać utrwalone.";$("wifi-msg").className="msg err";}
      var t = j.thread || {};
      $("st-thread").textContent = t.role === "busy" ? "Thread chwilowo zajęty" : t.dataset
        ? t.role + " - kanał " + t.channel + " - sieć gotowa"
        : "brak sieci Thread (połącz Wi-Fi)";
      var p = j.pairing || {};
      $("st-pair").textContent = p.state === "idle" ? "bezczynne" : (p.message || p.state);
      $("st-elapsed").textContent = p.elapsed ? p.elapsed + " s" : "—";
      var su = j.supla || {};
      $("st-supla").textContent = suplaLabel(su.state);
      if (activeTab() === "supla") { loadSupla(); }
      var s = j.system || {};
      $("sys-ver").textContent = s.version || "—";
      $("sys-up").textContent = fmtUptime(s.uptime);
      $("sys-heap").textContent = s.heap ? Math.round(s.heap / 1024) + " kB" : "—";
      $("sys-dev").textContent = s.devices != null ? s.devices : "—";
      $("metric-rssi").textContent = w.connected ? w.rssi + " dBm" : "—";
      $("metric-ssid").textContent = w.connected ? w.ssid : (w.mode === "ap" ? "Tryb konfiguracji" : "Brak połączenia");
      $("metric-thread").textContent = {leader:"Leader",router:"Router",child:"Child",detached:"Rozłączona",disabled:"Wyłączona",busy:"Chwilowo zajęta"}[t.role] || "—";
      $("metric-channel").textContent = t.dataset ? "kanał " + t.channel : "brak aktywnej sieci";
      var healthy = w.connected && t.dataset && ["leader","router","child"].indexOf(t.role) >= 0 && su.state === "connected";
      $("hero-state").textContent = healthy ? "Połączenia gotowe do pracy" : "Sprawdź konfigurację połączeń";
      $("hero-dot").classList.toggle("live", healthy);
      document.querySelector(".topology-hero").classList.toggle("is-connected", healthy);
      $("health-badge").textContent = healthy ? "Połączono" : "Wymaga uwagi";
      $("health-badge").classList.toggle("ok", healthy);
      $("connection-dot").classList.add("live"); $("side-dot").classList.add("live");
      $("side-state").textContent = "Bramka odpowiada";
      $("st-supla").classList.toggle("ok", su.state === "connected");
      $("last-sync").textContent = new Date().toLocaleTimeString("pl-PL");
      $("footer-sync").textContent = "Zaktualizowano o " + $("last-sync").textContent;
      updatePairingStatus(p);
    }).catch(function () {
      $("st-wifi").textContent = "brak łączności z bramką";
      $("hdr-info").textContent = "Brak łączności";
      $("hero-state").textContent = "Brak odpowiedzi — dane mogą być nieaktualne";
      document.querySelector(".topology-hero").classList.remove("is-connected");
      $("hero-dot").classList.remove("live"); $("connection-dot").classList.remove("live"); $("side-dot").classList.remove("live");
      $("side-state").textContent = "Brak odpowiedzi";
      $("health-badge").textContent = "Brak łączności"; $("health-badge").classList.remove("ok");
      $("footer-sync").textContent = "Utracono połączenie z bramką";
    }).finally(function () { statusPending = false; });
  }
  setInterval(tick, 2000); tick();

  /* ---- dodawanie: skaner QR ----------------------------------------------- */
  function normalizePayload(text) {
    if (!text) return null;
    text = String(text).trim();
    var m = text.match(/MT:[^\s]+/i);
    if (m) return "MT:" + m[0].slice(3);
    var digits = text.replace(/[\s-]/g, "");
    if (/^(?:\d{11}|\d{21})$/.test(digits)) return digits;
    return null;
  }

  var qrDecoderLoad;
  function loadQrDecoder() {
    if(typeof window.jsQR==="function")return Promise.resolve();
    if(!qrDecoderLoad)qrDecoderLoad=new Promise(function(resolve,reject){
      var script=document.createElement("script");
      script.src="jsQR.js";
      var timer=setTimeout(failed,18000);
      function failed(){
        clearTimeout(timer);
        script.remove();
        reject(new Error("Nie udało się pobrać skanera QR. Spróbuj ponownie lub wpisz kod ręcznie."));
      }
      script.onerror=failed;
      script.onload=function(){
        if(typeof window.jsQR!=="function"){failed();return;}
        clearTimeout(timer);
        resolve();
      };
      document.head.appendChild(script);
    }).catch(function(e){qrDecoderLoad=null;throw e;});
    return qrDecoderLoad;
  }
  function decodeImage(img) {
    var scales = [1600, 1100, 700];
    var i = 0;
    function attempt() {
      if (i >= scales.length) return Promise.resolve(null);
      var max = scales[i++];
      var w = img.naturalWidth, h = img.naturalHeight;
      var k = Math.min(1, max / Math.max(w, h));
      var cw = Math.max(1, Math.round(w * k)), ch = Math.max(1, Math.round(h * k));
      var c = document.createElement("canvas");
      c.width = cw; c.height = ch;
      var ctx = c.getContext("2d", { willReadFrequently: true });
      ctx.drawImage(img, 0, 0, cw, ch);
      var data;
      try { data = ctx.getImageData(0, 0, cw, ch); } catch (e) { return Promise.resolve(null); }
      var res = null;
      try { res = window.jsQR(data.data, cw, ch, { inversionAttempts: "attemptBoth" }); } catch (e) { res = null; }
      if (res && res.data) return Promise.resolve(res.data);
      return attempt();
    }
    return loadQrDecoder().then(attempt);
  }

  $("btn-scan").addEventListener("click", function () { $("file-scan").click(); });
  $("file-scan").addEventListener("change", function (ev) {
    var file = ev.target.files && ev.target.files[0];
    ev.target.value = "";
    if (!file) return;
    $("scan-msg").textContent = "Odczytuję zdjęcie...";
    $("scan-msg").className = "msg";
    var url = URL.createObjectURL(file);
    var img = new Image();
    img.onload = function () {
      $("scan-preview").src = url;
      $("scan-preview").style.display = "block";
      decodeImage(img).then(function (payload) {
        URL.revokeObjectURL(url);
        var code = normalizePayload(payload);
        if (!code) {
          $("scan-msg").textContent = payload
            ? "Znalazłem kod, ale to nie kod Matter: " + payload
            : "Nie udało się odczytać kodu ze zdjęcia. Spróbuj bliżej, ostro, cały kod w kadrze.";
          $("scan-msg").className = "msg err";
          return;
        }
        $("scan-msg").textContent = "Kod Matter odczytany.";
        $("scan-msg").className = "msg ok";
        addDevice(code);
      }).catch(function(e){
        URL.revokeObjectURL(url);
        $("scan-msg").textContent=e.message;
        $("scan-msg").className="msg err";
      });
    };
    img.onerror = function () {
      URL.revokeObjectURL(url);
      $("scan-msg").textContent = "Nie udało się wczytać zdjęcia.";
      $("scan-msg").className = "msg err";
    };
    img.src = url;
  });

  $("pair-network").addEventListener("change", function () {
    $("pair-wifi").hidden = this.value !== "wifi";
    if (this.value !== "wifi") $("pair-pass").value = "";
  });

  function updatePairingStatus(p) {
    var running = p.state === "started" || p.state === "commissioning" || p.state === "removing";
    var notice = $("add-msg");
    if (pairingNode !== null && p.node === pairingNode) {
      if (p.state === "success" || p.state === "failed") {
        var success = p.state === "success";
        var result = success ? "Urządzenie zostało sparowane." : "Nie udało się sparować urządzenia.";
        notice.textContent = p.message ? result + " " + p.message : result;
        notice.className = "msg " + (success ? "ok" : "err");
        if (activeTab() !== "add") toast(result);
        pairingNode = null;
        busy = false;
        if (success && activeTab() === "devices") loadDevices();
      } else if (p.state === "started" || p.state === "commissioning") {
        notice.textContent = p.message || "Trwa parowanie urządzenia…";
        notice.className = "msg";
      }
    } else if (pairingNode !== null && Date.now() - pairingAcceptedAt > 5000) {
      pairingNode = null;
      busy = false;
      notice.textContent = "Bramka nie potwierdziła wyniku parowania. Sprawdź listę urządzeń przed ponowieniem.";
      notice.className = "msg err";
      if (activeTab() !== "add") toast(notice.textContent);
    }
    $("btn-add").disabled = busy || running;
    $("btn-scan").disabled = busy || running;
  }

  function addDevice(code) {
    if (busy || $("btn-add").disabled) return;
    var epoch = authEpoch;
    var network = $("pair-network").value || "thread";
    var payload = { code: code, network: network };
    if (network === "wifi") {
      payload.ssid = $("pair-ssid").value;
      payload.password = $("pair-pass").value;
      if (!payload.ssid) {
        $("add-msg").textContent = "Podaj nazwę sieci Wi-Fi urządzenia.";
        $("add-msg").className = "msg err";
        return;
      }
    }
    busy = true;
    $("btn-add").disabled = true;
    $("btn-scan").disabled = true;
    $("add-msg").textContent = "Wysyłam kod do bramki...";
    $("add-msg").className = "msg";
    post("/api/matter/pair", payload).then(function (j) {
      if (!authorized || authEpoch !== epoch) return;
      if (!Number.isSafeInteger(j.node) || j.node < 1) throw new Error("Bramka nie podała numeru parowanego urządzenia.");
      pairingNode = j.node;
      pairingAcceptedAt = Date.now();
      $("add-msg").textContent = j.message || "Trwa parowanie urządzenia…";
      $("add-msg").className = "msg";
      $("code").value = "";
      $("pair-pass").value = "";
      tick();
    }).catch(function (error) {
      if (!authorized || authEpoch !== epoch) return;
      $("add-msg").textContent = error.message;
      $("add-msg").className = "msg err";
      busy = false;
      $("btn-add").disabled = false;
      $("btn-scan").disabled = false;
    });
  }
  $("btn-add").addEventListener("click", function () {
    var v = ($("code").value || "").trim();
    if (v) addDevice(v);
  });

  /* ---- urzadzenia ---------------------------------------------------------- */

  function ageLabel(value) { return typeof value==="number" && value>=0 ? (value<60?value+" s":Math.floor(value/60)+" min")+" temu" : "brak danych"; }
  function energyText(raw) {
    if(typeof raw!=="string" || !/^-?\d{1,19}$/.test(raw))return null;
    var negative=raw[0]==="-";if(negative)raw=raw.slice(1);
    raw=raw.padStart(7,"0");
    var whole=raw.slice(0,-6).replace(/^0+(?=\d)/,"");
    var fraction=raw.slice(-6).replace(/0+$/,"");
    return (negative?"−":"")+whole.replace(/\B(?=(\d{3})+(?!\d))/g,"\u00a0")+(fraction?","+fraction:"")+" kWh";
  }

  var relayPending={}, relayRequestsInFlight=0, nextDevicesStatusAt=0;
  function paintDeviceTelemetry(){
    Array.prototype.forEach.call(document.querySelectorAll('.device-live'),function(el){var node=Number(el.getAttribute('data-live-node')),d=deviceCache.find(function(x){return x.node===node});el.innerHTML=d?liveMarkup(d.telemetry,node):'<p class="small muted">Urządzenie usunięto. Odśwież listę.</p>';});
  }
  function confirmRelayReports(){
    deviceCache.forEach(function(d){((d.telemetry||{}).values||[]).forEach(function(v){var key=d.node+':'+v.endpoint,p=relayPending[key];if(p&&v.kind==='onoff'&&v.valid&&v.value===(p.on?1:0)){p.cancelTimer();delete relayPending[key];toast('Stan urządzenia: '+(p.on?'włączone':'wyłączone'));}});});
  }
  function controlRelay(node,endpoint,on){
    if(!authorized)return Promise.resolve();
    var d=deviceCache.find(function(x){return x.node===node}),v=d&&((d.telemetry||{}).values||[]).find(function(x){return x.kind==='onoff'&&x.endpoint===endpoint&&x.valid&&(x.value===0||x.value===1)}),key=node+':'+endpoint;
    if(!Number.isSafeInteger(node)||node<1||!Number.isInteger(endpoint)||endpoint<1||endpoint>=65535||typeof on!=='boolean'||!v||relayPending[key])return Promise.resolve();
    var pending={on:on,epoch:authEpoch,phase:"sending"};
    relayPending[key]=pending;
    ++relayRequestsInFlight;
    paintDeviceTelemetry();
    function stillPending(){return authorized&&pending.epoch===authEpoch&&relayPending[key]===pending;}
    var confirmationTimer=setTimeout(function(){
      if(!stillPending())return;
      delete relayPending[key];
      paintDeviceTelemetry();
      toast('Brak potwierdzenia stanu gniazdka. Sprawdź odczyt przed ponowieniem.');
    },30000);
    pending.cancelTimer=function(){clearTimeout(confirmationTimer);};
    return post('/api/device/control',{node:node,endpoint:endpoint,on:on},{timeoutMs:18000}).then(function(){
      if(!stillPending())return;
      pending.phase="accepted";
      paintDeviceTelemetry();
    }).catch(function(e){
      if(!stillPending())return;
      if(e.responseReceived) {
        pending.cancelTimer();
        delete relayPending[key];
        toast(e.message);
      } else {
        pending.phase="uncertain";
        toast('Brak odpowiedzi na polecenie. Sprawdzam stan gniazdka.');
      }
      paintDeviceTelemetry();
    }).finally(function(){
      --relayRequestsInFlight;
      if(stillPending())loadDeviceTelemetry();
    });
  }
  $('devices').addEventListener('click',function(e){var b=e.target.closest('button[data-relay-on]');if(!b||b.disabled)return;var box=b.closest('.device-live');if(box)controlRelay(Number(box.getAttribute('data-live-node')),Number(b.getAttribute('data-relay-ep')),b.getAttribute('data-relay-on')==='1');});

  function liveMarkup(t,node) {
    if(!t)return '<p class="small muted">Oczekiwanie na diagnostykę urządzenia.</p>';
    var titles={temperature:"Temperatura",humidity:"Wilgotność",battery:"Bateria",onoff:"Stan gniazdka",voltage:"Napięcie",current:"Prąd",active_power:"Moc czynna",imported_energy:"Energia pobrana",exported_energy:"Energia oddana"};
    var units={temperature:"°C",humidity:"%",battery:"%",voltage:"V",current:"A",active_power:"W",imported_energy:"kWh",exported_energy:"kWh"};
    var order=Object.keys(titles),values=(t.values||[]).filter(function(v){return Object.prototype.hasOwnProperty.call(titles,v.kind)});
    var electrical=values.some(function(v){return ["onoff","voltage","current","active_power","imported_energy","exported_energy"].indexOf(v.kind)>=0});
    if(!electrical && values.length && !values.some(function(v){return v.kind==="battery"}))values.push({kind:"battery",valid:false});
    values.sort(function(a,b){return order.indexOf(a.kind)-order.indexOf(b.kind) || Number(a.endpoint||0)-Number(b.endpoint||0)});
    var html='<div class="live-values'+(electrical?' electrical':'')+'">'+values.map(function(v){
      var value="—",energy=v.kind==="imported_energy" || v.kind==="exported_energy";
      if(v.valid && typeof v.value==="number" && isFinite(v.value)) {
        if(v.kind==="onoff")value=v.value===1?"Włączone":"Wyłączone";
        else value=(energy?energyText(v.raw):null) || v.value.toLocaleString("pl-PL",{maximumFractionDigits:energy?6:3})+" "+units[v.kind];
      }
      var endpoint = v.endpoint!==undefined?' · EP '+esc(v.endpoint):'';
      var detail = '<small>Odebrano: '+esc(ageLabel(v.received_age_s))+'<br>Zmiana: '+esc(ageLabel(v.changed_age_s))+
        (v.supla_channel!==undefined?'<br>Kanał SUPLA '+esc(v.supla_channel)+': obsłużono '+esc(ageLabel(v.applied_age_s)):'')+'</small>';
      if (v.kind!=='onoff') return '<div class="live-value"><small>'+titles[v.kind]+endpoint+'</small><strong>'+esc(value)+'</strong>'+detail+'</div>';

      var known = v.valid && (v.value===0 || v.value===1);
      var pending = relayPending[node+':'+v.endpoint];
      var controllable = Number.isSafeInteger(node)&&Number.isInteger(v.endpoint)&&v.endpoint>0&&v.endpoint<65535;
      var controls = '';
      if (controllable) {
        var buttons = [true,false].map(function(on) {
          var selected=known&&v.value===(on?1:0);
          return '<button type="button" class="btn relay-button'+(selected?' relay-selected':'')+'" data-relay-ep="'+v.endpoint+'" data-relay-on="'+(on?'1':'0')+'" aria-pressed="'+selected+'"'+(!known||pending||selected?' disabled':'')+'>'+(on?'Włącz':'Wyłącz')+'</button>';
        }).join('');
        controls = '<div class="relay-controls" role="group" aria-label="Sterowanie gniazdkiem EP '+v.endpoint+'"><div class="relay-buttons">'+buttons+'</div><small class="relay-feedback">'+(pending?(pending.phase==="sending"?'Wysyłam polecenie…':pending.phase==="uncertain"?'Sprawdzam stan…':'Czekam na potwierdzenie'):known?'Stan z ostatniego odczytu':'Brak potwierdzonego stanu')+'</small></div>';
      }
      var relayDetails = '<small class="relay-details"><span>Odebrano: '+esc(ageLabel(v.received_age_s))+'</span>'+
        '<span>Zmiana: '+esc(ageLabel(v.changed_age_s))+'</span>'+
        (v.supla_channel!==undefined?'<span>SUPLA '+esc(v.supla_channel)+': obsłużono '+esc(ageLabel(v.applied_age_s))+'</span>':'')+'</small>';
      return '<div class="live-value live-state"><div class="relay-state"><small>'+titles[v.kind]+endpoint+'</small><strong>'+esc(value)+'</strong></div>'+controls+relayDetails+'</div>';
    }).join("")+'</div>';
    if(!values.length)html='<p class="small muted">Oczekiwanie na pierwszy odczyt urządzenia.</p>';
    var signal=t.thread||{};
    var roles={leader:"lider Thread",router:"router Thread",sleepy_end_device:"uśpione urządzenie końcowe Thread",
      end_device:"urządzenie końcowe Thread",router_eligible:"urządzenie zdolne do pracy jako router Thread"};
    var link="";
    if(signal.source==="c6_child" || signal.direct_child)link="Bramka C6";
    if(signal.source==="c6_router")link="Bramka C6";
    if(signal.source==="matter_parent" && /^[0-9]+$/.test(String(signal.parent_node))){
      var parent=deviceCache.find(function(d){return String(d.node)===String(signal.parent_node);});
      link=parent&&parent.name?parent.name:"Router Thread (węzeł "+signal.parent_node+")";
    }
    var rf=link?"Łącze z "+link:"Łącze Thread niepotwierdzone";
    if(link && typeof signal.average_rssi_dbm==="number")rf+=" · RSSI "+signal.average_rssi_dbm+" dBm";
    else rf+=" · RSSI niedostępne";
    if(link && typeof signal.quality==="number")rf+=" · jakość "+signal.quality+"/3";
    if(link && typeof signal.last_heard_s==="number")rf+=" · kontakt radiowy "+ageLabel(signal.last_heard_s);
    html+='<div class="live-details"><span>Subskrypcja: '+(t.subscription_active?"aktywna":"oczekiwanie / odczyty awaryjne")+'</span><span>Raport Matter: '+esc(ageLabel(t.last_report_age_s))+'</span>'+
      (roles[signal.role]?'<span>Rola: '+esc(roles[signal.role])+'</span>':'')+'<span>'+esc(rf)+'</span></div>';
    if(link && typeof signal.quality==="number")html+='<div class="live-signal" aria-label="Jakość wskazanego łącza Thread"><span style="width:'+Math.max(0,Math.min(100,signal.quality/3*100))+'%"></span></div>';
    if(t.truncated)html+='<p class="small err">Część diagnostyki nie mieści się w buforze.</p>';
    return html;
  }
  var telemetryPending=false, refreshDeviceList=false;
  function loadDeviceTelemetry() {
    if(!authorized || telemetryPending || statusPending || relayRequestsInFlight)return;
    var epoch=authEpoch, renderList=refreshDeviceList;
    telemetryPending=true;
    refreshDeviceList=false;
    request(renderList?"/api/devices/list":"/api/devices",{cache:"no-store"}).then(function(r){return r.json();}).then(function(j){
      if(!authorized || epoch!==authEpoch)return;
      var devices=j.devices||[];
      if(renderList){
        for(var d of devices){
          var old=deviceCache.find(function(item){return item.node===d.node;});
          if(old)d.telemetry=old.telemetry;
        }
        deviceCache=devices;
        renderDevices();
      } else {
        deviceCache=devices;
        confirmRelayReports();
        paintDeviceTelemetry();
      }
    }).catch(function(){
      if(!renderList || epoch!==authEpoch)return;
      $("devices-msg").textContent="Nie udało się pobrać listy. Sprawdź połączenie i odśwież.";
      $("devices-msg").className="msg err";
    }).finally(function(){telemetryPending=false;});
  }

  function loadDevices() {
    refreshDeviceList=true;
    loadDeviceTelemetry();
  }
  var discoveryEpoch = 0, discoveryNode = null, pendingProposal = null, otaCandidate = null, otaOperation = null;
  var discoveryOwner = null;
  function stashMatterCapabilities() {
    var panel=$("matter-capabilities"),home=$("matter-panel-home");
    if(panel.parentElement&&panel.parentElement!==home)home.appendChild(panel);
  }
  function placeMatterCapabilities(card,node) {
    if(node!==discoveryOwner)return;
    card.appendChild($("matter-capabilities"));
  }
  function clearProposal() {
    stashMatterCapabilities();
    discoveryOwner = null;
    $("matter-discovery-result").textContent="";
    $("matter-ota-status").textContent="";
    discoveryNode = null;
    pendingProposal = null;
    otaCandidate = null;
    $("matter-merge-box").hidden = true;
    $("matter-meter-box").hidden = true;
    $("matter-meter-pair").checked=false;
    setOtaOperation(null);
    $("matter-ota-install").disabled = true;
    $("matter-prepare").disabled = true;
    $("matter-backup").disabled = true;
    $("matter-commit").disabled = true;
    $("matter-proposal").textContent = "";
  }
  $("matter-prepare").addEventListener("click", function () {
    var node = discoveryNode, epoch = discoveryEpoch;
    if (node === null) return;
    $("matter-prepare").disabled = true;
    $("matter-commit").disabled = true;
    $("matter-backup").disabled = true;
    pendingProposal = null;
    $("matter-proposal").textContent = "Sprawdzam mapę i zapisuję kopię…";
    var payload={node:node};
    if($("matter-merge").checked)payload.thermohygrometer={
      temperature_endpoint:Number($("matter-temp-ep").value),humidity_endpoint:Number($("matter-hum-ep").value)};
    if($("matter-meter-pair").checked)payload.electricity_meter={
      relay_endpoint:Number($("matter-relay-ep").value),meter_endpoint:Number($("matter-meter-ep").value)};
    post("/api/matter/channels/prepare",payload).then(function (proposal) {
      if (epoch !== discoveryEpoch) return;
      pendingProposal = proposal;
      var kinds = ["","On/Off","Temperatura / wilgotność","Licznik energii"];
      var roles = ["On/Off","temperatura","wilgotność","napięcie","prąd","moc","energia pobrana","energia oddana"];
      var lines = proposal.channels.map(function (channel) {
        return "#" + channel.number + " " + kinds[channel.kind] + (channel.retired ? " · archiwalny, bez nowych pomiarów" : "") + " · " + channel.sources.map(function (source) {
          return roles[source.role] + " EP " + source.ep;
        }).join(", ");
      });
      (proposal.relations || []).forEach(function (relation) {
        lines.push("Powiązanie przekaźnika #" + relation.relay + " z licznikiem #" + relation.meter);
      });
      lines.push(proposal.history_note || "Numery, typy i historia obecnych kanałów zostają. Nowe kanały zaczną zbierać własną historię.");
      $("matter-proposal").textContent = lines.join("\n");
      $("matter-backup").disabled = false;
      $("matter-commit").disabled = false;
    }).catch(function (error) {
      if (epoch === discoveryEpoch) {
        $("matter-proposal").textContent = error.message;
        $("matter-prepare").disabled = false;
      }
    });
  });
  $("matter-backup").addEventListener("click", function () {
    if (!pendingProposal) return;
    download("matter2supla-mapa-kopia.json", JSON.stringify({
      encoding:"hex", mapping:pendingProposal.backup_hex, sha256:pendingProposal.backup_sha256
    },null,2), "application/json");
  });
  $("matter-commit").addEventListener("click", function () {
    if (!pendingProposal || !confirm("Zastosować pokazany plan kanałów i uruchomić bramkę ponownie? Historia nie jest przenoszona między kanałami.")) return;
    var token = pendingProposal.token, epoch = discoveryEpoch;
    $("matter-commit").disabled = true;
    post("/api/matter/channels/commit",{token:token}).then(function (result) {
      if (epoch === discoveryEpoch) $("matter-proposal").textContent = result.message;
    }).catch(function (error) {
      if (epoch === discoveryEpoch) {
        $("matter-proposal").textContent = error.message;
        $("matter-prepare").disabled = false;
      }
    });
  });
  function renderDiscovery(node, epoch, remaining) {
    request("/api/matter/discovery", {cache:"no-store"}).then(function (r) { return r.json(); }).then(function (j) {
      if (epoch !== discoveryEpoch) return;
      var box = $("matter-discovery-result");
      discoveryNode = j.state === "complete" && j.node === node ? node : null;
      $("matter-prepare").disabled = discoveryNode === null;
      var temps=(j.endpoints||[]).filter(function(ep){return ep.servers.indexOf(1026)>=0});
      var hums=(j.endpoints||[]).filter(function(ep){return ep.servers.indexOf(1029)>=0});
      $("matter-merge-box").hidden=!(discoveryNode && temps.length && hums.length);
      $("matter-merge").checked=false;
      if(discoveryNode && temps.length && hums.length) {
        $("matter-temp-ep").value=temps[0].id;$("matter-hum-ep").value=hums[0].id;
        $("matter-merge").checked=temps.length===1 && hums.length===1 && temps[0].id!==hums[0].id;
      }
      var relays=(j.endpoints||[]).filter(function(ep){return ep.servers.indexOf(6)>=0});
      var meters=(j.endpoints||[]).filter(function(ep){return ep.servers.indexOf(145)>=0});
      $("matter-meter-box").hidden=!(discoveryNode && relays.length && meters.length);
      $("matter-meter-pair").checked=!!(discoveryNode && relays.length===1 && meters.length===1);
      if(relays.length)$("matter-relay-ep").value=relays[0].id;
      if(meters.length)$("matter-meter-ep").value=meters[0].id;
      $("matter-ota-check").disabled=discoveryNode===null||otaOperation!==null;

      if (j.node !== node) { box.textContent = "Odczyt zastąpiono innym skanem."; return; }
      var states = {reading:"Odczytuję możliwości…", complete:"Odczytano listy klastrów", incomplete:"Odczyt niepełny", failed:"Urządzenie nie odpowiedziało", capacity_exceeded:"Przekroczono limit diagnostyki"};
      var labels = {6:"On/Off",8:"Ściemnianie",69:"Kontakt",1026:"Temperatura",1029:"Wilgotność",1030:"Obecność",768:"Kolor",513:"Termostat",258:"Osłona okienna",144:"Moc",145:"Energia"};
      box.textContent = "Węzeł " + node + " — " + (states[j.state] || j.state) + "\n" +
        (j.endpoints || []).map(function (ep) {
          return "EP " + ep.id + ": " + ep.servers.map(function (id) {
            return (labels[id] ? labels[id] + " " : "") + "(0x" + id.toString(16) + ")";
          }).join(", ");
        }).join("\n");
      var fields = {voltage_mv:"Napięcie [mV]",active_current_ma:"Prąd czynny [mA]",
        active_power_mw:"Moc czynna [mW]",imported_mwh:"Energia pobrana [mWh]",
        exported_mwh:"Energia oddana [mWh]",power_mode:"Tryb pomiaru",
        power_features:"FeatureMap mocy",energy_features:"FeatureMap energii"};
      (j.electrical || []).forEach(function (entry) {
        var readings = Object.keys(fields).filter(function (key) {
          return Object.prototype.hasOwnProperty.call(entry,key);
        }).map(function (key) {
          var value = entry[key];
          return fields[key] + ": " + (value === null ? "brak wartości" : value === false ? "błąd odczytu" : value);
        });
        if (readings.length) box.textContent += "\nEP " + entry.endpoint + " — " + readings.join(" · ");
      });
      if (j.electrical_truncated) box.textContent += "\nPrzekroczono limit endpointów pomiarowych.";
      if (j.state === "reading" && remaining > 0) setTimeout(function () {
        if (epoch === discoveryEpoch) renderDiscovery(node, epoch, remaining - 1);
      }, 2000);
    }).catch(function (error) {
      if (epoch === discoveryEpoch) $("matter-discovery-result").textContent = error.message;
    });
  }
  function setOtaOperation(operation) {
    otaOperation=operation;
    var button=$("matter-ota-check");
    button.disabled=discoveryNode===null||operation!==null;
    button.textContent=operation==="check"?"Sprawdzam…":"Sprawdź firmware";
    button.setAttribute("aria-busy",operation==="check"?"true":"false");
  }
  function pollOta(node,epoch,remaining) {
    request("/api/matter/ota?node="+node,{cache:"no-store"}).then(function(r){if(!r.ok)throw Error("Błąd odczytu OTA");return r.json()}).then(function(j){
      if(epoch!==discoveryEpoch || node!==discoveryNode)return;
      otaCandidate=j.state==="available"?j:null;
      $("matter-ota-install").disabled=!otaCandidate;
      var labels={idle:"Aktualizacja nie była sprawdzana",waiting:"Oczekiwanie na urządzenie i katalog DCL…",available:"Dostępna aktualizacja",authorized:"Zezwolono na jedną aktualizację. Oczekiwanie na transfer i potwierdzenie urządzenia.",no_confirmed_update:"Nie otrzymano potwierdzonej propozycji. Może nie być aktualizacji albo urządzenie lub katalog nie odpowiedziały."};
      $("matter-ota-status").textContent=(labels[j.state]||j.state)+
        (j.current_version!==undefined?"\nObecnie: "+(j.current_version_string||j.current_version):"")+
        (j.available_version!==undefined?"\nNowa wersja: "+(j.available_version_string||j.available_version)+" · "+(j.image_bytes?j.image_bytes+" B":"rozmiar niepodany"):"")+
        "\nŹródło: "+j.catalog;
      if(j.state==="waiting"||j.state==="authorized"){
        if(remaining>0){
          setTimeout(function(){if(epoch===discoveryEpoch)pollOta(node,epoch,remaining-1)},3000);
          return;
        }
        $("matter-ota-status").textContent="Nie udało się potwierdzić wyniku w wyznaczonym czasie.";
      }
      setOtaOperation(null);
    }).catch(function(e){finishOtaError(node,epoch,e);});
  }
  function finishOtaError(node,epoch,error) {
    if(epoch!==discoveryEpoch||node!==discoveryNode)return;
    $("matter-ota-status").textContent=error.message;
    setOtaOperation(null);
  }
  $("matter-ota-check").addEventListener("click",function(){
    if(discoveryNode===null||otaOperation!==null)return;
    var node=discoveryNode,epoch=discoveryEpoch;
    setOtaOperation("check");
    otaCandidate=null;$("matter-ota-install").disabled=true;
    $("matter-ota-status").textContent="Zlecam sprawdzenie aktualizacji…";
    post("/api/matter/ota",{node:node,action:"check"}).then(function(){
      if(epoch===discoveryEpoch)pollOta(node,epoch,40);
    }).catch(function(e){finishOtaError(node,epoch,e);});
  });
  $("matter-ota-install").addEventListener("click",function(){
    if(!otaCandidate || discoveryNode!==otaCandidate.node || otaOperation!==null)return;
    var candidate=otaCandidate,node=discoveryNode,epoch=discoveryEpoch;
    if(!confirm("Zaktualizować firmware urządzenia do "+(candidate.available_version_string||candidate.available_version)+"? Funkcja testowa: pełna instalacja nie była jeszcze sprawdzona na tym urządzeniu. Pozostaw zasilanie bramki i urządzenia oraz wybudź czujnik. Po zakończeniu trzeba ponownie odczytać wersję firmware."))return;
    setOtaOperation("install");
    $("matter-ota-install").disabled=true;otaCandidate=null;
    post("/api/matter/ota",{node:node,action:"install",version:candidate.available_version}).then(function(){
      if(epoch===discoveryEpoch)pollOta(node,epoch,200);
    }).catch(function(e){finishOtaError(node,epoch,e);});
  });
  var openDevices={};
  function renderDevices() {
    stashMatterCapabilities();
    Array.prototype.forEach.call(document.querySelectorAll("details.device"),function(el){openDevices[el.getAttribute("data-node")]=el.open;});
      $("devices-msg").textContent = "";
      var query = $("device-search").value.toLocaleLowerCase("pl").trim();
      var list = deviceCache.filter(function (d) { return (String(d.name || "") + " " + d.node).toLocaleLowerCase("pl").indexOf(query) >= 0; });
      list.sort(function (a,b) { return $("device-sort").value === "name" ? String(a.name).localeCompare(String(b.name),"pl") : a.node-b.node; });
      if (!list.length) {
        $("devices").innerHTML = '<div class="empty">' + (query ? 'Brak urządzeń pasujących do wyszukiwania.' : 'Jeszcze tu pusto. Dodaj pierwsze urządzenie Matter.') + '</div>';
        return;
      }
      $("devices").innerHTML = list.map(function (d) {
        return '<details class="device" data-node="' + d.node + '"'+(openDevices[d.node]?' open':'')+'>' +
          '<summary class="head"><span>' + esc(d.name || "Urządzenie") + '</span><span class="node-tag">node ' + d.node + ' · rozwiń</span></summary>' +
          '<div class="meta">' + (d.vid ? "Vendor 0x" + d.vid.toString(16) + " / Product 0x" + (d.pid || 0).toString(16) : "Producent: brak danych") + '</div>' +
          '<div class="device-live" data-live-node="' + d.node + '">' + liveMarkup(d.telemetry,d.node) + '</div>' +
          '<div class="actions">' +
            '<input class="dev-name" type="text" maxlength="39" aria-label="Nazwa urządzenia node ' + d.node + '" value="' + esc(d.name || "") + '">' +
            '<button class="btn small-btn save-dev">Zapisz</button>' +
            '<button class="btn small-btn scan-dev">Sprawdź klastry</button>' +
            '<button class="btn small-btn danger del-dev">Usuń z bramki</button>' +
          '</div></details>';
      }).join("");
      Array.prototype.forEach.call(document.querySelectorAll(".device"), function (el) {
        var node = el.getAttribute("data-node");
        placeMatterCapabilities(el,node);
        el.addEventListener("toggle",function(){
          openDevices[node]=el.open;
          if(el.open && !el.querySelector(".live-values"))loadDeviceTelemetry();
        });
        el.querySelector(".save-dev").addEventListener("click", function () {
          if (!el.querySelector(".dev-name").value.trim()) { toast("Nazwa urządzenia nie może być pusta."); return; }
          post("/api/device/rename", { node: Number(node), name: el.querySelector(".dev-name").value })
            .then(function (j) { if (j.ok) { loadDevices(); toast("Zapisano nazwę urządzenia."); } else toast(j.message || "Nie zapisano nazwy."); }).catch(function () { toast("Brak odpowiedzi bramki."); });
        });
        el.querySelector(".scan-dev").addEventListener("click", function () {
          var epoch = ++discoveryEpoch;
          clearProposal();
          discoveryOwner=node;
          var device=deviceCache.find(function(d){return d.node===Number(node);});
          $("matter-capabilities-heading").textContent="Możliwości Matter · "+(device&&device.name?device.name:"node "+node);
          $("matter-capabilities").open=true;
          placeMatterCapabilities(el,node);
          $("matter-discovery-result").textContent = "Zlecam odczyt klastrów…";
          post("/api/matter/discovery", {node:Number(node)}).then(function () {
            renderDiscovery(Number(node), epoch, 31);
          }).catch(function (error) {
            if (epoch === discoveryEpoch) $("matter-discovery-result").textContent = error.message;
          });
        });
        el.querySelector(".del-dev").addEventListener("click", function () {
          if (!confirm("Usunąć urządzenie (node " + node + ") z bramki Matter? Jeśli zechcesz je dodać ponownie, trzeba będzie je sparować.")) return;
          post("/api/device/remove", { node: Number(node) }).then(function (j) {
            if (!j.ok) alert(j.message || "Błąd usuwania");
            loadDevices(); tick();
          }).catch(function () { toast("Nie udało się zlecić usunięcia urządzenia."); });
        });
      });
  }

  /* ---- wifi ----------------------------------------------------------------- */
  function loadWifiInfo() {
    request("/api/status", { cache: "no-store" }).then(function (r) { return r.json(); }).then(function (j) {
      var w = j.wifi || {};
      $("wifi-current").textContent = w.mode === "ap"
        ? "Tryb konfiguracji: bramka wystawia własną sieć \"" + (w.ssid || "") + "\". Połącz się z nią i wybierz docelową sieć poniżej. Panel: https://192.168.4.1"
        : (w.connected
          ? "Połączono z: " + w.ssid + " (" + w.ip + ", " + w.rssi + " dBm)"
          : "Brak połączenia z Wi-Fi.");
      if (!$("wifi-ssid").dataset.dirty) $("wifi-ssid").value = w.mode === "ap" ? "" : (w.ssid || "");
    }).catch(function () { $("wifi-current").textContent = "Brak odpowiedzi bramki."; });
  }
  $("btn-wifi-scan").addEventListener("click", function () {
    $("wifi-msg").textContent = "Skanuję...";
    $("wifi-msg").className = "msg";
    request("/api/wifi/scan", { cache: "no-store" }).then(function (r) { return r.json(); }).then(function (j) {
      var nets = j.networks || [];
      if (!nets.length) { $("wifi-msg").textContent = "Nie znaleziono sieci."; return; }
      $("wifi-msg").textContent = "Kliknij sieć na liście, aby wpisać nazwę.";
      $("wifi-networks").innerHTML = nets.map(function (n) {
        return '<button class="wifi" type="button" data-ssid="' + esc(n.ssid) + '"><span>' + esc(n.ssid) + '</span><span class="rssi">' + n.rssi + ' dBm' + (n.auth === "open" ? " · otwarta" : "") + '</span></button>';
      }).join("");
      Array.prototype.forEach.call(document.querySelectorAll(".wifi"), function (el) {
        el.addEventListener("click", function () {
          $("wifi-ssid").value = el.getAttribute("data-ssid");
          $("wifi-ssid").dataset.dirty = "1";
          $("wifi-pass").focus();
        });
      });
    }).catch(function () { $("wifi-msg").textContent = "Błąd skanowania."; });
  });
  $("btn-wifi-connect").addEventListener("click", function () {
    var ssid = $("wifi-ssid").value;
    var pass = $("wifi-pass").value;
    if (new TextEncoder().encode(ssid).length > 32 || (pass && !(/^[0-9a-f]{64}$/i.test(pass) || (new TextEncoder().encode(pass).length >= 8 && new TextEncoder().encode(pass).length <= 63)))) { toast("SSID: maks. 32 bajty. Hasło: 8–63 bajty lub 64 znaki hex."); return; }
    if (!ssid) { $("wifi-msg").textContent = "Podaj nazwę sieci."; $("wifi-msg").className = "msg err"; return; }
    $("wifi-msg").textContent = "Łączę z \"" + ssid + "\"...";
    $("wifi-msg").className = "msg";
    post("/api/wifi/connect", { ssid: ssid, pass: $("wifi-pass").value }).then(function (j) {
      $("wifi-msg").textContent = j.message || (j.ok ? "Łączę..." : "Błąd");
      $("wifi-msg").className = "msg " + (j.ok ? "ok" : "err");
      setTimeout(tick, 3000);
    }).catch(function () { $("wifi-msg").textContent = "Brak odpowiedzi bramki. Sprawdź adres IP po zmianie sieci."; $("wifi-msg").className = "msg err"; });
  });
  $("btn-wifi-forget").addEventListener("click", function () {
    if (!confirm("Usunąć zapisane Wi-Fi i zrestartować bramkę w trybie konfiguracji (własna sieć AP)?")) return;
    post("/api/wifi/forget", {}).then(function (j) {
      $("wifi-msg").textContent = j.message || "Restart...";
      $("wifi-msg").className = "msg ok";
    }).catch(function(e){$("wifi-msg").textContent=e.message;$("wifi-msg").className="msg err";});
  });

  /* ---- supla ------------------------------------------------------------------ */
  function suplaLabel(state) {
    switch (state) {
      case "connected": return "połączone z chmurą";
      case "connecting": return "łączenie z chmurą...";
      case "starting": return "start...";
      case "error": return "błąd - sprawdź dane";
      default: return "nieskonfigurowane";
    }
  }
  function fallbackCopy(text) {
    var ta = document.createElement("textarea");
    ta.value = text;
    ta.setAttribute("readonly", "");
    ta.style.position = "fixed";
    ta.style.top = "-1000px";
    document.body.appendChild(ta);
    ta.select();
    ta.setSelectionRange(0, text.length);
    var ok = false;
    try { ok = document.execCommand("copy"); } catch (e) { ok = false; }
    document.body.removeChild(ta);
    return ok;
  }
  function copySuplaField(inputId) {
    var text = ($(inputId).value || "").trim();
    if (!text) {
      $("supla-msg").textContent = "Pole jest puste.";
      $("supla-msg").className = "msg err";
      return;
    }
    function done(ok) {
      $("supla-msg").textContent = ok
        ? "Skopiowano do schowka."
        : "Nie udało się skopiować - zaznacz pole i skopiuj ręcznie.";
      $("supla-msg").className = "msg " + (ok ? "ok" : "err");
    }
    if (navigator.clipboard && window.isSecureContext) {
      navigator.clipboard.writeText(text).then(function () { done(true); }, function () { done(fallbackCopy(text)); });
    } else {
      done(fallbackCopy(text));
    }
  }
  function loadSupla() {
    request("/api/supla", { cache: "no-store" }).then(function (r) { if (!r.ok) throw new Error("HTTP"); return r.json(); }).then(function (j) {
      $("supla-state").textContent = suplaLabel(j.state) + (j.message ? " (" + j.message + ")" : "");
      $("supla-state").className = "hint " + (j.state === "error" ? "err" : (j.state === "connected" ? "ok" : ""));
      if (!suplaDirty) {
        $("supla-server").value = j.server || ""; $("supla-email").value = j.email || "";
        $("supla-guid").value = j.guid || ""; $("supla-authkey").placeholder = j.authkey_set ? "Zapisany — Pokaż lub wpisz nowy" : "32 znaki hex";
      }
    }).catch(function () { $("supla-state").textContent = "Nie można odczytać statusu SUPLI."; });
  }
  $("btn-supla-copy-guid").addEventListener("click", function () { copySuplaField("supla-guid"); });
  $("btn-supla-copy-key").addEventListener("click", function () { revealKey().then(function(){copySuplaField("supla-authkey");}).catch(function(e){toast(e.message);}); });
  $("btn-supla-save").addEventListener("click", function () {
    if (!$("supla-server").value.trim() || /\s|\//.test($("supla-server").value.trim())) { toast("Wpisz adres serwera bez https:// i spacji."); return; }
    if (!$("supla-email").value.trim() || !$("supla-email").checkValidity()) { toast("Wpisz prawidłowy e-mail konta."); return; }
    if (["supla-guid","supla-authkey"].some(function(id) { return $(id).value && !/^[0-9a-f]{32}$/i.test($(id).value.trim()); })) { toast("GUID i AuthKey muszą mieć po 32 znaki hex."); return; }
    $("supla-msg").textContent = "Zapisuję...";
    $("supla-msg").className = "msg";
    post("/api/supla/config", {
      server: $("supla-server").value.trim(),
      email: $("supla-email").value.trim(),
      guid: $("supla-guid").value.trim(),
      authkey: $("supla-authkey").value.trim()
    }).then(function (j) {
      $("supla-msg").textContent = j.message || (j.ok ? "Zapisano." : "Błąd");
      $("supla-msg").className = "msg " + (j.ok ? "ok" : "err");
      if (j.ok) {
        suplaDirty = false; keyDirty = false; $("supla-authkey").value = "";
        if (j.guid) { $("supla-guid").value = j.guid; }
        if (j.authkey) { keyDirty=false; $("supla-authkey").value = j.authkey; expireKey(); }
      }
      if (j.ok && j.restart) {
        setTimeout(function () {
          $("supla-msg").textContent = "Bramka się restartuje... odśwież stronę za ~20 s.";
        }, 1300);
      } else {
        loadSupla();
      }
    }).catch(function () {
      $("supla-msg").textContent = "Błąd połączenia z bramką.";
      $("supla-msg").className = "msg err";
    });
  });
  $("btn-supla-newid").addEventListener("click", function () {
    if (!confirm("Wygenerować nowe GUID + AuthKey? Stare dane w cloud.supla.org przestaną pasować.")) return;
    post("/api/supla/new_identity", {}).then(function (j) {
      $("supla-msg").textContent = j.message || (j.ok ? "OK" : "Błąd");
      $("supla-msg").className = "msg " + (j.ok ? "ok" : "err");
      if (j.ok) {
        if (j.guid) { $("supla-guid").value = j.guid; }
        if (j.authkey) { keyDirty=false; $("supla-authkey").value = j.authkey; expireKey(); }
        if (j.restart) {
          setTimeout(function () {
            $("supla-msg").textContent = "Bramka się restartuje... odśwież stronę za ~20 s.";
          }, 1300);
        }
      }
    }).catch(function () {
      $("supla-msg").textContent = "Błąd połączenia z bramką.";
      $("supla-msg").className = "msg err";
    });
  });

  /* ---- system --------------------------------------------------------------- */
  $("btn-reboot").addEventListener("click", function () {
    if (!confirm("Zrestartować bramkę?")) return;
    post("/api/system/reboot", {}).then(function () {
      $("sys-msg").textContent = "Przyjęto polecenie restartu. Odśwież stronę za ~15 s.";
      $("sys-msg").className = "msg ok";
    }).catch(function(e){$("sys-msg").textContent=e.message;$("sys-msg").className="msg err";});
  });

  /* ---- logi ------------------------------------------------------------------ */
  function loadLogs() {
    if (logsPaused) return;
    request("/api/logs?tail=300", { cache: "no-store" }).then(function (r) { if (!r.ok) throw new Error("HTTP"); return r.text(); }).then(function (t) {
      rawLogs = t; renderLogs();
    }).catch(function () { $("log-state").textContent = "Nie można pobrać logu — sprawdź połączenie"; });
  }
  function renderLogs() {
      var pre = $("logs");
      var atBottom = pre.scrollTop + pre.clientHeight >= pre.scrollHeight - 30;
      var filter = $("log-filter").value.toLowerCase();
      pre.textContent = rawLogs.split("\n").filter(function(line) { return line.toLowerCase().indexOf(filter) >= 0; }).join("\n") || "Brak pasujących wpisów.";
      if (atBottom) pre.scrollTop = pre.scrollHeight;
      $("log-state").textContent = logsPaused ? "Odświeżanie wstrzymane" : "Odświeżanie co 3 sekundy";
  }
  setInterval(function () { if (!document.hidden && activeTab() === "diagnostics") loadLogs(); }, 3000);
  Array.prototype.forEach.call(document.querySelectorAll("[data-open]"), function (b) { b.addEventListener("click", function () { openTab(b.dataset.open); }); });
  ["supla-server","supla-email","supla-guid","supla-authkey"].forEach(function(id) { $(id).addEventListener("input", function () { suplaDirty = true; }); });
  $("supla-authkey").addEventListener("input",function(){keyDirty=true;});
  $("wifi-ssid").addEventListener("input", function () { this.dataset.dirty = "1"; });
  $("device-search").addEventListener("input", renderDevices);
  $("device-sort").addEventListener("change", renderDevices);
  $("devices-refresh").addEventListener("click", loadDevices);
  $("log-filter").addEventListener("input", renderLogs);
  $("log-pause").addEventListener("click", function () { logsPaused = !logsPaused; this.textContent = logsPaused ? "Wznów" : "Wstrzymaj"; $("log-badge").textContent = logsPaused ? "Wstrzymano" : "Na żywo"; renderLogs(); if (!logsPaused) loadLogs(); });
  $("log-download").addEventListener("click", function () { if (!rawLogs) { toast("Brak logu do pobrania."); return; } download("matter2supla-log.txt", rawLogs, "text/plain;charset=utf-8"); });
  function expireKey() {
    clearTimeout(keyTimer); keyTimer=setTimeout(function(){ if (!keyDirty) $("supla-authkey").value=""; $("supla-authkey").type="password"; $("toggle-authkey").textContent="Pokaż"; },30000);
  }
  function revealKey() {
    if ($("supla-authkey").value) { expireKey(); return Promise.resolve(); }
    return post("/api/supla/reveal_key",{}).then(function(j){ $("supla-authkey").value=j.authkey || ""; expireKey(); });
  }
  $("toggle-authkey").addEventListener("click",function(){
    if ($("supla-authkey").type === "text") { $("supla-authkey").type="password"; this.textContent="Pokaż"; return; }
    revealKey().then(function(){ $("supla-authkey").type="text"; $("toggle-authkey").textContent="Ukryj"; }).catch(function(e){toast(e.message);});
  });
  $("export-diagnostics").addEventListener("click", function () {
    if (!latestStatus) { toast("Poczekaj na odpowiedź bramki."); return; }
    var j = latestStatus;
    request("/api/devices", {cache:"no-store"}).then(function(r) { if (!r.ok) throw new Error("HTTP"); return r.json(); }).then(function(list) {
      var report = {exported_at:new Date().toISOString(), system:j.system,
        wifi:{connected:j.wifi.connected,mode:j.wifi.mode,ssid:j.wifi.ssid,ip:j.wifi.ip,rssi:j.wifi.rssi},
        thread:{role:j.thread.role,channel:j.thread.channel,dataset:j.thread.dataset},
        supla:{state:j.supla.state,code:j.supla.code}, devices:(list.devices||[]).map(function(d) { return {node:d.node,name:d.name,vid:d.vid,pid:d.pid}; })};
      download("matter2supla-diagnostyka.json",JSON.stringify(report,null,2),"application/json"); toast("Pobrano diagnostykę.");
    }).catch(function () { toast("Nie udało się pobrać diagnostyki."); });
  });
  if(window.initGatewayPanel)window.gatewayPanel=window.initGatewayPanel({request:request,post:post,epoch:function(){return authEpoch;},authorized:function(){return authorized;},activeTab:activeTab,download:download});
  var loginPending=false;
  $("panel-token-reveal").addEventListener("click",function(){
    var field=$("panel-token"),shown=field.type==="password";
    field.type=shown?"text":"password";
    this.textContent=shown?"Ukryj":"Pokaż";
    this.setAttribute("aria-pressed",shown?"true":"false");
  });
  function login() {
    if(loginPending)return;
    if (!/^[!-~]{5,64}$/.test(panelToken)) { lockPanel(); $("panel-login-msg").textContent="Wpisz hasło panelu (pierwsze wejście: admin) lub token."; return; }
    loginPending=true;
    $("panel-submit").disabled=true;
    $("panel-login-msg").textContent="Sprawdzam dostęp…";
    request("/api/status",{cache:"no-store"}).then(function(r){return r.json();}).then(function(j){
      if(j.setup_required) {
        $("panel-setup").hidden=false;
        $("panel-login-msg").textContent="Ustaw własne hasło, aby otworzyć panel.";
        return;
      }
      $("panel-setup").hidden=true;
      if (!j.system || !j.wifi) throw new Error("Nieprawidłowa odpowiedź bramki.");
      authorized=true; $("panel-login").hidden=true; $("panel-token").value="";
      Array.prototype.forEach.call(document.body.children,function(el){el.inert=false;});
      setMobileMenu(false,false);
      try{sessionStorage.setItem("m2s-panel-token",panelToken);}catch(_){} tick();
    }).catch(function(e){lockPanel();$("panel-login-msg").textContent=e.message;}).finally(function(){
      loginPending=false;
      $("panel-submit").disabled=false;
    });
  }
  $("panel-login-form").addEventListener("submit",function(e){
    e.preventDefault();
    if(loginPending)return;
    panelToken=$("panel-token").value.trim();
    login();
  });
  function savePanelPassword(firstSetup) {
    var prefix=firstSetup?"panel-setup":"panel-password", next=$(prefix+"-new").value;
    var message=$(firstSetup?"panel-login-msg":"panel-password-msg");
    if(!/^[!-~]{8,64}$/.test(next)) { message.textContent="Użyj 8–64 znaków ASCII, bez spacji."; return; }
    if(next!==$(prefix+"-repeat").value) { message.textContent="Hasła muszą być identyczne."; return; }
    post("/api/panel/password",{password:next}).then(function(){
      lockPanel();
      $(prefix+"-new").value=""; $(prefix+"-repeat").value="";
      $("panel-setup").hidden=true;
      $("panel-login-msg").textContent="Hasło zapisane. Zaloguj się nowym hasłem.";
    }).catch(function(e){message.textContent=e.message;});
  }
  $("btn-panel-password").addEventListener("click",function(){savePanelPassword(false);});
  $("btn-panel-setup").addEventListener("click",function(){savePanelPassword(true);});
  $("panel-logout").addEventListener("click",lockPanel);
  if(panelToken) login(); else lockPanel();
  function applyTheme(theme) {
    if (["light", "dark", "thread"].indexOf(theme) < 0) theme = "light";
    document.documentElement.dataset.theme = theme;
    $("theme-toggle").value = theme;
  }
  var savedTheme = "light";
  try { savedTheme = localStorage.getItem("m2s-theme") || "light"; } catch (_) {}
  applyTheme(savedTheme);
  $("theme-toggle").addEventListener("change", function () {
    applyTheme(this.value);
    try { localStorage.setItem("m2s-theme", this.value); } catch (_) {}
  });
})();
