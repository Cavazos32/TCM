#ifndef INDEX_H
#define INDEX_H

#include <Arduino.h>

static const char index_html[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="es">
<head>
  <meta charset="utf-8"/>
  <meta name="viewport" content="width=device-width,initial-scale=1"/>
  <title>Motion · ASDA + OM</title>
  <style>
    :root {
      --bg0: #101418;
      --bg1: #1a2229;
      --panel: #222b33;
      --line: #3a4652;
      --text: #eef2f5;
      --muted: #9aabba;
      --ok: #3dba7a;
      --bad: #e05252;
      --warn: #d4a017;
      --accent: #2a9d8f;
      --cw: #5ee07a;
      --ccw: #ff7a6e;
      --radius: 12px;
      --font: "Segoe UI", Tahoma, sans-serif;
      --mono: Consolas, "Courier New", monospace;
    }
    * { box-sizing: border-box; }
    html, body {
      margin: 0;
      min-height: 100%;
      color: var(--text);
      font-family: var(--font);
      background:
        radial-gradient(900px 480px at 0% 0%, #243840 0%, transparent 55%),
        radial-gradient(700px 420px at 100% 10%, #2c3420 0%, transparent 50%),
        linear-gradient(160deg, var(--bg0), var(--bg1));
    }
    body { padding: 1.25rem 1rem 2.5rem; }
    .wrap { max-width: 960px; margin: 0 auto; }
    header {
      display: flex;
      flex-wrap: wrap;
      justify-content: space-between;
      gap: 1rem;
      align-items: flex-end;
      margin-bottom: 1rem;
    }
    .brand {
      margin: 0;
      font-size: clamp(1.5rem, 4vw, 2.1rem);
      letter-spacing: -.02em;
    }
    .brand span { color: var(--accent); }
    .sub {
      margin: .3rem 0 0;
      color: var(--muted);
      font-family: var(--mono);
      font-size: .82rem;
    }
    .chip {
      font-family: var(--mono);
      font-size: .75rem;
      color: var(--muted);
      border: 1px solid var(--line);
      border-radius: 999px;
      padding: .35rem .7rem;
    }
    .tabs {
      display: flex;
      gap: .4rem;
      margin-bottom: 1rem;
      flex-wrap: wrap;
    }
    .tab {
      border: 1px solid var(--line);
      background: #151b21;
      color: var(--muted);
      border-radius: 999px;
      padding: .55rem 1.1rem;
      font-weight: 600;
      cursor: pointer;
      font-family: var(--font);
    }
    .tab.active {
      background: var(--accent);
      color: #04201c;
      border-color: var(--accent);
    }
    .panel { display: none; }
    .panel.active { display: block; }
    .banner {
      border: 1px solid var(--line);
      border-radius: var(--radius);
      padding: 1rem 1.1rem;
      margin-bottom: 1rem;
    }
    .banner.ok {
      border-color: var(--ok);
      background: color-mix(in srgb, var(--ok) 16%, #141a20);
    }
    .banner.bad {
      border-color: var(--bad);
      background: color-mix(in srgb, var(--bad) 16%, #141a20);
    }
    .banner.wait {
      border-color: var(--warn);
      background: color-mix(in srgb, var(--warn) 14%, #141a20);
    }
    .banner h2 { margin: 0; font-size: 1.05rem; }
    .banner p {
      margin: .3rem 0 0;
      font-family: var(--mono);
      font-size: .78rem;
      color: var(--muted);
    }
    .grid { display: grid; gap: 1rem; }
    @media (min-width: 800px) {
      .grid { grid-template-columns: 1fr 1fr; }
    }
    section {
      background: color-mix(in srgb, var(--panel) 90%, transparent);
      border: 1px solid var(--line);
      border-radius: var(--radius);
      padding: 1rem 1.1rem;
    }
    h2 {
      margin: 0 0 .85rem;
      font-size: .8rem;
      letter-spacing: .08em;
      text-transform: uppercase;
      color: var(--muted);
    }
    .stats {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: .55rem;
    }
    .stat {
      background: #151b21;
      border: 1px solid var(--line);
      border-radius: 8px;
      padding: .65rem .7rem;
    }
    .stat-hide { display: none !important; }
    .stat span, .k {
      display: block;
      font-size: .68rem;
      color: var(--muted);
      text-transform: uppercase;
    }
    .stat b, .v {
      font-family: var(--mono);
      font-size: 1rem;
      font-weight: 500;
    }
    .v { margin-top: 4px; font-size: 1.15rem; }
    label {
      display: block;
      font-size: .75rem;
      color: var(--muted);
      margin: 0 0 .3rem;
    }
    input, select {
      width: 100%;
      padding: .6rem .65rem;
      border-radius: 8px;
      border: 1px solid var(--line);
      background: #151b21;
      color: var(--text);
      font-family: var(--mono);
    }
    input[readonly] {
      color: var(--accent);
      opacity: 1;
      cursor: default;
    }
    .row {
      display: grid;
      gap: .65rem;
      margin-bottom: .75rem;
    }
    @media (min-width: 480px) {
      .row.c2 { grid-template-columns: 1fr 1fr; }
      .row.c3 { grid-template-columns: 1fr 1fr 1fr; }
    }
    .btns { display: flex; flex-wrap: wrap; gap: .5rem; }
    button {
      border: 0;
      border-radius: 8px;
      padding: .65rem .95rem;
      font-weight: 600;
      cursor: pointer;
      font-family: var(--font);
    }
    button:disabled { opacity: .45; cursor: not-allowed; }
    .btn { background: var(--accent); color: #04201c; }
    .btn2 { background: var(--warn); color: #1a1408; }
    .btnG {
      background: transparent;
      color: var(--text);
      border: 1px solid var(--line);
    }
    .btnD { background: var(--bad); color: #fff; }
    .log {
      margin-top: 1rem;
      border-top: 1px dashed var(--line);
      padding-top: .75rem;
      font-family: var(--mono);
      font-size: .78rem;
      color: var(--muted);
      white-space: pre-wrap;
      min-height: 3rem;
    }
    .full { grid-column: 1 / -1; }
    .hint { margin: 0 0 .8rem; color: var(--muted); font-size: .82rem; line-height: 1.45; }
    .preview {
      font-family: var(--mono);
      font-size: .82rem;
      color: var(--accent);
      margin: .35rem 0 .75rem;
    }
    .dial-wrap { display: flex; flex-direction: column; align-items: center; gap: 10px; }
    .dial-outer {
      width: min(260px, 70vw); aspect-ratio: 1; border-radius: 50%;
      background: #1a2229; position: relative;
      box-shadow: 0 0 0 8px #151b21 inset;
    }
    .dial-sweep {
      position: absolute; inset: 0; width: 100%; height: 100%;
      z-index: 2; pointer-events: none;
      transform: rotate(-90deg);
    }
    .dial-hub {
      position: absolute; inset: 22%; border-radius: 50%; background: #151b21;
      display: grid; place-items: center; z-index: 3;
    }
    .deg { font-size: 1.8rem; font-variant-numeric: tabular-nums; font-family: var(--mono); }
    .unit { color: var(--muted); font-size: .8rem; }
    .mm-hero {
      font-size: 2.2rem; font-variant-numeric: tabular-nums; text-align: center;
      margin: 4px 0 2px; color: var(--accent); font-family: var(--mono);
    }
    .signals { display: flex; gap: 10px; margin-top: 14px; }
    .sig {
      flex: 1; text-align: center; padding: 10px; border-radius: 10px;
      border: 1px solid var(--line); background: #151b21; font-size: .8rem;
    }
    .led {
      width: 14px; height: 14px; border-radius: 50%; margin: 6px auto 0;
      background: #2a354d;
    }
    .led.on { background: var(--accent); box-shadow: 0 0 10px var(--accent); }
    .led.z.on { background: var(--warn); box-shadow: 0 0 10px var(--warn); }
    .dir-cw { color: var(--cw); }
    .dir-ccw { color: var(--ccw); }
    .ok { color: var(--ok); }
    .badc { color: var(--bad); }
    .enc-foot { color: var(--muted); font-size: .78rem; margin-top: .85rem; line-height: 1.4; }
    .enc-dual { display: grid; gap: 1rem; }
    @media (min-width: 800px) {
      .enc-dual { grid-template-columns: 1fr 1fr; }
    }
    .enc-card {
      background: #151b21;
      border: 1px solid var(--line);
      border-radius: var(--radius);
      padding: .9rem 1rem 1rem;
    }
    .enc-card-head {
      display: flex; align-items: center; justify-content: space-between;
      margin-bottom: .75rem;
    }
    .enc-side-tag {
      font-weight: 700; color: var(--accent); letter-spacing: .04em;
      font-size: .95rem;
    }
    .enc-hw {
      font-family: var(--mono); font-size: .72rem; color: var(--muted);
      border: 1px solid var(--line); border-radius: 999px; padding: .15rem .55rem;
    }
    .enc-hw.on { color: var(--ok); border-color: rgba(61,186,122,.45); }
    .enc-hw.off { color: var(--bad); border-color: rgba(224,82,82,.45); }
    .hit-grid {
      display: grid;
      gap: .65rem;
      grid-template-columns: 1fr;
    }
    @media (min-width: 700px) {
      .hit-grid.cols-2 { grid-template-columns: 1fr 1fr; }
      .hit-grid.cols-3 { grid-template-columns: 1fr 1fr 1fr; }
    }
    .hit {
      background: #151b21;
      border: 1px solid var(--line);
      border-radius: 8px;
      padding: .75rem .8rem;
      text-align: center;
    }
    .hit .lbl {
      font-size: .68rem;
      color: var(--muted);
      text-transform: uppercase;
      letter-spacing: .06em;
    }
    .hit .val {
      font-family: var(--mono);
      font-size: 1.35rem;
      margin: .35rem 0 .45rem;
    }
    .hit .subv {
      font-family: var(--mono);
      font-size: .78rem;
      color: var(--muted);
      margin-bottom: .45rem;
    }
    .ind {
      display: inline-block;
      width: 14px;
      height: 14px;
      border-radius: 50%;
      background: #3a4652;
      box-shadow: inset 0 0 0 1px #556270;
    }
    .ind.pending { background: var(--warn); box-shadow: 0 0 8px color-mix(in srgb, var(--warn) 50%, transparent); }
    .ind.ok { background: var(--ok); box-shadow: 0 0 8px color-mix(in srgb, var(--ok) 55%, transparent); }
    .ind.bad { background: var(--bad); box-shadow: 0 0 8px color-mix(in srgb, var(--bad) 55%, transparent); }
    .ov-note {
      color: var(--muted);
      font-size: .78rem;
      line-height: 1.45;
      margin: 0 0 .75rem;
    }
  </style>
</head>
<body>
  <div class="wrap">
    <header>
      <div>
        <h1 class="brand">Motion <span>ESP32</span></h1>
        <p class="sub" id="headerSub">Vista general · ASDA + OM</p>
      </div>
      <div class="chip" id="ipChip">…</div>
    </header>

    <nav class="tabs" role="tablist">
      <button type="button" class="tab active" id="tabOv" onclick="showTab('ov')">Vista general</button>
      <button type="button" class="tab" id="tabAsda" onclick="showTab('asda')">ASDA B3</button>
      <button type="button" class="tab" id="tabEnc" onclick="showTab('enc')">OM Encoder</button>
      <a class="tab" href="/feed" style="text-decoration:none;display:inline-flex;align-items:center">Alimentación CAN</a>
    </nav>

    <!-- ========== VISTA GENERAL ========== -->
    <div class="panel active" id="panelOv">
      <div class="banner wait" id="ovBanner">
        <h2 id="ovBannerTitle">Monitoreo</h2>
        <p id="ovBannerDetail">OM mide alimentación · ASDA confirma posiciones</p>
      </div>

      <div class="grid">
        <section class="full">
          <h2>Pieza</h2>
          <p class="ov-note">
            OM decide longitud (55 → reset → L−G). Valores OM al settle; ASDA al terminar movimiento.
          </p>
          <div class="stats">
            <div class="stat"><span>Pieza L</span><b id="ovL">—</b></div>
            <div class="stat"><span>Linear Actuator L−G</span><b id="ovLinAct">—</b></div>
            <div class="stat"><span>Extra depósito</span><b id="ovExtraShow">—</b></div>
            <div class="stat"><span>Tol ±mm</span><b id="ovTol">—</b></div>
            <div class="stat"><span>Pieza OK</span><b id="ovPieceOk">—</b></div>
            <div class="stat"><span></span><button class="btnG" type="button" onclick="ovRefresh()">Actualizar</button></div>
          </div>
        </section>

        <section>
          <h2>ASDA lineal</h2>
          <div class="hit-grid cols-2">
            <div class="hit">
              <div class="lbl">Avance Linear Actuator</div>
              <div class="val" id="ovAsdaLinActVal">—</div>
              <div class="subv" id="ovAsdaLinActAct">final —</div>
              <span class="ind pending" id="ovAsdaLinActInd" title="pendiente"></span>
            </div>
            <div class="hit">
              <div class="lbl">Depósito (G + Linear Actuator + extra)</div>
              <div class="val" id="ovAsdaDepVal">—</div>
              <div class="subv" id="ovAsdaDepAct">final —</div>
              <span class="ind pending" id="ovAsdaDepInd" title="pendiente"></span>
            </div>
          </div>
        </section>

        <section>
          <h2>OM Encoder</h2>
          <div class="hit-grid cols-3">
            <div class="hit">
              <div class="lbl">Alimentación inicial</div>
              <div class="val" id="ovOmFeedVal">—</div>
              <div class="subv" id="ovOmFeedAct">settle —</div>
              <span class="ind pending" id="ovOmFeedInd" title="pendiente"></span>
            </div>
            <div class="hit">
              <div class="lbl">Reset</div>
              <div class="val" id="ovOmResetVal">0</div>
              <div class="subv" id="ovOmResetAct">—</div>
              <span class="ind pending" id="ovOmResetInd" title="pendiente"></span>
            </div>
            <div class="hit">
              <div class="lbl">Longitud final</div>
              <div class="val" id="ovOmFinalVal">—</div>
              <div class="subv" id="ovOmFinalAct">settle —</div>
              <span class="ind pending" id="ovOmFinalInd" title="pendiente"></span>
            </div>
          </div>
        </section>
      </div>
    </div>

    <!-- ========== VENTANA ASDA B3 ========== -->
    <div class="panel" id="panelAsda">
      <div class="banner bad" id="banner">
        <h2 id="bannerTitle">Conectando…</h2>
        <p id="bannerDetail">Leyendo /api/status</p>
      </div>

      <div class="grid">
        <section>
          <h2>Estado</h2>
          <div class="stats">
            <div class="stat"><span>Posición PUU</span><b id="pos">—</b></div>
            <div class="stat"><span>Posición mm</span><b id="posMm">—</b></div>
            <div class="stat"><span>P5.007</span><b id="p5007">—</b></div>
            <div class="stat"><span>Busy</span><b id="busy">—</b></div>
          </div>
          <div class="btns" style="margin-top:1rem">
            <button class="btnG" type="button" onclick="refresh()">Actualizar</button>
            <button class="btnG" type="button" onclick="call('POST','/api/test')">Test Modbus</button>
            <button class="btnG" type="button" onclick="call('POST','/api/on')">Servo ON</button>
            <button class="btnG" type="button" onclick="call('POST','/api/off')">Servo OFF</button>
            <button class="btnD" type="button" onclick="call('POST','/api/stop')">STOP</button>
          </div>
        </section>

        <section>
          <h2>Homing por torque</h2>
          <p class="hint">Tope + = origen 0. Hold fijo 150 ms. Timeout auto según speed y recorrido máx. de búsqueda.</p>
          <div class="row c3">
            <div>
              <label for="dir">Dirección</label>
              <select id="dir">
                <option value="F" selected>Forward (F) · tope +</option>
                <option value="R">Reverse (R)</option>
              </select>
            </div>
            <div>
              <label for="tq">Torque %</label>
              <input id="tq" type="number" min="1" max="300" value="10"/>
            </div>
            <div>
              <label for="hRpm">Speed rpm</label>
              <input id="hRpm" type="number" min="0.1" step="0.1" value="10"/>
            </div>
          </div>
          <div class="btns">
            <button class="btn2" type="button" id="btnHome" onclick="doHome()">Iniciar HOME</button>
          </div>
        </section>

        <section class="full">
          <h2>Movimiento absoluto</h2>
          <p class="hint">
            Trabajo en PUU negativos (0 = home). Mm = solo lectura (fábrica 770/45).
            Timeout = distancia / velocidad (+ 1 s); si no llega → ALARMA.
          </p>
          <div class="row c2">
            <div>
              <label for="target">Posición (PUU)</label>
              <input id="target" type="number" value="-770" oninput="previewPuuMm()"/>
            </div>
            <div>
              <label for="targetMm">Equivale a (mm)</label>
              <input id="targetMm" type="text" value="—" readonly tabindex="-1"/>
            </div>
          </div>
          <div class="row c3">
            <div>
              <label for="mRpm">Velocidad (rpm)</label>
              <input id="mRpm" type="number" min="400" max="3000" step="50" value="1200" oninput="previewRpmMmS()"/>
            </div>
            <div>
              <label for="mMmS">Equivale a (mm/s)</label>
              <input id="mMmS" type="text" value="100.0" readonly tabindex="-1" title="Referencia: rpm/60 × 5 mm/rev"/>
            </div>
            <div class="btns" style="align-items:flex-end">
              <button class="btnG" type="button" id="btnRpmSave" onclick="saveRpm()">Guardar rpm</button>
            </div>
          </div>
          <div class="btns">
            <button class="btn" type="button" id="btnMove" onclick="doMove()">MOVE PUU</button>
            <button class="btnG" type="button" onclick="document.getElementById('target').value='0'; previewPuuMm()">Ir a 0</button>
          </div>
        </section>

        <section class="full">
          <div class="log" id="log">Listo. Abre http://10.10.32.20/</div>
        </section>
      </div>
    </div>

    <!-- ========== VENTANA OM ENCODER ========== -->
    <div class="panel" id="panelEnc">
      <div class="banner wait" id="encBanner">
        <h2 id="encBannerTitle">Encoder L / R</h2>
        <p id="encBannerDetail">RE30AJ2000F · ambos lados · valores al settle</p>
      </div>

      <div class="enc-dual">
        <section class="enc-card" id="encCardL">
          <div class="enc-card-head">
            <span class="enc-side-tag">Lado L</span>
            <span class="enc-hw" id="encHwL">HW …</span>
          </div>
          <div class="dial-wrap">
            <div class="mm-hero" id="encLMmAbs">—</div>
            <div class="unit">longitud final (settle)</div>
            <div class="dial-outer">
              <svg class="dial-sweep" viewBox="0 0 100 100" aria-hidden="true">
                <circle cx="50" cy="50" r="38" fill="none" stroke="#3a4652" stroke-width="12"/>
                <g id="encLSweepSegs"></g>
              </svg>
              <div class="dial-hub">
                <div>
                  <div class="deg" id="encLAngle">—</div>
                  <div class="unit">ángulo al settle</div>
                </div>
              </div>
            </div>
            <div class="sub" id="encLDirLabel">Dirección: —</div>
          </div>
          <div class="stats" style="margin-top:.85rem">
            <div class="stat"><span>mm con signo</span><b id="encLMm">—</b></div>
            <div class="stat"><span>RPM pico</span><b id="encLRpm">—</b></div>
            <div class="stat"><span>Velocidad pico</span><b id="encLMmS">—</b></div>
            <div class="stat"><span>Final (settle)</span><b id="encLSettle">—</b></div>
          </div>
          <div class="row" style="margin-top:.85rem;align-items:end">
            <label style="flex:1">Offset L (mm)
              <input id="encLOffsetMm" type="number" step="0.01" min="-5" max="5" value="0">
            </label>
            <button class="btn2" type="button" onclick="saveEncOffset('L')">Guardar</button>
          </div>
          <div class="btns" style="margin-top:.75rem">
            <button class="btn" type="button" onclick="resetEnc('L')">Set0 L</button>
          </div>
        </section>

        <section class="enc-card" id="encCardR">
          <div class="enc-card-head">
            <span class="enc-side-tag">Lado R</span>
            <span class="enc-hw" id="encHwR">HW …</span>
          </div>
          <div class="dial-wrap">
            <div class="mm-hero" id="encRMmAbs">—</div>
            <div class="unit">longitud final (settle)</div>
            <div class="dial-outer">
              <svg class="dial-sweep" viewBox="0 0 100 100" aria-hidden="true">
                <circle cx="50" cy="50" r="38" fill="none" stroke="#3a4652" stroke-width="12"/>
                <g id="encRSweepSegs"></g>
              </svg>
              <div class="dial-hub">
                <div>
                  <div class="deg" id="encRAngle">—</div>
                  <div class="unit">ángulo al settle</div>
                </div>
              </div>
            </div>
            <div class="sub" id="encRDirLabel">Dirección: —</div>
          </div>
          <div class="stats" style="margin-top:.85rem">
            <div class="stat"><span>mm con signo</span><b id="encRMm">—</b></div>
            <div class="stat"><span>RPM pico</span><b id="encRRpm">—</b></div>
            <div class="stat"><span>Velocidad pico</span><b id="encRMmS">—</b></div>
            <div class="stat"><span>Final (settle)</span><b id="encRSettle">—</b></div>
          </div>
          <div class="row" style="margin-top:.85rem;align-items:end">
            <label style="flex:1">Offset R (mm)
              <input id="encROffsetMm" type="number" step="0.01" min="-5" max="5" value="0">
            </label>
            <button class="btn2" type="button" onclick="saveEncOffset('R')">Guardar</button>
          </div>
          <div class="btns" style="margin-top:.75rem">
            <button class="btn" type="button" onclick="resetEnc('R')">Set0 R</button>
          </div>
        </section>
      </div>
      <p class="enc-foot">
        Polea ø50 mm · 2000 c/rev · 100 mm = 1273 cuentas.
        Cada lado tiene Set0 y offset independientes.
        La lectura visible y Set0 usan la base (redondeo 0/0.5/1);
        el offset es corrección interna de Motion/Feed y <b>no</b> altera el cero operativo.
      </p>
    </div>
  </div>

  <script>
    const $ = id => document.getElementById(id);
    let waiting = false;
    let activeTab = 'ov';
    let cfg = { stepsPerMm: 770 / 45, offsetSteps: 0, egearN: 1, factoryStepsPerMm: 770 / 45, moveRpm: 1200 };
    let encTimer = null;
    let ovTimer = null;
    let lastEncAngDraw = { L: -1, R: -1 };
    // Clave settle+mm+offset: si solo cambia offset/Set0, hay que repintar.
    let lastEncSettleKey = { L: null, R: null };

    function encPolar(cx, cy, r, deg) {
      const rad = deg * Math.PI / 180;
      return [cx + r * Math.cos(rad), cy + r * Math.sin(rad)];
    }

    function updateEncSweep(side, ang) {
      const a = ((ang % 360) + 360) % 360;
      if (Math.abs(a - lastEncAngDraw[side]) < 0.4) return;
      lastEncAngDraw[side] = a;
      const g = $('enc' + side + 'SweepSegs');
      if (!g) return;
      if (a < 0.5) { g.innerHTML = ''; return; }
      const R = 38, SW = 11, STEP = 7, GAP = 2.2;
      const parts = [];
      for (let from = 0; from < a; from += STEP) {
        const to = Math.min(from + STEP - GAP, a);
        if (to <= from + 0.3) continue;
        const t = from / Math.max(a, 1);
        const op = 0.18 + 0.82 * Math.pow(t, 0.65);
        const [x1, y1] = encPolar(50, 50, R, from);
        const [x2, y2] = encPolar(50, 50, R, to);
        const large = (to - from) > 180 ? 1 : 0;
        parts.push(
          '<path d="M' + x1.toFixed(2) + ' ' + y1.toFixed(2) +
          ' A' + R + ' ' + R + ' 0 ' + large + ' 1 ' +
          x2.toFixed(2) + ' ' + y2.toFixed(2) +
          '" fill="none" stroke="rgba(42,157,143,' + op.toFixed(2) +
          ')" stroke-width="' + SW + '" stroke-linecap="butt"/>'
        );
      }
      g.innerHTML = parts.join('');
    }

    function encClearDisplay(side) {
      lastEncAngDraw[side] = -1;
      lastEncSettleKey[side] = null;
      $('enc' + side + 'MmAbs').textContent = '—';
      $('enc' + side + 'Angle').textContent = '—';
      updateEncSweep(side, 0);
      const lab = $('enc' + side + 'DirLabel');
      lab.textContent = 'Dirección: —';
      lab.className = 'sub';
      ['Mm', 'Rpm', 'MmS', 'Settle'].forEach(k => {
        const el = $('enc' + side + k);
        if (el) el.textContent = '—';
      });
    }

    function encSettleKey(d) {
      return String(d.cSettle) + '|' + Number(d.mmSettle).toFixed(3) + '|' +
        Number(d.offsetMm != null ? d.offsetMm : 0).toFixed(3);
    }

    function encApplySettle(side, d, force) {
      const cpr = d.cpr || 2000;
      const cSettle = d.cSettle;
      const key = encSettleKey(d);
      if (!force && key === lastEncSettleKey[side]) return;
      lastEncSettleKey[side] = key;

      $('enc' + side + 'MmAbs').textContent = Number(d.mmSettle).toFixed(2) + ' mm';
      $('enc' + side + 'Settle').textContent = Number(d.mmSettle).toFixed(2) + ' mm';
      const wrapped = ((cSettle % cpr) + cpr) % cpr;
      const ang = 360 * wrapped / cpr;
      $('enc' + side + 'Angle').textContent = ang.toFixed(2) + '°';
      updateEncSweep(side, ang);

      const mmSigned = (d.mmSettleRound != null)
        ? Number(d.mmSettleRound) * (cSettle < 0 ? -1 : 1)
        : Number(d.mm);
      $('enc' + side + 'Mm').textContent = mmSigned.toFixed(2);

      const mmsPeak = d.mmsPeak != null ? Number(d.mmsPeak) : 0;
      const pulley = d.pulleyMm || 50;
      const rpmPeak = mmsPeak * 60 / (Math.PI * pulley);
      $('enc' + side + 'MmS').textContent = mmsPeak.toFixed(1) + ' mm/s';
      $('enc' + side + 'Rpm').textContent = rpmPeak.toFixed(1);

      const lab = $('enc' + side + 'DirLabel');
      lab.className = 'sub';
      if (cSettle > 0) { lab.textContent = 'Dirección: CW (horario)'; lab.classList.add('dir-cw'); }
      else if (cSettle < 0) { lab.textContent = 'Dirección: CCW (antihorario)'; lab.classList.add('dir-ccw'); }
      else { lab.textContent = 'Dirección: parado'; }
    }

    function paintEncSide(side, d, hwOk) {
      const hw = $('encHw' + side);
      if (hw) {
        hw.textContent = hwOk ? 'HW OK' : 'no instalado';
        hw.className = 'enc-hw ' + (hwOk ? 'on' : 'off');
      }
      const offEl = $('enc' + side + 'OffsetMm');
      if (d && d.offsetMm !== undefined && document.activeElement !== offEl) {
        offEl.value = Number(d.offsetMm).toFixed(3);
      }
      if (!d || !d.ok) {
        encClearDisplay(side);
        if ($('enc' + side + 'MmAbs'))
          $('enc' + side + 'MmAbs').textContent = hwOk ? '—' : 'N/A';
        return;
      }
      const moving = !d.settled && Number(d.mms) >= 20;
      if (moving) {
        $('enc' + side + 'MmAbs').textContent = 'midiendo…';
        return;
      }
      if (d.settled) {
        encApplySettle(side, d, !!d._forcePaint);
        return;
      }
      if (Math.abs(Number(d.c)) < 1) encClearDisplay(side);
    }

    function omSettleTxt(hit) {
      if (!hit) return 'esperando settle';
      if (hit.actualMm > 0) return 'settle ' + Number(hit.actualMm).toFixed(2) + ' mm';
      return 'esperando settle';
    }

    function asdaFinalTxt(hit) {
      if (!hit || !hit.done) return 'esperando movimiento';
      return 'final ' + Number(hit.actualMm).toFixed(2) + ' mm';
    }

    function setOmInd(id, hit) {
      const el = $(id);
      if (!el) return;
      el.className = 'ind';
      if (!hit) { el.classList.add('pending'); el.title = 'pendiente'; return; }
      if (hit.done) {
        el.classList.add(hit.ok ? 'ok' : 'bad');
        el.title = hit.ok ? 'OK' : 'fuera de tol';
      } else {
        el.classList.add('pending');
        el.title = hit.actualMm > 0 ? 'settle listo · pendiente marcar' : 'esperando settle';
      }
    }

    function showTab(name) {
      activeTab = name;
      $('panelOv').classList.toggle('active', name === 'ov');
      $('panelAsda').classList.toggle('active', name === 'asda');
      $('panelEnc').classList.toggle('active', name === 'enc');
      $('tabOv').classList.toggle('active', name === 'ov');
      $('tabAsda').classList.toggle('active', name === 'asda');
      $('tabEnc').classList.toggle('active', name === 'enc');
      $('headerSub').textContent = name === 'ov'
        ? 'Vista general · ASDA + OM'
        : (name === 'asda'
          ? 'ASDA-B3 · Home · move PUU · speed'
          : 'OM RE30AJ2000F · L / R al settle');
      stopEncPoll();
      stopOvPoll();
      if (name === 'enc') startEncPoll();
      else if (name === 'ov') startOvPoll();
      else if (!waiting) refresh();
    }

    function setBanner(mode, title, detail) {
      const b = $('banner');
      b.className = 'banner ' + mode;
      $('bannerTitle').textContent = title;
      $('bannerDetail').textContent = detail || '';
    }

    function log(msg) {
      $('log').textContent = msg;
    }

    function busyButtons() {
      return ['btnHome', 'btnRpmSave', 'btnMove'];
    }

    function setBusyUi(on, label) {
      waiting = !!on;
      busyButtons().forEach(id => { const el = $(id); if (el) el.disabled = waiting; });
      if (waiting) {
        setBanner('wait', label || 'Esperando…', 'home/move bloquean hasta terminar');
      }
    }

    async function api(method, url, body) {
      const opt = { method, headers: {} };
      if (body !== undefined) {
        opt.headers['Content-Type'] = 'application/json';
        opt.body = JSON.stringify(body);
      }
      const r = await fetch(url, opt);
      let j = {};
      try {
        j = await r.json();
      } catch (e) {
        j = { ok: false, message: 'JSON invalido' };
      }
      j._http = r.status;
      return j;
    }

    function puuToMm(puu) {
      const spm = cfg.factoryStepsPerMm || cfg.stepsPerMm || (770 / 45);
      if (spm < 0.001) return 0;
      const pasos = Math.abs(Number(puu)) / (cfg.egearN || 1);
      return pasos / spm;
    }

    function previewPuuMm() {
      const puu = Number($('target').value);
      if (!isFinite(puu)) {
        $('targetMm').value = '—';
        return;
      }
      $('targetMm').value = puuToMm(puu).toFixed(2);
    }

    const LINEAR_MM_PER_MOTOR_REV = 5;

    function previewRpmMmS() {
      const rpm = Number($('mRpm').value);
      if (!isFinite(rpm)) {
        $('mMmS').value = '—';
        return;
      }
      $('mMmS').value = ((rpm / 60) * LINEAR_MM_PER_MOTOR_REV).toFixed(1);
    }

    function applyCfg(c) {
      if (!c) return;
      cfg = Object.assign(cfg, c);
      if (c.moveRpm != null && document.activeElement !== $('mRpm'))
        $('mRpm').value = c.moveRpm;
      if (c.moveRpmMin != null) $('mRpm').min = c.moveRpmMin;
      if (c.moveRpmMax != null) $('mRpm').max = c.moveRpmMax;
      previewPuuMm();
      previewRpmMmS();
    }

    function paint(s) {
      if (!s) return;
      if (!waiting) {
        if (s.alarm) {
          var alarmTxt = s.ui || (s.exxx ? (s.exxx + ': Motion, ' + (s.message || '')) : (s.message || 'Timeout de movimiento'));
          setBanner('bad', s.exxx || 'ALARMA', alarmTxt);
        } else {
          setBanner(
            s.ok ? 'ok' : 'bad',
            s.ok ? (s.busy ? 'Ocupado en movimiento' : 'Enlace OK') : 'Sin enlace Modbus',
            s.message || ('P5.007=' + (s.p5007 ?? '—'))
          );
        }
      }
      $('pos').textContent = (s.positionPuu != null) ? String(s.positionPuu) : '—';
      $('posMm').textContent = (s.positionMm != null) ? Number(s.positionMm).toFixed(2) : '—';
      $('p5007').textContent = (s.p5007 != null) ? String(s.p5007) : '—';
      $('busy').textContent = s.busy ? 'SI' : 'NO';
      if (s.ip) $('ipChip').textContent = s.ip;
    }

    async function refresh() {
      if (activeTab !== 'asda') return;
      try {
        const j = await api('GET', '/api/status');
        paint(j);
        if (!waiting) log('status ' + j._http + ': ' + JSON.stringify(j));
      } catch (e) {
        if (!waiting) setBanner('bad', 'Error de red', e.message);
      }
    }

    async function loadCfg() {
      try {
        applyCfg(await api('GET', '/api/config'));
      } catch (e) {}
    }

    async function call(method, url, body) {
      log(method + ' ' + url + ' …');
      try {
        const j = await api(method, url, body);
        log(method + ' ' + url + ' → ' + j._http + '\n' + JSON.stringify(j, null, 2));
        refresh();
      } catch (e) {
        log('Error: ' + e.message);
      }
    }

    async function doHome() {
      const body = {
        direction: $('dir').value,
        torque: Number($('tq').value),
        speedRpm: Number($('hRpm').value)
      };
      setBusyUi(true, 'Homing en curso…');
      log('POST /api/home … timeout auto');
      try {
        const j = await api('POST', '/api/home', body);
        log('HOME → ' + j._http + '\n' + JSON.stringify(j, null, 2));
        if (j.alarm || j._http === 504) {
          setBanner('bad', 'ALARMA', j.message || 'Timeout home');
        }
      } catch (e) {
        log('Error HOME: ' + e.message);
      }
      setBusyUi(false);
      refresh();
    }

    async function saveRpm() {
      try {
        const j = await api('POST', '/api/config', { moveRpm: Number($('mRpm').value) });
        log('RPM → ' + j._http + '\n' + JSON.stringify(j, null, 2));
        if (j.ok) applyCfg(j);
      } catch (e) {
        log('Error RPM: ' + e.message);
      }
    }

    async function doMove() {
      const body = {
        position: Number($('target').value),
        speedRpm: Number($('mRpm').value)
      };
      setBusyUi(true, 'Movimiento en curso…');
      log('POST /api/move PUU … timeout auto');
      try {
        const j = await api('POST', '/api/move', body);
        log('MOVE → ' + j._http + '\n' + JSON.stringify(j, null, 2));
        if (j.alarm || j._http === 504) {
          setBanner('bad', 'ALARMA', j.message || 'Timeout move');
        }
      } catch (e) {
        log('Error MOVE: ' + e.message);
      }
      setBusyUi(false);
      refresh();
    }

    function paintEnc(d) {
      if (!d) return;
      const okL = !!(d.hwL || (d.l && d.l.ok));
      const okR = !!(d.hwR || (d.r && d.r.ok));
      const eb = $('encBanner');
      eb.className = 'banner ' + ((okL || okR) ? 'ok' : 'bad');
      $('encBannerTitle').textContent = (okL || okR) ? 'Encoder L / R' : 'Encoder no iniciado';
      $('encBannerDetail').textContent =
        'L=' + (okL ? 'OK' : 'off') + ' · R=' + (okR ? 'OK' : 'off') +
        ' · indicadores al settle';
      paintEncSide('L', d.l || null, okL);
      paintEncSide('R', d.r || null, okR);
    }

    async function pollEnc() {
      if (activeTab !== 'enc') return;
      try {
        paintEnc(await api('GET', '/api/encoder'));
      } catch (e) {
        $('encBanner').className = 'banner bad';
        $('encBannerTitle').textContent = 'Error de red';
        $('encBannerDetail').textContent = e.message;
      }
    }

    function startEncPoll() {
      stopEncPoll();
      pollEnc();
      encTimer = setInterval(pollEnc, 400);
    }

    function stopEncPoll() {
      if (encTimer) { clearInterval(encTimer); encTimer = null; }
    }

    async function resetEnc(side) {
      try {
        if (side) lastEncSettleKey[side] = null;
        else { lastEncSettleKey.L = null; lastEncSettleKey.R = null; }
        const q = side ? ('?side=' + encodeURIComponent(side)) : '';
        const j = await api('POST', '/api/encoder/reset' + q);
        if (j && j.l) j.l._forcePaint = true;
        if (j && j.r) j.r._forcePaint = true;
        paintEnc(j);
        log(side ? ('Set0 ' + side + ' OK') : 'Set0 L+R OK');
      } catch (e) {
        log('Set0 falló: ' + e.message);
      }
    }

    async function saveEncOffset(side) {
      const offsetMm = Number($('enc' + side + 'OffsetMm').value);
      try {
        lastEncSettleKey[side] = null;
        // Query + body: ESP WebServer a veces no entrega JSON en "plain".
        const q = '?side=' + encodeURIComponent(side) +
          '&offsetMm=' + encodeURIComponent(String(offsetMm));
        const j = await api('POST', '/api/encoder' + q, { side: side, offsetMm: offsetMm });
        if (j && j.l) j.l._forcePaint = true;
        if (j && j.r) j.r._forcePaint = true;
        paintEnc(j);
        const got = (side === 'R' ? (j && j.r) : (j && j.l));
        const saved = got && got.offsetMm != null ? Number(got.offsetMm) : offsetMm;
        log('OM offsetMm ' + side + ' → ' + saved.toFixed(3));
      } catch (e) {
        log('Error offset OM ' + side + ': ' + e.message);
      }
    }

    function setInd(id, hit, metLabel) {
      const el = $(id);
      if (!el) return;
      el.className = 'ind';
      if (!hit) { el.classList.add('pending'); el.title = 'pendiente'; return; }
      if (hit.done) {
        el.classList.add(hit.ok ? 'ok' : 'bad');
        el.title = hit.ok ? 'OK' : 'fuera de tol';
      } else if (hit.met) {
        el.classList.add('ok');
        el.title = metLabel || 'met';
      } else {
        el.classList.add('pending');
        el.title = 'pendiente';
      }
    }

    function paintOv(d) {
      if (!d) return;
      const b = $('ovBanner');
      const feedOk = d.omFeed55 && d.omFeed55.ok;
      if (d.pieceOk) {
        b.className = 'banner ok';
        $('ovBannerTitle').textContent = 'Pieza OK';
        $('ovBannerDetail').textContent = 'Hitos cumplidos · tol ±' + Number(d.tolMm).toFixed(2) + ' mm';
      } else if (feedOk && d.stepOk) {
        b.className = 'banner ok';
        $('ovBannerTitle').textContent = 'Alimentación OK';
        $('ovBannerDetail').textContent = 'OM ≥ ' + Number((d.omFeed55 && d.omFeed55.targetMm) || 55).toFixed(1) + ' mm';
      } else {
        b.className = 'banner wait';
        $('ovBannerTitle').textContent = 'Monitoreo';
        $('ovBannerDetail').textContent = 'L=' + Number(d.pieceMm || 0).toFixed(1)
          + ' · OM esperando settle';
      }
      $('ovL').textContent = d.pieceMm != null ? Number(d.pieceMm).toFixed(1) + ' mm' : '—';
      $('ovLinAct').textContent = d.linearActuatorMm != null ? Number(d.linearActuatorMm).toFixed(1) + ' mm' : '—';
      $('ovExtraShow').textContent = d.extraMm != null ? Number(d.extraMm).toFixed(1) + ' mm' : '—';
      $('ovTol').textContent = d.tolMm != null ? '±' + Number(d.tolMm).toFixed(2) : '—';
      const pok = $('ovPieceOk');
      pok.textContent = d.pieceOk ? 'SI' : (feedOk ? 'parcial' : '—');
      pok.className = d.pieceOk ? 'ok' : '';

      const ap = d.asdaLinearActuator || {};
      const ad = d.asdaDeposit || {};
      const of = d.omFeed55 || {};
      const or = d.omReset || {};
      const ofn = d.omFinal || {};

      $('ovAsdaLinActVal').textContent = ap.targetMm != null ? Number(ap.targetMm).toFixed(1) + ' mm' : '—';
      $('ovAsdaLinActAct').textContent = asdaFinalTxt(ap);
      setInd('ovAsdaLinActInd', ap);

      $('ovAsdaDepVal').textContent = ad.targetMm != null ? Number(ad.targetMm).toFixed(1) + ' mm' : '—';
      $('ovAsdaDepAct').textContent = asdaFinalTxt(ad);
      setInd('ovAsdaDepInd', ad);

      $('ovOmFeedVal').textContent = of.targetMm != null ? Number(of.targetMm).toFixed(1) + ' mm' : '—';
      $('ovOmFeedAct').textContent = omSettleTxt(of);
      setOmInd('ovOmFeedInd', of);

      $('ovOmResetVal').textContent = 'Reset';
      $('ovOmResetAct').textContent = or.done ? (or.ok ? 'hecho' : 'fallo') : 'pendiente';
      setInd('ovOmResetInd', or);

      $('ovOmFinalVal').textContent = ofn.targetMm != null ? Number(ofn.targetMm).toFixed(1) + ' mm' : '—';
      $('ovOmFinalAct').textContent = omSettleTxt(ofn);
      setOmInd('ovOmFinalInd', ofn);
    }

    async function ovRefresh() {
      if (activeTab !== 'ov') return;
      try {
        paintOv(await api('GET', '/api/overview'));
      } catch (e) {
        $('ovBanner').className = 'banner bad';
        $('ovBannerTitle').textContent = 'Error de red';
        $('ovBannerDetail').textContent = e.message;
      }
    }

    function startOvPoll() {
      stopOvPoll();
      ovRefresh();
      ovTimer = setInterval(ovRefresh, 800);
    }

    function stopOvPoll() {
      if (ovTimer) { clearInterval(ovTimer); ovTimer = null; }
    }

    previewPuuMm();
    previewRpmMmS();
    loadCfg();
    startOvPoll();
    setInterval(() => { if (activeTab === 'asda' && !waiting) refresh(); }, 1500);
  </script>
</body>
</html>
)HTML";


static const char feed_index_html[] PROGMEM = R"FEEDHTML(<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>Motion · Alimentación CAN</title>
<style>
:root{--bg:#101418;--panel:#222b33;--line:#3a4652;--text:#eef2f5;--muted:#9aabba;--ok:#3dba7a;--bad:#e05252;--accent:#2a9d8f;--font:"Segoe UI",sans-serif;--mono:Consolas,monospace}
*{box-sizing:border-box}body{margin:0;min-height:100vh;background:linear-gradient(160deg,#101418,#1a2229);color:var(--text);font-family:var(--font);padding:1rem 1.25rem 2rem}
.wrap{max-width:920px;margin:0 auto}h1{margin:0 0 .25rem;font-size:1.6rem}h1 span{color:var(--accent)}
.sub{color:var(--muted);font-family:var(--mono);font-size:.8rem;margin:0 0 1rem}
.nav{margin-bottom:1rem}.nav a{color:var(--accent);text-decoration:none;font-weight:600}
section{background:color-mix(in srgb,var(--panel) 92%,transparent);border:1px solid var(--line);border-radius:12px;padding:1rem;margin-bottom:1rem}
h2{margin:0 0 .75rem;font-size:.75rem;text-transform:uppercase;letter-spacing:.08em;color:var(--muted)}
.feed-side-row{display:flex;flex-wrap:wrap;gap:.5rem;align-items:flex-end;margin-bottom:.75rem}
.feed-side-tag{font-weight:700;color:var(--accent);min-width:1.2rem}
label{font-size:.78rem;color:var(--muted);display:flex;flex-direction:column;gap:4px}
input[type=number]{background:#151b21;border:1px solid var(--line);color:var(--text);border-radius:8px;padding:.45rem .55rem;min-width:5rem}
.btn{border:1px solid var(--line);background:#151b21;color:var(--text);border-radius:8px;padding:.5rem .85rem;font-weight:600;cursor:pointer}
.btn-accent{background:var(--accent);color:#04201c;border-color:var(--accent)}
.btn:disabled{opacity:.45;cursor:not-allowed}
canvas{width:100%;max-width:640px;height:160px;background:#151b21;border:1px solid var(--line);border-radius:8px;display:block;margin:.5rem 0}
.stats{font-family:var(--mono);font-size:.78rem;color:var(--muted)}
.msg{margin-top:.75rem;padding:.65rem .8rem;border-radius:8px;font-size:.85rem;display:none}
.msg.show{display:block}.msg.ok{border:1px solid var(--ok);background:color-mix(in srgb,var(--ok) 14%,#141a20)}
.msg.bad{border:1px solid var(--bad);background:color-mix(in srgb,var(--bad) 14%,#141a20)}
.msg.wait{border:1px solid #d4a017;background:color-mix(in srgb,#d4a017 12%,#141a20)}
.chip{display:inline-block;font-family:var(--mono);font-size:.75rem;padding:.2rem .5rem;border-radius:999px;border:1px solid var(--line);margin-right:.35rem}
.chip.ok{border-color:var(--ok);color:var(--ok)}.chip.bad{border-color:var(--bad);color:var(--bad)}
.grid2{display:grid;gap:1rem}@media(min-width:700px){.grid2{grid-template-columns:1fr 1fr}}
</style>
</head>
<body>
<div class="wrap">
  <div class="nav"><a href="/">← ASDA / OM</a></div>
  <h1>Motion <span>Feed</span></h1>
  <p class="sub">Modelo S · Servos L/R CiA402 · OM local · 10.10.32.20</p>
  <p id="canChip"><span class="chip">CAN …</span></p>

  <section>
    <h2>Test feed · L / R</h2>
    <p style="color:var(--muted);font-size:.82rem;margin:0 0 .75rem">Target fijo 55 mm · Approach % (estrategia) · Vel nominal · Comp. vel % (Approach/corr).</p>
    <div class="feed-side-row">
      <label>Approach %<input type="number" id="approachPct" min="50" max="95" step="1" value="80"></label>
      <span class="stats" id="approachMmHint">→ 44.0 mm</span>
      <label>Comp. vel %<input type="number" id="moveSpeedPct" min="10" max="100" step="1" value="50"></label>
    </div>
    <div class="feed-side-row">
      <span class="feed-side-tag">L</span>
      <label>Target mm<input type="number" id="solidL" min="0" max="200" step="0.5" value="55"></label>
      <label>Vel mm/s<input type="number" id="velL" min="10" step="10" value="650"></label>
      <label>Dec ramp (ms)<input type="number" id="decMsL" min="10" max="1000" step="10" value="100"></label>
      <button type="button" class="btn" id="btnTestL">Test L</button>
    </div>
    <canvas id="canvasL" width="640" height="160"></canvas>
    <p class="stats" id="statsL">—</p>
    <div class="feed-side-row" style="margin-top:1rem">
      <span class="feed-side-tag">R</span>
      <label>Target mm<input type="number" id="solidR" min="0" max="200" step="0.5" value="55"></label>
      <label>Vel mm/s<input type="number" id="velR" min="10" step="10" value="650"></label>
      <label>Dec ramp (ms)<input type="number" id="decMsR" min="10" max="1000" step="10" value="100"></label>
      <button type="button" class="btn" id="btnTestR">Test R</button>
    </div>
    <canvas id="canvasR" width="640" height="160"></canvas>
    <p class="stats" id="statsR">—</p>
    <div style="margin-top:.75rem;display:flex;gap:.5rem;flex-wrap:wrap">
      <button type="button" class="btn btn-accent" id="btnSave">Guardar config</button>
      <button type="button" class="btn" id="btnFeedReset">Reset Feed + OM</button>
      <button type="button" class="btn" id="btnZeroL">Set0 L</button>
      <button type="button" class="btn" id="btnZeroR">Set0 R</button>
    </div>
    <div class="msg" id="msg"></div>
  </section>

  <section class="grid2">
    <div>
      <h2>Calibración L</h2>
      <label>Medido mm<input type="number" id="calMeasL" step="0.1" min="0.1"></label>
      <label>Offset mm<input type="number" id="offL" step="0.5" value="0"></label>
      <button type="button" class="btn" id="btnCalL" style="margin-top:.5rem">Calibrar L</button>
      <button type="button" class="btn" id="btnOffL" style="margin-top:.5rem">Guardar offset L</button>
      <p class="stats" id="scaleL">—</p>
    </div>
    <div>
      <h2>Calibración R</h2>
      <label>Medido mm<input type="number" id="calMeasR" step="0.1" min="0.1"></label>
      <label>Offset mm<input type="number" id="offR" step="0.5" value="0"></label>
      <button type="button" class="btn" id="btnCalR" style="margin-top:.5rem">Calibrar R</button>
      <button type="button" class="btn" id="btnOffR" style="margin-top:.5rem">Guardar offset R</button>
      <p class="stats" id="scaleR">—</p>
    </div>
  </section>
</div>
<script>
function $(id){return document.getElementById(id)}
function showMsg(t,cls){var m=$('msg');m.textContent=t;m.className='msg show '+(cls||'');}
function cfgQuery(){
  return 'solidMm='+encodeURIComponent($('solidL').value)
    +'&solidMmR='+encodeURIComponent($('solidR').value)
    +'&velocityMmSL='+encodeURIComponent($('velL').value)
    +'&velocityMmSR='+encodeURIComponent($('velR').value)
    +'&decRampMsL='+encodeURIComponent($('decMsL').value)
    +'&decRampMsR='+encodeURIComponent($('decMsR').value)
    +'&approachPct='+encodeURIComponent($('approachPct').value)
    +'&moveSpeedPct='+encodeURIComponent($('moveSpeedPct').value);
}
function applyFeedCfg(d){
  if(!d)return;
  if(d.solidMm!=null)$('solidL').value=d.solidMm;
  if(d.solidMmR!=null)$('solidR').value=d.solidMmR;
  if(d.velocityMmSL!=null)$('velL').value=d.velocityMmSL;
  if(d.velocityMmSR!=null)$('velR').value=d.velocityMmSR;
  if(d.decRampMsL!=null)$('decMsL').value=d.decRampMsL;
  if(d.decRampMsR!=null)$('decMsR').value=d.decRampMsR;
  if(d.approachPct!=null)$('approachPct').value=d.approachPct;
  if(d.moveSpeedPct!=null)$('moveSpeedPct').value=d.moveSpeedPct;
  var apMm=d.approachMm!=null?d.approachMm:(55*Number($('approachPct').value)/100);
  $('approachMmHint').textContent='→ '+Number(apMm).toFixed(1)+' mm (target 55)';
  drawPlan('canvasL','statsL',d.planL,parseFloat(d.solidMm),parseFloat(d.velocityMmSL));
  drawPlan('canvasR','statsR',d.planR,parseFloat(d.solidMmR),parseFloat(d.velocityMmSR));
}
function drawPlan(canvasId,statsId,plan,target,vMm){
  var c=$(canvasId),ctx=c.getContext('2d'),w=c.width,h=c.height;
  ctx.fillStyle='#151b21';ctx.fillRect(0,0,w,h);
  if(!plan||!plan.valid){$(statsId).textContent='Perfil inválido';return;}
  var acc=plan.accMmS2||1,dec=plan.decMmS2||1,v=vMm,dAcc=plan.dAccMm||0,dDec=plan.dDecMm||0,dCr=plan.dCruiseMm||0;
  var tAcc=Math.sqrt(2*dAcc/acc),tDec=Math.sqrt(2*dDec/dec),tCr=dCr/v,tTot=tAcc+tCr+tDec;
  if(tTot<0.01)tTot=0.01;
  ctx.strokeStyle='#2a9d8f';ctx.lineWidth=2;ctx.beginPath();
  for(var i=0;i<=w;i++){
    var t=tTot*i/w,vAt=0;
    if(t<=tAcc)vAt=acc*t; else if(t<=tAcc+tCr)vAt=v; else vAt=Math.max(0,v-dec*(t-tAcc-tCr));
    var y=h-8-(vAt/v)*(h-16);
    if(i===0)ctx.moveTo(i,y);else ctx.lineTo(i,y);
  }
  ctx.stroke();
  $(statsId).textContent='V='+vMm.toFixed(0)+' mm/s · acc='+acc.toFixed(0)+' · dec='+dec.toFixed(0)
    +' · dAcc='+dAcc.toFixed(1)+' dDec='+dDec.toFixed(1)+' cruise='+dCr.toFixed(1)+' mm';
}
// fromDisk=true: leer NVS/runtime (sin query). false: preview con valores del formulario.
function refreshProfile(fromDisk){
  var url=fromDisk?'/getFeedTestConfig':('/getFeedTestConfig?'+cfgQuery());
  fetch(url).then(r=>r.json()).then(applyFeedCfg)
    .catch(function(){showMsg('Error cargando perfil','bad');});
}
function updateApproachHint(){
  var pct=Number($('approachPct').value);
  if(!isFinite(pct))return;
  $('approachMmHint').textContent='→ '+(55*pct/100).toFixed(1)+' mm (target 55)';
}
function pollCan(){
  fetch('/api/feed/status').then(r=>r.json()).then(function(s){
    var el=$('canChip');
    var txt='CAN FAIL', cls='bad';
    if(s.canInitialized && s.servoCanReady){ txt='CAN OK'; cls='ok'; }
    else if(s.canInitialized){ txt='TWAI OK · servos OFF'; cls='bad'; }
    else { txt='CAN FAIL'; cls='bad'; }
    if(s.canBitrateKbps) txt+=' · '+s.canBitrateKbps+'k';
    el.innerHTML='<span class="chip '+cls+'">'+txt+'</span>'
      +(s.feedActive?'<span class="chip">Feed…</span>':'');
    if(s.feedActive)return setTimeout(pollCan,400);
  }).catch(function(){});
}
function waitFeedDone(){
  return new Promise(function(resolve){
    function tick(){
      fetch('/api/feed/status').then(r=>r.json()).then(function(s){
        if(!s.feedActive){resolve(s);return;}
        setTimeout(tick,350);
      }).catch(function(){setTimeout(tick,500);});
    }
    tick();
  });
}
function runTest(side){
  showMsg('Encolando test '+side+'…','wait');
  var q='/feedTestSensor?side='+side
    +'&solidMm='+encodeURIComponent($('solidL').value)
    +'&solidMmR='+encodeURIComponent($('solidR').value)
    +'&velocityMmS='+encodeURIComponent($('velL').value)
    +'&velocityMmSR='+encodeURIComponent($('velR').value)
    +'&decRampMsL='+encodeURIComponent($('decMsL').value)
    +'&decRampMsR='+encodeURIComponent($('decMsR').value)
    +'&approachPct='+encodeURIComponent($('approachPct').value)
    +'&moveSpeedPct='+encodeURIComponent($('moveSpeedPct').value);
  fetch(q).then(r=>r.json()).then(function(d){
    if(!d.ok){showMsg(d.error||'Fallo','bad');return;}
    showMsg('Moviendo servos '+side+'…','wait');
    return waitFeedDone();
  }).then(function(st){
    if(!st)return;
    if(st.feedOk){showMsg('Feed OK · OM '+st.omOfficial+' mm','ok');}
    else showMsg(st.ui||st.fault||'Feed falló','bad');
    pollCan();
  }).catch(function(){showMsg('Error red','bad');});
}
$('btnSave').onclick=function(){
  fetch('/setFeedTestConfig?'+cfgQuery()).then(r=>r.json()).then(function(d){
    if(d&&d.error){showMsg(d.error,'bad');return;}
    showMsg('Guardado','ok');
    applyFeedCfg(d);
  }).catch(function(){showMsg('Error guardando','bad');});
};
$('btnFeedReset').onclick=function(){
  showMsg('Reset Feed + OM…','wait');
  fetch('/api/feed/reset').then(r=>r.json()).then(function(s){
    if(s.feedReset){showMsg('Reset OK · OM en 0','ok');pollCan();}
    else showMsg(s.error||'Reset falló','bad');
  }).catch(function(){showMsg('Error red','bad');});
};
$('btnZeroL').onclick=function(){
  fetch('/api/feed/encoder/zero?side=L').then(r=>r.json()).then(function(d){
    showMsg(d.ok?'Set0 L OK':'Set0 L falló',d.ok?'ok':'bad');
  });
};
$('btnZeroR').onclick=function(){
  fetch('/api/feed/encoder/zero?side=R').then(r=>r.json()).then(function(d){
    showMsg(d.ok?'Set0 R OK':'Set0 R falló',d.ok?'ok':'bad');
  });
};
$('btnTestL').onclick=function(){runTest('L');};
$('btnTestR').onclick=function(){runTest('R');};
$('btnCalL').onclick=function(){
  fetch('/setFeedCal?side=L&measuredMm='+encodeURIComponent($('calMeasL').value)).then(r=>r.json()).then(loadCal);
};
$('btnCalR').onclick=function(){
  fetch('/setFeedCal?side=R&measuredMm='+encodeURIComponent($('calMeasR').value)).then(r=>r.json()).then(loadCal);
};
$('btnOffL').onclick=function(){
  fetch('/setFeedOffset?offsetMm='+encodeURIComponent($('offL').value)).then(function(){showMsg('Offset L OK','ok');});
};
$('btnOffR').onclick=function(){
  fetch('/setFeedOffset?offsetMmB='+encodeURIComponent($('offR').value)).then(function(){showMsg('Offset R OK','ok');});
};
function loadCal(){
  fetch('/getFeedCal').then(r=>r.json()).then(function(d){
    $('scaleL').textContent='spm L: '+d.countsPerMmL.toFixed(2);
    $('scaleR').textContent='spm R: '+d.countsPerMmR.toFixed(2);
  });
}
['solidL','solidR','velL','velR','decMsL','decMsR','approachPct','moveSpeedPct'].forEach(function(id){
  $(id).addEventListener('change',function(){refreshProfile(false);});
});
$('approachPct').addEventListener('input',updateApproachHint);
refreshProfile(true);loadCal();pollCan();
</script>
</body>
</html>)FEEDHTML";

#endif
