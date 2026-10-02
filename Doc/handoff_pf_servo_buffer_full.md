# Handoff — PreFeeder RC no frena con Buffer Full

**Fecha:** 2026-10-02  
**Módulo:** PreFeeder L/R (`PF/PF_LR/`)  
**Síntoma:** El servo RC de rotación continua **sigue girando** aunque el sensor Buffer Full (GPIO19) esté activo (HIGH).  
**Clasificación de fallo (norma TCM):** **C — Command OK / execution failure** (o B si el GPIO no se interpreta). **No es fallo de enlace TCP/WiFi.** No tocar Communication Core.

**Conversaciones previas:**
- [Servo Detach Buffer Full](38fc6eec-7de0-4dc7-91aa-39e3396e6ace) — diagnóstico Detach vs neutro + hard-stop
- Esta sesión — refinamiento anti-Detach periódico

---

## 1. Contexto del hardware / firmware

| Pieza | Detalle |
|--------|---------|
| MCU | ESP32, firmware `PF/PF_LR/PF_LR.ino` (+ `Config.h`) |
| Servo | RC **continuo** GPIO **26** (LEDC ch 7, 50 Hz, 14 bit) |
| Marcha L | ~**800 µs** |
| Marcha R | ~**2000 µs** |
| Neutro (stop) | ~**1500 µs** — hay que **seguir emitiendo** PWM; no basta cortar señal |
| Buffer Full | GPIO **19**, HIGH = activo |
| Conflicto conocido | `StepperRMT` (DeReeler GPIO32 / Feeder GPIO18) al hacer `Stop`/`setSpeed` **suelta el GPIO del LEDC** del servo. El canal puede “leer” 1500 y el cache creerá parado, pero el pin queda con el último pulso de marcha (failsafe RC ~3 s). Flag: `servoPinDirty` |

**Modo Materialista (`idleMode`):** el refill manual **ignora** Buffer Full a propósito (`refillManualActive`). El corte Full aplica en auto / In process, no en JOG Materialista con `refillServoOn`.

---

## 2. Soluciones anteriores que **no resolvieron** el síntoma (o lo empeoraron)

### Intento A — Interlock lógico Buffer Full (`aad9f45`)

- `bufferFullStopNow()` = `bufferFullStable || bufferFullRaw()` (parar al primer HIGH).
- Rearme solo con `bufferFullAllowsMotion()` (OFF estable ~200 ms).
- `servoAssertRun` / `serviceAuto` / `loop` cortan DeReeler+servo cuando Full.

**Por qué no bastó:** la lógica de estado sí dejaba de *pedir* marcha, pero el **pin físico** podía seguir emitiendo el último pulso de marcha (cache LEDC / RMT). El software creía parado; el RC seguía.

### Intento B — “Solo Detach para parar”

- Idea de operador/agente: cortar PWM con `ledcDetach` al Full.

**Rechazado / incorrecto:** en RC continuo, **sin PWM ≈ failsafe ≈ última consigna de marcha**. Detach **no** es freno; el freno es **seguir emitiendo 1500**.

### Intento C — Hard-stop siempre con remux (`5af59c6`, chat 38fc6eec)

Se introdujo:

- `servoHardStopNeutral(force)` → **siempre** `Detach` + `Attach` + `ledcWrite(1500)`
- `servoServiceBufferFullCut()` → flanco Full remux inmediato; mientras Full reafirma ~cada 20 ms
- `forceStopDereelerAndServo()` → remux antes/después de `motor->Stop()`
- `loop()`: Full → primero `servoServiceBufferFullCut()`, luego `forceStopDereelerAndServo()`

**Por qué no funcionó / pudo empeorar:**

1. Mientras Full permanecía ON, el reassert cada ~20 ms **volvía a hacer Detach**.
2. Cada Detach **corta el PWM** → el RC entra en failsafe de **última marcha** (800/2000).
3. Luego Attach+1500 debería frenar, pero si el RMT vuelve a ensuciar el pin, o el remux no “pega”, el síntoma es: **sensor Full ON y servo sigue (o casi no para)**.
4. Es decir: el remux es necesario **cuando el pin está huérfano (`dirty`)**, pero **Detach periódico con LEDC sano es contraproducente**.

### Cosas ya correctas en dominio (no reabrir sin evidencia)

- Polaridad Full: HIGH = activo (`bufferFullRaw`).
- Neutro = 1500; marcha L/R = 800/2000 (`Config.h`).
- No tocar Master TCP / WiFi / heartbeat por este síntoma (clase B/C).

---

## 3. Qué se hizo **ahora** (2026-10-02, sin flashear verificado aún)

**Archivo:** `PF/PF_LR/PF_LR.ino` — función `servoHardStopNeutral`  
**Estado:** cambio **local / uncommitted** (además de otros diffs no relacionados en el mismo archivo: tensión, Tfeed, Materialista HTML, etc.).

### Cambio

Antes: **siempre** remux (`Detach`+`Attach`+1500) en hard-stop (flanco y reassert).

Ahora:

| Condición | Acción |
|-----------|--------|
| `servoPinDirty` (RMT ensució pin) | Remux + 1500 |
| `force && !wasRunning` (flanco Full pero software ya decía neutro → cache mentiroso) | Remux + 1500 |
| Servo aún en marcha y pin **no** dirty | **Solo** `ledcWrite(1500)` — **sin Detach** |
| Reafirmación limpia (ya neutro, no dirty) | Solo `ledcWrite(1500)` cada ~20 ms; si `ledcWrite` falla → remux de recuperación |

Hipótesis: el fallo residual era **Detach de más**, no falta de remux.

### Qué **no** se tocó

- Communication Core (TCP/WiFi/reconnect/heartbeat/loop order de Master).
- Polaridad GPIO19, opcodes, Master, HMI.
- Semántica Materialista (refill ignora Full).

---

## 4. Rutas de código relevantes (para el agente siguiente)

```
loop()
  updateBufferFullFilter()
  if (!idleMode && bufferFullStopNow())
    servoServiceBufferFullCut()   // flanco → hardStop(edge)
    forceStopDereelerAndServo()   // hardStop + motor->Stop + dirty + hardStop
  ...
  serviceServoPwm()               // si Full → servoServiceBufferFullCut
  serviceAuto()                   // si Full → forceStop + HOME_HOLD
  serviceServoPwm()
```

Funciones clave en `PF_LR.ino`:

- `servoWriteUsForced` — remux solo si `servoPinDirty`; early-return si duty igual (glitch si dirty mal marcado).
- `servoHardStopNeutral` — **punto del último cambio**.
- `servoServiceBufferFullCut` — latch de flanco Full.
- `forceStopDereelerAndServo` — DeReeler + RC.
- `servoMarkPinDirty` — tras casi todo `Stop`/`setSpeed` RMT (loop y tarea `motor2HolguraTask`).
- `bufferFullStopNow` / `bufferFullAllowsMotion` — parada vs rearme.

---

## 5. Análisis para el siguiente agente

### Diagnóstico en cadena (norma)

```
¿salió comando de neutro? → ¿LEDC sigue dueño del pin? → ¿duty 1500 en pin?
→ ¿servo interpreta 1500 como stop? → ¿algo reescribe marcha después?
```

| Clase | Pregunta | Dónde mirar |
|-------|----------|-------------|
| Sensor | ¿GPIO19 es HIGH real cuando UI dice Full? | Multímetro / HTML L-R `home` / Serial |
| Cache LEDC | ¿`servoLastOutputUs==1500` y `servoRunning==false` pero pin aún marcha? | Scope GPIO26; `servoPinDirty` |
| RMT race | ¿tarea M2 marca dirty y roba pin entre remux? | `motor2HolguraTask` + `servoMarkPinDirty` |
| Neutro mecánico | ¿1500 no es muerto en este servo? | Ajustar deadband / NVS `servo_pwm` |
| Modo | ¿está en Materialista con refill? | Full no debe cortar |
| Rearme | ¿Full chatter OFF→ON y `HOME_HOLD` relanza lead? | `bufferFullAllowsMotion` + `beginAutoCwWithServoLead` |

### Hipótesis ordenadas (prioridad)

1. **Detach/remux excesivo** (mitigada en este cambio; **falta validar en máquina**).
2. **RMT deja pin huérfano sin `dirty`** → early-return en `servoWriteUsForced` / hard-stop cree que ya está en 1500.
3. **Neutro 1500 ≠ stop** en el servo concreto (calibración).
4. **Sensor / polaridad / cable** (UI Full ON pero GPIO LOW, o al revés).
5. **Algo reescribe marcha** tras el corte (`servoAssertRun` / refill / lead) — menos probable en auto con `bufferFullStopNow` en los guards actuales.

### Si el fix actual **falla** en banco — caminos sugeridos (sin tocar Communication Core)

1. **Verificar flasheo** de L y R con este `PF_LR.ino` (el cambio es solo local hasta commit).
2. Scope GPIO26 al flanco Full: ¿pasa a ~1.5 ms @ 50 Hz y se mantiene, o desaparece/revierte a 0.8/2.0 ms?
3. Si el pin pierde LEDC sin dirty: forzar remux **solo** cuando `ledcRead`/propiedad del pin falle; o `gpio_matrix_out` explícito post-RMT.
4. Alternativa estructural: **no compartir recursos** LEDC↔RMT — MCPWM / timer dedicado / otro pin para el servo (cambio HW o driver).
5. Calibrar neutro (trimpot / offset µs) si 1500 no detiene el modelo de servo.
6. Log mínimo temporal (solo si se pide): Full edge, dirty, lastUs, remux sí/no — **sin** spam continuo.

### Criterio de aceptación

- Auto + In process: GPIO19 HIGH → RC **para de inmediato** y **permanece quieto** mientras Full ON.
- Full OFF estable ~200 ms → reanuda lead (800 L / 2000 R) + DeReeler.
- Materialista + refill: Full **no** debe bloquear el JOG (comportamiento intencional).
- Sin regresiones: vibración RC por `ledcWrite` cada frame; DeReeler no “trabado” por `Stop()` spam.

### Archivos a tocar (si hace falta más)

- Preferente: `PF/PF_LR/PF_LR.ino`, quizá `PF/PF_LR/Config.h` (tiempos / neutro).
- **No** por defecto: `PreFeeder_Master.ino`, `HMI/tcp_link.py`, WiFi/TCP/heartbeat.
- Si se documenta norma nueva de comportamiento Full↔servo: actualizar `Doc/normas_arquitectura.md` y alinear HTML local (M4).

### COMMUNICATION IMPACT

```
COMMUNICATION IMPACT
Componente protegido: ninguno tocado en este cambio
Cambio propuesto: solo PWM servo / Buffer Full en PF_LR
Relación causal: N/A (dominio actuador)
Riesgo: ninguno de enlace si se mantiene el alcance
Alternativa sin tocar comunicación: ya es la vía correcta
```

---

## 6. Checklist rápido para el agente receptor

- [ ] Confirmar con usuario: ¿ya flasheó L/R con el cambio del 2026-10-02?
- [ ] Reproducir: auto armado, alimentar hasta Full ON, observar RC (no solo DeReeler).
- [ ] Si falla: scope GPIO26 + leer GPIO19 en el mismo instante.
- [ ] Descartar Materialista/refill.
- [ ] No “arreglar” TCP/WiFi por este síntoma.
- [ ] Si se cambia de nuevo `servoHardStopNeutral`, no volver a **Detach siempre** en reassert.

---

## 7. Resumen ejecutivo

Se intentó primero interlock lógico, luego remux agresivo (Detach+1500 siempre). El remux agresivo es necesario cuando RMT deja el pin huérfano, pero **Detach periódico mientras Full está ON puede mantener el RC en failsafe de marcha**. El cambio actual limita Detach a dirty / cache mentiroso y, si el servo aún marcha con LEDC sano, solo escribe 1500. **Pendiente validación en máquina.**
