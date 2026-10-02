# Trigger PreFeeder (Tfeed)

El **trigger** es el Tfeed al PreFeeder (`0x4C` derecha / `0x51` izquierda). No es el Feed de Motion ni el *In process ON*.

Va **después del corte** (paso 18: post-corte, antes del depósito). Así el PreFeeder rellena el buffer mientras depósito / despeje / HOME. El 21 solo abre pinzas. El feed Motion de la siguiente es el paso 25 (tras HOME, ASDA=0).

**Interruptor de ciclo** (`pfTriggerEnabled` en `cycle_config.json`, UI Cycle → Material handling): **ON** (default) manda Tfeed tras el corte de las piezas 2…N. **OFF** omite siempre el paso 18. No es `Tfeed (s) = 0` en el HTML de L/R: ese valor se clampa a 0.1 s y el feeder igual arranca.

**Regla de lote:** la 1ª pieza ya trae feed de referencia (el operador lo dejó, o el Tfeed de la última pieza del lote anterior). Por eso la 1ª **omite**. Las demás, incluida la última, **mandan** Tfeed si el interruptor está ON.

**Arquitectura (2026-10):** TFEED es independiente de AUTO / In process / Holgura (Holgura eliminada del producto). El esclavo encola el trigger (cola hasta 8) y `feederTask` lo ejecuta si safety OK (`systemFault`, Buffer Max, enlace). No depende de `autoEnabled` ni `sensorsMotionArmed`.

Si el lado **ya está en un Tfeed TCP**: el nuevo queda pendiente y arranca al terminar. Así L y R no quedan asimétricos.

Sí se rechaza con falla activa, sin enlace Master↔esclavo, Buffer Max en el instante del comando, o cola llena.

**Buffer Max breve** tras haber encolado: no borra el pendiente; al bajar Max arranca. Falla / Materialista refill manual sí cancelan el pendiente.

**Buffer Full** corta Servo+DeReeler (AUTO); **no** aborta un TFEED en curso.

---

## Trigger de ciclo (paso 18, post-corte)

| Acción | Resultado | Por qué |
|---|---|---|
| 1ª pieza del lote (tras el corte) | **Omite** | Ya trae feed de referencia |
| `pfTriggerEnabled` = OFF | **Omite** | Operador desactiva Tfeed de ciclo |
| Piezas intermedias | **Trigger** a `feedSides` | Reponer para la siguiente |
| Última pieza del lote | **Trigger** a `feedSides` | Referencia para el lote siguiente |
| C2 Resume **antes** del Tfeed | **Trigger** | Corte de este intento aún no mandó Tfeed |
| C2 Resume **después** de Tfeed | **Omite** | Ya mandado |
| C2 / reinicio de la **1ª** pieza | **Omite** | Regla de 1ª |
| C3 misma pieza post-paso 18 | **Omite** | No re-manda |
| C3 Resume → pieza siguiente | **Trigger** | Flujo normal de esa pieza |
| Pause / Resume normal | **Omite** | No re-manda Tfeed |
| C1, Stop o aborto | **Omite** | |
| PreFeeder sin enlace | **Omite** | |
| Lado no en `feedSides` | **Omite** ese lado | |
| Refill / purga | **Omite** | |
| Botones manuales Trigger L/R | **Trigger** | Fuera del ciclo |

El **timing y la cantidad** de envíos los define `HMI/cycle.py` (paso 18). No cambiarlos al refactorizar firmware.

---

## Lo que no es trigger

| Evento | Notas |
|--------|--------|
| Start + In process ON | Arma AUTO / sensores. Espera Buffer Full. No es Tfeed. |
| Resume máquina | Rearma In process + espera Buffer Full. No es Tfeed. |
| Fin de lote | Settled = Buffer Full + M2 idle → In process OFF. Sin Tfeed extra. |
