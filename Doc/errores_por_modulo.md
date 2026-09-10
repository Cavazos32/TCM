# Catálogo de errores por módulo (TCM)

Fuente de verdad: headers `*States.h` / `Status_Mode.h` / `machine_states.py`.  
Ref. opcodes: [opcode_reference_list.md](opcode_reference_list.md).

**Flujo:** esclavo reporta **error detalle** → HMI decide estado máquina → Andon recibe solo el byte de máquina (torre). Andon **no** interpreta el detalle de PLC/Motion/PF.

| Tipo | Qué es |
|------|--------|
| **Detalle** | Byte específico del fallo (sensor, timeout, NG, etc.) |
| **Estado Error** | Byte genérico del módulo (hay fallo activo) |
| **Reset** | Comando para limpiar faltas |

### Enum ≠ función (no son lo mismo)

| Concepto | Qué es | Ejemplo |
|----------|--------|---------|
| **Byte / opcode** | Valor en el cable TCP | `0x1F` |
| **Enum** | Nombre C/Python del **código** (constante) | `PLC_ERR_CUTTER` |
| **Función** | Rutina que **ejecuta** o **emite** ese byte | `CutterE()` |

El enum **identifica** el mensaje. La función **hace** el trabajo (detectar, TX, aplicar torre, etc.). Pueden existir el uno sin el otro a medias (enum definido, función stub / TX pendiente).

En tablas de la Parte A:
- columna **Enum** = código
- columna **Función** = handler / rutina asociada (no el enum)

### Estructura de este doc

| Parte | Contenido |
|-------|-----------|
| **A (§1–5)** | Ya tienen **opcode** (o tag lógico PF) — oficial, no mover |
| **B (§6)** | **Por clasificar** — sin opcode de detalle; pendientes |

Columna **Comentarios** (parte A): matices de cableado HMI/TX. Vacío = OK documentado y usable.

### Leyenda — ¿de dónde viene? (capa)

| Etiqueta | Quién lo genera | Archivos típicos |
|----------|-----------------|------------------|
| **Motion (ESP)** | Firmware Motion | `Motion/Motion.ino`, `Servo_Feed.cpp` |
| **PLC (ESP)** | Firmware PLC | `PLC/PLC.ino` |
| **PF L/R (ESP)** | PreFeeder lado | `PF/PF_LR/PF_LR.ino`, `Status_Mode.h` |
| **PF Master** | Orquestador PF | `PreFeeder_Master.ino` |
| **Andon (ESP)** | Torre Andon | `Andon/Andon.ino` |
| **HMI ciclo** | Orquestador lote | `HMI/cycle.py` |
| **HMI red** | Cliente TCP | `HMI/state.py`, `tcp_link.py` |
| **HMI máquina** | Estado torre/ciclo | `HMI/machine_states.py` |
| **UI legacy** | Web monolito | `Temp/Index.html` |
| **Ciclo legacy** | Firmware monolito | `Temp/TCM.ino` |

> Un mismo fallo físico puede vivir en **dos capas** (ej. pinzas = PLC `0x20` + wrapper ciclo E021). El opcode modular es el del esclavo.

---

# PARTE A — Con opcode (oficial)

No reclasificar ni mover estas filas. Si hace falta trabajo, es de **cableado**, no de catálogo.

## 1. Motion

**Viene de:** **Motion (ESP)** · `Motion/MotionStates.h` · reset `0x16` · consume: **HMI**.

### Errores detalle (Motion ESP → HMI)

| Byte | Dec | Enum (código) | Función | Viene de | Descripción | Comentarios |
|------|-----|---------------|---------|----------|-------------|-------------|
| `0x11` | 17 | `MOT_ERR_ENCODER` | `Error()` | Motion ESP · encoder | Sin cambio en lectura | HMI etiqueta el **byte** en `state.py` |
| `0x15` | 21 | `MOT_ERR_LENGTH_NG_L` | `LenghtNG_L()` | Motion ESP · feed L | Longitud L fuera tolerancia | HMI OK; ciclo usa LengthOK/NG |
| `0x4B` | 75 | `MOT_ERR_LENGTH_NG_R` | `LenghtNG_R()` | Motion ESP · feed R | Longitud R fuera tolerancia | HMI OK |
| `0x4D` | 77 | `MOT_ERR_LASER_R` | `LaserR()` | Motion ESP · GPIO 32 | Láser / material R | Enum + función + TX ESP OK; HMI sin label dedicado para el byte |
| `0x4E` | 78 | `MOT_ERR_LASER_L` | `LaserL()` | Motion ESP · GPIO 34 | Láser / material L | Igual que `0x4D` |
| `0x4F` | 79 | `MOT_ERR_EXHAUST` | `Exhaust()` | Motion ESP · GPIO 26 | Safety exhaust | Enum + función + TX ESP OK; HMI sin label dedicado para el byte |

### Estado Error (módulo)

| Byte | Dec | Enum (código) | Función | Viene de | Descripción | Comentarios |
|------|-----|---------------|---------|----------|-------------|-------------|
| `0x0C` | 12 | `MOT_ST_ERROR` | `ErrorState()` | Motion ESP (agregado) | Módulo en falla | Enum ≠ función. Estado genérico; detalle = bytes de arriba si hubo. Alarmas ASDA/feed sin detalle → solo este byte (§6.3–6.4) |

**Total detalle: 6** (+ 1 estado).

---

## 2. PLC

**Viene de:** **PLC (ESP)** · `PLC/PlcStates.h` · reset `0x1E` · consume: **HMI**.

### Errores detalle (PLC ESP → HMI)

| Byte | Dec | Enum (código) | Función | GPIO | Viene de | Descripción | Comentarios |
|------|-----|---------------|---------|------|----------|-------------|-------------|
| `0x1F` | 31 | `PLC_ERR_CUTTER` | `CutterE()` | 26 | PLC ESP · cortador | Cutter error | Completo: enum + función + TX + HMI labels |
| `0x20` | 32 | `PLC_ERR_GRIPPER` | `GripperE()` | 27 | PLC ESP · pinzas | Gripper error | Completo |
| `0x21` | 33 | `PLC_ERR_HOLDER` | `HolderE()` | 32 | PLC ESP · sujetador | Holder error | Completo |
| `0x22` | 34 | `PLC_ERR_ENCODER` | `EncoderE()` | 34 | PLC ESP · bandeja | Encoder / bandeja | Completo |

### Estado Error (módulo)

| Byte | Dec | Enum (código) | Función | Viene de | Descripción | Comentarios |
|------|-----|---------------|---------|----------|-------------|-------------|
| `0x27` | 39 | `PLC_ST_ERROR` | `ErrorState()` | PLC ESP (agregado) | Módulo en falla | Genérico; detalle = `0x1F–0x22` |

**Total detalle: 4** (+ 1 estado).

---

## 3. PreFeeder

**Viene de:** **PF L/R (ESP)** → **PF Master** → **HMI**.  
`PF/PfStates.h` + `Status_Mode.h` · reset `0x2C`.

### Errores lógicos (`PfErrorId`) + byte TCP (por lado)

Tag/`PfErrorId` = catálogo lógico interno. Byte/enum TCP = mensaje en el cable. Función = rutina que reporta.

| Tag | ID lógico | Descripción | Nivel | Byte R | Enum TCP R | Byte L | Enum TCP L | Función R / L | Comentarios |
|-----|-----------|-------------|-------|--------|------------|--------|------------|---------------|-------------|
| PF-001 | 1 | Buffer Max (endstop) | 3 | `0x2E` | `PF_ERR_BUFFER_MAX_R` | `0x34` | `PF_ERR_BUFFER_MAX_L` | `BufferMR()` / `BufferML()` | Completo |
| PF-002 | 2 | Tension timeout | 2 | `0x2F` | `PF_ERR_TENSION_R` | `0x35` | `PF_ERR_TENSION_L` | `TensionerR()` / `TensionerL()` | Completo |
| PF-003 | 3 | Cilindro abierto | 2 | `0x30` | `PF_ERR_CILINDRO_R` | `0x36` | `PF_ERR_CILINDRO_L` | `CilindroR()` / `CilindroL()` | Completo |
| PF-004 | 4 | Manguera ausente | 2 | `0x31` | `PF_ERR_MANGUERA_R` | `0x37` | `PF_ERR_MANGUERA_L` | `MangueraR()` / `MangueraL()` | Completo |
| PF-005 | 5 | Buffer sin relleno | 2 | `0x2D` | `PF_ERR_BUFFER_FULL_R` | `0x33` | `PF_ERR_BUFFER_FULL_L` | `BufferFR()` / `BufferFL()` | Completo |
| PF-006 | 6 | Sin holgura | 2 | `0x32` | `PF_ERR_HOLGURA_R` | `0x38` | `PF_ERR_HOLGURA_L` | `HolguraR()` / `HolguraL()` | Completo |
| PF-007 | 7 | Parada operador | 2 | — | — | — | — | (→ Stop, sin función TX detalle) | Tag lógico sí; **sin** enum/byte TCP detalle. Ver §6.1 |

Nivel: `1` aviso · `2` pause · `3` safety.

### Bytes TCP detalle (lista plana)

| Byte | Dec | Lado | Enum (código) | Función | Viene de | Comentarios |
|------|-----|------|---------------|---------|----------|-------------|
| `0x2D` | 45 | R | `PF_ERR_BUFFER_FULL_R` | `BufferFR()` | PF R ESP | = PF-005 R |
| `0x2E` | 46 | R | `PF_ERR_BUFFER_MAX_R` | `BufferMR()` | PF R ESP | = PF-001 R |
| `0x2F` | 47 | R | `PF_ERR_TENSION_R` | `TensionerR()` | PF R ESP | = PF-002 R |
| `0x30` | 48 | R | `PF_ERR_CILINDRO_R` | `CilindroR()` | PF R ESP | = PF-003 R |
| `0x31` | 49 | R | `PF_ERR_MANGUERA_R` | `MangueraR()` | PF R ESP | = PF-004 R |
| `0x32` | 50 | R | `PF_ERR_HOLGURA_R` | `HolguraR()` | PF R ESP | = PF-006 R |
| `0x33` | 51 | L | `PF_ERR_BUFFER_FULL_L` | `BufferFL()` | PF L ESP | = PF-005 L |
| `0x34` | 52 | L | `PF_ERR_BUFFER_MAX_L` | `BufferML()` | PF L ESP | = PF-001 L |
| `0x35` | 53 | L | `PF_ERR_TENSION_L` | `TensionerL()` | PF L ESP | = PF-002 L |
| `0x36` | 54 | L | `PF_ERR_CILINDRO_L` | `CilindroL()` | PF L ESP | = PF-003 L |
| `0x37` | 55 | L | `PF_ERR_MANGUERA_L` | `MangueraL()` | PF L ESP | = PF-004 L |
| `0x38` | 56 | L | `PF_ERR_HOLGURA_L` | `HolguraL()` | PF L ESP | = PF-006 L |

### Wire legacy (interno PF)

Base L=20 · R=30 · `wire = base + PfErrorId` → L 21–27 · R 31–37.  
**Comentarios:** solo Master↔L/R; no es opcode hacia HMI.

### Estado Error (módulo)

| Byte | Dec | Enum (código) | Función | Viene de | Descripción | Comentarios |
|------|-----|---------------|---------|----------|-------------|-------------|
| `0x3C` | 60 | `PF_ST_ERROR` | `ErrorState()` | PF Master / L/R | Módulo en falla | Genérico; detalle = `0x2D–0x38` |

**Total detalle TCP: 12** · lógicos con TCP: **6** · PF-007 sin TCP · (+ 1 estado).

---

## 4. Andon

Define: `Andon/AndonStates.h` · `HMI/machine_states.py`.

### Error propio (Andon ESP → HMI)

| Byte | Dec | Enum (código) | Función | GPIO | Viene de | Descripción | Comentarios |
|------|-----|---------------|---------|------|----------|-------------|-------------|
| `0x50` | 80 | `ANDON_ERR_PRESSURE` | `PressureError()` | 33 | Andon ESP · FRL | Baja / nula presión | Enum + función local OK; **TX TCP del byte a HMI pendiente** (`TODO`) |

### Estado máquina en torre (HMI → Andon)

| Byte | Dec | Enum (código) | Función | Torre | Viene de | Descripción | Comentarios |
|------|-----|---------------|---------|-------|----------|-------------|-------------|
| `0x46` | 70 | `ANDON_RX_ERROR` / `MACH_ERROR` | `ErrorState()` | Rojo + buzzer | **HMI máquina** | Estado máquina Error | Enum de estado ≠ detalle sensor; Andon solo refleja el byte |

**Total detalle Andon: 1**.

---

## 5. Máquina / HMI

**Viene de:** **HMI máquina** + **HMI ciclo** · `machine_states.py` / `cycle.py`.

| Byte | Dec | Enum (código) | Función / capa | Viene de | Descripción | Comentarios |
|------|-----|---------------|----------------|----------|-------------|-------------|
| `0x46` | 70 | `MACH_ERROR` | ciclo → `ErrorState` / torre | HMI ciclo → HMI máquina | Máquina en Error → Andon + Stop/Error a esclavos | Enum de **estado** OK. Slugs `_fault` del ciclo **no** son enums ni opcodes → §6.2 |

---

## Resumen parte A

| Módulo | Viene de | Detalle opcode | Estado | Reset | Comentarios globales |
|--------|----------|----------------|--------|-------|----------------------|
| Motion | Motion ESP | 6 | `0x0C` | `0x16` | Láser/exhaust: falta UI HMI |
| PLC | PLC ESP | 4 | `0x27` | `0x1E` | Completo punta a punta |
| PreFeeder | PF L/R + Master | 12 TCP (+ PF-001…006) | `0x3C` | `0x2C` | PF-007 sin byte TCP |
| Andon | Andon ESP | 1 (`0x50`) | — | máquina `0x43` | TX `0x50` pendiente |
| Máquina/HMI | HMI | — | `0x46` | `0x43` | Estado OK; slugs ciclo en §6 |

**Bytes de error detalle oficiales: 23** (6+4+12+1).

---

# PARTE B — Por clasificar (sin opcode de detalle)

Errores que existen en código/UI pero **no** tienen byte TCP de detalle en el protocolo modular.  
Caen en `ErrorState` genérico o en string (`_fault`, `feedFaultReason`, …).

| Columna | Significado |
|--------|-------------|
| **Viene de** | Capa que genera el fallo |
| Archivo | Ruta en código |
| ID legacy | E### UI / E0## monolito |
| Slug / mensaje | ID actual |
| Módulo sugerido | Dónde debería vivir el opcode futuro |

### 6.1 Huecos (tienen tag/estado, falta byte detalle)

| Viene de | Archivo | ID legacy | Slug / mensaje | Módulo sugerido | Nota |
|----------|---------|-----------|----------------|-----------------|------|
| PF L/R (ESP) | `Status_Mode.h` | E127/E137 / PF-007 | `operator_stop` | PreFeeder | Tag sí; TCP detalle no (también listado en §3) |
| Motion (ESP) | `Motion.ino` | — | solo `0x0C` | Motion | Alarma sin byte detalle |
| PLC (ESP) | `PLC.ino` | — | solo `0x27` | PLC | Sin mapear a `0x1F–0x22` |
| PF Master | Master | — | solo `0x3C` | PreFeeder | Sin `PF_ERR_*` |
| HMI ciclo + máquina | `cycle.py` | — | `0x46` + `_fault` | HMI | Detalle solo string |

### 6.2 HMI ciclo (`cycle.py`)

**Viene de:** **HMI ciclo** — falló espera/comando del lote, no necesariamente el sensor.

| Viene de | Archivo | ID legacy | Slug / mensaje | Módulo sugerido | Descripción |
|----------|---------|-----------|----------------|-----------------|-------------|
| HMI ciclo | `HMI/cycle.py` | E010 ≈ | `timeout_motion` | Motion / Ciclo | Timeout Idle/Reached |
| HMI ciclo | `HMI/cycle.py` | E011 ≈ | `timeout_feed` | Motion / Ciclo | Sin LengthOK |
| HMI ciclo | `HMI/cycle.py` | E040 ≈ | `home_cmd` | Motion / Ciclo | HOME falló |
| HMI ciclo | `HMI/cycle.py` | — | `feed_cmd` | Motion / Ciclo | Feed L+R falló |
| HMI ciclo | `HMI/cycle.py` | E042 ≈ | `move_cmd` | Motion / Ciclo | Move ABS falló |
| HMI ciclo | `HMI/cycle.py` | E072 ≈ | `prefeeder_all_ok` | PreFeeder / Ciclo | All OK en corte |
| HMI ciclo | `HMI/cycle.py` | E071 ≈ | `deposit_cmd` | Motion / Ciclo | Depósito falló |
| HMI ciclo | `HMI/cycle.py` | E069 ≈ | `feed_incomplete` | Motion / Ciclo | Feed incompleto |
| HMI ciclo | `HMI/cycle.py` | — | `aborted` | Ciclo | Abort |
| HMI ciclo | `HMI/cycle.py` | E012/E099 ≈ | `cycle_failed` | Ciclo | Error genérico |
| HMI ciclo | `HMI/cycle.py` | E099 ≈ | `exception:…` | Ciclo | Excepción Python |
| HMI ciclo | `HMI/cycle.py` | E030 ≈ | (PF ready soft) | PreFeeder | `pf_ready_timeout_s` |

### 6.3 Motion — ASDA / lineal

| Viene de | Archivo | ID legacy | Slug / mensaje | Módulo sugerido | Descripción |
|----------|---------|-----------|----------------|-----------------|-------------|
| Motion (ESP) | `Motion.ino` | E010 | `ALARMA: timeout — no llego a destino` | Motion | Timeout PR |
| Motion (ESP) | `Motion.ino` | E059 | `ALARMA: Modbus timeout leyendo P5.007` | Motion | ASDA Modbus |
| Motion (ESP) | `Motion.ino` | E040 | HOME fallido | Motion | |
| Motion (ESP) / UI legacy | `Motion.ino` / `Index.html` | E041 | Safe zone fallida | Motion | |
| Motion (ESP) / UI legacy | `Motion.ino` / `Index.html` | E042 | Movimiento fallido | Motion | |
| Motion (ESP) | `Motion.ino` | — | `No se puede resetear error en movimiento` | Motion | |
| Motion (ESP) | `Motion.ino` | — | `sin encoder OM instalado` | Motion | |
| UI legacy | `Index.html` | E059 | ASDA OM no responde | Motion | Solo catálogo UI |

### 6.4 Motion — Feed / CAN

| Viene de | Archivo | ID legacy | Slug / mensaje | Módulo sugerido | Descripción |
|----------|---------|-----------|----------------|-----------------|-------------|
| Motion (ESP) | `Servo_Feed.cpp` | E008 | Servo CAN no listo | Motion | |
| Motion (ESP) | `Servo_Feed.cpp` | E060 | `FEED: target L + offset negativo` | Motion | |
| Motion (ESP) | `Servo_Feed.cpp` | E061 | `FEED: target R + offset negativo` | Motion | |
| Motion (ESP) | `Servo_Feed.cpp` | E064 | `FEED: no 6064 L` | Motion | |
| Motion (ESP) | `Servo_Feed.cpp` | E065 | `FEED: no 6064 R` | Motion | |
| Motion (ESP) | `Servo_Feed.cpp` | E059≈ | `FEED: sin lectura OM tras move` | Motion | |
| Motion (ESP) | `Servo_Feed.cpp` | E017–E019 | `OM … >/< target±tol` | Motion | LengthNG puede cubrir |
| Motion (ESP) | `Servo_Feed.cpp` | — | `FEED: sentido horario…` | Motion | |
| Motion (ESP) | `Servo_Feed.cpp` | E011/E066/E068 | `FEED: timeout` | Motion | |
| UI legacy | `Index.html` | E032 | Target/offset inválido | Motion | Solo UI |
| UI legacy | `Index.html` | E046 | Perfil/velocidad inválida | Motion | Solo UI |
| UI legacy | `Index.html` | E047 | Calibración fallida | Motion | Solo UI |
| UI legacy | `Index.html` | E048 | Offset fallido | Motion | Solo UI |
| UI legacy | `Index.html` | E049 | Prueba feed fallida | Motion | Solo UI |
| UI legacy | `Index.html` | E062 | Perfil L inválido | Motion | Solo UI |
| UI legacy | `Index.html` | E063 | Perfil R inválido | Motion | Solo UI |
| UI legacy | `Index.html` | E067 | Reservado (láser timeout) | Motion | Solo UI |
| UI legacy / HMI ciclo | `Index.html` / `cycle.py` | E070 | Prefetch/handoff fallido | Motion / Ciclo | |
| UI legacy / Ciclo legacy | `Index.html` / `TCM.ino` | E074 / E016 | Sensor manguera feed | Motion | ≠ láser `0x4D`/`0x4E` |

### 6.5 PLC / safety de ciclo (wrapper)

Sensor = PLC `0x1F–0x22` (parte A). Aquí solo la **capa ciclo** u otros sin opcode propio.

| Viene de | Archivo | ID legacy | Slug / mensaje | Módulo sugerido | Descripción |
|----------|---------|-----------|----------------|-----------------|-------------|
| Ciclo / UI legacy | `TCM.ino` / `Index.html` | E004 | Manguera (safety CAN) | PLC / Motion | |
| Ciclo / UI legacy | `TCM.ino` / `Index.html` | E006 / E026 | Parada externa PreFeeder | Ciclo / PF | |
| Ciclo / UI legacy | `TCM.ino` / `Index.html` | E007 / E027 | Sin comunicación CAN | Motion / PLC | |
| Ciclo legacy | `TCM.ino` | E020 | Safety stop genérico | Ciclo | |
| Ciclo legacy | `TCM.ino` | E021–E025 | Safety stop · pinzas…bandeja | Ciclo | Wrapper sobre PLC |
| UI legacy | `Index.html` | E056 | PLC seguridad activa | PLC | |
| PLC (ESP) | `PLC.ino` | alert 1/2/3/5 | `sendAlert(n)` | PLC | Interno sensor_tubecut |

### 6.6 PreFeeder / red / enlace

| Viene de | Archivo | ID legacy | Slug / mensaje | Módulo sugerido | Descripción |
|----------|---------|-----------|----------------|-----------------|-------------|
| PF / Master | `PF_LR` / Master | E120 | Sin enlace TCP L | PreFeeder | |
| PF / Master | Master | E130 | Sin enlace TCP R | PreFeeder | |
| PF Master | Master | — | `no_link` | PreFeeder | |
| HMI ciclo / UI | `cycle.py` / `Index.html` | E030 | PF no inicializado | PreFeeder | |
| Ciclo / UI legacy | `TCM.ino` / `Index.html` | E031 | Peer TCP perdido | PreFeeder | |
| UI legacy | `Index.html` | E045 | Disparo TCP fallido | PreFeeder | |
| UI legacy | `Index.html` | E057 | Materialista activo | PreFeeder | ¿aviso o error? |
| HMI red | `state.py` | E100≈ | Sin enlace Motion | Red / HMI | `:8767` |
| HMI red | `state.py` | E100≈ | Sin enlace PLC | Red / HMI | `:8766` |
| HMI red | `state.py` | E100≈ | Sin enlace PreFeeder | Red / HMI | `:8768` |
| UI legacy | `Index.html` | E100 | Sin conexión TCM | Red / HMI | |

### 6.7 Ciclo / UI / config

| Viene de | Archivo | ID legacy | Slug / mensaje | Módulo sugerido | Descripción |
|----------|---------|-----------|----------------|-----------------|-------------|
| UI legacy | `Index.html` | E043 | Stop rechazado | Ciclo | |
| UI legacy | `Index.html` | E044 | Reset errores fallido | Ciclo | |
| UI legacy | `Index.html` | E050 | Depósito no guardado | Config | |
| UI legacy | `Index.html` | E051 | Velocidad no guardada | Config | |
| UI legacy | `Index.html` | E052 | Cycle time no reseteado | Monitoreo | |
| UI legacy | `Index.html` | E053 | Operación de modelo fallida | Modelos | |
| UI legacy | `Index.html` | E054 | Parámetros inválidos | UI | |
| UI legacy | `Index.html` | E055 | Ciclo en conflicto | Ciclo | |
| UI legacy | `Index.html` | E058 | Almacenamiento no disponible | Sistema | |
| UI / Ciclo legacy | `Index.html` / `TCM.ino` | E099 | Falla no clasificada | Ciclo | |
| UI legacy | `Index.html` | E016 (OM) | Reservado (láser) | Motion | Conflicto vs E016 hose |
| Ciclo legacy | `TCM.ino` | E001 | Abort (omitido) | Ciclo | No es error máquina |
| Ciclo legacy | `TCM.ino` | E012 | Alimentación fallida | Motion / Ciclo | |
| UI legacy | — | E012 | (no en ERROR_CATALOG UI) | — | Unificar |

### 6.8 Inventario

| Grupo | ≈ |
|-------|---|
| 6.1 Huecos | 5 |
| 6.2 HMI ciclo | 12 |
| 6.3 ASDA | 8 |
| 6.4 Feed/CAN | 18 |
| 6.5 Safety ciclo | 8 |
| 6.6 Red/PF | 11 |
| 6.7 UI/config | 13 |
| **Total** | **~75** |

Al asignar opcode → **mover a Parte A** y quitar de aquí.

---

## Notas

1. **Enum ≠ función:** el enum es el código (`PLC_ERR_CUTTER`); la función es la rutina (`CutterE()`). En `opcode_reference_list.md`, la columna «Ejecuta» es la **función**, no el enum.
2. `LenghtOK` (`0x14` / `0x4A`) no son errores.
3. Parte A = catálogo oficial. Parte B = pendientes de opcode.
4. Excel/`ERROR_CATALOG` legacy = referencia histórica.
5. Comentarios en Parte A = gaps de cableado, no de definición de opcode.
