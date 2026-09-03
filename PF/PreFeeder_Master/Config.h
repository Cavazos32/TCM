#pragma once

#include <Arduino.h>
#include <WiFi.h>

// PreFeeder Master — orquestador L/R
// IP fija .100 (.10 ocupada en la LAN corporativa) — HMI TCP :8768.
// Esclavos PF_LR: TCP :8765 (interno Master → L/R).

static const char* const PF_MASTER_WIFI_SSID = "R&D_TCM";
static const char* const PF_MASTER_WIFI_PASS = "TCM2026!r&d";
static const IPAddress PF_MASTER_STA_IP(10, 10, 32, 100);
static const IPAddress PF_MASTER_STA_GW(10, 10, 32, 72);
static const IPAddress PF_MASTER_STA_MASK(255, 255, 255, 0);

constexpr uint16_t PF_MASTER_TCP_PORT = 8768;
constexpr uint8_t  PF_MASTER_PROTO_VER = 1;
constexpr unsigned long PF_MASTER_WIFI_CONNECT_TIMEOUT_MS = 30000;
