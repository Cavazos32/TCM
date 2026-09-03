#pragma once

#include <Arduino.h>
#include <WiFi.h>

// Andon — MCU salidas torre/luz + buzzer.
// Esclavo TCP :8768 (pendiente). IP fija .60.

static const char* WIFI_SSID = "R&D_TCM";
static const char* WIFI_PASS = "TCM2026!r&d";
static const IPAddress STA_IP(10, 10, 32, 60);
static const IPAddress STA_GW(10, 10, 32, 72);
static const IPAddress STA_MASK(255, 255, 255, 0);

constexpr uint16_t ANDON_TCP_PORT = 8768;
constexpr uint8_t ANDON_PROTO_VER = 1;
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 30000;

// --- Salidas torre Andon ---
constexpr uint8_t PIN_RED_LED    = 18;
constexpr uint8_t PIN_YELLOW_LED = 17;
constexpr uint8_t PIN_GREEN_LED  = 16;
constexpr uint8_t PIN_BUZZER     = 20;

#ifndef ANDON_ACTIVE_HIGH
#define ANDON_ACTIVE_HIGH 1
#endif
#ifndef ANDON_DEBUG
#define ANDON_DEBUG 1
#endif