# Revisión técnica — PreFeeder RC / Buffer Full (para ChatGPT)

**Fecha del documento:** 2026-10-02  
**Proyecto:** TCM — PreFeeder L/R  
**Objetivo de este MD:** dar a otro modelo (ChatGPT) todo el contexto de análisis, evidencias de banco, intentos fallidos/parciales y estado del código, para proponer el siguiente paso **sin reinventar** ni tocar Communication Core.

**Archivos de código:** `PF/PF_LR/PF_LR.ino`, `PF/PF_LR/Config.h`  
**Handoff corto previo:** `Doc/handoff_pf_servo_buffer_full.md`  
**Norma TCM:** fallo clase **B/C** (comando/ejecución). **No** es TCP/WiFi/heartbeat. No modificar `PreFeeder_Master`, `HMI/tcp_link.py`, ni orden de `loop()` de comunicación.

---

## 1. Problema original (síntoma de operador)

El servo RC de **rotación continua** del PreFeeder **sigue alimentando** aunque el sensor **Buffer Full** (GPIO19 HIGH) esté activo.

Más tarde aparecieron síntomas relacionados:

| # | Síntoma | Cuándo |
|---|---------|--------|
| S1 | RC no frena con Full; sigue ~2–3 s | Tras `HOME_HOLD` en serial |
| S2 | Solo **Pause** (`In process OFF`) detiene el eje de verdad | Mismo ensayo |
| S3 | En Pause el eje **salta / tira a tirones** | Tras spam de `Brake()` |
| S4 | Interrupt WDT / resets | Soft-PWM con `esp_timer` + `delayMicroseconds` |
| S5 | Lado **L** pierde ~7 Tfeeds teóricos | Cola de 1 slot pisaba triggers |
| S6 | Con “stop” (neutro), RC **creep** lento + giros salteados | Soft-PWM FreeRTOS (jitter del pulso 1500) |

---

## 2. Hardware / contrato PWM (hechos estables)

| Pieza | Valor |
|--------|--------|
| MCU | ESP32, firmware `PF_LR` |
| Servo | RC **continuo**, GPIO **26** |
| Marcha L | ~**800 µs** |
| Marcha R | ~**2000 µs** |
| Neutro (stop) | ~**1500 µs** — **hay que seguir emitiendo PWM**; cortar señal ≠ freno |
| Buffer Full | GPIO **19**, HIGH = activo |
| DeReeler | Stepper vía **RMT** (GPIO32 + MOSFET 33) |
| Feeder M2 | Stepper RMT (GPIO18), Tfeed por trigger TCP |
| Conflicto | `StepperRMT` `Stop`/`setSpeed`/`Brake` puede **soltar el GPIO del LEDC** del servo → pin huérfano; RC entra en **failsafe ~2–3 s** con la **última marcha** |

**Regla RC continuo (crítica):**

- Sin PWM / Detach → failsafe ≈ última consigna de marcha (800 o 2000).
- Freno real → PWM estable a **1500 µs @ 50 Hz**.
- Por eso **“solo Detach para parar” está mal**.

**Excepción de producto:** en Materialista (`idleMode` + refill JOG) Buffer Full **no** debe cortar a propósito.

---

## 3. Cadena de diagnóstico TCM (aplicada)

```
¿salió? → ¿llegó? → ¿recibido? → ¿interpretado? → ¿lógica permitió? → ¿actuador ejecutó?
```

| Capa | Resultado en banco | Evidencia |
|------|--------------------|-----------|
| Sensor GPIO19 | OK | Serial: `BUFFER: Full ON (GPIO19 estable)` |
| Lógica auto | OK | `AUTO: Buffer Full -> HOME_HOLD (force stop)` |
| Orden raw vs estable | OK (intencional) | `bufferFullStopNow() = raw \|\| stable` → HOME_HOLD puede salir **antes** del log “Full ON estable” |
| Ciclo Full→Tfeed→OFF→lead | OK (diseño) | Tras Full, Tfeed vacía el lazo → Full OFF → vuelve a alimentar |
| PWM 1500 efectivo en GPIO26 | **FALLABA** | Operador: RC seguía ~2–3 s; solo Pause detenía |
| Clasificación | **C** (execution) | No tocar Communication Core |

### Traza típica que cerró el diagnóstico (log ~10:10 / 10:28)

```
Full OFF → servo lead → DeReeler CW
… ~3 s relleno …
HOME_HOLD (force stop)     ← software ya cortó
BUFFER: Full ON estable
… ~2–3 s el eje SIGUE …
In process: OFF (Pause)    ← ahí sí se detuvo
```

Conclusión operativa: **HOME_HOLD era software; el actuador no aplicaba neutro a tiempo.** Pause sí porque desarma y vuelve a llamar `forceStopDereelerAndServo()`.

### Interpretación errónea descartada

- “HOME_HOLD al revés / se rearranca solo” — las vueltas Full OFF→lead tras un Tfeed son **ciclo normal** (M2 vacía el buffer).
- Logs solo de `M2: timed_feed` **no** validan el corte del RC (otro motor).

---

## 4. Historial de intentos (cronológico) y resultados

### Intento A — Interlock lógico Buffer Full (`aad9f45`)

- `bufferFullStopNow()` = raw|stable; rearme solo con OFF estable.
- Guards en `servoAssertRun` / `serviceAuto` / `loop`.

**Resultado:** deja de *pedir* marcha, pero el pin puede seguir con el último pulso. **No resolvió S1.**

### Intento B — “Solo Detach para parar”

**Rechazado:** Detach = failsafe de marcha. Empeora o no frena.

### Intento C — Hard-stop siempre Detach+Attach+1500 + reassert ~20 ms (`5af59c6`)

**Resultado / análisis:** remux necesario si RMT deja pin huérfano, pero **Detach periódico mientras Full ON corta el PWM** → failsafe de marcha otra vez. **Contraproducente.**

### Intento D — Remux solo dirty / cache mentiroso; con LEDC sano solo `ledcWrite(1500)`

**Resultado:** más correcto en papel; en banco el RC **aún seguía ~2 s** tras HOME_HOLD. Hipótesis: tras `motor->Stop()` el pin queda huérfano un instante / remux no “pega”.

### Intento E — Remux fuerte (Detach → GPIO LOW → Attach ×2) + burst 1500 ~150 ms post-Stop

**Resultado:** sigue el patrón HOME_HOLD → Pause ~2–3 s. LEDC+RMT en el mismo ESP32 **no fiable** para neutro inmediato.

### Intento F — Soft-PWM `esp_timer` 50 Hz + `Brake()` en vez de `Stop()`

Motivación:

1. Sacarse LEDC del camino del RMT.
2. `Stop()` = rampa `MOTOR_DECEL` → DeReeler sigue ~1–2 s; `Brake()` corta más seco.

**Resultado parcial:**

- Pause sí corta (confirmado antes).
- Soft-PWM con `delayMicroseconds` en callback → **Interrupt WDT** (S4).
- Luego Soft-PWM en tarea FreeRTOS → **creep + saltos en stop** (S6): el 1500 µs **tiembla** al preemptarse → RC interpreta micro-marcha.
- `Brake()` en cada loop / cada 40 ms en Pause → **saltitos** (S3). Fix: Brake solo si hay consigna/RPM; si quieto, solo neutro RC.

### Intento G — Cola Tfeed profundidad 8 + hist IDs

**Problema:** un solo slot pendiente → en L se perdían triggers (~7 teóricos).  
**Fix:** ring queue `M2_TRIGGER_QUEUE_DEPTH = 8`. Retenido en el build actual.

### Intento H — Bugs de estado alrededor de Full

1. En `HOME_HOLD`, si DeReeler aún tenía RPM y Full chatteaba, código podía **volver a lead** (huérfano). Fix: en ese caso **forceStop**, no lead.
2. Full OFF filter 200→**400 ms** para no rearrancar por chatter.

### Intento I (estado actual del árbol) — Volver a **LEDC hardware** + fixes retenidos

Tras S4/S6, se **abandonó soft-PWM** y se restauró LEDC estable @ 50 Hz.

**Se mantiene:**

- Cola Tfeed depth 8  
- Brake idempotente (sin spam)  
- Full OFF 400 ms  
- HOME_HOLD no rearranca lead huérfano  
- Hard-stop: remux si dirty/force+cache mentiroso; si pin sano, `ledcWrite(1500)`  
- Log boot: `SERVO: LEDC 50 Hz (pulso estable — sin soft-PWM)`  
- Log Full: `FULL CUT: LEDC→1500`

**Pendiente de validación en máquina** con este build LEDC+cola (flasheo L+R).

---

## 5. Estado actual del firmware (resumen de diseño)

### Constantes relevantes (`Config.h`)

```
SERVO_PWM_NEUTRAL_US     = 1500
SERVO_LEDC_CHANNEL       = 7, 50 Hz, 14 bit
SERVO_STOP_REASSERT_MS   = 20
BUFFER_FULL_ON_FILTER_MS = 80
BUFFER_FULL_OFF_FILTER_MS= 400
BUFFER_FULL_GLITCH_MS    = 40
M2_TRIGGER_QUEUE_DEPTH   = 8
SERIAL_VERBOSE           = 1 (debug)
```

Lado L/R: comentar/descomentar `PREFEEDER_SIDE_RIGHT` (L: 800 µs / IP .101; R: 2000 µs / IP .102).

### Flujo de corte Full

```
loop()
  updateBufferFullFilter()
  if (!idleMode && bufferFullStopNow())
    servoServiceBufferFullCut()   // flanco → hardStop
    forceStopDereelerAndServo()   // hardStop + Brake si hace falta + dirty awareness
  serviceServoPwm()
  serviceAuto()                   // Full → forceStop + HOME_HOLD
```

### Funciones clave

- `servoRemuxWriteUs` — Detach / GPIO LOW / Attach / write ×2  
- `servoHardStopNeutral` — remux solo dirty o force+cache mentiroso  
- `servoWriteUsForced` — no reescribir duty igual (evita glitch/vibración)  
- `forceStopDereelerAndServo` — Brake solo con consigna o RPM residual; si quieto no toca RMT  
- `bufferFullStopNow` / `bufferFullAllowsMotion`  
- Cola `triggerQ[]` para Tfeed  

---

## 6. Conclusiones analíticas consolidadas

1. **Sensor y FSM están bien.** El bug histórico es **ejecución del PWM de neutro** en GPIO26, no detección Full ni Master TCP.
2. En RC continuo, **Detach / pérdida de PWM = marcha**, no stop. Neutro = **1500 sostenido**.
3. **RMT (DeReeler/Feeder) ensucia LEDC** → pin huérfano → failsafe ~2–3 s. Explica S1 casi a la perfección.
4. Soft-PWM resolvía el conflicto RMT/LEDC en teoría, pero:
   - `esp_timer` + `delayMicroseconds` → WDT  
   - FreeRTOS bit-bang → jitter → creep/saltos en “parado”
5. **Pause detenía** porque rearmaba el corte (`forceStop`), no porque Full “no existiera”.
6. Spam de `Brake()` es regresión de stop agresivo → saltos en Pause.
7. Pérdida de Tfeeds en L = **cola de 1**, no lógica Full.
8. Si con LEDC 1500 **estable** el eje aún “empuja” sin jitter digital, sospechar **neutro mecánico ≠ 1500** (trimpot / calibración NVS `servo_pwm`).

---

## 7. Hipótesis abiertas (prioridad para el revisor)

| Prio | Hipótesis | Cómo falsificar |
|------|-----------|-----------------|
| 1 | Tras `Brake`/`Stop` RMT, LEDC pierde pin un instante → failsafe marcha | Scope GPIO26 al flanco Full: ¿pasa a 1.5 ms y se mantiene? |
| 2 | `servoPinDirty` no se marca en algún path RMT → early-return cree que ya hay 1500 | Auditar todos los `Stop`/`Brake`/`setSpeed` |
| 3 | Neutro 1500 no es muerto en el servo físico | Probar 1480–1520 o trimpot; UI NVS |
| 4 | Build no flasheado en L o R | Boot debe mostrar línea `SERVO: LEDC 50 Hz…` |
| 5 | Separación HW de drivers (MCPWM / otro pin / timer dedicado) | Solo si 1–3 fallan |

---

## 8. Criterios de aceptación

- [ ] Auto + In process, Full HIGH → RC **para de inmediato** y **queda quieto** (sin Pause manual).
- [ ] Sin creep ni saltos con neutro.
- [ ] Pause quieto (sin Brake spam).
- [ ] L ejecuta la cola de Tfeeds (~profundidad 8), sin perder ráfagas.
- [ ] Sin Interrupt WDT / soft-PWM.
- [ ] Full OFF estable ~400 ms → reanuda lead (800 L / 2000 R) + DeReeler.
- [ ] Materialista+refill: Full **no** bloquea JOG.
- [ ] Boot: `SERVO: LEDC 50 Hz…` ; Full: `FULL CUT: LEDC→1500`.

---

## 9. Qué NO hacer (restricciones de proyecto)

- No “arreglar” TCP / WiFi / reconnect / heartbeat / Master por este síntoma.
- No volver a Detach periódico en reassert Full.
- No reintroducir soft-PWM bit-bang en ISR/`esp_timer` con `delayMicroseconds`.
- No usar FreeRTOS soft-PWM para el pulso crítico de 1500 µs (jitter → creep).
- No confundir logs M2 Tfeed con validación del servo RC.
- No interpretar “segunda vuelta lead tras Tfeed” como bug de HOME_HOLD.

---

## 10. Preguntas concretas para ChatGPT (revisor)

1. Dado RMT que ensucia LEDC en el mismo ESP32, ¿cuál es la forma más robusta de garantizar **1500 µs @ 50 Hz continuo** en GPIO26 sin soft bit-bang? (MCPWM, LEDC + `gpio_matrix_out` forzado post-RMT, pin dedicado, etc.)
2. Tras `StepperRMT::Brake()`, ¿hay secuencia mínima documentada para reclamar un pin LEDC sin ventana de failsafe RC?
3. Si el scope muestra 1500 estable y el eje aún gira: ¿calibración / deadband / modelo de servo?
4. ¿Conviene separar DeReeler y servo en pines/periféricos que no compartan mux, aunque el resto del diseño esté bien?
5. Revisa el diseño actual de `servoHardStopNeutral` + `forceStopDereelerAndServo` (LEDC) y señala regresiones o race conditions restantes.

---

## 11. Archivos / conversaciones de referencia

- Código: `PF/PF_LR/PF_LR.ino`, `PF/PF_LR/Config.h`, `PF/PF_LR/Types.h` (`AUTO_HOME_HOLD`, `AUTO_SERVO_LEAD`)
- Doc corto: `Doc/handoff_pf_servo_buffer_full.md`
- Trigger/cola: `Doc/pf_trigger.md` (si existe en repo)
- Chats Cursor: handoff Detach Buffer Full; sesión 66b144c9 (este hilo de análisis)

---

## 12. Resumen ejecutivo (una frase)

El PreFeeder **detecta Full y entra en HOME_HOLD correctamente**; el fallo es que el **PWM de neutro del RC continuo no queda estable en el pin** (conflicto LEDC↔RMT / failsafe ~2–3 s). Soft-PWM se descartó por WDT y creep. El árbol actual vuelve a **LEDC + cola Tfeed + Brake sin spam**; falta validar en banco que Full frene solo, sin Pause y sin creep.
