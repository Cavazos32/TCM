#pragma once

#include <Arduino.h>
#include <WiFi.h>

// Andon — MCU salidas torre/luz + buzzer + presión FRL.
// Esclavo TCP :8769 (no chocar con PreFeeder :8768). IP fija .60.
// Pines: Doc/gpio_list_updated.md (fuente de verdad).

static const char* WIFI_SSID = "R&D_TCM";
static const char* WIFI_PASS = "TCM2026!r&d";
static const IPAddress STA_IP(10, 10, 32, 60);
static const IPAddress STA_GW(10, 10, 32, 72);
static const IPAddress STA_MASK(255, 255, 255, 0);

constexpr uint16_t ANDON_TCP_PORT = 8769;
constexpr uint8_t ANDON_PROTO_VER = 1;
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 30000;

// --- Salidas torre Andon (reaccionan a bytes Machine/HMI) ---
constexpr uint8_t PIN_GREEN_LED  = 16;  // 0x40 / 0x44 / 0x45 / seq 0x47
constexpr uint8_t PIN_YELLOW_LED = 17;  // 0x48 Pause / 0x49 Materialist / seq 0x47
constexpr uint8_t PIN_BUZZER     = 25;  // 0x46 / 0x47 (seq) / 0x49
constexpr uint8_t PIN_RED_LED    = 21;  // 0x42 / 0x46 / seq 0x47

// Fin de WO (0x47): R→Y→G en secuencia + buzzer; no es estado permanente.
constexpr uint32_t ANDON_FINISH_STEP_MS = 400;
constexpr uint8_t  ANDON_FINISH_CYCLES  = 3;

// --- Entrada presión FRL — local: torreta Error + TX 0x50 a HMI (sin RX) ---
constexpr uint8_t PIN_PRESSURE_FRL = 33;
constexpr unsigned long ANDON_PRESSURE_DEBOUNCE_MS = 150;

// --- Interfaz Ethernet (SPI) — MISO no listado en gpio_list ---
constexpr uint8_t PIN_ETH_CS       = 5;
constexpr uint8_t PIN_ETH_CLK      = 18;
constexpr uint8_t PIN_ETH_RST      = 22;
constexpr uint8_t PIN_ETH_MOSI     = 23;
constexpr uint8_t PIN_ETH_INT      = 35;

#ifndef ANDON_ACTIVE_HIGH
#define ANDON_ACTIVE_HIGH 1
#endif
#ifndef ANDON_DEBUG
#define ANDON_DEBUG 1
#endif
