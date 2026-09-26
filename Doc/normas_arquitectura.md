# TCM — Normas de arquitectura

Documento **normativo** del proyecto. Define las reglas que rigen diseño, comunicación entre módulos, estados, errores, documentación y cambios de código.

No es un catálogo (opcodes, EXXX, GPIO). Esos viven en:

| Fuente | Contenido |
|--------|-----------|
| `Doc/TCM - D.xlsx` | Opcodes, Error list, arquitectura de referencia |
| `Doc/gpio_list_updated.md` | Pines GPIO por módulo |
| Headers `*States.h` / HMI | Enums y bytes en firmware / Main |

Si algo no encaja aquí, **se actualiza este documento antes** de improvisar en código.

---

## 1. Arquitectura general

### Módulos

El sistema es modular: **Motion**, **PLC**, **PreFeeder**, **Andon**, orquestados por **HMI / Main** (ciclo + UI).

- Cada esclavo ejecuta su dominio (mover, válvulas, buffer, torre).
- **Main** decide el lote, la política ante fallos y qué estado de máquina ven los demás.
- Un módulo **no** necesita el detalle interno de otro: solo comandos y estados que le afecten.

### Regla A1 — Responsabilidad acotada

Cada módulo solo conoce lo necesario para su función.  
Ejemplo: PreFeeder no recibe “el ASDA no llegó”; recibe Stop / ErrorState / Idle según lo que Main publique.

### Regla A2 — Main interpreta la política; el EXXX es identidad compartida

La **política de recuperación** (latch EXXX, Reset validado, Start/Resume) la aplica **solo HMI / Main**.  
El **código EXXX** es la identidad del fallo del módulo: el esclavo lo genera y Main lo muestra.  
Entre esclavos no se reenvía el EXXX; se alinean por **estado**.  
C1/C2/C3 **no** seleccionan el flujo de recuperación.

---

## 2. Comunicación: flip-flop, no sondeo

### Regla C1 — Evento Set / Res

Estados, errores y condiciones relevantes **no** se preguntan en bucle (“¿sigues en error?”).  
Se trata como **flip-flop**:

| Señal | Significado |
|-------|-------------|
| **Set** | Un evento activa la condición (sensor, timeout, comando fallido, enlace caído, cambio de estado). |
| **Res** | Limpia la condición: **Reset HMI** explícito **después** de validar que la causa ya no está activa. |

Mientras no haya **Set**, no hay acción obligatoria. No inventar polling de “por si acaso”.

**Reset HMI (Res):** envía Reset al módulo fuente, lee la condición actual (caché actualizada / sensores del EXXX) y **solo entonces** libera el latch. Si la causa sigue activa, el EXXX y el estado ERROR se conservan. Start y Resume quedan bloqueados.  
No hay Res observacional: recuperar el enlace o ver Idle en el esclavo **no** borra el latch. E06x y E068 también exigen Reset explícito.

**Excepción de enlace:** el heartbeat TCP (`ping`/`pong` o keepalive) solo verifica que el socket vive.  
No es sondeo de sensores ni de estado de aplicación. Los sensores/válvulas se publican por **evento** (cambio) o status push del esclavo.

### Regla C2 — Opcode = contrato

Todo comando, estado o error de detalle que cruce TCP lleva **opcode** acordado en el Excel / Opcodes.  
No se inventan bytes sueltos en un `.ino` sin fila en la fuente de verdad.

### Regla C3 — PLC: válvulas por impulso (KEEP Set/Res)

En el esclavo PLC, cada comando de válvula (`on` / `off`) genera **un pulso** en el GPIO; la salida **no** queda enclavada en el ESP.  
El enclavado lo hace el PLC neumático (KEEP: Set / Res).  
- Querer **ON** (Set) → un pulso (solo si el estado lógico cambia a on).  
- Querer **OFF** (Res) → otro pulso (solo si cambia a off).  
- **Boot / All Off** = pulsos OFF de cada válvula lógica ON (holder incluido). El holder lo activa rutina o manual. Solo bajo demanda (UI All Off / ciclo / **Home de máquina**), **no** desde Stop, Pause ni Reset HMI.  
- **Reset PLC (`0x1E`):** desde control PLC **o** desde **Reset HMI** si el EXXX es del PLC. Tras Reset, el esclavo deja estados lógicos en OFF; HMI refleja OFF (no inventa All Off previo ni pulsos extra). Pause / Error de máquina **no** mandan Reset PLC.  
- Ancho de pulso: `VALVE_PULSE_MS` (100 ms) para que el KEEP lea bien.  
No pulsar si el estado lógico ya coincide (un pulso de más invertiría el KEEP).  
El estado lógico (JSON/status/UI) refleja la posición pretendida; el pin físico solo es impulso.  
HMI/ciclo siguen enviando `on: true|false` como hasta ahora.

**Límite máquina ↔ PLC:** error general de máquina, Stop y Pause **no** mandan válvulas KEEP.  
**Reset HMI** manda **Reset PLC (`0x1E`)** solo si el EXXX latcheado es del PLC (UI refleja OFF).  
**Home de máquina** (control de máquina): ASDA → posición 0 + encoders Set0 L/R + **All Off** neumática.  
Operar PLC manual = pulso ON/OFF, All Off, o Reset PLC.

**Excepción Blower:** no es KEEP. El pin queda **ON (nivel)** durante `durationSec` (ajustable en UI PLC; el esclavo debe respetar ese valor, no un default fijo si viene en el comando) y luego **OFF** automático.  
Pause y Error de máquina pueden mandar **solo Blower OFF** (cancela el timer). No es All Off ni pulso KEEP de otras válvulas.

---

## 3. Estados

### Qué es un estado

Opcode de condición del módulo o de la máquina: Init, Idle, Busy, Error, Stop, Return (y en máquina: Start, Reset, Finish, Materialist, …).

Informa **qué está pasando** sin explicar el detalle del fallo.

### Regla E1 — Estado ≠ EXXX

ErrorState (`0x0C` / `0x27` / `0x3C` / `0x46`, etc.) es **estado**, no entrada del catálogo EXXX.  
La UI no muestra “E###: módulo en falla genérica” cuando existe (o debió existir) el detalle concreto.

### Regla E2 — Propagación por estado

Cuando Main debe alinear al resto: publica estado de máquina (p. ej. Error `0x46`, Stop) y/o comandos Stop/Reset de módulo.  
No reenvía el EXXX a todos los esclavos.

**Pause / Error → PreFeeder Idle.** Al publicar Pause (`0x48`) o Error (`0x46`), Main desarma el PreFeeder (`In process OFF` → Idle). No usa Stop `0x2B` (enclava PF-007). Resume / Busy / Continuar ciclo rearma In process y **espera Buffer Full** (igual que Start) antes de seguir. C1 y Stop de operador sí mandan Stop `0x2B`.

---

## 4. Errores

### Clasificación

Los errores de detalle se clasifican en **C1**, **C2** y **C3**. La clase define la reacción de Main y la **única** secuencia de salida válida.

| Clase | Al Set (activar) | Salida |
|-------|------------------|--------|
| **C1** | Stop inmediato Motion/PF (+ ciclo); **no** PLC/válvulas | Secuencia C1 fija |
| **C2** | Pausar; **no** lanzar el siguiente step; **no** PLC | Secuencia C2 fija |
| **C3** | En lote: terminar la **pieza en curso** (incl. corte) de forma segura, luego Pause; fuera de lote: terminar el paso en curso; **no** PLC | Secuencia C3 fija |

### Forma en UI

**`EXXX: Module, Descripción`**

Misma cadena (o al menos el mismo **EXXX** + descripción oficial) en:

- HMI / Main  
- HTML local del módulo (p. ej. IP del PreFeeder, Motion, PLC)

### Regla R0 — Un EXXX, un nombre, todas las pantallas

Cada módulo es dueño de **sus** errores EXXX (los de su dominio en el Excel).  
Si ocurre un fallo en PreFeeder y abres la IP/HTML de ese módulo, debes ver **el mismo EXXX** (y la misma descripción de catálogo) que verías en Main — no un alias, slug legacy, ni otro texto inventado (“Buffer Full” vs `E052: Pre-Feeder, Buffer sin relleno`).

- Prohibido: dos nombres distintos para el mismo opcode/fallo (uno en web local, otro en HMI).  
- Permitido: en la web local mostrar solo los EXXX **de ese módulo**; Main muestra cualquier EXXX que reciba.  
- El HTML local no inventa catálogo paralelo: usa el mismo código/descripción que Error list.
- Firmware local debe exponer en JSON al menos `exxx` + `ui` (`EXXX: Module, Descripción`) para que la web del módulo no reinvente textos.

### Regla R1 — Fuente de verdad del catálogo

Errores oficiales = hoja **Error list** (+ Opcodes) en `Doc/TCM - D.xlsx`.  
Deben tener opcode, enum, handler y clase. Secuencia de códigos según el Excel.

### Regla R2 — Altas y bajas

- Alta: siguiente EXXX + byte asignado en Excel → enum en el módulo → catálogo HMI **y** UI local del módulo si tiene HTML.  
- Baja: marcado **Eliminar** → no implementar / quitar del código (Main y HTML local).  
- No crear EXXX solo para “estado genérico”.

### Regla R3 — Alcance del detalle en red

Detalle EXXX por TCP → **solo Main/HMI** (no se reenvía a otros esclavos).  
En la **propia** web del módulo sí se muestra su EXXX activo (misma identidad).  
Resto de módulos en red → estados / stop / reset.

### Regla R4 — Salida fija (no variable)

Salir de un error **solo** con la secuencia unificada.  
No hay caminos distintos por clase C1/C2/C3, por EXXX (salvo E050 documentado) ni por atajo de operador.  
Cambiar un paso u orden = cambiar **este documento** primero.

#### Secuencia unificada

```text
ERROR → latch EXXX → mostrar EXXX → detener secuencia
  → módulos no afectados conservan su estado
  → PF In process OFF
  → esperar RESET
  → Reset PF 0x2C (si hay enlace) + Reset módulo fuente + validar condición real
       ├─ sigue activa → conservar EXXX + ERROR (Start/Resume bloqueados)
       └─ desapareció → liberar latch
            ├─ lote activo → Pause → esperar RESUME
            └─ sin lote → Idle → esperar START
```

1. UI muestra `EXXX: Module, Descripción` y queda enclavado.  
2. Se detiene la secuencia. **No** se pinta ERROR en todos los esclavos.  
3. Pause / Error máquina → PreFeeder Idle (`In process OFF`). No es Stop `0x2B`.  
4. Corregir causa física si aplica.  
5. **Reset HMI:** Reset del módulo fuente → validar condición. **Siempre** Reset PF `0x2C` si hay enlace (igual que Start siempre manda `0x2A`). Si sigue, no se libera el latch. **No Home. No Start automático.** Reset PLC `0x1E` solo si el EXXX latcheado es del PLC.  
6. **Sin lote:** Idle. Esperar Start.  
7. **Con lote activo:** Pause. **Resume** (solo si el latch ya no está) → Start/Init PF → Buffer Full → terminar la pieza → review OK → purga (refill existente; no alimentar sola: espera Retry o Long feed; no mover ASDA si ya está en park) → Continuar ciclo → Buffer Full (igual que Start/Resume) → siguiente pieza.

**Home** es comando explícito del operador. Reset jamás llama Home.

Prohibido: Resume o Start con latch activo; borrar el EXXX sin validar; Home automático desde Reset; Stop automático del lote (solo el botón); tocar válvulas KEEP desde Pause/Error; añadir pasos de recuperación a FLOW_STEPS.

#### Excepción E050 (lote/pieza activo)

E050 (Encoder / aire, manguera o cilindro). **Fuera de lote** (y durante refill/purga suelta): secuencia unificada (Reset validado → Idle/Start).

Con **pieza/lote en curso** (no refill): Pause. HMI pregunta si requiere Materialist.

1. **No Materialist** → recuperación unificada del lote (Reset → Resume → pieza → review → purga).  
2. **Sí Materialist** → se libera solo el latch E050 para entrar a Materialist; HOME de esa ruta; esperar Materialist OFF; continuar lote.

**Purga / Refill con E050:** el operador puede vaciar manguera **con el latch E050 aún activo**. No libera el latch. Start y Resume siguen bloqueados.  
La purga no alimenta sola: tras park espera Retry (55 mm) o Long feed (100 mm).  
Si el lote está en Pause por E050, Purge es comando de operador: suelta el hilo del lote (mismo efecto que Stop) y corre la purga suelta. No es Stop automático por el EXXX.

Stop del lote solo si el operador pulsa Stop **o** Purge (caso E050 anterior). No añadir pasos a FLOW_STEPS.

---

### Feed / láser (Motion) — aceptación y post-corrección

El láser es el **tope de feed** (L y R): no alimentar más allá de su referencia. ON → **FEED_OK**, sin otro movimiento (p. ej. OM 53 + láser ON → ya está). OM > **58 mm** → ya pasó → LengthNG. OM ≤ 0 → NG. Láser OFF tras approach → **LASER_SEEK** hasta flanco ON / E004/E005. No hay corrección ciega a 55 mm: ese movimiento pasaba la referencia si el halt llegaba tarde.

**Start y tras error (HMI):** antes de mandar Feed (1ª pieza al Start, misma pieza tras error, o siguiente tras Continuar ciclo), Main **valida la referencia** (GET `/api/status` una vez; si falla, caché TCP). Si el láser de todos los `feedSides` está ON, **omite el feed**. Feed ya hecho post-HOME (handoff) no revalida. No es sondeo de ciclo.

**Feed entre piezas:** no hay prefetch en paralelo con depósito/despeje/HOME. Tras HOME, Main confirma ASDA en 0 (caché Reached, ±0.5 mm; si no, MOVE_ZERO) y entonces Feed de la siguiente. Tfeed sigue en el paso 3. Última pieza: sin feed post-HOME.

Si el láser está **OFF al iniciar** el feed (hunt / 1ª carga): no hay approach rápido a 44–55 mm. **LASER_SEEK** avanza a trozos cortos (~1.5 mm, ~25 mm/s) hasta flanco ON. Al ON: CW Halt + **congelar destino = posición actual** (el Halt solo no cancela el perfil largo). GPIO crudo, sin debounce HMI de 150 ms. Tras halt → **FEED_OK** sin ventana PHYS OM (50–58).

Láser ya ON al iniciar (cuerpo de manguera / remanente): approach normal, sin halt por nivel (evitar parar en 0). Al terminar, si sigue ON, no hay seek. Purga (`skipValidate`) no usa el tope láser. El offset de comando (`offL`/`offR`) aplica en purga, no infla el approach de ciclo.

Si el seek no ve el láser a tiempo → E004/E005. El seek puede sacar el encoder de rango; eso no es LengthNG.

**E028** solo si el encoder **live** no incrementó tras el feed. Settle tardío con live ya en ~55 (p. ej. halt por láser) no es E028: se usa la lectura live oficial.

### Feed — modo Velocity + Sensor (experimental, Motion)

Modo **opcional** para comparar contra Position + Sensor. **No cambia producción:** el default NVS es Position + Sensor (`FEED_MODE_STEPS_SENSOR`). Purga/refill (`skipValidate`) sigue siempre en Profile Position.

Jerarquía en Velocity + Sensor:

| Señal | Rol |
|-------|-----|
| LR-X GPIO crudo (`feedLaserMaterialPresentRaw`) | Tope físico. ON → **primer** frame CAN = CW Halt (`0x010F`, HaltOpt=2 / rampa `0x6085` ya primada). Sin SDO ni lectura OM antes del Halt. Sin avance posterior. |
| OM | Estimación de progreso (transición fast→slow) y watchdog de sobrepaso. **No** es destino. Se lee **después** del Halt. |
| Encoder del servo | Realimentación CiA402 Profile Velocity (`0x6060=3`, `0x60FF`). |

Láser **OFF al start** (remanente post-corte / 1ª carga): hunt a `velocitySlowPct`, no FAST. El flanco suele llegar a pocos mm; FAST a 100 % se pasaba la referencia. L y R: un Halt de cada lado en el mismo tick **antes** de burst / `0x60FF=0`. `TVel=0` solo después del Halt (si va antes, el servo entra en rampa `0x6084` y se pasa).

No se detiene en OM = 55 mm ni se corrige después del flanco LR-X. Watchdogs: OM ≥ `velocityMaxTravelMm` (E004/E005), timeout (E031), OM sin incremento (E028). En **cualquier** watchdog o fallo de Velocity + Sensor: Halt + `0x60FF=0` y esperar velocidad real ~0 (`0x606C`) **antes** de notificar EXXX y restaurar Profile Position (`0x6060=1`). No cambiar a PP con el servo aún en movimiento.

## 5. Andon / torre

### Regla T1

Andon refleja **estado de máquina** (`0x40`–`0x49`), no el listado EXXX.  
Un sensor propio de Andon (p. ej. presión) puede reportar EXXX a Main; la torre sigue lo que Main mande como estado.  
El fallo de presión (`0x50` / E064) **no** bloquea RX de `0x40`–`0x49` ni comandos manuales de debug.

Mapeo torre (estado máquina → luces / buzzer):

| Byte | Estado | Torre |
|------|--------|-------|
| `0x40` | Init | Green |
| `0x41` | Start | N/A (no cambia) |
| `0x42` | Stop | Red |
| `0x43` | Reset | N/A |
| `0x44` | Idle | Green |
| `0x45` | Busy | Green (máquina trabajando) |
| `0x46` | Error | Red + Buzzer (**prioridad** sobre estados normales) |
| `0x47` | FinishParts | Secuencia temporal R→Y→G + Buzzer (no permanente; luego Idle/Green) |
| `0x48` | Pause | Yellow (sin buzzer; distinto de Materialist) |
| `0x49` | Materialist | Yellow + Buzzer |

Estados mutuamente coherentes: al aplicar un byte la torre apaga el resto de salidas y deja solo la combinación definida. Buzzer solo en Error, Finish (durante la secuencia) y Materialist.

**Mute buzzer:** preferencia de HMI (Configuración → Debug). Se envía a Andon por TCP; no cambia el color de torre.

**Enlace:** Andon TCP `:8769` (no compartir puerto con PreFeeder `:8768`). HMI arranca el cliente Andon por defecto (`ANDON_ENABLE=0` lo apaga).

---

## 6. Documentación y GPIO

### Regla D1

Pines y dirección I/O: `Doc/gpio_list_updated.md` (y Excel I-O si aplica).  
No hardcodear pines “porque sí” sin alinear la doc.

### Regla D2

Opcodes y errores: Excel primero; código después.  
Docs derivados (`errores_por_modulo.md`, etc.) no sustituyen la norma ni el Excel.

---

## 7. Código y mantenimiento

### Regla M1 — Dónde se define el contrato

Opcodes/enums de cada módulo en su `*States.h` (o equivalente HMI).  
Latch EXXX, Reset validado y recuperación de lote en Main/HMI.

### Regla M2 — Sin ruido de depuración

No agregar mensajes de debug (logs verbosos, spam, prints de sondeo) salvo pedido explícito.  
No dejar comentarios de debug / restos de prueba en archivos salvo que se requiera.

### Regla M3 — Cambios normativos

Todo cambio de arquitectura (comunicación, clases, secuencias de salida, alcance de detalle) se refleja **aquí** primero.  
Un PR de error nuevo: Excel → enum/opcode → HMI → mismo EXXX en HTML local. Sin eso, no es oficial.

### Regla M4 — Norma ↔ archivos alineados

Cada norma vigente debe reflejarse en los **archivos necesarios**, no solo en este documento.

| Tipo de norma | Debe quedar alineado en |
|---------------|-------------------------|
| Contrato opcode / estado / EXXX | Excel (`Doc/TCM - D.xlsx`) + `*States.h` / HMI + HTML local del módulo si aplica |
| Latch EXXX y Set/Res | HMI/Main (`error_policy`, ciclo, catálogo) |
| GPIO / I-O | `Doc/gpio_list_updated.md` (+ Excel I-O) y firmware del módulo |
| Comportamiento de esclavo | `.ino` / headers del módulo afectado |
| Communication Core (M5) | `.cursor/rules/tcm-communication.mdc` + resumen en `.cursor/rules/tcm-arquitectura.mdc` |
| Resumen para el agente | `.cursor/rules/tcm-arquitectura.mdc` (si cambia una prohibición dura) |

Prohibido: documentar una norma nueva o cambiada y dejar código, Excel o UI local en el comportamiento anterior.  
Al cerrar un cambio normativo: listar qué archivos se actualizaron para cumplirla.

### Regla M5 — Communication Core protegido

La comunicación que ya funciona es un **subsistema protegido**. No es intocable: solo se modifica si existe **relación causal demostrada** con el problema a resolver.

**Problema a evitar:** fallo de rutina / Motion / PreFeeder / PLC → corrección de dominio → regresión accidental de TCP/WiFi/reconnect/heartbeat/loop.

Coherente con **M3** (norma primero) y **M4** (alinear archivos). Resumen operativo para el agente: `.cursor/rules/tcm-communication.mdc`.

#### M5.1 — Componentes protegidos (implementación real)

| Ámbito | Archivos / piezas |
|--------|-------------------|
| HMI TCP | `HMI/tcp_link.py`, `ModuleTcpClient` (vía `HMI/state.py`): threads RX (`_rx_loop`) y reconnect/heartbeat (`_bg_loop`); locks `_io_lock` / `_conn_lock` / `_connect_gate`; `_session` y callbacks diferidos; `connect` / `reconnect` / `disconnect` / `_drop_link`; heartbeat; timeouts (`RX_TIMEOUT_SEC`, `RX_JOIN_TIMEOUT_SEC`, etc.) |
| Motion | `Motion/Motion.ino`: `serviceWifi()`, `wifiKickConnect()`, `serviceAsdaTcp()`, `asdaTcpEnsureServices()`, `asdaTcpStopServices()`, `asdaTcpAcceptIncoming()`, `asdaTcpRxDrain()`, `asdaTcpPollEvents()`, protocolo ASDA TCP, orden de `loop()` |
| PreFeeder Master | `PF/PreFeeder_Master/PreFeeder_Master.ino`: `masterServiceWiFi()`, `masterServiceTcpHmi()`, `pfTcpEnsureServices()`, `pfTcpStopServices()`, `pfTcpAcceptIncoming()`, `pfTcpRxDrain()`, `pfServiceSide` / L–R (`pfTryConnect`, `pfWatchLink`, `pfKeepalive`), protocolo PF |

**No** convertir los waits de `HMI/cycle.py` (`CycleRunner`) en polling TCP ni mover la rutina al hilo de comunicación.

#### M5.2 — Loop / ciclo de servicio (zona crítica)

En `Motion/Motion.ino` y `PF/PreFeeder_Master/PreFeeder_Master.ino`, toda función llamada desde `loop()` es sensible a latencia.

Patrón: **service → trabajo corto → regresar → siguiente service**.  
Prohibido en el camino del `loop()`: `delay` / `while` / `for` que esperen condición externa; esperas de movimiento, sensor, TCP, WiFi, Modbus largo, reconnect bloqueante o retry prolongado que impidan re-entrar a los demás services.

WiFi Motion ya es no bloqueante (`serviceWifi` / `wifiKickConnect`); no revertir a `while` de conexión ni `delay` en reconexión.

#### M5.3 — Operaciones no bloqueantes (Motion)

Preservar preparación incremental: `motionPrepPollOnce()` + `motionPrepKind` / `motionPrepStep` / `motionJobActive`.  
No reemplazar por esperas del tipo `waitUntilMotionFinished()` dentro del `loop()`. Nuevas operaciones de movimiento, encoder o feeder = pasos/estados no bloqueantes.

`server.handleClient()` comparte el mismo `loop()` que `serviceAsdaTcp()` y el resto: handlers HTTP de larga duración afectan TCP/WiFi/ciclo. Considerarlos en análisis de latencia; no introducir handlers nuevos con esperas físicas largas.

#### M5.4 — Diferenciar comunicación de ejecución

| Clase | Criterio | Actuar sobre |
|-------|----------|--------------|
| **A. Link failure** | Socket realmente caído | TCP, WiFi, reconnect, heartbeat, lifecycle |
| **B. Transport OK / command failure** | Comando llegó; no se procesó bien | opcode, parser, handler, estado, aceptación |
| **C. Command OK / execution failure** | Comando procesado; mecanismo falló | dominio Motion/PLC/PF, encoder, sensores, rutina |

En **B** y **C** no modificar el Communication Core solo por el síntoma. Cadena: ¿salió? → ¿llegó? → ¿recibido? → ¿interpretado? → ¿lógica? → ¿actuador?

#### M5.5 — Communication Impact Check

Antes de tocar archivos de tarea de máquina, responder: ¿TCP? ¿WiFi? ¿reconnect? ¿heartbeat? ¿timeout? ¿sockets? ¿protocolo? ¿orden del `loop()`? ¿función desde `loop()`? ¿bloqueante? ¿retrasa `serviceAsdaTcp` / `serviceWifi` / `serviceCANRx` / `feedLoop` / TCP PF?

Si la tarea no es de comunicación y alguna respuesta es sí: **no tocar** la zona protegida; buscar solución en el dominio. Si hay causalidad real, documentar antes:

```text
COMMUNICATION IMPACT
Componente protegido: …
Cambio propuesto: …
Relación causal: …
Riesgo: …
Alternativa sin tocar comunicación: …
```

#### M5.6 — Sin refactors “de paso”

Prohibido aprovechar correcciones de rutina, Motion, PreFeeder, PLC, encoder, feeder, estados, errores o producción para “limpiar/modernizar/simplificar” arquitectura TCP, WiFi, reconnect, heartbeat, threading, socket lifecycle, protocolos, Core/task o timing sin necesidad causal.

---

## 8. Resumen

| Tema | Norma corta |
|------|-------------|
| Quién manda el lote / política | Main / HMI |
| Cómo se avisa sin detalle | Opcode de estado |
| Cómo se dice qué falló | EXXX (mismo en Main y HTML local del módulo) |
| Quién aplica la recuperación | Solo Main |
| Cómo se activa/limpia | Flip-flop Set / Res (Reset HMI validado; no observacional) |
| PLC válvulas | Pulso ON/OFF / All Off / Reset PLC; Reset HMI de EXXX PLC → Reset PLC; Home máquina → All Off; no desde Stop/Pause |
| Cómo se sale de un fallo | Secuencia unificada (Reset valida → Idle o Resume); E050+lote: Pause → preguntar Materialist; E050: Purge permitido con latch activo |
| Pause / Error máquina | PreFeeder Idle (`In process OFF`); Resume/Busy/Continuar ciclo rearma y espera Buffer Full (como Start); Stop operador → Stop `0x2B` |
| Start / Reset HMI → PF | Start siempre manda `0x2A`; Reset HMI siempre manda `0x2C` (si hay enlace). Reset PLC `0x1E` solo si el EXXX es del PLC |
| De dónde salen códigos | Excel + GPIO doc |
| Debug | Solo si se pide |
| Norma nueva/cambiada | Reflejar en todos los archivos necesarios (M4) |
| Communication Core | Protegido (M5): no tocar sin causalidad; loop no bloqueante; impacto documentado |

Si una decisión no cabe en esta tabla, se actualiza **este documento** antes que el código.
