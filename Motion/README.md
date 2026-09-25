# Motion (ESP32 unificado)

Firmware **Motion**: ASDA-B3 RS-485 + encoder OM + servos feeder CAN (CiA402).

Basado en `ESP32_ASDA_B3_RS485_API.ino` + lógica feed de TCM.

## Hardware

| Bus | Pin / nota |
|-----|------------|
| RS-485 | RX 16, TX 17 |
| OM encoder | A 18, B 19, Z 21 |
| CAN (TWAI / Adafruit) | TX **GPIO 4**, RX **GPIO 5**, 500 kbps |

Servos CAN: nodo **R=1** (`0x601`), **L=2** (`0x602`).

## Arduino IDE

1. Abrir carpeta `Motion/Doc/Motion/` (sketch **`Motion.ino`**)
2. Librerías: ESP32 core (TWAI incluido; ya no se usa `mcp_can`)
3. IP estática: `10.10.32.20` · hostname: `motion`

## Motion Master (UI orquestador)

Sketch separado en `Motion/Motion_Master/` — mismo patrón que `PF/PreFeeder_Master/`.

| Dispositivo | IP | Rol |
|-------------|-----|-----|
| Motion | `10.10.32.20` | ASDA + OM + Feed (esclavo) |

Abrir en Arduino IDE: `Motion/Motion_Master/Motion_Master.ino`

| Ruta Master | Descripción |
|-------------|-------------|
| `/` | Dashboard Motion (estado + controles rápidos) |
| `/ui/motion` | Redirige a UI completa del esclavo |
| `/ui/feed` | Redirige a `/feed` del esclavo |
| `/api/status` | JSON agregado (ASDA, OM, overview, feed) |
| `/api/cmd?command=stop\|on\|off\|test\|encReset` | Proxy POST → Motion |

## URLs (Motion esclavo .20)

| Ruta | Descripción |
|------|-------------|
| `/` | UI ASDA + OM |
| `/feed` | UI alimentación servos |
| `/api/feed/status` | Estado CAN + feed (incluye `feedMode` / `velocityStage`) |

### Modos de feed (producción vs experimental)

| Modo | NVS `feedMode` | Uso |
|------|----------------|-----|
| Position + Sensor | `2` (default) | Profile Position + LASER_SEEK. Producción. |
| Velocity + Sensor | `3` | Profile Velocity (`0x6060=3`) + tope LR-X crudo. Experimental. |

Purga/refill no cambia de modo. Parámetros Velocity en NVS: `vFastPct`, `vSlowPct`, `vTransPct`, `vMaxMm`, `vToMs`.

## TCP maestro ASDA (`:8767`)

Mismo framing que PF_LR / PLCA: **JSON + newline**. El maestro conecta como cliente a `10.10.32.20:8767`.

| Byte | Acción | Dirección | Función interna |
|------|--------|-----------|-----------------|
| `0x01` | Search home | Recibe | `homeTorque()` + job async |
| `0x02` | Stop | Recibe | `stopMotion()` → estado `0x0D` |
| `0x03` | Servo off | Recibe | `servoOff()` |
| `0x04` | Servo on | Recibe | `servoOn()` |
| `0x05` | Move to position | Recibe | `moveAbsolute()` + job async |
| `0x06` | Position reached | Manda | evento al completar PR |
| `0x07` | Move to 0 | Recibe | `moveAbsolute(0)` |
| `0x08` | Estatus | Recibe / Manda | snapshot JSON (poll periódico) |
| `0x09` | Init | Manda | al conectar TCP |
| `0x0A` | Idle | Manda | sin busy ni alarma |
| `0x0B` | Busy | Manda | movimiento o rutina activa |
| `0x0C` | Error | Manda | alarma Modbus/timeout |
| `0x0D` | Stop | Manda | tras comando stop |
| `0x0E` | ReturnStop | Manda | tras stop completado |

Ejemplo comando home: `{"type":"command","byte":1,"direction":"F"}`

Ejemplo move PUU: `{"type":"command","byte":5,"position":-7000,"speedRpm":1200}`

## Archivos (estilo PF_LR)

| Archivo | Rol |
|---------|-----|
| `Motion.ino` | Loop principal: ASDA, OM, WiFi, HTTP |
| `Config.h` | WiFi / red |
| `Asda.h` | Config servo lineal Delta (Modbus) |
| `Encoder.h` | Config encoder OM (PCNT) |
| `FeederCan.h` | Config CAN/feed + enums y structs |
| `Servo_Feed.h` | API CAN + feed + rutas HTTP |
| `Servo_Feed.cpp` | CiA402, máquina de estados, handlers `/feed` |
| `Index.h` | HTML embebido (`index_html`, `feed_index_html`) |
| `Doc/Motion/Index.HTML` | Fuente editable UI principal (ASDA + OM + rutina) |
| `Doc/Motion/feed_index.html` | Fuente editable UI feed CAN |
