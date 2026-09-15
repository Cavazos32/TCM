#ifndef INDEX_H
#define INDEX_H

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML>
<html lang="es">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta http-equiv="Cache-Control" content="no-store">
  <title>Prefeeder v2</title>
  <style>
    :root {
      --bg-primary: #0d1117;
      --bg-secondary: #161b22;
      --bg-tertiary: #21262d;
      --input-bg: #0d1117;
      --accent: #ffffff;
      --accent-hover: #8091db;
      --accent-blue: #58a6ff;
      --success: #3fb950;
      --warning: #e2a41d;
      --error: #f85149;
      --text-primary: #f0f6fc;
      --text-secondary: #8b949e;
      --border: #30363d;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
      background: var(--bg-primary);
      color: var(--text-primary);
      min-height: 100vh;
      display: flex;
      justify-content: center;
      align-items: flex-start;
      padding: 20px;
    }
    .container {
      width: 100%;
      max-width: 480px;
      background: var(--bg-secondary);
      border: 1px solid var(--border);
      border-radius: 12px;
      padding: 24px 22px;
      box-shadow: 0 16px 48px rgba(0,0,0,0.4);
    }
    .header {
      position: relative;
      text-align: center;
      margin-bottom: 20px;
      padding-bottom: 18px;
      border-bottom: 1px solid var(--border);
    }
    .icon-btn {
      position: absolute;
      top: 0;
      right: 0;
      width: 40px;
      height: 40px;
      border: 1px solid var(--border);
      border-radius: 8px;
      background: var(--bg-tertiary);
      cursor: pointer;
      display: flex;
      align-items: center;
      justify-content: center;
    }
    .icon-btn svg { width: 22px; height: 22px; fill: var(--text-primary); }
    .icon-btn:hover { border-color: rgba(88, 166, 255, 0.45); }
    .info-overlay {
      display: none;
      position: fixed;
      inset: 0;
      z-index: 1000;
      background: rgba(0, 0, 0, 0.65);
      padding: 16px;
      align-items: center;
      justify-content: center;
    }
    .info-overlay.open { display: flex; }
    .info-panel {
      width: 100%;
      max-width: 400px;
      max-height: min(88vh, 640px);
      overflow-y: auto;
      background: var(--bg-secondary);
      border: 1px solid var(--border);
      border-radius: 12px;
      padding: 18px 16px 16px;
      box-shadow: 0 16px 48px rgba(0, 0, 0, 0.5);
    }
    .info-panel-header {
      display: flex;
      align-items: flex-start;
      justify-content: space-between;
      gap: 12px;
      margin-bottom: 12px;
    }
    .info-panel-title { font-size: 0.9375rem; font-weight: 600; line-height: 1.3; }
    .info-panel-close {
      width: 32px;
      height: 32px;
      flex-shrink: 0;
      border: 1px solid var(--border);
      border-radius: 8px;
      background: var(--bg-tertiary);
      color: var(--text-primary);
      cursor: pointer;
      font-size: 1.125rem;
      line-height: 1;
    }
    .info-panel-close:hover { border-color: rgba(88, 166, 255, 0.45); }
    .info-section-title {
      font-size: 0.75rem;
      font-weight: 600;
      color: var(--text-secondary);
      margin: 14px 0 8px;
      text-transform: uppercase;
      letter-spacing: 0.04em;
    }
    .info-section-title:first-of-type { margin-top: 0; }
    .info-flow {
      list-style: none;
      counter-reset: flow;
      margin: 0;
      padding: 0;
    }
    .info-flow li {
      counter-increment: flow;
      position: relative;
      padding: 8px 0 8px 28px;
      font-size: 0.8125rem;
      line-height: 1.45;
      border-bottom: 1px solid rgba(48, 54, 61, 0.6);
    }
    .info-flow li.info-step-wait {
      padding-left: 36px;
      font-size: 0.75rem;
      color: var(--text-secondary);
      background: rgba(88, 166, 255, 0.05);
      border-left: 2px solid rgba(88, 166, 255, 0.28);
      margin-left: 6px;
      border-radius: 0 6px 6px 0;
    }
    .info-flow li.info-step-wait::before {
      content: "⏱";
      background: transparent;
      border: none;
      width: auto;
      height: auto;
      top: 8px;
      left: 10px;
      font-size: 0.75rem;
    }
    .info-flow li.info-step-note {
      font-size: 0.75rem;
      color: var(--text-secondary);
      font-style: italic;
      border-bottom: none;
      padding-top: 4px;
      padding-bottom: 4px;
    }
    .info-flow li.info-step-note::before {
      content: "·";
      background: transparent;
      border: none;
      color: var(--text-secondary);
      font-size: 1rem;
      top: 4px;
    }
    .info-param { font-weight: 600; color: #58a6ff; }
    .info-flow li::before {
      content: counter(flow);
      position: absolute;
      left: 0;
      top: 8px;
      width: 20px;
      height: 20px;
      border-radius: 50%;
      background: rgba(88, 166, 255, 0.15);
      border: 1px solid rgba(88, 166, 255, 0.3);
      color: #58a6ff;
      font-size: 0.6875rem;
      font-weight: 700;
      display: flex;
      align-items: center;
      justify-content: center;
    }
    .info-note {
      margin-top: 0;
      margin-bottom: 12px;
      font-size: 0.6875rem;
      color: var(--text-secondary);
      line-height: 1.45;
    }
    h1 { font-size: 1.35rem; font-weight: 600; margin-bottom: 4px; }
    .subtitle {
      color: var(--text-secondary);
      font-size: 0.8125rem;
      line-height: 1.45;
    }
    .status-bar {
      display: flex;
      align-items: center;
      gap: 8px;
      background: var(--bg-primary);
      padding: 10px 14px;
      border-radius: 8px;
      margin-bottom: 10px;
      font-size: 0.875rem;
      border: 1px solid var(--border);
    }
    #control-estado-card .status-bar { margin-bottom: 0; }
    #control-estado-card .meta.compact { margin-top: 10px; margin-bottom: 0; }
    #control-estado-card .btn-row { margin-top: 12px; margin-bottom: 0; }
    .status-dot {
      width: 10px;
      height: 10px;
      border-radius: 50%;
      background: var(--success);
      flex-shrink: 0;
      animation: pulse 2s infinite;
    }
    .status-dot.error { background: var(--error); animation: none; }
    .comm-row {
      display: flex;
      align-items: center;
      gap: 8px;
      margin-bottom: 10px;
      font-size: 0.8125rem;
    }
    .comm-direction {
      display: inline-flex;
      align-items: center;
      padding: 3px 8px;
      border-radius: 999px;
      border: 1px solid var(--border);
      background: rgba(88, 166, 255, 0.1);
      color: var(--accent-blue);
      font-size: 0.6875rem;
      font-weight: 600;
      flex-shrink: 0;
    }
    .comm-age {
      margin-left: auto;
      color: var(--text-secondary);
      font-size: 0.6875rem;
      white-space: nowrap;
    }
    .comm-message {
      min-height: 48px;
      padding: 10px;
      border-radius: 8px;
      border: 1px solid var(--border);
      background: var(--bg-primary);
      color: var(--text-secondary);
      font-family: Consolas, "Courier New", monospace;
      font-size: 0.6875rem;
      line-height: 1.45;
      overflow-wrap: anywhere;
    }
    @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.5; } }
    .card {
      background: var(--bg-tertiary);
      border: 1px solid var(--border);
      border-radius: 10px;
      padding: 14px;
      margin-bottom: 12px;
    }
    .card.alarm {
      border-color: rgba(248, 81, 73, 0.55);
      box-shadow: 0 0 0 1px rgba(248, 81, 73, 0.2);
    }
    .card h2 {
      font-size: 0.8125rem;
      font-weight: 600;
      margin-bottom: 12px;
      color: var(--text-primary);
    }
    .panel-desc {
      font-size: 0.6875rem;
      color: var(--text-secondary);
      margin: -4px 0 12px 0;
      line-height: 1.45;
    }
    .sensor-row {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 12px;
      font-size: 0.875rem;
    }
    .sensor-list {
      display: flex;
      flex-direction: column;
    }
    .sensor-list .sensor-row {
      padding: 9px 0;
      border-bottom: 1px solid rgba(48, 54, 61, 0.65);
      font-size: 0.8125rem;
    }
    .sensor-list .sensor-row:first-child { padding-top: 0; }
    .sensor-list .sensor-row:last-child { border-bottom: none; padding-bottom: 0; }
    .sensor-list .sensor-row.alarm {
      margin: 0 -8px;
      padding-left: 8px;
      padding-right: 8px;
      border-radius: 8px;
      background: rgba(248, 81, 73, 0.08);
      border-bottom-color: transparent;
    }
    .trigger-section {
      padding-top: 14px;
      margin-top: 14px;
      border-top: 1px solid rgba(48, 54, 61, 0.65);
    }
    .trigger-section:first-of-type {
      padding-top: 0;
      margin-top: 0;
      border-top: none;
    }
    .trigger-section .sensor-row { margin-bottom: 8px; }
    .subsection-title {
      font-size: 0.75rem;
      font-weight: 600;
      margin-bottom: 10px;
      color: var(--text-primary);
    }
    .sensor-title {
      flex: 1;
      min-width: 0;
      line-height: 1.3;
    }
    .sensor-list .led {
      width: 28px;
      height: 28px;
    }
    .sensor-config {
      margin-top: 14px;
      padding-top: 14px;
      border-top: 1px solid var(--border);
    }
    .config-subtitle {
      font-size: 0.75rem;
      font-weight: 600;
      color: var(--text-secondary);
      margin-bottom: 10px;
      text-transform: uppercase;
      letter-spacing: 0.03em;
    }
    .led {
      width: 40px;
      height: 40px;
      border-radius: 50%;
      flex-shrink: 0;
      border: 2px solid var(--border);
      background: var(--bg-primary);
      transition: background 0.15s, box-shadow 0.15s, border-color 0.15s;
    }
    .led.on {
      background: var(--success);
      border-color: rgba(63, 185, 80, 0.55);
      box-shadow: 0 0 16px rgba(63, 185, 80, 0.45);
    }
    .led.tension.on,
    .led.warning.on {
      background: var(--warning);
      border-color: rgba(226, 164, 29, 0.55);
      box-shadow: 0 0 16px rgba(226, 164, 29, 0.45);
    }
    .led.error.on {
      background: var(--error);
      border-color: rgba(248, 81, 73, 0.55);
      box-shadow: 0 0 16px rgba(248, 81, 73, 0.45);
    }
    .badge {
      display: inline-flex;
      align-items: center;
      padding: 4px 10px;
      border-radius: 999px;
      font-weight: 600;
      font-size: 0.6875rem;
      letter-spacing: 0.02em;
      text-transform: uppercase;
      border: 1px solid var(--border);
      background: rgba(139, 148, 158, 0.12);
      color: var(--text-secondary);
      flex-shrink: 0;
    }
    .badge.on {
      background: rgba(63, 185, 80, 0.14);
      color: var(--success);
      border-color: rgba(63, 185, 80, 0.32);
    }
    .badge.tension.on,
    .badge.warning.on {
      background: rgba(226, 164, 29, 0.14);
      color: var(--warning);
      border-color: rgba(226, 164, 29, 0.32);
    }
    .form-group { margin-bottom: 12px; }
    .form-row {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 10px;
      margin-bottom: 12px;
    }
    .form-row .form-group { margin-bottom: 0; }
    .form-row label { font-size: 0.6875rem; }
    .form-row input[type="number"] {
      padding: 8px 10px;
      font-size: 0.9375rem;
    }
    label {
      display: block;
      font-size: 0.75rem;
      font-weight: 600;
      color: var(--text-secondary);
      margin-bottom: 6px;
    }
    .field-desc {
      font-size: 0.6875rem;
      color: var(--text-secondary);
      margin: -2px 0 6px 0;
      line-height: 1.4;
    }
    input[type="number"], select {
      width: 100%;
      padding: 10px 12px;
      font-size: 1rem;
      background: var(--input-bg);
      border: 1px solid var(--border);
      border-radius: 8px;
      color: var(--text-primary);
      color-scheme: dark;
      font-family: inherit;
    }
    input[type="number"]:focus, select:focus {
      outline: none;
      border-color: var(--accent-blue);
      box-shadow: 0 0 0 3px rgba(88, 166, 255, 0.18);
    }
    .btn-row {
      display: flex;
      gap: 10px;
      margin-top: 4px;
    }
    .btn-row.cols-2 { display: grid; grid-template-columns: 1fr 1fr; }
    .btn-row.cols-3 { display: grid; grid-template-columns: 1fr 1fr 1fr; }
    .btn-row.cols-4 { display: grid; grid-template-columns: 1fr 1fr 1fr 1fr; }
    .btn-row.cols-1 .btn { width: 100%; }
    button, .btn {
      flex: 1;
      padding: 12px 10px;
      font-size: 0.8125rem;
      font-weight: 600;
      border: none;
      border-radius: 8px;
      cursor: pointer;
      font-family: inherit;
      transition: background 0.15s, border-color 0.15s, transform 0.1s;
    }
    button:active { transform: scale(0.98); }
    .btn-primary { background: var(--accent); color: var(--bg-primary); }
    .btn-primary:hover { background: var(--accent-hover); }
    .btn-success { background: var(--success); color: var(--bg-primary); }
    .btn-success:hover { background: #46c35a; }
    .btn-danger { background: var(--error); color: #fff; }
    .btn-danger:hover { background: #da3633; }
    .btn-warning { background: var(--warning); color: var(--bg-primary); }
    .btn-warning:hover { background: #c9951a; }
    .btn-blue { background: rgba(88, 166, 255, 0.18); color: var(--accent-blue); border: 1px solid rgba(88, 166, 255, 0.35); }
    .btn-blue:hover { border-color: rgba(88, 166, 255, 0.55); }
    .btn-purple { background: rgba(188, 140, 255, 0.18); color: #bc8cff; border: 1px solid rgba(188, 140, 255, 0.35); }
    .btn-purple:hover { border-color: rgba(188, 140, 255, 0.55); }
    .btn-toggle {
      background: var(--bg-tertiary);
      color: var(--text-secondary);
      border: 1px solid var(--border);
    }
    .btn-toggle:hover { border-color: rgba(88, 166, 255, 0.45); color: var(--text-primary); }
    .btn-toggle.on {
      background: rgba(63, 185, 80, 0.2);
      color: var(--success);
      border-color: rgba(63, 185, 80, 0.5);
    }
    .btn-toggle.on:hover { border-color: rgba(63, 185, 80, 0.7); }
    .btn-toggle.test-mode.on {
      background: rgba(226, 164, 29, 0.22);
      color: var(--warning);
      border-color: rgba(226, 164, 29, 0.55);
    }
    .btn-toggle.test-mode.on:hover { border-color: rgba(226, 164, 29, 0.75); }
    .btn-toggle.mute.on {
      background: rgba(139, 148, 158, 0.22);
      color: var(--text-primary);
      border-color: rgba(139, 148, 158, 0.55);
    }
    .btn-toggle:disabled {
      opacity: 0.45;
      cursor: not-allowed;
    }
    .btn-toggle:disabled:hover {
      border-color: var(--border);
      color: var(--text-secondary);
    }
    .refill-actions { display: grid; gap: 10px; }
    .refill-actions .btn-row { margin-top: 0; }
    .meta {
      margin-top: 12px;
      padding-top: 12px;
      border-top: 1px solid var(--border);
      font-size: 0.75rem;
      color: var(--text-secondary);
      line-height: 1.6;
    }
    .meta strong { color: var(--text-primary); font-weight: 600; }
    .meta.compact { margin-top: 0; padding-top: 0; border-top: none; margin-bottom: 12px; }
    .motor-state {
      margin-top: 12px;
      font-size: 0.75rem;
      color: var(--text-secondary);
      line-height: 1.6;
    }
    .motor-state strong { color: var(--text-primary); font-weight: 600; }
    .hint { font-size: 0.6875rem; color: var(--text-secondary); }
    .save-all-wrap { margin-top: 4px; margin-bottom: 8px; }
    .save-all-wrap .btn {
      width: 100%;
      flex: none;
      display: block;
      padding: 14px 16px;
      font-size: 0.9375rem;
    }
    .footer {
      margin-top: 8px;
      text-align: center;
      font-size: 0.6875rem;
      color: var(--text-secondary);
    }
  </style>
</head>
<body>
  <div class="container">
    <div class="header">
      <button type="button" class="icon-btn" id="btnRoutineInfo" onclick="toggleRoutineInfo(true)" aria-label="Rutina del sistema" title="Rutina del sistema">
        <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm1 15h-2v-6h2v6zm0-8h-2V7h2v2z"/></svg>
      </button>
      <h1 id="pf-page-title">Prefeeder v2</h1>
    </div>

    <div id="routineInfoOverlay" class="info-overlay" onclick="if(event.target===this) toggleRoutineInfo(false)">
      <div class="info-panel" role="dialog" aria-labelledby="routineInfoTitle">
        <div class="info-panel-header">
          <h2 class="info-panel-title" id="routineInfoTitle">Rutina del sistema</h2>
          <button type="button" class="info-panel-close" onclick="toggleRoutineInfo(false)" aria-label="Cerrar">×</button>
        </div>

        <p class="info-note">Dos núcleos en paralelo: <strong>Núcleo 1</strong> (loop) = DeReeler, servo, fallas y web. <strong>Núcleo 0</strong> (tarea FreeRTOS) = Feeder por trigger Tfeed. Los tiempos en azul salen de la configuración actual (los fijos no se editan).</p>

        <p class="info-section-title">Núcleo 1 · DeReeler + Servo (con Iniciar)</p>
        <ol class="info-flow">
          <li class="info-step-note"><strong>Secuencia:</strong> trigger Tfeed → feeder consume · DeReeler/servo siguen hasta Full ON</li>
          <li>Buffer Full GPIO 19 estable (ON ~80 ms) → DeReeler parado · servo parado</li>
          <li>Feeder en Tfeed no pausa DeReeler/servo (relleno en paralelo)</li>
          <li>Buffer Full OFF sostenido (~200 ms) → servo primero · DeReeler CW tras <span class="info-param">100</span> ms · <span class="info-param" data-info-key="autoRpm">60 RPM</span></li>
          <li>Servo GPIO 26 gira en CW e inversión → PWM <span class="info-param" data-info-key="servoPwm">800 µs</span> (ajustable · neutro 1500)</li>
          <li>Tensión GPIO 23 → +30 RPM (solo velocidad · sentido = bobinas)</li>
          <li class="info-step-wait"><span class="info-param" data-info-key="autoRev">2.0 s</span> · duración boost tensión</li>
          <li>Vuelve a CW si Buffer Full sigue inactivo</li>
          <li class="info-step-wait"><span class="info-param" data-info-key="tensionCooldown">0 s</span> · espera entre rutinas de tensión</li>
          <li class="info-step-note"><strong>Fallas enclavadas</strong> hasta Reset + Iniciar (se reportan al TCM por TCP)</li>
          <li>Buffer Max GPIO 21 → para todo</li>
          <li>Cilindro abierto GPIO 25 → para todo</li>
          <li>Tensión GPIO 23 &gt; <span class="info-param">10 s</span> → para todo</li>
          <li>Buffer Full GPIO 19 consumido sin relleno en <span class="info-param">10.0 s</span> → para todo</li>
        </ol>

        <p class="info-section-title">Núcleo 0 · Feeder (automático, con Iniciar)</p>
        <ol class="info-flow">
          <li class="info-step-note">Tarea <em>m2_holgura</em> · ciclo ~5 ms · feeder: trigger TCP / helper holgura / refill</li>
          <li class="info-step-note">Misma condición: ventana de relleno + Auto ON</li>
          <li>Holgura GPIO 22: ausente ≥ <span class="info-param" data-info-key="holguraHelperMs">100 ms</span> → helper feed · ≥ <span class="info-param" data-info-key="holguraFault">1.5 s</span> → falla</li>
          <li>Helper holgura (prioridad &gt; trigger TCP) · <span class="info-param" data-info-key="holguraHelperRpm">60 RPM</span> · <span class="info-param" data-info-key="holguraHelperS">1.0 s</span></li>
          <li>Trigger TCP desde TCM → alimenta Tfeed · <span class="info-param" data-info-key="rpm2">60 RPM</span> · en paralelo con relleno</li>
          <li class="info-step-wait"><span class="info-param" data-info-key="triggerFeed">3.66 s</span> · Tfeed = L·(1+f) / V</li>
          <li class="info-step-note">L=<span class="info-param" data-info-key="pieceLength">—</span> mm · V=<span class="info-param" data-info-key="feedSpeed">100</span> mm/s · f=<span class="info-param">0.1</span></li>
          <li>Fin Tfeed → feeder idle · DeReeler/servo siguen si Full OFF</li>
          <li class="info-step-wait">80 ms · filtro holgura estable</li>
          <li class="info-step-note">Sin enlace TCP con TCM → feeder en pausa</li>
        </ol>

        <p class="info-section-title">Refill material</p>
        <ol class="info-flow">
          <li><strong>Refill material</strong> → DeReeler + servo + Feeder (un pulso; duración desde la UI)</li>
          <li><strong>DeReeler / Servo / Feeder</strong> → clic = ON y auto-OFF; otro clic apaga ya</li>
          <li class="info-step-note">Refill solo en Materialista</li>
          <li class="info-step-note">GPIO 27 ausente → Error enclavado (sin Materialista auto); operador decide</li>
        </ol>

        <p class="info-section-title">Control global</p>
        <ol class="info-flow">
          <li class="info-step-note">Idle quieto: sensores <strong>bloqueados</strong> hasta Iniciar o In process del TCM/Master</li>
          <li><strong>Iniciar</strong> → rellena una vez (buffer+holgura) y congela sensores</li>
          <li><strong>In process ON</strong> → sensores armados · relleno continuo · helper holgura</li>
          <li><strong>Detener</strong> → enclava todo hasta Reset (luego Iniciar)</li>
          <li><strong>Reset</strong> → libera falla / parada operador; luego Iniciar</li>
          <li><strong>Materialista ON</strong> → torre naranja; bloquea buffer/holgura; GPIO 27 off; solo refill manual; TCM no produce</li>
          <li class="info-step-note">Idle: al quitar In process congela (anti-tamper)</li>
        </ol>
      </div>
    </div>

    <div class="card" id="control-estado-card">
      <h2>Control &amp; Estado</h2>
      <div class="status-bar">
        <span class="status-dot" id="status-dot"></span>
        <span id="status-text">Conectando…</span>
      </div>
      <p class="meta compact" id="error-reason-line" hidden>Motivo: <strong id="error-reason">—</strong></p>
      <p class="meta compact" id="mode-line">Modo: <strong id="mode-label">Idle</strong> · Sensores: <strong id="sensors-armed-label">bloqueados</strong></p>
      <div class="btn-row cols-3">
        <button class="btn-success" onclick="autoCmd('start')">Iniciar</button>
        <button class="btn-danger" onclick="autoCmd('stop')">Detener</button>
        <button class="btn-warning" onclick="autoCmd('reset')">Reset</button>
      </div>
      <div class="btn-row cols-1" style="margin-top:10px">
        <button type="button" class="btn-toggle test-mode" id="idle-mode-btn" onclick="toggleIdleMode()">Idle</button>
      </div>
      <div class="btn-row cols-1" style="margin-top:10px">
        <button type="button" class="btn-toggle mute" id="buzzer-mute-btn" onclick="toggleBuzzerMute()" title="Silencia el buzzer de la torre (las luces siguen)">Buzzer · ON</button>
      </div>
    </div>

    <div class="card" id="refill-card">
      <h2>Refill material</h2>
      <p class="meta compact">
        Manual: DeReeler, servo y Feeder. Solo en Materialista. Clic = pulso y auto-apagado. Otro clic apaga de inmediato.
      </p>
      <div class="form-group" style="margin-bottom:12px">
        <label for="refill-pulse-s">Duración del pulso (s)</label>
        <input type="number" id="refill-pulse-s" min="0.2" max="10" step="0.1" value="1.0">
      </div>
      <div class="refill-actions">
        <button type="button" class="btn-toggle" id="refill-material-btn">Refill material · OFF</button>
        <div class="btn-row cols-3">
          <button type="button" class="btn-toggle" id="refill-dereeler-btn">DeReeler · OFF</button>
          <button type="button" class="btn-toggle" id="refill-servo-btn">Servo · OFF</button>
          <button type="button" class="btn-toggle" id="refill-feeder-btn">Feeder · OFF</button>
        </div>
      </div>
    </div>

    <div class="card" id="communication-card">
      <h2>Estatus de comunicación</h2>
      <div class="comm-row">
        <span class="status-dot error" id="comm-dot"></span>
        <strong id="comm-status">Sin enlace TCP</strong>
        <span class="comm-direction" id="comm-direction">—</span>
        <span class="comm-age" id="comm-age">—</span>
      </div>
      <div class="comm-message" id="comm-last-message">Sin mensajes</div>
    </div>

    <div class="card" id="sensors-card">
      <h2>Sensores</h2>
      <div class="sensor-list">
        <div class="sensor-row">
          <div class="led off" id="home-led"></div>
          <span class="sensor-title">GPIO 19 · Buffer Full</span>
          <span class="badge off" id="home-badge">—</span>
        </div>
        <div class="sensor-row">
          <div class="led off" id="endstop-led"></div>
          <span class="sensor-title">GPIO 21 · Buffer Max</span>
          <span class="badge off" id="endstop-badge">—</span>
        </div>
        <div class="sensor-row">
          <div class="led off" id="buffer-tension-led"></div>
          <span class="sensor-title">GPIO 22 · Holgura</span>
          <span class="badge off" id="buffer-tension-badge">—</span>
        </div>
        <div class="sensor-row">
          <div class="led tension off" id="tension-led"></div>
          <span class="sensor-title">GPIO 23 · Tensión</span>
          <span class="badge tension off" id="tension-badge">—</span>
        </div>
        <div class="sensor-row" id="cylinder-row">
          <div class="led off" id="cylinder-led"></div>
          <span class="sensor-title">GPIO 25 · Cilindro</span>
          <span class="badge off" id="cylinder-badge">—</span>
        </div>
        <div class="sensor-row" id="hose-belt-row">
          <div class="led off" id="hose-belt-led"></div>
          <span class="sensor-title">GPIO 27 · Cinta/manguera</span>
          <span class="badge off" id="hose-belt-badge">—</span>
        </div>
        <div class="sensor-row">
          <div class="led off" id="trigger2-led"></div>
          <span class="sensor-title">Trigger TCP (desde TCM)</span>
          <span class="badge off" id="trigger2-badge">—</span>
        </div>
      </div>
    </div>

    <div class="card" id="triggers-card">
      <h2>Triggers</h2>
      <p class="meta compact">
        Tfeed = longitud × (1 + 0.1) / velocidad. Longitud desde TCM.
      </p>
      <div class="form-group">
        <label for="piece-length-mm">Longitud total (mm)</label>
        <p class="field-desc">Desde el modelo activo en TCM · solo visual · no editable</p>
        <input type="number" id="piece-length-mm" min="0" step="0.1" value="0" readonly aria-readonly="true">
      </div>
      <div class="form-row">
        <div class="form-group">
          <label for="feed-speed-mm-s">Velocidad (mm/s)</label>
          <input type="number" id="feed-speed-mm-s" min="1" max="2000" step="1" value="100">
        </div>
        <div class="form-group">
          <label for="trigger-feed-sec">Tfeed (s)</label>
          <input type="number" id="trigger-feed-sec" min="0.1" max="60" step="0.01" value="2.0" readonly aria-readonly="true">
        </div>
      </div>
    </div>

    <div class="card" id="holgura-helper-card">
      <h2>Helper Holgura</h2>
      <p class="meta compact">
        Si holgura ausente ≥ umbral → feeder a velocidad/duración propias (prioridad sobre trigger TCP).
        Si ausente ≥ falla → error PF-006 / opcode Holgura L|R.
      </p>
      <div class="form-row">
        <div class="form-group">
          <label for="holgura-helper-rpm">Velocidad (RPM)</label>
          <input type="number" id="holgura-helper-rpm" min="1" max="600" step="1" value="60">
        </div>
        <div class="form-group">
          <label for="holgura-helper-s">Duración (s)</label>
          <input type="number" id="holgura-helper-s" min="0.05" max="60" step="0.05" value="1.0">
        </div>
      </div>
      <div class="form-row">
        <div class="form-group">
          <label for="holgura-helper-absent-ms">Ausente → helper (ms)</label>
          <input type="number" id="holgura-helper-absent-ms" min="20" max="5000" step="10" value="100">
        </div>
        <div class="form-group">
          <label for="holgura-fault-s">Ausente → falla (s)</label>
          <input type="number" id="holgura-fault-s" min="0.3" max="30" step="0.1" value="1.5">
        </div>
      </div>
    </div>

    <div class="card">
      <h2>StepMotor</h2>

      <div class="trigger-section">
        <p class="subsection-title">StepMotor – DeReeler</p>
        <div class="form-group">
          <label for="auto-rpm">Velocidad RPM</label>
          <input type="number" id="auto-rpm" min="1" max="600" step="1" value="60">
        </div>
      </div>

      <div class="trigger-section">
        <p class="subsection-title">StepMotor – Feeder</p>
        <div class="form-group">
          <label for="rpm2">Velocidad RPM</label>
          <input type="number" id="rpm2" min="1" max="600" step="1" value="60">
        </div>
      </div>
    </div>

    <div class="card">
      <h2>CW - CCW Settings</h2>
      <p class="meta compact">
        GPIO 23 tensión: +30 RPM · no cambia sentido (bobinas en planta) · duración = campo abajo.
        Timeout tensión / Buffer Full fijos: 10 s.
      </p>
      <div class="form-group">
        <label for="auto-rev">Boost tensión (s)</label>
        <p class="field-desc">TEMP: RPM UI + 30 en CW · duración del boost al detectar tensión.</p>
        <input type="number" id="auto-rev" min="0.1" max="60" step="0.1" value="2.0">
      </div>
      <div class="form-group">
        <label for="tension-cooldown">Espera entre rutinas (s)</label>
        <p class="field-desc">Pausa antes de volver a activar boost por tensión.</p>
        <input type="number" id="tension-cooldown" min="0" max="60" step="0.1" value="0">
      </div>
      <div class="form-group">
        <label for="servo-pwm">Servo PWM (µs)</label>
        <p class="field-desc">500–2500 · neutro 1500 = parado. &lt;1500 y &gt;1500 = sentidos opuestos. Este PreFeeder guarda su propio valor.</p>
        <input type="number" id="servo-pwm" min="500" max="2500" step="10" value="SERVO_PWM_UI_DEFAULT">
      </div>
    </div>

    <div class="card">
      <div class="save-all-wrap">
        <button type="button" class="btn btn-primary" onclick="saveAllCfg()">Guardar configuración</button>
      </div>
    </div>

    <p class="footer">Actualización automática cada 100 ms</p>
  </div>

  <script>
    function setStatus(ok, text) {
      var dot = document.getElementById('status-dot');
      var txt = document.getElementById('status-text');
      dot.className = 'status-dot' + (ok ? '' : ' error');
      if (text) txt.textContent = text;
    }
    var routineInfoKeys = {
      autoRpm: { id: 'auto-rpm', suffix: ' RPM' },
      autoRev: { id: 'auto-rev', suffix: ' s' },
      tensionCooldown: { id: 'tension-cooldown', suffix: ' s' },
      servoPwm: { id: 'servo-pwm', suffix: ' µs' },
      rpm2: { id: 'rpm2', suffix: ' RPM' },
      triggerFeed: { id: 'trigger-feed-sec', suffix: ' s' },
      pieceLength: { id: 'piece-length-mm', suffix: '' },
      feedSpeed: { id: 'feed-speed-mm-s', suffix: '' },
      holguraHelperRpm: { id: 'holgura-helper-rpm', suffix: ' RPM' },
      holguraHelperS: { id: 'holgura-helper-s', suffix: ' s' },
      holguraHelperMs: { id: 'holgura-helper-absent-ms', suffix: ' ms' },
      holguraFault: { id: 'holgura-fault-s', suffix: ' s' }
    };
    function refreshRoutineInfo() {
      Object.keys(routineInfoKeys).forEach(function(key) {
        var cfg = routineInfoKeys[key];
        var input = document.getElementById(cfg.id);
        if (!input) return;
        document.querySelectorAll('.info-param[data-info-key="' + key + '"]').forEach(function(el) {
          el.textContent = input.value + cfg.suffix;
        });
      });
    }
    function toggleRoutineInfo(open) {
      var overlay = document.getElementById('routineInfoOverlay');
      if (!overlay) return;
      overlay.classList.toggle('open', !!open);
      if (open) refreshRoutineInfo();
    }
    document.addEventListener('keydown', function(e) {
      if (e.key === 'Escape') toggleRoutineInfo(false);
    });
    function setInput(prefix, input, activeHigh) {
      var active = input.active;
      var led = document.getElementById(prefix + '-led');
      var badge = document.getElementById(prefix + '-badge');
      var style = (prefix === 'tension') ? ' tension' : '';
      led.className = 'led' + style + (active ? ' on' : ' off');
      badge.className = 'badge' + style + (active ? ' on' : ' off');
      badge.textContent = active ? 'Activo' : 'Inactivo';
    }
    var autoInit = false;
    var motor2Init = false;
    var trigger2Init = false;
    var tensionInit = false;
    var cfgDirty = false;
    var cfgFieldIds = [
      'auto-rpm', 'auto-rev', 'rpm2',
      'feed-speed-mm-s', 'tension-cooldown', 'servo-pwm', 'refill-pulse-s',
      'holgura-helper-rpm', 'holgura-helper-s', 'holgura-helper-absent-ms', 'holgura-fault-s'
    ];
    function markCfgDirty() { cfgDirty = true; }
    function bindCfgDirty() {
      cfgFieldIds.forEach(function(id) {
        var el = document.getElementById(id);
        if (!el || el._cfgDirtyBound) return;
        el._cfgDirtyBound = true;
        el.addEventListener('input', markCfgDirty);
        el.addEventListener('change', markCfgDirty);
      });
    }
    function syncCfg(id, v) {
      var el = document.getElementById(id);
      if (!el || cfgDirty || document.activeElement === el || v === undefined) return;
      if (String(el.value) !== String(v)) el.value = v;
    }
    function applyTrigger2(t) {
      var bufLed = document.getElementById('buffer-tension-led');
      var bufBadge = document.getElementById('buffer-tension-badge');
      var holgura = (t.holgura_present !== undefined) ? t.holgura_present : t.buffer_tension;
      if (holgura) {
        bufLed.className = 'led on';
        bufBadge.className = 'badge on';
        bufBadge.textContent = 'HOLGURA';
      } else {
        bufLed.className = 'led warning on';
        bufBadge.className = 'badge warning on';
        bufBadge.textContent = 'SIN HOLGURA';
      }
      var trigLed = document.getElementById('trigger2-led');
      var trigBadge = document.getElementById('trigger2-badge');
      var trigActive = t.trigger_active;
      trigLed.className = 'led' + (trigActive ? ' on' : ' off');
      trigBadge.className = 'badge' + (trigActive ? ' on' : ' off');
      if (!trigActive) trigBadge.textContent = 'Inactivo';
      else if (t.trigger_via === 'holgura_helper') trigBadge.textContent = 'Helper holgura';
      else trigBadge.textContent = 'Trigger TCP';
      syncCfg('rpm2', t.rpm_boost);
      syncCfg('feed-speed-mm-s', t.feed_speed_mm_s);
      syncCfg('holgura-helper-rpm', t.holgura_helper_rpm);
      syncCfg('holgura-helper-s', t.holgura_helper_s);
      syncCfg('holgura-helper-absent-ms', t.holgura_helper_absent_ms);
      syncCfg('holgura-fault-s', t.holgura_fault_s);
      motor2Init = true;
      // Solo lectura: siempre reflejar longitud/Tfeed del firmware.
      var lenEl = document.getElementById('piece-length-mm');
      if (lenEl && t.piece_length_mm !== undefined) lenEl.value = t.piece_length_mm;
      var tfeedEl = document.getElementById('trigger-feed-sec');
      if (tfeedEl && t.trigger_feed_s !== undefined) tfeedEl.value = t.trigger_feed_s;
      trigger2Init = true;
    }
    function saveAllCfg() {
      var rpm = parseFloat(document.getElementById('auto-rpm').value);
      if (isNaN(rpm) || rpm < 1) rpm = 60;
      var rev = parseFloat(document.getElementById('auto-rev').value);
      if (isNaN(rev) || rev < 0.1) rev = 2.0;
      if (rev > 60) rev = 60;
      var cd = parseFloat(document.getElementById('tension-cooldown').value);
      if (isNaN(cd) || cd < 0) cd = 0;
      var servoPwm = parseInt(document.getElementById('servo-pwm').value, 10);
      if (isNaN(servoPwm) || servoPwm < 500) servoPwm = 500;
      if (servoPwm > 2500) servoPwm = 2500;
      var rpm2 = parseFloat(document.getElementById('rpm2').value);
      if (isNaN(rpm2) || rpm2 < 1) rpm2 = 60;
      var feedSpeed = parseFloat(document.getElementById('feed-speed-mm-s').value);
      if (isNaN(feedSpeed) || feedSpeed < 1) feedSpeed = 100;
      if (feedSpeed > 2000) feedSpeed = 2000;
      var hRpm = parseFloat(document.getElementById('holgura-helper-rpm').value);
      if (isNaN(hRpm) || hRpm < 1) hRpm = 60;
      var hSec = parseFloat(document.getElementById('holgura-helper-s').value);
      if (isNaN(hSec) || hSec < 0.05) hSec = 1.0;
      if (hSec > 60) hSec = 60;
      var hAbsMs = parseInt(document.getElementById('holgura-helper-absent-ms').value, 10);
      if (isNaN(hAbsMs) || hAbsMs < 20) hAbsMs = 100;
      if (hAbsMs > 5000) hAbsMs = 5000;
      var hFault = parseFloat(document.getElementById('holgura-fault-s').value);
      if (isNaN(hFault) || hFault < 0.3) hFault = 1.5;
      if (hFault > 30) hFault = 30;
      var autoUrl = '/api/auto?rpm=' + encodeURIComponent(rpm)
        + '&reverse=' + encodeURIComponent(rev)
        + '&tension_cooldown=' + encodeURIComponent(cd)
        + '&servo_pwm=' + encodeURIComponent(servoPwm);
      var pulseS = parseFloat(document.getElementById('refill-pulse-s').value);
      if (isNaN(pulseS) || pulseS < 0.2) pulseS = 1.0;
      if (pulseS > 10) pulseS = 10;
      var m2Url = '/api/motor2?rpm=' + encodeURIComponent(rpm2)
        + '&feed_speed_mm_s=' + encodeURIComponent(feedSpeed)
        + '&holgura_helper_rpm=' + encodeURIComponent(hRpm)
        + '&holgura_helper_s=' + encodeURIComponent(hSec)
        + '&holgura_helper_absent_ms=' + encodeURIComponent(hAbsMs)
        + '&holgura_fault_s=' + encodeURIComponent(hFault);
      var refillUrl = '/api/refill?pulse_s=' + encodeURIComponent(pulseS);
      fetch(autoUrl)
        .then(function(r) { return r.json(); })
        .then(function(data) {
          if (data.auto) applyAuto(data.auto, data.error, data);
          return fetch(m2Url);
        })
        .then(function(r) { return r.json(); })
        .then(function(data) {
          if (data.trigger2) applyTrigger2(data.trigger2);
          return fetch(refillUrl);
        })
        .then(function(r) { return r.json(); })
        .then(function(data) {
          if (data.refill) applyRefill(data.refill, document.getElementById('idle-mode-btn').classList.contains('on'));
          cfgDirty = false;
          setStatus(true, 'Configuración guardada');
        })
        .catch(function() {
          setStatus(false, 'Error de conexión');
        });
    }
    function setToggleBtn(id, on, label, extraClass) {
      var btn = document.getElementById(id);
      if (!btn) return;
      btn.className = 'btn-toggle' + (extraClass ? (' ' + extraClass) : '') + (on ? ' on' : '');
      btn.textContent = label + ' · ' + (on ? 'ON' : 'OFF');
    }
    function applyModeFlags(data) {
      data = data || {};
      var idleOn = !!(data.idleMode !== undefined ? data.idleMode : data.testMode);
      var inProc = !!data.inProcess;
      var armed = data.sensorsArmed !== undefined ? !!data.sensorsArmed : (!idleOn && inProc);
      var idleBtn = document.getElementById('idle-mode-btn');
      if (idleBtn) {
        idleBtn.className = 'btn-toggle test-mode' + (idleOn ? ' on' : '');
        idleBtn.textContent = idleOn ? 'Materialista' : 'Idle';
      }
      var modeEl = document.getElementById('mode-label');
      var armedEl = document.getElementById('sensors-armed-label');
      if (modeEl) {
        if (data.machineState) {
          var msMap = {
            off: 'Off',
            idle: 'Idle',
            start: 'Idle',
            production: 'Idle',
            materialist: 'Materialista',
            in_process: 'In process',
            error: 'Error',
            stop: 'Stop'
          };
          modeEl.textContent = msMap[data.machineState] || data.machineState;
        } else if (idleOn) modeEl.textContent = 'Materialista';
        else if (inProc) modeEl.textContent = 'In process';
        else modeEl.textContent = 'Idle';
      }
      if (armedEl) armedEl.textContent = armed ? 'armados' : 'bloqueados';
    }
    function applyBuzzerMute(muted) {
      var btn = document.getElementById('buzzer-mute-btn');
      if (!btn) return;
      btn.className = 'btn-toggle mute' + (muted ? ' on' : '');
      btn.textContent = muted ? 'Buzzer · silenciado' : 'Buzzer · ON';
    }
    function toggleBuzzerMute() {
      var btn = document.getElementById('buzzer-mute-btn');
      var nextMute = !(btn && btn.classList.contains('on'));
      fetch('/api/auto?buzzer_mute=' + (nextMute ? '1' : '0'))
        .then(function(r) { return r.json(); })
        .then(function(data) {
          applyBuzzerMute(!!(data.buzzerMuted !== undefined ? data.buzzerMuted : nextMute));
          setStatus(true, nextMute ? 'Buzzer silenciado' : 'Buzzer activo');
        })
        .catch(function() {
          setStatus(false, 'Error de conexión');
        });
    }
    function toggleIdleMode() {
      var btn = document.getElementById('idle-mode-btn');
      // class "on" = Materialista; sin "on" = Idle
      var nextIdle = !(btn && btn.classList.contains('on'));
      fetch('/api/auto?idle_mode=' + (nextIdle ? '1' : '0'))
        .then(function(r) { return r.json(); })
        .then(function(data) {
          applyModeFlags(data);
          if (data.refill) applyRefill(data.refill, nextIdle);
          if (data.auto) applyAuto(data.auto, data.error, data);
          var inProc = !!(data.inProcess);
          setStatus(true, nextIdle
            ? 'Materialista · solo manual · torre naranja · TCM no produce'
            : (inProc ? 'In process · sensores armados'
              : 'Idle · sensores bloqueados / Iniciar o In process'));
        })
        .catch(function() {
          setStatus(false, 'Error de conexión');
        });
    }
    function applyRefill(r, idleOn) {
      r = r || {};
      setToggleBtn('refill-material-btn', !!r.material, 'Refill material');
      setToggleBtn('refill-dereeler-btn', !!r.dereeler, 'DeReeler');
      setToggleBtn('refill-servo-btn', !!r.servo, 'Servo');
      setToggleBtn('refill-feeder-btn', !!r.feeder, 'Feeder');
      if (r.pulse_s !== undefined) syncCfg('refill-pulse-s', r.pulse_s);
      var allow = !!idleOn;
      var pulseTxt = (r.pulse_s !== undefined) ? String(r.pulse_s) : '1';
      ['refill-material-btn', 'refill-dereeler-btn', 'refill-servo-btn', 'refill-feeder-btn'].forEach(function(id) {
        var b = document.getElementById(id);
        if (!b) return;
        b.disabled = !allow;
        b.title = allow
          ? ('Clic: pulso ' + pulseTxt + ' s · otro clic apaga')
          : 'Solo disponible en Materialista';
      });
    }
    function refillPulse(which, on) {
      var idleBtn = document.getElementById('idle-mode-btn');
      var idleOn = idleBtn && idleBtn.classList.contains('on');
      if (on && !idleOn) {
        setStatus(false, 'Refill solo en Materialista');
        return;
      }
      var url = '/api/refill?' + encodeURIComponent(which) + '=' + (on ? '1' : '0');
      fetch(url)
        .then(function(r) { return r.json(); })
        .then(function(data) {
          if (data.refill) applyRefill(data.refill, idleOn);
          if (data.error === 'idle_required') {
            setStatus(false, 'Refill solo en Materialista');
          } else if (data.error === 'system_fault' || (data.error && data.error.active)) {
            if (data.auto) applyAuto(data.auto, data.error, { idleMode: idleOn });
            else setStatus(false, 'Sistema en falla');
          } else if (data.refill && data.refill.active) {
            setStatus(true, 'Materialista · Refill pulso');
          } else if (data.ok && on) {
            setStatus(true, 'Materialista · pulso refill');
          }
        })
        .catch(function() {
          setStatus(false, 'Error de conexión');
        });
    }
    function bindRefillClick(btnId, which) {
      var btn = document.getElementById(btnId);
      if (!btn || btn._refillBound) return;
      btn._refillBound = true;
      btn.addEventListener('click', function() {
        if (btn.disabled) return;
        refillPulse(which, !btn.classList.contains('on'));
      });
    }
    function applyAuto(a, err, mode) {
      a = a || {};
      mode = mode || {};
      var idleOn = !!(mode.idleMode !== undefined ? mode.idleMode : mode.testMode);
      var inProc = !!mode.inProcess;
      var txt = 'Automático desactivado';
      var ok = true;
      if (err && err.active) {
        ok = false;
        if (err.ui) txt = err.ui;
        else if (err.reason === 'endstop') txt = (err.exxx || 'E053') + ': Pre-Feeder, Buffer Max (endstop)';
        else if (err.reason === 'tension_timeout') txt = (err.exxx || 'E054') + ': Pre-Feeder, Tension timeout';
        else if (err.reason === 'cylinder_open') txt = (err.exxx || 'E055') + ': Pre-Feeder, Cilindro abierto';
        else if (err.reason === 'hose_absent') txt = (err.exxx || 'E056') + ': Pre-Feeder, Manguera ausente';
        else if (err.reason === 'buffer_timeout') txt = (err.exxx || 'E052') + ': Pre-Feeder, Buffer sin relleno';
        else if (err.reason === 'holgura_timeout') txt = (err.exxx || 'E057') + ': Pre-Feeder, Sin holgura';
        else if (err.reason === 'operator_stop') txt = 'PF-007: Pre-Feeder, Parada operador';
        else txt = err.tag || 'Error';
      } else if (idleOn) {
        txt = 'Materialista · solo manual';
      } else if (!a.enabled) {
        txt = 'Detenido · falta Iniciar';
      } else if (a.state === 'cw') {
        txt = 'Girando CW';
      } else if (a.state === 'servo_lead') {
        txt = 'Servo ON · DeReeler en 350 ms';
      } else if (a.state === 'home_hold') {
        txt = inProc ? 'Buffer Full · espera' : 'Listo · sensores congelados';
      } else if (a.state === 'endstop_fault') { ok = false; txt = 'Buffer Max'; }
      else if (a.state === 'tension_fault') { ok = false; txt = 'Tensión prolongada'; }
      else if (a.state === 'cylinder_fault') { ok = false; txt = 'Cilindro abierto'; }
      else if (a.state === 'hose_fault') { ok = false; txt = 'Cinta/manguera ausente'; }
      else if (a.state === 'buffer_fault') {
        ok = false;
        txt = 'Buffer no rellenó (10 s)';
      }
      else if (a.state === 'holgura_fault') {
        ok = false;
        var hf2 = document.getElementById('holgura-fault-s');
        txt = 'Sin holgura (' + (hf2 ? hf2.value : '1.5') + ' s)';
      }
      else if (a.state === 'operator_stop') {
        ok = false;
        txt = 'Parada operador · falta Reset';
      }
      else if (inProc) {
        txt = 'In process · sensores armados';
      } else txt = 'Idle · sensores bloqueados';
      if (window._pfSideTag) txt += ' · ' + window._pfSideTag;
      setStatus(ok, txt);
      if (!autoInit || !cfgDirty) {
        syncCfg('auto-rpm', a.rpm);
        syncCfg('auto-rev', a.reverse_s);
        if (a.servo_pwm_us !== undefined) syncCfg('servo-pwm', a.servo_pwm_us);
        autoInit = true;
      }
      if (a.tension_cooldown_s !== undefined && (!tensionInit || !cfgDirty)) {
        syncCfg('tension-cooldown', a.tension_cooldown_s);
        tensionInit = true;
      }
    }
    function autoCmd(action) {
      var rpm = parseFloat(document.getElementById('auto-rpm').value);
      if (isNaN(rpm) || rpm < 1) rpm = 60;
      var rev = parseFloat(document.getElementById('auto-rev').value);
      if (isNaN(rev) || rev < 0.1) rev = 2.0;
      var servoPwm = parseInt(document.getElementById('servo-pwm').value, 10);
      if (isNaN(servoPwm) || servoPwm < 500) servoPwm = 500;
      if (servoPwm > 2500) servoPwm = 2500;
      var url = '/api/auto?rpm=' + encodeURIComponent(rpm)
        + '&reverse=' + encodeURIComponent(rev)
        + '&servo_pwm=' + encodeURIComponent(servoPwm);
      if (action === 'start') url += '&enable=1';
      else if (action === 'stop') url += '&enable=0';
      else if (action === 'reset') url += '&reset=1';
      fetch(url)
        .then(function(r) { return r.json(); })
        .then(function(data) {
          if (data.auto) applyAuto(data.auto, data.error, data);
        })
        .catch(function() {
          setStatus(false, 'Error de conexión');
        });
    }
    function applyCylinder(c) {
      var open = c.open;
      var row = document.getElementById('cylinder-row');
      var led = document.getElementById('cylinder-led');
      var badge = document.getElementById('cylinder-badge');
      led.className = 'led error' + (open ? ' on' : ' off');
      badge.className = 'badge' + (open ? ' on' : ' off');
      badge.textContent = open ? 'ABIERTO' : 'CERRADO';
      row.className = 'sensor-row' + (open ? ' alarm' : '');
    }
    function applyHoseBelt(h) {
      h = h || {};
      var absent = !!h.absent;
      var row = document.getElementById('hose-belt-row');
      var led = document.getElementById('hose-belt-led');
      var badge = document.getElementById('hose-belt-badge');
      led.className = 'led error' + (absent ? ' on' : ' off');
      badge.className = 'badge' + (absent ? ' on' : ' off');
      badge.textContent = absent ? 'AUSENTE' : 'OK';
      if (row) row.className = 'sensor-row' + (absent ? ' alarm' : '');
    }
    function applyPeer(peer) {
      peer = peer || {};
      var linked = !!(peer.link || peer.link_ok);
      var dot = document.getElementById('comm-dot');
      var status = document.getElementById('comm-status');
      var direction = document.getElementById('comm-direction');
      var age = document.getElementById('comm-age');
      var message = document.getElementById('comm-last-message');
      if (dot) dot.className = 'status-dot' + (linked ? '' : ' error');
      if (status) status.textContent = linked
        ? ('PreFeeder ↔ Master' + (peer.master_ip ? ' (' + peer.master_ip + ')' : ''))
        : 'Sin enlace TCP (Master apagado o sin conectar)';
      if (direction) {
        if (peer.last_message_direction === 'R') direction.textContent = 'RECIBIDO';
        else if (peer.last_message_direction === 'E') direction.textContent = 'ENVIADO';
        else direction.textContent = '—';
      }
      if (age) {
        var ms = Number(peer.last_message_ms_ago || 0);
        age.textContent = ms ? ('hace ' + (ms < 1000 ? ms + ' ms' : (ms / 1000).toFixed(1) + ' s')) : '—';
      }
      if (message) message.textContent = peer.last_message || 'Sin mensajes';
    }
    function apply(data) {
      setInput('home', data.home, true);
      setInput('endstop', data.endstop, true);
      setInput('tension', data.tension, false);
      if (data.cylinder) applyCylinder(data.cylinder);
      if (data.hose_belt) applyHoseBelt(data.hose_belt);
      if (data.tension && data.tension.blocked) {
        var tBadge = document.getElementById('tension-badge');
        tBadge.textContent = 'Espera ' + data.tension.cooldown_s + ' s';
      }
      if (data.tension && data.tension.cooldown_s !== undefined) {
        syncCfg('tension-cooldown', data.tension.cooldown_s);
        tensionInit = true;
      }
      if (data.error) {
        var active = data.error.active;
        var reason = 'Ninguno';
        if (active && data.error.ui) {
          reason = data.error.ui;
        } else if (data.error.reason === 'endstop') {
          reason = (data.error.exxx || data.error.tag || 'E053') + ': Pre-Feeder, Buffer Max (endstop)';
        } else if (data.error.reason === 'tension_timeout') {
          reason = (data.error.exxx || data.error.tag || 'E054') + ': Pre-Feeder, Tension timeout';
        } else if (data.error.reason === 'cylinder_open') {
          reason = (data.error.exxx || data.error.tag || 'E055') + ': Pre-Feeder, Cilindro abierto';
        } else if (data.error.reason === 'hose_absent') {
          reason = (data.error.exxx || data.error.tag || 'E056') + ': Pre-Feeder, Manguera ausente';
        } else if (data.error.reason === 'buffer_timeout') {
          reason = (data.error.exxx || data.error.tag || 'E052') + ': Pre-Feeder, Buffer sin relleno';
        } else if (data.error.reason === 'holgura_timeout') {
          reason = (data.error.exxx || data.error.tag || 'E057') + ': Pre-Feeder, Sin holgura';
        } else if (data.error.reason === 'operator_stop') {
          reason = 'PF-007: Pre-Feeder, Parada operador';
        } else if (active && data.error.tag) {
          reason = data.error.tag + (data.error.reason ? ' · ' + data.error.reason : '');
        }
        document.getElementById('error-reason').textContent = reason;
        document.getElementById('error-reason-line').hidden = !active;
      }
      applyModeFlags(data);
      if (data.buzzerMuted !== undefined) applyBuzzerMute(!!data.buzzerMuted);
      var idleOnPoll = !!(data.idleMode !== undefined ? data.idleMode : data.testMode);
      if (data.refill) applyRefill(data.refill, idleOnPoll);
      if (data.trigger2) applyTrigger2(data.trigger2);
      if (data.auto) {
        if (data.refill && data.refill.active && idleOnPoll && !(data.error && data.error.active))
          setStatus(true, 'Materialista · Refill pulso');
        else
          applyAuto(data.auto, data.error, data);
      } else if (data.error && data.error.active) {
        applyAuto({ enabled: false, state: 'off' }, data.error, data);
      }
      applyPeer(data.peer);
      if (data.side) {
        window._pfSideTag = data.side;
        var sideLabel = data.side === 'R' ? 'Derecho' : 'Izquierdo';
        document.title = 'Prefeeder ' + data.side + ' · ' + sideLabel;
        var titleEl = document.getElementById('pf-page-title');
        if (titleEl) titleEl.textContent = 'Prefeeder ' + data.side + ' · ' + sideLabel;
      }
      var infoOpen = document.getElementById('routineInfoOverlay');
      if (infoOpen && infoOpen.classList.contains('open')) refreshRoutineInfo();
    }
    function poll() {
      fetch('/api/status', { cache: 'no-store' })
        .then(function(r) {
          if (!r.ok) throw new Error('HTTP ' + r.status);
          return r.json();
        })
        .then(apply)
        .catch(function(e) {
          setStatus(false, e && e.message ? e.message : 'Error de conexión');
        });
    }
    setInterval(poll, 600);
    bindCfgDirty();
    bindRefillClick('refill-material-btn', 'material');
    bindRefillClick('refill-dereeler-btn', 'dereeler');
    bindRefillClick('refill-servo-btn', 'servo');
    bindRefillClick('refill-feeder-btn', 'feeder');
    (function bindRefillPulseSave() {
      var el = document.getElementById('refill-pulse-s');
      if (!el || el._pulseSaveBound) return;
      el._pulseSaveBound = true;
      el.addEventListener('change', function() {
        var v = parseFloat(el.value);
        if (isNaN(v) || v < 0.2) v = 1.0;
        if (v > 10) v = 10;
        el.value = v;
        fetch('/api/refill?pulse_s=' + encodeURIComponent(v))
          .then(function(r) { return r.json(); })
          .then(function(data) {
            var idleOn = document.getElementById('idle-mode-btn').classList.contains('on');
            if (data.refill) applyRefill(data.refill, idleOn);
          })
          .catch(function() {});
      });
    })();
    poll();
  </script>
</body>
</html>
)rawliteral";

#endif
