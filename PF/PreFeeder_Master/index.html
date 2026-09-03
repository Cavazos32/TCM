#ifndef MASTER_HTML_H
#define MASTER_HTML_H

const char master_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="Cache-Control" content="no-store">
<title>PreFeeder Master</title>
<style>
:root{--bg:#0d1117;--card:#161b22;--border:#30363d;--txt:#f0f6fc;--muted:#8b949e;--ok:#3fb950;--err:#f85149;--warn:#e2a41d;--accent:#58a6ff}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,sans-serif;background:var(--bg);color:var(--txt);padding:16px}
.wrap{max-width:920px;margin:0 auto}
h1{font-size:1.1rem;margin-bottom:4px}
.sub{color:var(--muted);font-size:.8rem;margin-bottom:16px}
.comm{display:flex;gap:12px;flex-wrap:wrap;margin-bottom:16px}
.comm span{font-size:.75rem;padding:6px 10px;border:1px solid var(--border);border-radius:8px;background:var(--card)}
.comm .on{border-color:var(--ok);color:var(--ok)}
.comm .off{border-color:var(--err);color:var(--err)}
.comm .warn{border-color:var(--warn);color:var(--warn)}
.status-card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:14px;margin-bottom:14px}
.status-card h2{font-size:.85rem;margin-bottom:10px}
.status-bar{display:flex;align-items:center;gap:8px;font-size:.78rem;margin-bottom:8px}
.status-dot{width:10px;height:10px;border-radius:50%;background:#30363d;flex-shrink:0}
.status-dot.ok{background:var(--ok)}.status-dot.err{background:var(--err)}.status-dot.warn{background:var(--warn)}
.error-global{font-size:.82rem;padding:10px 12px;border-radius:8px;background:#0d1117;border:1px solid var(--border);min-height:2.4em}
.error-global.ok{border-color:var(--ok);color:var(--ok)}
.error-global.err{border-color:var(--err);color:var(--err)}
.error-global.warn{border-color:var(--warn);color:var(--warn)}
.paused{border:1px solid var(--warn);background:rgba(226,164,29,.08);padding:10px;border-radius:8px;margin-bottom:14px;font-size:.78rem}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:14px}
@media(max-width:700px){.grid{grid-template-columns:1fr}}
.panel{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:14px}
.panel h2{font-size:.85rem;margin-bottom:10px;display:flex;align-items:center;gap:8px}
.badge{font-size:.65rem;padding:2px 8px;border-radius:999px;border:1px solid var(--border);color:var(--muted)}
.badge.exec{border-color:var(--accent);color:var(--accent)}
.state{font-size:.75rem;color:var(--muted);margin-bottom:10px}
.sensors{display:grid;gap:6px}
.row{display:flex;justify-content:space-between;align-items:center;font-size:.78rem;padding:6px 8px;background:#0d1117;border-radius:6px}
.dot{width:10px;height:10px;border-radius:50%;background:#30363d;flex-shrink:0;margin-left:8px}
.dot.on{background:var(--ok)}.dot.alarm{background:var(--err)}.dot.warn{background:var(--warn)}
.ctrl{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:14px;margin-top:14px}
.ctrl h2{font-size:.85rem;margin-bottom:10px}
.ctrl p{font-size:.72rem;color:var(--muted);margin-bottom:10px}
.btns{display:flex;flex-wrap:wrap;gap:8px}
button{border:1px solid var(--border);background:#21262d;color:var(--txt);padding:8px 14px;border-radius:8px;font-size:.78rem;cursor:pointer}
button:hover{border-color:var(--accent)}
button.danger{background:var(--err);border-color:var(--err)}
button.primary{background:var(--accent);border-color:var(--accent);color:#0d1117}
.toggles{display:flex;flex-wrap:wrap;gap:12px;margin-top:10px;font-size:.78rem}
.toggles label{display:flex;align-items:center;gap:6px;cursor:pointer}
.shortcuts{display:flex;flex-wrap:wrap;gap:8px;margin-bottom:14px}
.shortcut{font-size:.75rem;padding:6px 12px;border:1px solid var(--border);border-radius:8px;background:var(--card);color:var(--accent);text-decoration:none}
.shortcut:hover{border-color:var(--accent)}
.panel-links{margin:-4px 0 10px;display:flex;gap:8px;flex-wrap:wrap}
.panel-links a{font-size:.72rem;color:var(--accent);text-decoration:none}
.panel-links a:hover{text-decoration:underline}
</style>
</head>
<body>
<div class="wrap">
<h1>PreFeeder Master</h1>
<p class="sub">Master orquesta L/R · errores agregados · control manual global · triggers/settings por lado (API)</p>
<div class="shortcuts">
<a class="shortcut" id="link-ui-l" href="/ui/l" target="_blank" rel="noopener">UI L · 10.10.32.101</a>
<a class="shortcut" id="link-ui-r" href="/ui/r" target="_blank" rel="noopener">UI R · 10.10.32.40</a>
</div>
<div id="paused-banner" class="paused" hidden>PAUSE (Nivel 2) — Reset o Detener para continuar</div>
<section class="status-card">
<h2>Comunicación</h2>
<div class="status-bar"><span class="status-dot ok" id="dot-wifi"></span><span id="txt-wifi">WiFi Master: …</span></div>
<div class="status-bar"><span class="status-dot" id="dot-ml"></span><span id="txt-ml">Master ↔ L (.30): …</span></div>
<div class="status-bar"><span class="status-dot" id="dot-mr"></span><span id="txt-mr">Master ↔ R (.40): …</span></div>
</section>
<section class="status-card">
<h2>Control &amp; Estado</h2>
<div class="error-global ok" id="error-global">Sin error — sistema OK</div>
</section>
<div class="grid">
<section class="panel" id="panel-l">
<h2>Lado L <span class="badge exec">ESCLAVO</span></h2>
<div class="panel-links"><a id="panel-link-l" href="/ui/l" target="_blank" rel="noopener">Abrir UI independiente L</a></div>
<div class="state" id="state-l">Auto: —</div>
<div class="sensors" id="sens-l"></div>
</section>
<section class="panel" id="panel-r">
<h2>Lado R <span class="badge exec">ESCLAVO</span></h2>
<div class="panel-links"><a id="panel-link-r" href="/ui/r" target="_blank" rel="noopener">Abrir UI independiente R</a></div>
<div class="state" id="state-r">Auto: —</div>
<div class="sensors" id="sens-r"></div>
</section>
</div>
<section class="ctrl">
<h2>Control manual (global L + R)</h2>
<p>Misma API que PF_LR (<code>/api/auto</code>): Iniciar/Detener/Reset, Materialista e In process se reenvían por TCP a L+R (<code>peerDoCmd</code>).</p>
<div class="btns">
<button class="primary" onclick="autoCmd('start')">Iniciar</button>
<button class="danger" onclick="autoCmd('stop')">Detener</button>
<button onclick="autoCmd('reset')">Reset</button>
</div>
<div class="toggles">
<label><input type="checkbox" id="tog-idle" onchange="setIdleMode(this.checked)"> Materialista</label>
<label><input type="checkbox" id="tog-proc" onchange="setInProcess(this.checked)"> In process</label>
</div>
</section>
</div>
<script>
var SENS=[['home','Buffer Full'],['endstop','Buffer Max'],['tension','Tensión'],['cylinderOpen','Cilindro abierto'],['hoseAbsent','Manguera ausente'],['holgura','Holgura']];
function mkSens(id){var el=document.getElementById(id);el.innerHTML=SENS.map(function(s){return '<div class="row"><span>'+s[1]+'</span><span class="dot" id="'+id+'-'+s[0]+'"></span></div>';}).join('');}
mkSens('sens-l');mkSens('sens-r');
function setDot(id,on,alarm){var d=document.getElementById(id);if(!d)return;d.className='dot'+(alarm?' alarm':(on?' on':''));}
function setDotEl(id,cls){var d=document.getElementById(id);if(d)d.className='status-dot '+cls;}
function linkTxt(side,info){
if(!info.tcp)return side+': sin TCP — esclavo apagado, IP distinta o puerto '+8765;
if(info.waiting)return side+': TCP ok, esperando status del esclavo…';
if(info.warn)return side+': datos lentos · '+info.msAgo+'ms';
return side+': ok · datos hace '+info.msAgo+'ms';
}
function linkDot(info){
if(!info.tcp)return 'err';
if(info.waiting)return 'warn';
if(info.ok)return 'ok';
return info.warn?'warn':'err';
}
function applySide(prefix,d){
var stEl=document.getElementById('state-'+prefix);
var errLine=d.error?' · FALTA ENCLAVADA':'';
stEl.textContent='Auto: '+(d.autoEnabled?'ON':'OFF')+' · '+d.autoState+errLine;
SENS.forEach(function(s){var k=s[0];setDot('sens-'+prefix+'-'+k,!!d[k],false);});
}
function applyErrorGlobal(me,paused){
var el=document.getElementById('error-global');
if(!me.active){el.textContent='Sin error — sistema OK (L+R)';el.className='error-global ok';return;}
var txt=me.tag||('PF-'+String(me.code).padStart(3,'0'));
txt+=' · Nivel '+me.level;
if(me.side&&me.side!=='-')txt+=' · lado '+me.side;
txt+=' · '+me.reason;
if(paused)txt+=' · PAUSE';
el.textContent=txt;el.className='error-global '+(me.level>=3?'err':(me.level>=2?'warn':'warn'));
}
function poll(){fetch('/api/status',{cache:'no-store'}).then(function(r){
if(!r.ok)throw new Error('HTTP '+r.status);
return r.json();
}).then(function(d){
var me=d.masterError||{};var c=d.comm||{};
var ml=c.masterL||{};var mr=c.masterR||{};
setDotEl('dot-wifi',d.wifiOk?'ok':'err');
document.getElementById('txt-wifi').textContent='WiFi Master: '+(d.wifiOk?('OK '+d.wifiIp):'sin red');
setDotEl('dot-ml',linkDot(ml));
setDotEl('dot-mr',linkDot(mr));
document.getElementById('txt-ml').textContent='Master ↔ '+linkTxt('L (.30)',ml);
document.getElementById('txt-mr').textContent='Master ↔ '+linkTxt('R (.40)',mr);
applyErrorGlobal(me,d.paused);
document.getElementById('paused-banner').hidden=!d.paused;
applySide('l',d.l);
applySide('r',d.r);
if(d.uiL){var a=document.getElementById('link-ui-l');if(a)a.href=d.uiL;var pl=document.getElementById('panel-link-l');if(pl)pl.href=d.uiL;}
if(d.uiR){var b=document.getElementById('link-ui-r');if(b)b.href=d.uiR;var pr=document.getElementById('panel-link-r');if(pr)pr.href=d.uiR;}
var idle=document.getElementById('tog-idle');if(idle&&!idle._user)idle.checked=!!(d.l.idleMode||d.r.idleMode);
var proc=document.getElementById('tog-proc');if(proc&&!proc._user)proc.checked=!!(d.l.inProcess||d.r.inProcess);
}).catch(function(e){
setDotEl('dot-wifi','err');
document.getElementById('txt-wifi').textContent='Poll: '+(e&&e.message?e.message:'sin respuesta')+' — prueba /ping y /api/health';
setDotEl('dot-ml','err');document.getElementById('txt-ml').textContent='Master ↔ L (.30): poll pendiente';
setDotEl('dot-mr','err');document.getElementById('txt-mr').textContent='Master ↔ R (.40): poll pendiente';
});}
function autoFetch(q){return fetch('/api/auto?'+q,{cache:'no-store'}).then(function(r){return r.json();}).then(function(d){if(!d.ok)throw new Error(d.error||'fail');poll();}).catch(function(e){alert(e&&e.message?e.message:'Error de conexión');});}
function autoCmd(action){
if(action==='start')return autoFetch('enable=1');
if(action==='stop')return autoFetch('enable=0');
if(action==='reset')return autoFetch('reset=1');
}
function setIdleMode(on){autoFetch('idle_mode='+(on?'1':'0'));}
function setInProcess(on){autoFetch('in_process='+(on?'1':'0'));}
['tog-idle','tog-proc'].forEach(function(id){var el=document.getElementById(id);el.addEventListener('mousedown',function(){el._user=true;setTimeout(function(){el._user=false;},800);});});
setInterval(poll,500);poll();
</script>
</body>
</html>
)rawliteral";

#endif
