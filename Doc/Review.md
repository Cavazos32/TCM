# Review — Logs de ciclo y tiempo de ciclo (CT)

## Definición de CT (verificada en código)

| Segmento | ¿Cuenta en CT? | Ejemplos |
|----------|----------------|----------|
| Preparación | **No** | `prepareBeforeCut`, ASDA→0, PreFeeder In process ON, espera Buffer Full (Start y Resume) |
| Holgura pre-pieza | **No** | El ciclo no fuerza Tfeed; holgura la recupera el helper PF |
| Secuencia productiva | **Sí** | Holder ON (1ª) → Tfeed (piezas 2…N si `pfTriggerEnabled`; 1ª, C2 y OFF omiten) → feed → pinzas → lineal → corte → depósito → WIP/HOME → asentar |
| Holgura post-pieza | **No** | El ciclo no manda Tfeed extra; holgura la recupera el helper PF |
| Pause | **No** | excluido del reloj |
| Fin de lote | **No** | Finish tools, espera PF settled, In process OFF |

**Reloj:**
1. **Start** = tras Buffer Full confirmado (y holgura pre-pieza si aplica), justo antes de Holder/feed. **Resume** = misma espera Buffer Full (fuera de CT) antes de continuar.
2. **Freeze** = al entrar a `post_piece` (después de asentar/HOME) — **no espera Finish/settled**.
3. **Log** `Pieza OK · Xs` = solo si la pieza cierra bien (después de holgura post).
4. **CT lote** = wall 1ª→última freeze − Pause; N piezas en serie se suman.
5. UI (`elapsedSec`) usa el valor **congelado** durante Finish (no infla con settled).

Ejemplo lote 1 pz: wall Start→Idle puede ser ~10 s; **CT ≈ tiempo Holder…asentar**.

**Watchdog de pieza** (`pieceWatchTimeoutS`, default 12 s): corta E008/E009 si la pieza se atasca. La 1ª lleva feed (~5–6 s). El `prefetch_join` espera el feed de la **siguiente** y **no** usa este tope (sí `feedWaitTimeoutS`).

---

## Log esperado (checklist)

### Preparación (fuera de CT)

```
Cycle Start (0x040) …
prepareBeforeCut: cutters/grippers safe + Holder/Encoder cerrados
ASDA ya en 0 … / PreFeeder: In process ON …
PreFeeder: Buffer Full confirmado · L:Full   # o espera Buffer Full (Start) · L:buffer≠Full
BusyState
```

### Pieza (dentro de CT)

```
Holder+Encoder ON (inicio pieza 1)          # solo 1ª
Delay · Delay Holder ON: …
trigger PreFeeder: omitido (1ª pieza)       # Tfeed desde pieza 2; última sí manda
Feed: validar referencia láser lados=…      # live/caché
Feed omitido (referencia láser visible) …   # si ya ON al Start; si no:
Feed start lados=…
Feed OK lados=…
Pinzas ON (cierran)                         # ← antes faltaba
Delay · Delay tras cerrar pinzas: …
enc_set0: omitido …
Holder+Encoder OFF …
Delay · Delay Holder/Encoder OFF: …
Lineal MOVE TCP …
Lineal MOVE TCP OK …
Delay · Delay antes del corte: …
Holder+Encoder ON (cierran pre-corte)
Delay · Delay tras cerrar holder: …
Cortador ON (Set) …
Delay · Delay entre Set y Res cortador: …
Cortador OFF (Res) …
Delay · Delay post-corte: …
Depósito MOVE → …                           # ← CMD
Depósito MOVE ok → … (WIP start ref …)      # ← RX
Delay · Delay tras depósito: …
Pinzas OFF (abren)                          # ← antes faltaba
Delay · Delay tras abrir pinzas: …
Despeje MOVE → … (+|clearance|=…)           # ← CMD
Despeje MOVE ok → …; WIP fin ref=…          # ← RX
WIP Delivery (continuo/match-pieza): … (blower=Xs ≡ |L|)
WIP Delivery MOVE continuo → HOME=0.0 mm …
WIP Delivery blower ON @ HOME en vuelo hold=Xs (≡ |L|; PLC apaga)
WIP Delivery move ok @ HOME=0.0 mm (continuo)
Delay · Delay asentar: …
Pieza 1/1 OK · 7.0s
```

### Fin (fuera de CT)

```
Finish: Holder/Encoder cerrados; cutters/grippers OFF
PreFeeder: espera settled … → Idle
Lote completado — 1/1 piezas · CT=7.0s
```

> Nota WIP: con `WIP_BLOWER_CONTINUOUS=True` hay **un** soplo en vuelo: pinzas abiertas → MOVE→0 → delay corto → blower ON ≡ |L| (no el HOME de batch). PLC apaga. No stop–soplo–stop.

---

## Captura de referencia (antes del fix de logs/CT)

///////Preparacion///////// (NO CT)

[08:21:58.495] [MAQUINA] [INFO] Modo prueba en vacío OFF (bypass sensores/encoder)
[08:21:58.502] [MAQUINA] [ERROR] (0x040) Cycle Start (0x040) length=-265.0 mm qty=1
[08:21:58.502] [MAQUINA] [INFO] prepareBeforeCut: cutters/grippers safe + Holder/Encoder cerrados
[08:21:58.503] [MAQUINA] [CMD] ASDA ya en 0 (pos=0.18 mm) — sin MOVE_ZERO al Start
[08:21:58.503] [MAQUINA] [INFO] PreFeeder: In process ON (ciclo Busy)
[08:21:58.609] [ANDON] [INFO] BusyState

///////Inicio CT///////// S1
[08:21:58.756] [MAQUINA] [INFO] Delay · Delay Holder ON: 45 ms
[08:21:58.802] [MAQUINA] [CMD] Feed start lados=L
    # FALTABA: Feed OK + Pinzas ON (cierran)
[08:22:00.415] [MAQUINA] [INFO] Delay · Delay tras cerrar pinzas: 20 ms

///////Inicio///////// S2
[08:22:00.435] [MAQUINA] [INFO] enc_set0: omitido (lineal TCP no usa OM)
[08:22:00.436] [MAQUINA] [INFO] Holder+Encoder OFF (abren para lineal)
[08:22:00.436] [MAQUINA] [INFO] Delay · Delay Holder/Encoder OFF: 20 ms
[08:22:00.456] [MAQUINA] [INFO] Lineal MOVE TCP modelMm=-265.0 cutOffset→targetAbsMm=257.5 sides=L rpm=3000
[08:22:01.293] [MAQUINA] [RX] Lineal MOVE TCP OK targetAbsMm=257.5 rpm=3000
[08:22:01.293] [MAQUINA] [INFO] Delay · Delay antes del corte: 20 ms
[08:22:01.314] [MAQUINA] [INFO] Holder+Encoder ON (cierran pre-corte)
[08:22:01.314] [MAQUINA] [INFO] Delay · Delay tras cerrar holder: 45 ms
[08:22:01.360] [MAQUINA] [INFO] Cortador ON (Set) lados=L
[08:22:01.360] [MAQUINA] [INFO] Delay · Delay entre Set y Res cortador: 150 ms
[08:22:01.510] [MAQUINA] [INFO] Cortador OFF (Res) lados=L
[08:22:01.511] [MAQUINA] [INFO] Delay · Delay post-corte: 25 ms

///////WIP Delivery (1st STEP)/////////
[08:22:02.276] [MAQUINA] [CMD] WIP start ref=deposit_target 387.5 mm (post Idle/Reached)
    # FALTABA: Depósito MOVE → / ok (el move sí ocurrió entre post-corte y esta línea)
[08:22:02.276] [MAQUINA] [INFO] Delay · Delay tras depósito: 35 ms
[08:22:02.312] [MAQUINA] [RX] (0x4C) trigger PreFeeder Tfeed lados=L — R(0x4C)=omit L(0x51)=ok
    # FALTABA: Pinzas OFF (abren)

///////WIP Delivery (2ND STEP)/////////
[08:22:02.312] [MAQUINA] [INFO] Delay · Delay tras abrir pinzas: 50 ms
[08:22:02.834] [MAQUINA] [INFO] Despeje post-pinzas → 392.5 mm (+|clearance|=5.0); WIP fin ref=392.5 mm
[08:22:02.834] [MAQUINA] [INFO] WIP Delivery: fin(grippers)=392.5 → offset soplo=294.5 → inicio(cortador)=98.0 → HOME (0.25s c/u, offset=98.0)
[08:22:03.607] [MAQUINA] [RX] WIP Delivery move ok @ fin_offset=294.5 mm
[08:22:03.607] [MAQUINA] [INFO] WIP Delivery blower ON @ fin=294.5 mm hold=0.25s
[08:22:03.858] [MAQUINA] [RX] WIP Delivery blower OFF ok @ fin=294.5 mm
[08:22:04.612] [MAQUINA] [RX] WIP Delivery move ok @ inicio=98.0 mm
[08:22:04.613] [MAQUINA] [INFO] WIP Delivery blower ON @ inicio=98.0 mm hold=0.25s
[08:22:04.864] [MAQUINA] [RX] WIP Delivery blower OFF ok @ inicio=98.0 mm
    # FALTABA: WIP Delivery MOVE/ok @ HOME=0.0 mm (hueco ~0.67 s hasta asentar)
[08:22:05.532] [MAQUINA] [INFO] Delay · Delay asentar: 30 ms
[08:22:05.815] [MAQUINA] [RX] Pieza 1/1 OK
/////// Fin CT ≈ 7.06 s (58.756 → 05.815)

///////Final no CT
[08:22:05.816] [MAQUINA] [INFO] Finish: Holder/Encoder cerrados; cutters/grippers OFF
[08:22:05.816] [MAQUINA] [INFO] PreFeeder: espera settled antes de Idle · L:buffer≠Full
[08:22:08.636] [MAQUINA] [RX] PreFeeder: settled (Full+holgura/M2) → Idle ok · L:OK
[08:22:08.636] [MAQUINA] [INFO] PreFeeder: In process OFF → Idle
[08:22:08.636] [MAQUINA] [INFO] Lote completado — 1/1 piezas en 10s
    # ↑ 10s era wall Start→fin (incl. prep+settled). Tras fix: CT≈7.0s
[08:22:08.649] [ANDON] [INFO] FinishParts
[08:22:08.650] [ANDON] [INFO] IdleState
