# Análisis de arquitectura — Refactor PreFeeder / TCP / Holgura / TFEED / Servo

**Fecha:** 2026-10-02  
**Repo:** TCM (`Cavazos32/TCM`)  
**Alcance:** solo análisis. **No se implementa código en este documento.**  
**Fuentes:** `PF/PF_LR/`, `PF/PreFeeder_Master/`, `HMI/cycle.py`, `HMI/tcp_link.py`, `HMI/state.py`, `Doc/normas_arquitectura.md`, `Doc/pf_trigger.md`, `.cursor/rules/tcm-*.mdc`.

> No existe `Rules.md` en el repo. La norma vive en `Doc/normas_arquitectura.md` + `.cursor/rules/`.

---

## Aclaración de roles (tres capas)

Hoy **no** hay un solo “Master” con la lógica de ciclo:

| Capa | Rol real |
|------|----------|
| **HMI** (`cycle.py` + `state.py` + `tcp_link.py`) | Lógica de ciclo / máquina; cliente TCP al PreFeeder Master |
| **PreFeeder Master** (`PreFeeder_Master.ino`) | Bridge HMI↔L/R: TCP HMI `:8768`, 2 sockets a L/R `:8765`, HTTP, agregación estado/errores |
| **PF_LR** (L y R) | Actuadores: AUTO (Servo+DeReeler), TFEED (Feeder M2), sensores, peer TCP |

```text
HMI Python                 PreFeeder Master ESP32              PF-L / PF-R
─────────────              ──────────────────────              ──────────
ModuleTcpClient ──TCP──►   :8768 pfTcpServer                   peer :8765
                           pfClientL ──TCP──►                  .101:8765
                           pfClientR ──TCP──►                  .102:8765

Motion :8767 · PLC :8766 · Andon :8769 (otros ModuleTcpClient)
```

HMI **no** abre sockets a L/R. Orquestación L/R = Master.

El diagrama “Cycle → TX queue → TCP TX → PF” encaja en **HMI** (ciclo ya separado) o como colas internas del Master. Separar RX/TX FreeRTOS en el ESP Master **toca Communication Core (M5)**; requiere actualizar norma + COMMUNICATION IMPACT antes de implementar.

---

## 1. Tareas actuales

### PF_LR (cada esclavo L/R)

| Tarea | Core | Creación |
|-------|------|----------|
| `loop()` Arduino | Núcleo 1 (default ESP32 Arduino) | runtime |
| `motor2HolguraTask` (`"m2_holgura"`) | `M2_TRIGGER_CORE` = **1** | `xTaskCreatePinnedToCore` en `setup()` |

Ambos en core 1 → no hay paralelismo real de núcleos; sí dos contextos que se turnan. El aislamiento real es **quién llama al driver** (Feeder solo en la tarea M2; DeReeler/Servo solo en `loop`).

Sin otros `xTaskCreate*`, sin soft-PWM/`esp_timer` activos (abandonados). Servo = LEDC HW @ 50 Hz.

### PreFeeder Master

| Tarea | Notas |
|-------|--------|
| Solo `loop()` | **Sin** FreeRTOS de aplicación |

Orden actual:

```text
HTTP → WiFi → TCP HMI → lado L → HTTP → lado R → status → HTTP
```

Hay `delay(20)` / `delay(50)` en reconnect/WiFi (sensibles a latencia M5).

### HMI

| “Tarea” | Notas |
|---------|--------|
| Thread `_rx_loop` + `_bg_loop` de `ModuleTcpClient` | Communication Core HMI |
| Thread `CycleRunner` | Ciclo; **no** es TCP |

---

## 2. Qué hace cada tarea

### PF_LR — `loop()`

- HTTP local  
- Buffer Full / Max (corte DeReeler + Servo)  
- Filtros tensión; monitores de fallo (Max, cilindro, manguera, buffer refill, **holgura timeout**, tensión)  
- Peer TCP Master↔esclavo (`peerEnsureServices` / `peerService`: accept, RX, TX status/events)  
- `serviceServoPwm`, `serviceAuto` (lead / CW / CCW / HOME_HOLD)  
- Refill Materialista (DeReeler+Servo; Feeder vía flag hacia la otra tarea)

### PF_LR — `motor2HolguraTask` (nombre engañoso)

Dueño del **Feeder M2**, no solo Holgura:

1. `updateHolguraFilter()`  
2. Abort M2 si `motor2AbortRequested`  
3. `updateHolguraHelper()` (puede arrancar timed feed helper)  
4. `serviceTimedFeeder()` si hay Tfeed/helper en curso  
5. Refill feeder Materialista  
6. Guards: `systemFault`, Buffer Max (corta feeder, **conserva cola** Tfeed)  
7. Arranque Tfeed desde cola si armado + Auto  
8. `serviceHolguraFeeder()` (casi solo “asegurar idle si no hay timed feed”)  
9. `vTaskDelay(5 ms)`

**No** mueve DeReeler/Servo (RMT no thread-safe entre escritores).

### Master — `loop()`

- Cliente HMI: `masterServiceTcpHmi` / `pfTcpRxDrain` / despacho opcodes  
- Clientes L/R: `pfServiceSide` (connect, RX JSON, keepalive, watch)  
- Forward de comandos (`pfDispatchCmd`)  
- Agregación status / EXXX hacia HMI  

**No hay ciclo de producción en Master.**

### HMI — Cycle + TCP

- Cycle decide **cuándo** Start / In process / Trigger / Pause Idle  
- TCP solo transporta  

---

## 3. Qué hace actualmente TCP RX

### Master ← HMI (`:8768`)

- `pfTcpRxDrain` → `pfTcpOnLine`  
- Recibe JSON / bytes de comando: Start/Stop/Reset/InProcess/TriggerL|R/Materialist/refill…  
- En el **mismo hilo** despacha a L/R y a veces responde ACK/estado  

### Master ← L/R

- `pfRx` dentro de `pfServiceSide`  
- Parsea status/events → `SideView` (home, endstop, holgura, triggerActive, faults…)  
- Empuja errores/estado a HMI  

### PF_LR ← Master (`:8765`)

- `peerService` / `peerOnLine` → `peerDoCmd`  
- Comandos: `trigger`, `setInProcess`, `start`/`stop`/`reset`, refill, settings, idleMode, `ping`…  
- RX + despacho en el mismo `loop()`  
- **Sin ACK TCP de comando** (fire-and-forget). El “OK” lo da Master/HMI al forward; el esclavo reporta por `event`/`status`.

### HMI ← Master

- `_rx_loop` de `ModuleTcpClient` → callbacks → caché en `state.py`  

**Hoy RX no es una tarea aislada:** leer + interpretar + (en Master) reenviar viven juntos.

---

## 4. Qué hace actualmente TCP TX

### Master → HMI

- `pfTcpTx` / `pfTcpTxState` / `pfTcpTxEvent` / status JSON / ACK  

### Master → L/R

- JSON `type:command` en `WiFiClient` L/R + keepalive ping  

### PF_LR → Master

- `peerTx` / `peerTxEvents` / status periódico (`PEER_STATUS_MS` / fast si timed feed)  

### HMI → Master

- `ModuleTcpClient.send` síncrono bajo `_io_lock` (desde el hilo del ciclo/UI)  

**Hoy TX tampoco es tarea dedicada:** es “print en el momento del evento”.

---

## 5. Qué debe moverse a TCP RX (propuesta)

Solo **transporte + parseo + encolar eventos**, sin ciclo ni AUTO:

| Origen | Eventos a encolar |
|--------|-------------------|
| HMI ← Master | estado máquina PF, EXXX L/R, status L/R (Full, Max, triggerActive, inProcess…) |
| Master ← L/R | mismos campos por lado |
| PF_LR ← Master | `CmdReceived{cmd,id,val}` → cola de comandos de aplicación |

**No** en RX: esperar Buffer Full, decidir Tfeed de pieza, `serviceAuto`, helper Holgura.

---

## 6. Qué debe moverse a TCP TX (propuesta)

Solo **drenar cola de salida**:

| Emisor | Mensajes |
|--------|----------|
| Cycle/HMI → cola → Master | Start, Stop, Reset, In process, TriggerL/R (**timing igual** que hoy en `cycle.py`) |
| Master → cola → L/R | forward + keepalive |
| PF_LR → cola → Master | status/events (sin generar política) |

Compatible **si** hay cola y el productor es Cycle / dispatcher de aplicación, no el parser.

**Incompatibilidad actual:** Master mezcla RX→dispatch→TX en un solo `loop` sin colas. FreeRTOS RX/TX ahí choca con M5 hasta reescribir norma.

**Mejor ancla del diagrama “Cycle → TX queue”:** HMI (ya tiene ciclo separado). Master como bridge con colas internas es opcional/secundario.

---

## 7. Holgura — qué puede eliminarse

### Exclusivo Holgura (candidato a borrar)

| Área | Símbolos / piezas |
|------|-------------------|
| Sensor / filtro | GPIO22, `holguraActive`, `updateHolguraFilter`, `holguraFilterReset`, `holguraStableActive`, `motor2BufferActiveHigh` |
| Helper | `updateHolguraHelper`, params `holguraHelper*`, `M2_FEED_HOLGURA` |
| Falla | `updateHolguraFaultMonitor`, `holguraFault*`, `FAULT_HOLGURA_TIMEOUT`, `AUTO_HOLGURA_FAULT` |
| Opcodes | `PF_ERR_HOLGURA_L/R` (0x38/0x32), E057/E063 |
| UI/API | HTML L/R helper+fault, Master `SENS` holgura, cmds `setHolgura*` en `master_cmds.h` |
| HMI espejo | `pf_holgura_present`, settled “Full+holgura”, labels frontend |
| Docs | Holgura en `Doc/pf_trigger.md`, R0b E057/E063, `Doc/Review.md` |

`serviceHolguraFeeder()` casi no alimenta: limpia fase residual si no hay timed feed.

### Reutilizable (no es Holgura)

- Cola Tfeed depth 8, dedup IDs, `motor2StartTimedFeed` / `serviceTimedFeeder`  
- `M2_PHASE_TIMED_FEED` + `M2_FEED_TCP`  
- Abort M2, Buffer Max sobre feeder, refill feeder  
- La **tarea** completa, renombrada p.ej. `feederTask` / `motor2FeederTask`

---

## 8. Holgura — dependencias a resolver

| Dependencia | Hoy | Tras quitar Holgura |
|-------------|-----|---------------------|
| `fillUntilReady` | Cierra con **Full + holgura + M2 idle** | Redefinir: ¿solo Full (+ M2 idle)? |
| `sensorsMotionArmed` | `!idle && (inProcess \|\| fillUntilReady)` — **compartido** AUTO/fallas/arranque Tfeed | Conservar para AUTO; **sacar** del gate de TFEED |
| Tfeed vs helper | Holgura gana; Tfeed espera | Solo cola Tfeed / safety |
| Fin de lote `cycle.py` | `_wait_pf_settled_before_idle` = Full+holgura+M2 | Sin holgura: ¿solo Full+M2 idle? (**no es timing de trigger**, pero sí cambio de ciclo) |
| EXXX E057/E063 | Catálogo + Excel + HTML | Actualizar norma M4 o retirar del producto |
| `pfTriggerEnabled=OFF` | Doc: “solo helper holgura” | ¿significa “sin Tfeed y sin helper”? |
| Nombre tarea / UI Index | “dos núcleos / Holgura” | Renombrar |

**Comportamiento sin dueño si se borra Holgura sin sustituir:** recuperación automática de slack entre piezas (hoy helper). El diseño nuevo asume que bastan **Tfeed + AUTO por Buffer Full**.

**Nota:** no cambiar *cuándo* `cycle.py` manda trigger; quitar Holgura **sí** obliga a tocar settled/UI/errores — distinto del paso 18.

---

## 9. Todos los puntos que controlan el Servo (GPIO26)

### Escritura física PWM (único camino real)

`servoRemuxWriteUs` / `servoWriteUsForced` / `ledcWrite` / `ledcAttach|Detach` / `digitalWrite` en setup → vía:

- `servoHardStopNeutral`  
- `servoAssertNeutral` / `servoStop`  
- `servoAssertRun`  
- `setupRotationServo`  
- `servoServiceBufferFullCut`  
- `beginAutoCwWithServoLead` (`servoWriteUsForced` directo)

**RC continuo:** STOP = mantener PWM neutro ~**1500 µs**. Detach ≠ freno (failsafe ≈ última marcha ~2–3 s).

### Orquestadores (quién *pide* marcha/neutro)

| Origen | Mecanismo |
|--------|-----------|
| Buffer Full | `loop` → `servoServiceBufferFullCut` + `forceStopDereelerAndServo` |
| AUTO FSM | `serviceAuto` → lead/CW/CCW/HOME_HOLD → `syncServoToAutoState` / `servoAssertRun` |
| Sync periódico | `serviceServoPwm` / `syncServoToAutoState` |
| Materialista refill | `refillServoOn` vía sync/asserts |
| Pause / In process OFF | `setInProcess` → `forceStop` + sync |
| Start / fillUntilReady / fault | varios `forceStop` + sync |
| Tension / `motorRun` | DeReeler + a menudo sync servo |
| RMT side-effect | `servoMarkPinDirty` tras Stop/Brake/setSpeed → remux en hard-stop |

### Contraste Full vs Pause vs Tfeed

| Evento | DeReeler/Servo | Tfeed en curso | Nuevos Tfeed |
|--------|----------------|----------------|--------------|
| Buffer Full HIGH | Stop inmediato (neutro 1500) | **Continúa** | Puede encolar; ejecutar si armado |
| In process OFF | Stop | **Continúa** | Encola; no arranca hasta ON |
| Buffer Max | Stop + EXXX | Abort; cola conservada | NACK |
| `stop` / fault | Stop todo | Abort | NACK |

### Dueño lógico propuesto

Un solo módulo `AutoMaterial` (Servo+DeReeler):

- API: `requestRun()` / `requestStopNeutral()`  
- Único escritor PWM: `servoApply(us)`  
- Buffer Full = fuente de verdad de STOP  
- Refill Materialista = otro modo del mismo dueño  
- TFEED **nunca** escribe servo  

---

## 10. Arquitectura propuesta

```text
HMI CycleRunner  (sin cambio de timing de trigger)
    │  encola cmds
    ▼
HMI TCP TX  ──►  PreFeeder Master (bridge)
                      │ TX queue L / R
                      ▼
                   PF_LR peer RX  → CmdQueue
                      │
         ┌────────────┼────────────┐
         ▼            ▼            ▼
   AutoMaterial    FeederM2     Safety/Faults
   (Servo+Dereel)  (TFEED only) (Max, EXXX…)
         ▲
         └── Buffer Full → STOP neutro 1500 + Brake DeReeler

PF_LR status/events → Master RX → HMI RX → Cycle (observación)
```

### PF_LR — dos dominios

1. **`AutoMaterial`** (hoy en `loop`): Servo+DeReeler; Full = stop; In process arma ventana AUTO (no TFEED).  
2. **`feederTask`** (ex-`motor2HolguraTask`): solo cola TFEED + timed feed.

### TFEED — gates deseados vs hoy

| Condición | Hoy al aceptar (encolar) | Hoy al arrancar | Deseado |
|-----------|--------------------------|-----------------|---------|
| `systemFault` | bloquea | bloquea | bloquea |
| Buffer Max | bloquea | corta feeder | bloquea |
| `peerLinkOk` | bloquea | conserva cola si blip | política enlace |
| `autoEnabled` | bloquea | bloquea | **quitar** |
| `sensorsMotionArmed` / In process | no bloquea enqueue | bloquea **arranque** | **quitar del arranque** |
| Holgura helper | puede retrasar | — | **eliminar** |

Conservar: cola 8, dedup id, momento en que `cycle.py` envía `0x4C`/`0x51`.

### Master RX/TX

- Preferible: colas drenadas desde el mismo `loop` (menos riesgo M5)  
- FreeRTOS RX/TX **solo tras** actualizar `normas_arquitectura` M5 + impact check  
- Cycle **no** entra en esas tareas  

### Compatibilidad del diagrama con el código

| Idea | ¿Compatible hoy? |
|------|------------------|
| Cycle fuera de TCP | **Sí** en HMI |
| TX queue Cycle→PF | **Sí** a nivel HMI; Master hoy es sync forward |
| RX queue → Machine Logic | **Sí** conceptualmente; hoy callbacks directos |
| FreeRTOS RX/TX en Master | **No sin** reescritura M5 + colas |
| Holgura out + TFEED independiente | **Sí** en PF_LR con trabajo grande; HMI settled/errores acoplados |
| Un solo dueño Servo | **Sí** — arreglo estructural del bug Buffer Full |

---

## 11. Riesgos / regresiones

1. **M5 Communication Core** — tareas TCP en Master/HMI sin causalidad de enlace = regresión prohibida por norma.  
2. **Servo RC** — multi-escritor + RMT dirty; unificar dueño mal → creep/failsafe otra vez.  
3. **Quitar Holgura** — slack entre piezas sin helper; E057/E063 fantasma si no se alinea M4.  
4. **TFEED sin In process** — feeder puede mover con AUTO idle; definir política en Pause/Error (hoy Tfeed en curso puede seguir).  
5. **`fillUntilReady` / settled** — Start/fin de lote pueden quedar “listos” demasiado pronto o nunca.  
6. **Misma core loop+M2** — refactor de tareas no arregla solo LEDC↔RMT.  
7. **L/R asimetría** — gates nuevos deben ser idénticos.  
8. **Docs/UI/Excel** — `pf_trigger.md`, Index, Master HTML, i18n, Error list.

---

## 12. Archivos a modificar posteriormente

### PF esclavo
- `PF/PF_LR/PF_LR.ino`  
- `PF/PF_LR/Config.h`, `Types.h`, `Status_Mode.h`, `Index.h`  

### Master
- `PF/PreFeeder_Master/PreFeeder_Master.ino`  
- `master_cmds.h`, `master_html.h` / `index.html`  
- `PF/PfStates.h` (opcodes Holgura si salen del producto)  

### HMI (sin tocar timing/cantidad de trigger)
- `HMI/state.py`, `HMI/prefeeder.py`, `HMI/error_catalog.py`  
- `HMI/cycle.py` **solo** settled/post-pieza Holgura — **no** paso 18 timing  
- Frontend mappers/types/i18n Holgura  

### Norma / doc (obligatorio si cambia producto o TCP)
- `Doc/normas_arquitectura.md` (R0b, M4; M5 si hay RX/TX tasks)  
- `.cursor/rules/tcm-communication.mdc` / `tcm-arquitectura.mdc`  
- `Doc/pf_trigger.md`, `Doc/Review.md`  
- Excel Error list / GPIO  

### Fuera de alcance de esta refactor
- Timing/cantidad de trigger en `cycle.py`  
- Protocolo de opcodes de enlace (salvo retirar Holgura EXXX con norma)  
- CAN, tiempos de ciclo Motion  

---

## Prioridad de implementación sugerida (cuando se autorice código)

1. Dueño único AUTO Servo+DeReeler + Full → STOP neutro 1500  
2. `feederTask` solo TFEED con gates de seguridad (sin Holgura / sin armado AUTO)  
3. Borrar Holgura + alinear norma / HMI settled / Excel  
4. Colas RX/TX — primero encima de HMI o Master con impacto M5 documentado  

---

## Resumen ejecutivo

El PreFeeder mezcla en un solo firmware tres dominios (AUTO material, TFEED, Holgura) y el Master mezcla RX/TX/dispatch en un `loop` cooperativo. La simplificación pedida es viable: **eliminar Holgura**, **separar Feeder de AUTO**, **un solo dueño del Servo**, y tratar TCP como colas RX/TX **sin meter el ciclo dentro**. El split FreeRTOS en el ESP Master es el cambio más caro normativamente (M5); el mayor valor de producto está en PF_LR (Servo/Full + TFEED limpio + Holgura out).
