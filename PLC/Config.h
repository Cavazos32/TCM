#pragma once

#include <Arduino.h>
#include <WiFi.h>

// PLCA — MCU I/O TubeCut (sensores + válvulas).
// Esclavo TCP :8766 (mismo framing JSON+newline que PreFeeder / sensor_tubecut).
// IP fija .50 — TCM u otro maestro se conectan como clientes.
// Pines: Doc/gpio_list_updated.md (fuente de verdad).

static const char* WIFI_SSID = "R&D_TCM";
static const char* WIFI_PASS = "TCM2026!r&d";
static const IPAddress STA_IP(10, 10, 32, 50);
static const IPAddress STA_GW(10, 10, 32, 72);
static const IPAddress STA_MASK(255, 255, 255, 0);

constexpr uint16_t PLCA_TCP_PORT = 8766;
constexpr uint8_t  PLCA_PROTO_VER = 1;
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 30000;

// --- Entradas sensores (errores → TX 0x1F–0x22) ---
constexpr uint8_t PIN_CUTTER          = 26;  // 0x1F Cutter error
constexpr uint8_t PIN_GRIPPER         = 27;  // 0x20 Gripper error
constexpr uint8_t PIN_HOLDER          = 32;  // 0x21 Holder error
constexpr uint8_t PIN_ENCODER         = 34;  // 0x22 Encoder error

// --- Salidas válvulas / relés (Opcode TCP → GPIO) ---
//  0x19  Cutter R  → 25
//  0x1A  Cutter L  → 14
//  0x1B  Gripper   → 16
//  0x1C  Holder    → 17
//  0x1D  Encoder   → 21
//  0x23  Blower    → 4
//  0x1E  Reset     → 33
constexpr uint8_t PIN_OUT_CUTTER_R = 25;
constexpr uint8_t PIN_OUT_CUTTER_L = 14;
constexpr uint8_t PIN_OUT_GRIPPERS = 16;
constexpr uint8_t PIN_OUT_HOLDER   = 17;
constexpr uint8_t PIN_OUT_ENCODER  = 21;
constexpr uint8_t PIN_OUT_BLOWER   = 4;
constexpr uint8_t PIN_OUT_RESET    = 33;

// --- Interfaz Ethernet (SPI) ---
constexpr uint8_t PIN_ETH_CLK      = 18;
constexpr uint8_t PIN_ETH_MISO     = 19;
constexpr uint8_t PIN_ETH_MOSI     = 23;
constexpr uint8_t PIN_ETH_CS       = 5;
constexpr uint8_t PIN_ETH_RST      = 22;
constexpr uint8_t PIN_ETH_INT      = 35;

#ifndef VALVE_ACTIVE_HIGH
#define VALVE_ACTIVE_HIGH 1
#endif

constexpr uint8_t BIT_GRIPPER = (1 << 0);
constexpr uint8_t BIT_HOLDER  = (1 << 1);
constexpr uint8_t BIT_CUTTER  = (1 << 2);
constexpr uint8_t BIT_HOSE_A  = (1 << 3);
constexpr uint8_t BIT_HOSE_B  = (1 << 4);
constexpr uint8_t BIT_ENCODER = (1 << 5);

constexpr unsigned long SENSOR_DEBOUNCE_MS = 150;
constexpr unsigned long BOOT_GRACE_MS = 15000;
constexpr unsigned long SNAPSHOT_HEARTBEAT_MS = 3000;
constexpr unsigned long STATUS_PUSH_MS = 1000;

#ifndef PLCA_DEBUG
#define PLCA_DEBUG 1
#endif
