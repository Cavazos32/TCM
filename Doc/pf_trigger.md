# Trigger PreFeeder (Tfeed)

El **trigger** es el Tfeed al PreFeeder (`0x4C` derecha / `0x51` izquierda). No es el Feed de Motion ni el *In process ON*.

Va **antes de la alimentación CAN de Motion** (paso 3: Tfeed → feed). Así el PreFeeder empuja material al buffer cuando el servo CAN empieza a tirar. En piezas 2…N el feed físico suele ocurrir en el paso `feed_after_home` de la pieza anterior; ahí también se manda Tfeed justo antes del `_run_feed`, y el paso 3 de la pieza siguiente hace handoff (omite re-mandar).

**Interruptor de ciclo** (`pfTriggerEnabled` en `cycle_config.json`, UI Cycle → Material handling): **ON** (default) manda Tfeed antes del feed CAN de las piezas 2…N. **OFF** omite siempre el paso 3. No es `Tfeed (s) = 0` en el HTML de L/R: ese valor se clampa a 0.1 s y el feeder igual arranca.

**Regla de lote:** la 1ª pieza ya trae feed de referencia (el operador lo dejó, o material del lote anterior). Por eso la 1ª **omite**. Las demás **mandan** Tfeed si el interruptor está ON y hay alimentación Motion real (no handoff / skip por láser).

**Arquitectura (2026-10):** TFEED es independiente de AUTO / In process / Holgura (Holgura eliminada del producto). El esclavo encola el trigger (cola hasta 8) y `feederTask` lo ejecuta si safety OK (`systemFault`, Buffer Max, enlace). No depende de `autoEnabled` ni `sensorsMotionArmed`.

Si el lado **ya está en un Tfeed TCP**: el nuevo queda pendiente y arranca al terminar. Así L y R no quedan asimétricos.

Sí se rechaza con falla activa, sin enlace Master↔esclavo, Buffer Max en el instante del comando, o cola llena.

**Buffer Max breve** tras haber encolado: no borra el pendiente; al bajar Max arranca. Falla / Materialista refill manual sí cancelan el pendiente.

**Buffer Full** corta Servo+DeReeler (AUTO); **no** aborta un TFEED en curso.

**Pause (In process OFF):** aborta DeReeler, servo **y M2 timed feed** al instante. El esclavo latcha `tfeedIncomplete` + `tfeedResumeSec` (segundos restantes). Al **Resume**, `cycle.py` re-manda trigger **solo** en lados incompletos, con duración parcial (`value` en `0x4C`/`0x51`). Tfeed ya completado → sin retry. **Stop `0x2B`** no latcha ni retry. Fin de lote / Materialista sin cambio.

---

## Trigger de ciclo (paso 3, pre-feed CAN)

| Acción | Resultado | Por qué |
|---|---|---|
| 1ª pieza del lote (antes del feed) | **Omite** | Ya trae feed de referencia |
| `pfTriggerEnabled` = OFF | **Omite** | Operador desactiva Tfeed de ciclo |
| Piezas intermedias (feed real) | **Trigger** a `feedSides` | Reponer al tirar CAN |
| Feed post-HOME (pieza N → N+1) | **Trigger** con `rep=N+1` | Mismo instante que el feed CAN |
| Handoff (paso 3 tras feed post-HOME) | **Omite** | Ya mandado (prearmed) |
| Sin alimentación Motion (láser ON / skip) | **Omite** | No hay tirón CAN |
| Última pieza del lote | **Omite** al final | Sin feed post-HOME |
| C2 Resume **antes** del Tfeed | **Trigger** | Este intento aún no mandó Tfeed |
| C2 Resume **después** de Tfeed | **Omite** | Ya mandado |
| C2 / reinicio de la **1ª** pieza | **Omite** | Regla de 1ª |
| C3 misma pieza post-paso 3 | **Omite** | No re-manda |
| C3 Resume → pieza siguiente | **Trigger** | Flujo normal de esa pieza |
| Pause / Resume normal | **Retry parcial** si Tfeed abortado | Paso 3 omite; Resume completa solo lo pendiente |
| C1, Stop o aborto | **Omite** | |
| PreFeeder sin enlace | **Omite** | |
| Lado no en `feedSides` | **Omite** ese lado | |
| Refill / purga | **Omite** | |
| Botones manuales Trigger L/R | **Trigger** | Fuera del ciclo |

El **timing y la cantidad** de envíos los define `HMI/cycle.py` (paso 3 / feed post-HOME). No cambiarlos al refactorizar firmware.

---

## Lo que no es trigger

| Evento | Notas |
|--------|--------|
| Start + In process ON | Arma AUTO / sensores. Espera Buffer Full. No es Tfeed. |
| Resume máquina | Rearma In process + espera Buffer Full. No es Tfeed. |
| Fin de lote | Settled = Buffer Full + M2 idle → In process OFF. Sin Tfeed extra. |
