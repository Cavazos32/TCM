#pragma once

const char PF_PAGE_HTML[] PROGMEM = R"html(
<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="Cache-Control" content="no-store">
<title>Prefeeder</title>
<style>
:root{--bg:#0d1117;--card:#161b22;--panel:#21262d;--border:#30363d;--text:#f0f6fc;--muted:#8b949e;--ok:#3fb950;--warn:#e2a41d;--err:#f85149;--blue:#58a6ff}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,-apple-system,sans-serif;background:var(--bg);color:var(--text);padding:16px;line-height:1.4}
.wrap{max-width:1040px;margin:0 auto}
.hdr{text-align:center;margin-bottom:16px;padding-bottom:12px;border-bottom:1px solid var(--border)}
.hdr h1{font-size:1.25rem;font-weight:600}
.status-banner{display:flex;align-items:center;gap:10px;padding:10px 12px;background:var(--bg);border:1px solid var(--border);border-radius:8px;margin-bottom:10px;font-size:.875rem}
.status-banner.ok{border-color:rgba(63,185,80,.35);background:rgba(63,185,80,.08)}
.status-banner.warn{border-color:rgba(226,164,29,.4);background:rgba(226,164,29,.1)}
.status-banner.err{border-color:rgba(248,81,73,.4);background:rgba(248,81,73,.1)}
.card{background:var(--card);border:1px solid var(--border);border-radius:10px;padding:14px 16px;margin-bottom:12px}
.card h2{font-size:.8rem;font-weight:600;margin-bottom:10px}
.card h3{font-size:.72rem;text-transform:uppercase;letter-spacing:.04em;color:var(--muted);margin:12px 0 8px}
.card h3:first-child{margin-top:0}
.note{font-size:.72rem;color:var(--muted);margin-bottom:10px}
.dot{width:10px;height:10px;border-radius:50%;background:var(--muted);flex-shrink:0}
.dot.ok{background:var(--ok);box-shadow:0 0 8px rgba(63,185,80,.5)}
.dot.err{background:var(--err)}
.meta{font-size:.78rem;color:var(--muted);margin-bottom:10px}
.meta strong{color:var(--text)}
.pills{display:grid;grid-template-columns:repeat(4,1fr);gap:6px;margin-bottom:10px}
.pill{text-align:center;font-size:.68rem;font-weight:600;padding:6px 4px;border-radius:6px;border:1px solid var(--border);color:var(--muted);background:var(--panel)}
.pill.on{background:rgba(63,185,80,.22);color:var(--ok);border-color:rgba(63,185,80,.5)}
#bench-card.on{border-color:rgba(88,166,255,.55);box-shadow:0 0 0 1px rgba(88,166,255,.25)}
.btn-row{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-top:8px}
button,.btn{padding:10px 8px;border-radius:8px;border:none;font-weight:600;font-size:.8rem;cursor:pointer;font-family:inherit}
.btn-go{background:var(--ok);color:#0d1117}
.btn-stop{background:var(--err);color:#fff}
.btn-warn{background:var(--warn);color:#0d1117}
.btn-save{background:rgba(88,166,255,.2);color:var(--blue);border:1px solid rgba(88,166,255,.4);width:100%;margin-top:10px;transition:background .15s,color .15s,border-color .15s}
.btn-save.saved{background:rgba(63,185,80,.28);color:#aff5b4;border-color:rgba(63,185,80,.55)}
.btn-save.err{background:rgba(248,81,73,.22);color:#ffa198;border-color:rgba(248,81,73,.45)}
.btn-refill{background:var(--panel);color:var(--text);border:1px solid var(--border);transition:background .12s,border-color .12s,box-shadow .12s}
.btn-refill.on{background:rgba(226,164,29,.35);color:#ffe8b3;border-color:rgba(226,164,29,.65);box-shadow:0 0 10px rgba(226,164,29,.35)}
.btn-refill.flash{background:rgba(88,166,255,.35);border-color:rgba(88,166,255,.6)}
.refill-bar.locked{opacity:.45;pointer-events:none}
.refill-bar.locked .note{color:var(--warn)}
.grid{display:grid;gap:12px}
@media(min-width:760px){.grid.cols-2{grid-template-columns:1fr 1fr;align-items:start}}
.col-stack{display:flex;flex-direction:column;gap:12px;min-width:0}
.col-stack>.card{margin-bottom:0}
label{display:block;font-size:.72rem;font-weight:600;color:var(--muted);margin:8px 0 4px}
input[type=number]{width:100%;padding:8px 10px;border-radius:6px;border:1px solid var(--border);background:var(--bg);color:var(--text)}
.f2{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.sensor-list{display:flex;flex-direction:column;gap:0}
.srow{display:flex;align-items:center;gap:10px;padding:8px 0;border-bottom:1px solid rgba(48,54,61,.6);font-size:.8rem}
.srow:last-child{border-bottom:none}
.led{width:28px;height:28px;border-radius:50%;border:2px solid var(--border);background:var(--bg);flex-shrink:0}
.led.on{background:var(--ok);border-color:rgba(63,185,80,.5);box-shadow:0 0 12px rgba(63,185,80,.4)}
.led.warn.on{background:var(--warn);border-color:rgba(226,164,29,.5);box-shadow:0 0 12px rgba(226,164,29,.4)}
.led.err.on{background:var(--err);border-color:rgba(248,81,73,.5)}
.stitle{flex:1;min-width:0}
.badge{font-size:.65rem;font-weight:700;padding:3px 8px;border-radius:999px;border:1px solid var(--border);color:var(--muted)}
.badge.on{color:var(--ok);border-color:rgba(63,185,80,.4);background:rgba(63,185,80,.1)}
.badge.warn.on{color:var(--warn);border-color:rgba(226,164,29,.4);background:rgba(226,164,29,.1)}
.badge.err.on{color:var(--err);border-color:rgba(248,81,73,.4);background:rgba(248,81,73,.1)}
.span2{grid-column:1/-1}
.refill-grid{display:flex;flex-direction:column;gap:12px}
@media(min-width:760px){.refill-grid{flex-direction:row;align-items:flex-end;gap:16px}.refill-pulse{flex:0 0 280px}.refill-btns{flex:1;margin-top:0!important}}
</style>
</head>
<body>
<div class="wrap">
<header class="hdr">
<h1>Prefeeder — [<span id="side-tag">—</span>]</h1>
</header>

<section class="card">
<h2>Control y estado</h2>
<div id="status-banner" class="status-banner" role="status" aria-live="polite"><span class="dot" id="status-dot"></span><span id="status-text">Conectando…</span></div>
<p class="meta" id="mode-line">Modo: <strong id="mode-label">—</strong> · Auto: <strong id="auto-label">—</strong> · TCP Maestro: <strong id="peer-label">—</strong></p>
<div class="pills">
<span class="pill" id="pill-feeder">Feeder</span>
<span class="pill" id="pill-buffer">Buffer</span>
<span class="pill" id="pill-trigger">Trigger</span>
</div>
<div class="btn-row">
<button type="button" class="btn-go" id="start">Iniciar</button>
<button type="button" class="btn-stop" id="stop">Detener</button>
<button type="button" class="btn-warn" id="reset">Reset</button>
</div>
</section>

<section class="card" id="bench-card">
<h2>Prueba en banco · simular En proceso</h2>
<p class="note">Arma holgura, buffer y alimentación como en lote (sin HMI). No usar si el Master/HMI ya controla el lado.</p>
<div class="btn-row">
<button type="button" class="btn-go" id="bench_on">Activar simulación</button>
<button type="button" class="btn-stop" id="bench_off">Desactivar</button>
<button type="button" class="btn-save" id="bench_trig" title="Prueba Tfeed local">Pulsar Tfeed</button>
</div>
<p class="meta">Simulación En proceso: <strong id="bench-label">—</strong></p>
</section>

<section class="card refill-bar" id="refill-bar">
<h2>Materialista · refill manual</h2>
<p class="note">Solo con modo Materialista activo (TCM/HMI). Mantén pulsado cada botón para mover; al soltar, para. El pulso (s) es solo para toques cortos desde la HMI.</p>
<div class="refill-grid">
<div class="refill-pulse f2">
<div><label>Pulso (s)</label><input id="refill_s" type="number" min="0.2" max="10" step="0.1"></div>
<div style="display:flex;align-items:flex-end"><button type="button" class="btn-save" id="saveRefill" style="margin-top:0">Guardar pulso</button></div>
</div>
<div class="btn-row refill-btns" style="grid-template-columns:repeat(4,1fr)">
<button type="button" class="btn-refill" data-ref="material">Material</button>
<button type="button" class="btn-refill" data-ref="dereeler">DeReeler</button>
<button type="button" class="btn-refill" data-ref="servo">Servo</button>
<button type="button" class="btn-refill" data-ref="feeder">Feeder</button>
</div>
</div>
</section>

<div class="grid cols-2">
<div class="col-stack">
<section class="card">
<h2>Sensores · buffer y seguridad</h2>
<div class="sensor-list">
<div class="srow"><div class="led" id="led-full"></div><span class="stitle">GPIO 19 · Buffer full</span><span class="badge" id="bdg-full">OFF</span></div>
<div class="srow"><div class="led" id="led-mid"></div><span class="stitle">GPIO 34 · Media altura</span><span class="badge" id="bdg-mid">OFF</span></div>
<div class="srow"><div class="led" id="led-high"></div><span class="stitle">GPIO 35 · Alta</span><span class="badge" id="bdg-high">OFF</span></div>
<div class="srow"><div class="led err" id="led-max"></div><span class="stitle">GPIO 21 · Buffer max</span><span class="badge" id="bdg-max">OFF</span></div>
<div class="srow"><div class="led warn" id="led-tension"></div><span class="stitle">GPIO 23 · Tensión</span><span class="badge" id="bdg-tension">OFF</span></div>
<div class="srow"><div class="led err" id="led-cyl"></div><span class="stitle">GPIO 25 · Cilindro</span><span class="badge" id="bdg-cyl">OFF</span></div>
<div class="srow"><div class="led err" id="led-hose"></div><span class="stitle">GPIO 27 · Manguera</span><span class="badge" id="bdg-hose">OFF</span></div>
<div class="srow"><div class="led" id="led-holg"></div><span class="stitle">GPIO 22 · Holgura</span><span class="badge" id="bdg-holg">OFF</span></div>
</div>
</section>

<section class="card">
<h2>DeReeler</h2>
<label>RPM</label><input id="rpm" type="number" min="1" max="600" step="1">
<h3>Impulso media altura (GPIO 34)</h3>
<div class="f2"><div><label>RPM boost</label><input id="mid_rpm" type="number"></div><div><label>Duración (s)</label><input id="mid_s" type="number" step="0.1"></div></div>
<label>Servo Δµs</label><input id="mid_dus" type="number" step="10">
<h3>Impulso alta (GPIO 35)</h3>
<div class="f2"><div><label>RPM boost</label><input id="hi_rpm" type="number"></div><div><label>Duración (s)</label><input id="hi_s" type="number" step="0.1"></div></div>
<label>Servo Δµs</label><input id="hi_dus" type="number" step="10">
<button type="button" class="btn-save" id="saveDer">Guardar DeReeler</button>
</section>
</div>

<div class="col-stack">
<section class="card">
<h2>Feeder · holgura y Tfeed</h2>
<p class="note">GPIO 22 OFF → alimenta hasta flanco ON; asentamiento tras flanco; luego n s de feeder (sin mirar el sensor). Tras alimentar, OFF sostenido &gt; N s → fallo. Tfeed TCP solo si está permitido y holgura ON al pulso.</p>
<div class="f2">
<div><label>RPM Feeder</label><input id="m2" type="number" min="1" max="600"></div>
<div><label>Tfeed (s)</label><input id="trig" type="number" min="0.1" max="60" step="0.1"></div>
</div>
<div class="f2">
<div><label>Asentamiento tras flanco (ms)</label><input id="holg_set" type="number" min="10" max="500" step="10"></div>
<div><label>Tras flanco ON (s)</label><input id="holg_on" type="number" min="0.1" max="120" step="0.1"></div>
</div>
<div><label>Fallo OFF tras ciclo (s)</label><input id="holg_off" type="number" min="0.5" max="300" step="0.5"></div>
<label><input type="checkbox" id="tfeed_en"> Permitir Tfeed por trigger (como HMI)</label>
<button type="button" class="btn-save" id="saveFeed">Guardar Feeder</button>
</section>
<section class="card">
<h2>Sistema · buffer</h2>
<p class="note">DeReeler/servo con buffer no lleno (GPIO 19). Timeout si no llena a tiempo.</p>
<label>Timeout sin buffer lleno (s)</label><input id="buf_to" type="number" min="1" max="120" step="0.5">
<button type="button" class="btn-save" id="saveSys">Guardar sistema</button>
</section>
<section class="card">
<h2>Servo RC</h2>
<label>PWM activo (µs)</label><input id="servo" type="number" min="500" max="2500" step="10">
<p class="note">Durante relleno de buffer el servo queda en marcha continua (mismo PWM + delta de boost).</p>
<button type="button" class="btn-save" id="saveServo">Guardar Servo</button>
</section>
<section class="card">
<h2>Inversión por tensión</h2>
<div class="f2">
<div><label>Duración inversión (s)</label><input id="reverse" type="number" min="0.1" max="60" step="0.1"></div>
<div><label>RPM inversión</label><input id="t_rev" type="number" min="1" max="600" step="1"></div>
</div>
<div class="f2">
<div><label>Cooldown (s)</label><input id="t_cd" type="number" min="0" max="60" step="0.1"></div>
<div><label>Fallo tensión (s)</label><input id="t_fault" type="number" min="1" max="60" step="0.1"></div>
</div>
<button type="button" class="btn-save" id="saveTension">Guardar tensión</button>
</section>
</div>
</div>
</div>
<script>
function q(id){return document.getElementById(id)}
var FAULTS={0:"",1:"Buffer max",2:"Tensión",3:"Cilindro",4:"Manguera",5:"Timeout buffer",6:"Holgura",7:"Operador"};
function setLed(el,bdg,on,warn,err){
  el.className="led"+(warn?" warn":"")+(err?" err":"")+(on?" on":"");
  bdg.className="badge"+(warn?" warn":"")+(err?" err":"")+(on?" on":"");
  bdg.textContent=on?"ON":"OFF";
}
function enc(v){return encodeURIComponent(String(v))}
function setInp(id,v){var el=q(id);if(!el)return;el.value=v;}
function setChk(id,on){var el=q(id);if(!el)return;el.checked=!!on;}
var _formReady=false;
var _btnSaveT={};
var _bannerFlashUntil=0;
function setStatusBanner(msg,kind){
  var ban=q('status-banner'),dot=q('status-dot'),st=q('status-text');
  if(!ban||!st)return;
  ban.className='status-banner'+(kind?' '+kind:'');
  st.textContent=msg;
  if(dot){
    dot.className='dot'+(kind==='ok'?' ok':'')+(kind==='err'?' err':'');
  }
}
function flashBanner(msg,kind,ms){
  setStatusBanner(msg,kind||'err');
  _bannerFlashUntil=Date.now()+(ms||3500);
}
function btnSaveFeedback(btn,ok,errMs,noBanner){
  if(!btn)return;
  var id=btn.id||'save';
  clearTimeout(_btnSaveT[id]);
  if(!btn.dataset.label)btn.dataset.label=btn.textContent;
  var label=btn.dataset.label;
  btn.classList.remove('saved','err');
  if(ok){
    btn.classList.add('saved');
    btn.textContent='Guardado';
    _btnSaveT[id]=setTimeout(function(){btn.classList.remove('saved');btn.textContent=label;},1000);
  }else{
    btn.classList.add('err');
    btn.textContent='Error';
    if(!noBanner)flashBanner('No se pudo guardar','err');
    _btnSaveT[id]=setTimeout(function(){btn.classList.remove('err');btn.textContent=label;},errMs||1800);
  }
}
function sync(j,opt){
  opt=opt||{};var force=!!opt.forceForm;
  var a=j.auto||{},t=j.trigger2||{},r=j.refill||{},s=j.sensors||{},h=j.home||{},act=j.actuators||{};
  q('side-tag').textContent=j.side||"—";
  if(force){
    setInp('rpm',a.rpm);setInp('reverse',a.reverse_s);setInp('buf_to',a.buffer_refill_fault_s);
    setInp('t_rev',a.tension_reverse_rpm);setInp('t_cd',a.tension_cooldown_s);setInp('t_fault',a.tension_fault_s);
    setInp('servo',a.servo_pwm_us);
    setInp('mid_rpm',a.mid_boost_rpm);setInp('mid_s',a.mid_boost_s);setInp('mid_dus',a.mid_servo_delta_us);
    setInp('hi_rpm',a.high_boost_rpm);setInp('hi_s',a.high_boost_s);setInp('hi_dus',a.high_servo_delta_us);
    setInp('m2',t.m2_rpm);setInp('trig',t.trigger_feed_s);
    setInp('holg_set',t.holgura_settle_ms);setInp('holg_on',t.holgura_on_s);setInp('holg_off',t.holgura_off_fault_s);
    setChk('tfeed_en',t.hmi_tfeed_enabled!==false);
    setInp('refill_s',r.pulse_s);
  }
  var fault=j.fault|0,peer=j.peer||{};
  var bdgFull=q('bdg-full'),ledFull=q('led-full');
  if(h.stable){setLed(ledFull,bdgFull,true);bdgFull.textContent='LLENO';}
  else if(h.raw){ledFull.className='led warn on';bdgFull.className='badge warn on';bdgFull.textContent='RAW';}
  else{setLed(ledFull,bdgFull,false);}
  setLed(q('led-mid'),q('bdg-mid'),!!s.mid);
  setLed(q('led-high'),q('bdg-high'),!!s.high);
  setLed(q('led-max'),q('bdg-max'),!!s.max,false,true);
  setLed(q('led-tension'),q('bdg-tension'),!!s.tension,true);
  setLed(q('led-cyl'),q('bdg-cyl'),!!s.cylinder,false,true);
  setLed(q('led-hose'),q('bdg-hose'),!!s.hose,false,true);
  setLed(q('led-holg'),q('bdg-holg'),!!s.holgura);
  function pill(id,on){var e=q(id);if(!e)return;e.className='pill'+(on?' on':'');}
  pill('pill-feeder',!!act.feeder);
  pill('pill-buffer',!!act.buffer);
  pill('pill-trigger',!!t.trigger_active);
  if(Date.now()>=_bannerFlashUntil){
    if(fault)setStatusBanner("Falla: "+(FAULTS[fault]||("código "+fault)),'err');
    else if(a.enabled&&j.inProcess&&!j.idleMode)setStatusBanner('Producción armada','ok');
    else if(a.enabled)setStatusBanner('Auto ON · esperando in process','ok');
    else setStatusBanner('Detenido','');
  }
  var mode=j.benchInProcess?"Banco · En proceso":(j.idleMode?"Materialista":(j.inProcess?"En proceso":"Idle"));
  q('mode-label').textContent=mode;
  var bc=q('bench-card'),bl=q('bench-label');
  if(bl)bl.textContent=j.benchInProcess?"ACTIVA":"OFF";
  if(bc)bc.classList.toggle('on',!!j.benchInProcess);
  q('auto-label').textContent=a.enabled?"ON":"OFF";
  q('peer-label').textContent=peer.link?"Enlace":"Sin enlace";
  var rb=q('refill-bar'),mr=j.manualRefill||{};
  if(rb)rb.classList.toggle('locked',!j.idleMode);
  function refOn(k){return !!mr[k];}
  document.querySelectorAll('[data-ref]').forEach(function(b){
    var k=b.getAttribute('data-ref')||'';
    b.classList.toggle('on',refOn(k));
    b.disabled=!j.idleMode;
  });
}
function cfg(u,btn){
  fetch(u,{cache:'no-store'}).then(function(r){if(!r.ok)throw new Error('http');return r.json();})
    .then(function(j){sync(j,{forceForm:true});btnSaveFeedback(btn,true);})
    .catch(function(){btnSaveFeedback(btn,false);});
}
function pull(){fetch('/api/status',{cache:'no-store'}).then(function(r){if(!r.ok)throw new Error('http');return r.json()}).then(function(j){sync(j,_formReady?{}:{forceForm:true});_formReady=true;}).catch(function(){if(Date.now()>=_bannerFlashUntil)setStatusBanner('Sin conexión con el equipo','err');})}
q('saveDer').onclick=function(){cfg('/api/config?rpm='+enc(q('rpm').value)+'&mid_boost_rpm='+enc(q('mid_rpm').value)+'&mid_boost_s='+enc(q('mid_s').value)+'&mid_servo_delta_us='+enc(q('mid_dus').value)+'&high_boost_rpm='+enc(q('hi_rpm').value)+'&high_boost_s='+enc(q('hi_s').value)+'&high_servo_delta_us='+enc(q('hi_dus').value),this)};
q('saveServo').onclick=function(){cfg('/api/config?servo_pwm_us='+enc(q('servo').value),this)};
q('saveTension').onclick=function(){cfg('/api/config?reverse='+enc(q('reverse').value)+'&tension_reverse_rpm='+enc(q('t_rev').value)+'&tension_cooldown='+enc(q('t_cd').value)+'&tension_fault_s='+enc(q('t_fault').value),this)};
q('saveSys').onclick=function(){cfg('/api/config?buffer_refill_fault_s='+enc(q('buf_to').value),this)};
q('saveFeed').onclick=function(){
  cfg('/api/config?m2_rpm='+enc(q('m2').value)+'&trigger_feed_s='+enc(q('trig').value)+'&holgura_settle_ms='+enc(q('holg_set').value)+'&holgura_on_s='+enc(q('holg_on').value)+'&holgura_off_fault_s='+enc(q('holg_off').value)+'&hmi_tfeed_enabled='+(q('tfeed_en').checked?'1':'0'),this);
};
q('saveRefill').onclick=function(){
  var btn=this;
  fetch('/api/refill?pulse_s='+enc(q('refill_s').value),{cache:'no-store'}).then(function(r){
    if(r.status===409){flashBanner('Pulso: requiere modo Materialista','err');btnSaveFeedback(btn,false,1800,true);throw new Error('idle');}
    if(!r.ok)throw new Error('http');return r.json();
  }).then(function(j){sync(j,{forceForm:true});btnSaveFeedback(btn,true);}).catch(function(e){if(e&&e.message!=='idle')btnSaveFeedback(btn,false);});
};
q('start').onclick=function(){
  fetch('/api/auto?enable=1',{cache:'no-store'}).then(function(r){
    if(r.status===409){flashBanner('Iniciar: activa Materialista desde HMI o espera in process','err');return pull();}
    return pull();
  });
};
q('stop').onclick=function(){fetch('/api/auto?enable=0').then(pull)};
q('reset').onclick=function(){fetch('/api/auto?reset=1').then(pull)};
q('bench_on').onclick=function(){fetch('/api/auto?bench=1',{cache:'no-store'}).then(function(r){if(!r.ok)throw new Error('http');return pull();}).catch(function(){flashBanner('No se pudo activar simulación','err');});};
q('bench_off').onclick=function(){fetch('/api/auto?bench=0',{cache:'no-store'}).then(pull)};
q('bench_trig').onclick=function(){
  fetch('/api/auto?trigger=1',{cache:'no-store'}).then(function(r){
    if(r.status===409){flashBanner('Tfeed prueba: activa simulación En proceso','err');return pull();}
    return pull();
  });
};
function refillChannel(ch,on){
  if(!ch)return;
  fetch('/api/refill?hold=1&'+ch+'='+(on?'1':'0'),{cache:'no-store'}).then(function(r){
    if(r.status===409){if(on)flashBanner('Refill: activa Materialista desde HMI','err');return;}
    pull();
  });
}
document.querySelectorAll('[data-ref]').forEach(function(b){
  var ch=b.getAttribute('data-ref');
  function down(e){
    if(b.disabled)return;
    if(e&&e.pointerType==='mouse'&&e.button!==0)return;
    e.preventDefault();
    b.classList.add('on','flash');
    refillChannel(ch,true);
  }
  function up(){
    b.classList.remove('on','flash');
    if(!b.disabled)refillChannel(ch,false);
  }
  b.addEventListener('pointerdown',down);
  b.addEventListener('pointerup',up);
  b.addEventListener('pointerleave',up);
  b.addEventListener('pointercancel',up);
});
pull();setInterval(pull,1500);
</script>
</body>
</html>
)html";
