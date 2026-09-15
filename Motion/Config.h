#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include "MotionStates.h"

// Motion esclavo — ASDA + OM + Feed CAN
// IP fija .20 — maestro TCM u otro cliente TCP se conectan como clientes.
// TCP :8767 — framing JSON + newline (mismo patrón que PF_LR / PLCA).
// Pines: Doc/gpio_list_updated.md · Estados/errores: MotionStates.h

static const char* const WIFI_SSID = "R&D_TCM";
static const char* const WIFI_PASS = "TCM2026!r&d";
static const IPAddress STA_IP(10, 10, 32, 20);
static const IPAddress STA_GW(10, 10, 32, 72);
static const IPAddress STA_MASK(255, 255, 255, 0);
static const IPAddress STA_DNS(10, 10, 32, 72);

constexpr uint16_t MOTION_TCP_PORT = 8767;
constexpr uint8_t  MOTION_PROTO_VER = 1;
// Tiempo máximo de un intento STA (no bloquea el loop; solo marca reintento).
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 30000;
// Intervalo mínimo entre WiFi.begin() de reintento (millis, no delay).
constexpr unsigned long WIFI_RETRY_INTERVAL_MS = 5000;

// --- Sensores Láser (Keyence LR-X) — MOT_ERR_LASER_* ---
// R: GPIO32 (pull-up interno OK). L: GPIO34 input-only ESP32 — sin pull-up
// interno; hace falta pull-up externo ~10k a 3.3V si el Keyence es NPN/OC.
constexpr uint8_t PIN_LRX_LASER_R  = 32;
constexpr uint8_t PIN_LRX_LASER_L  = 34;

// --- Safety exhaust — MOT_ERR_EXHAUST ---
constexpr uint8_t PIN_E_STOP       = 26;
constexpr uint8_t PIN_SAFETY_EXHAUST = PIN_E_STOP;

#ifndef PIN_ESTOP_ACTIVE_HIGH
#define PIN_ESTOP_ACTIVE_HIGH 1
#endif

// Eventos sensor → maestro (Set detalle + status push niveles). 1 = activo.
#ifndef MOT_IO_SENSOR_EVENTS
#define MOT_IO_SENSOR_EVENTS 1
#endif

constexpr unsigned long MOT_SENSOR_DEBOUNCE_MS = 150;

// Instrumentación de latencia MOVE/TCP/Modbus (Serial). 0 = desactivado.
#ifndef MOT_LATENCY_DEBUG
#define MOT_LATENCY_DEBUG 0
#endif

// --- Interfaz Ethernet (SPI - Bus HSPI sin conflictos) ---
constexpr uint8_t PIN_ETH_CLK      = 14;  // CLK / SCK (HSPI)
constexpr uint8_t PIN_ETH_MISO     = 12;  // MISO (HSPI)
constexpr uint8_t PIN_ETH_MOSI     = 13;  // MOSI (HSPI)
constexpr uint8_t PIN_ETH_CS       = 15;  // CS / SS (HSPI)
constexpr uint8_t PIN_ETH_RST      = 27;  // RST (Reset)
constexpr uint8_t PIN_ETH_INT      = 33;  // INT (Interrupción)
