#pragma once

#include <Arduino.h>
#include <WiFi.h>

// Motion esclavo — ASDA + OM + Feed CAN
// IP fija .20 — maestro TCM u otro cliente TCP se conectan como clientes.
// TCP :8767 — framing JSON + newline (mismo patrón que PF_LR / PLCA).

static const char* const WIFI_SSID = "R&D_TCM";
static const char* const WIFI_PASS = "TCM2026!r&d";
static const IPAddress STA_IP(10, 10, 32, 20);
static const IPAddress STA_GW(10, 10, 32, 72);
static const IPAddress STA_MASK(255, 255, 255, 0);
static const IPAddress STA_DNS(10, 10, 32, 72);

constexpr uint16_t MOTION_TCP_PORT = 8767;
constexpr uint8_t  MOTION_PROTO_VER = 1;
// Estatus (0x008) bajo demanda. Bytes 0x09–0x0E = estatus general del módulo Motion.
constexpr uint8_t  MOT_CMD_RESET_ERR = 0x16;  // maestro → esclavo: limpiar errores (sin reset contador OM)
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 30000;

// --- Sensores Láser (Keyence LR-X) ---
constexpr uint8_t PIN_LRX_LASER_R  = 34;  // Sensor láser lado derecho (R)
constexpr uint8_t PIN_LRX_LASER_L  = 35;  // Sensor láser lado izquierdo (L)

// --- Paro de Emergencia E-Stop (Class 3) ---
constexpr uint8_t PIN_E_STOP       = 26;  // Entrada E-Stop (GPIO libre con soporte de INPUT_PULLUP)

#ifndef PIN_ESTOP_ACTIVE_HIGH
#define PIN_ESTOP_ACTIVE_HIGH 1
#endif

// --- Interfaz Ethernet (SPI - Bus HSPI sin conflictos) ---
constexpr uint8_t PIN_ETH_CLK      = 14;  // CLK / SCK (HSPI)
constexpr uint8_t PIN_ETH_MISO     = 12;  // MISO (HSPI)
constexpr uint8_t PIN_ETH_MOSI     = 13;  // MOSI (HSPI)
constexpr uint8_t PIN_ETH_CS       = 15;  // CS / SS (HSPI)
constexpr uint8_t PIN_ETH_RST      = 27;  // RST (Reset)
constexpr uint8_t PIN_ETH_INT      = 33;  // INT (Interrupción)
