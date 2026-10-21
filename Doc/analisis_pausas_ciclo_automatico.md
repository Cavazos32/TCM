# Análisis — Pausas largas entre pasos del ciclo automático

Fecha: 2026-10-21

## Alcance y reglas revisadas

No existe `Rules.md` en la raíz. Se revisaron `.cursor/rules/*.mdc` y
`Doc/normas_arquitectura.md`, en particular M5 / Communication Core protegido:
no tocar TCP, WiFi, reconnect, heartbeat, timeouts ni el orden del `loop()`
sin causalidad demostrada, y clasificar el fallo (A enlace / B comando /
C ejecución) antes de atribuirlo a comunicación.

Sin cambios de código. No se usan logs como evidencia: todo sale del código.
Se separan **hechos** de **hipótesis**.

Nota de contexto: conviven `PF/` y `PF_V2/`. Las rutas normativas de las
reglas apuntan a `PF/`; se cita esa y se contrasta con `PF_V2/` donde difiere.
Dato relevante: **ninguna de las dos** implementa las tareas
`masterRxTask`/`masterTxTask` ni `communicationTask` que describen las reglas
— ver §3.

---

## 1. Mapa paso → módulo → condición para avanzar

Secuencia de `_run_pieces` en `HMI/cycle.py` (`FLOW_STEPS`, líneas 129–180):

| Paso | Ejecuta (HMI/cycle.py) | Módulo | Condición para avanzar |
|---|---|---|---|
| Prep. | `_prepare_before_cut`, `_run_lot` | PLC + Motion + PF Master | Herramientas a seguro, Holder+Encoder ON con ACK del PLC, ASDA en 0, PF Start + In process, Buffer Full **con status fresco** (`pf_status_seq` mayor que al pedirlo) |
| 1–2 Sujetar | `_arm_holder_encoder` | PLC | `wait_plc_ack` de Holder **y** Encoder (ACK llega tras el pulso); si no, E066 |
| 3 Pedir material | `_do_pf_trigger` | PF Master → L/R | Solo que el envío TCP salga; **no** espera fin de Tfeed |
| 4 Cargar material | `_run_feed` → `_wait_feed` | Motion (feed CAN) | Evento LengthOK en **todos** los lados pedidos (`wait_feed_length_ok_for`); NG cierra con E002/E003 |
| 5 Ajuste | — | Motion (interno) | Sin espera en HMI: el offset `offL/offR` va dentro del LengthOK |
| 6–8 Pinzas / ref. | `cmd_plc_gripper(True)` | PLC | Solo envío + espera configurada; `enc_set0` se omite a propósito |
| 9–10 Liberar | `_open_holder_encoder` | PLC | ACK de apertura de Holder **y** Encoder antes del lineal |
| 11 Avance lineal | `_run_lineal_abs` → `_wait_motion` | Motion | Reached/Idle **y** posición ASDA en destino ±3 mm; Idle fuera de destino 1 s → E015 |
| 13–14 Pre-corte | `_arm_holder_encoder` | PLC | Espera + drenaje calculado de pulsos + ACK de ambos cierres |
| 15–18 Corte | `cmd_plc_cutters` Set/Res | PLC | Pulso, Res, y **asiento**: mínimo 700 ms desde el Set antes de mover ASDA (`_ensure_cutter_settled_before_travel`) |
| 19 Depósito | `cmd_motion_move_mm` → `_wait_motion` | Motion | Reached en destino ±3 mm |
| 21–22 Abrir pinzas | `cmd_plc_gripper(False)` | PLC | ACK de apertura antes del despeje |
| 23 Despeje | MOVE corto → `_wait_motion` | Motion | Reached ±3 mm |
| 24 Vuelta a inicio | `_home_with_wip_delivery_*` | Motion + PLC (soplador) | Reached en 0 (MOVE_ZERO es un MOVE más, no homing por torque) |
| 25 Preparar siguiente | `_run_feed_after_home` | Motion + PF | ASDA confirmado en 0 (caché; si no, otro MOVE_ZERO), Tfeed de la siguiente y LengthOK → handoff |
| 26–27 Cierre | — | — | Sin condición de módulo; contabilidad de pieza y prompts si aplica |

Hecho estructural: entre los pasos 5 y 24 el ciclo **solo habla con PLC y
Motion**. El PreFeeder no interviene en esa ventana salvo el chequeo tras
Resume (`_ensure_pf_buffer_full_after_resume`, que retorna inmediato si no
hay Resume pendiente).

---

## 2. Bloqueos posibles (hechos del código)

### Afectan a muchos pasos

#### A. PLC — pulsos síncronos dentro del procesamiento TCP (determinista, cada pieza)

`PLC/PLC.ino`: `tcpRxDrain()` procesa cada línea en línea, y `plcTcpDoByte()`
llama a `valveSetPulse()` **antes** de enviar el ACK. `valvePulsePin()` ocupa
el ESP con `valvePulseWait()` (100 ms, `VALVE_PULSE_MS` en `PLC/Config.h`)
más `valvePulseGapWait()` (hasta 50 ms). Holder y Encoder son **dos comandos
separados** desde HMI (`state.py: cmd_plc_holder` / `cmd_plc_encoder`), así
que se serializan: ~150 ms por válvula, ~300 ms el par.

En el ciclo eso se traduce en esperas reales más allá de las configuradas:
en `holder_off` la espera configurada es 60 ms y luego `wait_plc_ack` espera
el resto (~240 ms) antes del lineal; en pre-corte hay drenaje explícito de
`2 × 150 − holder_on_ms` ms (`_plc_precut_drain_remain_ms`) y luego ACK; tras
el corte, `_ensure_cutter_settled_before_travel` impone 700 ms desde el Set
(ya transcurridos pulso+post, quedan ~400 ms). Suma del orden de
**0,8–1 s por pieza** en esperas de código no configurables, repartidas entre
pasos. Acotado: si el ACK no llega en 2 s (`PLC_ACK_TIMEOUT_S`) es E066, no
una pausa silenciosa más larga.

#### B. Motion — finalización por sondeo Modbus bloqueante (cada MOVE)

`Motion/Motion.ino: pollMotionOnce()` + `transact()` (con `Motion/Asda.h`):
cada sondeo es una transacción Modbus síncrona en el `loop()`:
`delay(MODBUS_PRE_TX_GAP_MS)` de 10 ms y espera de respuesta hasta
`RESPONSE_TIMEOUT_MS` = 300 ms a 9600 baudios. El sondeo está espaciado por
`MOTION_POLL_MIN_MS` = 40 ms, lee `P5.007`, y al detectar fin hace **otra**
transacción (`read32(P5.016)`) antes de poner `asdaTcpNotifyReached = true`.
El `Reached` se publica recién en `asdaTcpPollEvents()`, en la siguiente
pasada del `loop()`. Orden del `loop()`: `motionPrepPollOnce()` →
`pollMotionOnce()` → encoder → `serviceCANRx()` → `feedLoop()`. Consecuencia:
cada MOVE (lineal, depósito, despeje, HOME: 4+ por pieza) cierra con una
latencia de sondeo, y una transacción lenta retrasa también `feedLoop()` y
la publicación del evento. La preparación de cada MOVE
(`motionPrepPollOnce`, pasos MOVE) es otra cadena de transacciones, una por
pasada de `loop()`. `waitPrComplete()` (con `delay(40)`) existe pero solo lo
usan handlers HTTP: **no** está en la ruta TCP del ciclo.

#### C. Motion — lecturas CAN síncronas en `feedLoop()` (pasos 4 y 25, con arrastre al resto)

`Motion/Servo_Feed.cpp`: `canReadSdoI32()` tiene presupuesto de 150 ms
(`CAN_SDO_TIMEOUT_MS`, 3 intentos) y `canReadStatusWord()` 80–600 ms según
contexto (`FeederCan.h`). `feedLoop()` atiende L y R **en serie** en la misma
pasada (OM live, freeze al láser, `feedSideService` por lado), así que un
nodo que no responde multiplica el bloqueo por lectura y por lado, y esa
pasada retrasa el siguiente `pollMotionOnce()` y la publicación de LengthOK
vía `motionTcpPollFeedEvents()`.

### Solo una rama específica

#### D. Motion — recuperación CAN síncrona en `feedLoop()` (~5 s, rama normalmente inhibida)

`reconnectCANBus(true)` / `setupServoFeeder()` encadenan `delay()` que suman
≈4,8 s (300+500+500 de NMT, ~950 ms por nodo CiA402 ×2, home, etc.) más
`verifyServoEnergized()` con dos lecturas de hasta 600 ms. **Hecho clave**:
la rama está inhibida si hubo TCP HMI en los últimos 60 s (`tcpHoldoff`), si
hay feed activo o si ASDA está ocupado, y solo se reintenta cada
`FEED_CAN_RETRY_MS` = 15 s. En ciclo normal con enlace HMI vivo, esta rama
**no debería activarse**; no se da por activa sin evidencia.

#### E. PreFeeder Master — reconexión L/R síncrona en un `loop()` único (solo puertas PF)

`PF/PreFeeder_Master/PreFeeder_Master.ino`: el `loop()` es único (HTTP ×12
pasadas, `masterServiceTcpHmi()`, `pfServiceSide(L)`, HTTP,
`pfServiceSide(R)`, HTTP). `pfTryConnect()` hace `client.stop()`,
`delay(20)` y `client.connect(ip, PF_PORT, CONNECT_TIMEOUT_MS)` = **hasta
2 000 ms bloqueando todo el Master** por lado caído, con backoff de 2,5 s ×
fallos (máx. 12 s) entre intentos. Mientras dura, no se drena el TCP de HMI
ni se reenvía al otro lado. Afecta solo a lo que el ciclo pide al PF:
Start/In process y la espera de Buffer Full
(`_wait_pf_buffer_full_on_start`, que exige status fresco cada 100 ms). El
trigger de los pasos 3/25 no espera respuesta del Master: el envío HMI
retorna al salir por el socket. `PF_V2/PreFeeder_Master` es equivalente
(mismas funciones, líneas +6/+8).

#### F. PreFeeder L/R — comandos peer ejecutados en línea en el `loop()` (solo transiciones)

`PF/PF_LR/PF_LR.ino`: `loop()` único (HTTP 12+8+4 pasadas, sensores,
`peerService()`, AUTO/servo); solo el feeder M2 tiene tarea propia
(`motor2FeederTask`). `peerRx()` → `peerDoCmd()` se ejecuta **dentro** del
`loop()`: `trigger` solo levanta un flag para la tarea M2 (barato), pero
`start` / `setInProcess` / `halt` / `stop` pueden encadenar
`resumeAutoFromSensors()` / `forceStopDereelerAndServo()` →
`motorStartSigned()` / `motorHardStop()` → `motorWaitStopped()` con tope de
200 ms (con `server.handleClient()` + `delay(1)`) y varios
`delay(MOTOR_DIR_SETUP_MS)` de 15 ms (`PF_LR/Config.h`). Eso retrasa
`peerTxEvents()`/`peerPushNow()` y por tanto la foto que el Master entrega a
HMI. Solo ocurre en Start/In process/Stop, no entre pasos de pieza.
`saveSettings()` (NVS) solo lo disparan comandos de configuración, no los
del ciclo. En `PF_V2/PF_LR`, `lidarPoll()` está acotado a 64 bytes por
pasada: no es un bloqueo relevante.

#### G. HMI — ramas de transporte (solo si el enlace cae)

`state.py: _cmd_motion_move_with_recovery()`: en el camino feliz solo espía
el ACK 20 ms (`MOTION_MOVE_ACK_PEEK_S`). Si el enlace está caído:
`_wait_motion_link` hasta 5 s por intento, 3 intentos, con `reconnect()` de
`tcp_link.py` (connect 2 s + verificación 3 s) en el hilo del ciclo. Es clase
A (enlace), no el camino normal. Los sondeos internos del ciclo están
acotados y son finos: `wait_plc_ack` 10 ms, `_wait_motion` 100 ms,
`_wait_feed` rodajas de 200/50 ms, Buffer Full 100 ms.

---

## 3. Hallazgo más probable

**Hipótesis principal (pausas sub-segundo entre casi todos los pasos): la
cadena A.** Es la única que ocurre **siempre**, sin necesidad de ningún
fallo: cada transición de válvula paga su pulso serie de ~150 ms dentro del
handler TCP del PLC, el ciclo espera el ACK más allá de la espera
configurada, y el post-corte añade el asiento fijo hasta 700 ms desde el Set.
Evidencia: `valvePulsePin/valvePulseWait` y ACK posterior al pulso en
`plcTcpDoByte`; `wait_plc_ack(..., PLC_ACK_TIMEOUT_S)` tras esperas más
cortas en `holder_off`/pre-corte; `_plc_precut_drain_remain_ms` y
`CUTTER_SETTLE_BEFORE_DEPOSIT_MS` en `cycle.py`.

**Hipótesis condicional (pausas de segundos, concentradas en pasos de
movimiento): B.** Si el ASDA tarda en responder por Modbus, cada sondeo puede
ocupar hasta ~300 ms el `loop()` y el `Reached` solo nace al final de esa
cadena (más la lectura de posición de cierre); con 4+ MOVE por pieza el
efecto se multiplica y además frena `feedLoop()`. Que esta rama esté activa
es **hipótesis**: el código la permite, pero sin logs no hay evidencia de
respuestas Modbus lentas, y una transacción fallida aborta la preparación en
vez de pausar.

Descartadas como explicación de pausas *entre pasos de pieza*: E y F (el
ciclo no habla con el PreFeeder entre los pasos 5–24, y el trigger no espera
confirmación), D (inhibida con TCP HMI reciente), G (solo con enlace caído).

---

## 4. Corrección mínima recomendada y riesgos

**No tocar el Communication Core.** Nada de lo anterior justifica modificar
TCP/WiFi/reconnect/heartbeat/timeouts de `tcp_link.py`, Motion, Master o L/R
(clases B/C según M5, no A demostrada).

Corrección mínima candidata, en dominio HMI y de un solo punto: en el
pre-corte, el ciclo ya espera `wait_plc_ack` de Holder y Encoder, y según
`plcTcpDoByte()` ese ACK se envía **después** de completar cada pulso; el
drenaje fijo previo (`_plc_precut_drain_remain_ms`, hasta ~180 ms) duplica
esa misma garantía. Sustituir el drenaje por la espera de ACK existente (que
ya está) es el recorte de menor alcance. Todo lo demás (asiento de corte de
700 ms, ACK antes del lineal/despeje) tiene justificación física documentada
en el propio código —KEEP que debe asentarse antes de mover el ASDA— y no
debería recortarse sin validación en máquina.

```text
COMMUNICATION IMPACT
Componente protegido: ninguno (cambio confinado a HMI/cycle.py, secuencia)
Cambio propuesto: eliminar el drenaje fijo pre-corte y fiar el avance al wait_plc_ack ya existente
Relación causal: el ACK del PLC se emite tras el pulso (plcTcpDoByte), misma condición que el drenaje calcula por tiempo
Riesgo: si un ACK llegara sin pulso real (p. ej. estado lógico ya coincidente en el PLC), el cortador podría recibir el Set con el ESP aún ocupado → corte desplazado al extra, pieza larga (riesgo ya descrito en cycle.py:100-113)
Alternativa sin tocar comunicación: ninguna necesaria; no se toca PLC ni protocolo
```

Si las pausas observadas son de **segundos en pasos de movimiento** y no
sub-segundo en válvulas, esta corrección no aplica: antes de tocar código
habría que confirmar la rama B con la instrumentación que el ciclo ya tiene
(`_begin_op/_end_op` por operación y los avisos de "cmd lento"/"ACK lento"
existentes), y la causa probable estaría en la respuesta Modbus del drive
(dominio Motion/ejecución), no en el transporte TCP.
