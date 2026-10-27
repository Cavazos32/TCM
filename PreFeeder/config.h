#pragma once

#include <Arduino.h> 

// Lado: comentar la línea siguiente para L, descomentar para R (misma base de firmware).
//#define PF_SIDE_RIGHT
//.102 Lado R
//.101 Lado L
#ifndef PF_CONFIG_SIDE
#if defined(PF_SIDE_RIGHT)
#define PF_CONFIG_SIDE 2
#else
#define PF_CONFIG_SIDE 1
#endif
#endif

#if PF_CONFIG_SIDE == 2
#define PF_SIDE_CHAR 'R'
#define PF_SIDE_TAG "R"
#define PF_ROLE "prefeeder_R"
#define PF_STA_IP_0 10
#define PF_STA_IP_1 10
#define PF_STA_IP_2 32
#define PF_STA_IP_3 102
constexpr uint16_t PF_SERVO_ACTIVE_US = 2000;
constexpr int8_t PF_SERVO_SPEED_SIGN = 1;
constexpr uint8_t PF_FAULT_WIRE_BASE = 30;
#define PF_OTA_HOST "prefeeder-R"
#else
#define PF_SIDE_CHAR 'L'
#define PF_SIDE_TAG "L"
#define PF_ROLE "prefeeder_L"
#define PF_STA_IP_0 10
#define PF_STA_IP_1 10
#define PF_STA_IP_2 32
#define PF_STA_IP_3 101
constexpr uint16_t PF_SERVO_ACTIVE_US = 800;
constexpr int8_t PF_SERVO_SPEED_SIGN = -1;
constexpr uint8_t PF_FAULT_WIRE_BASE = 20;
#define PF_OTA_HOST "prefeeder-L"
#endif

// GPIO (contrato hardware conocido)
constexpr uint8_t PIN_BUF_FULL = 19;   // HIGH = activo (buffer lleno / “home” en peer)
constexpr uint8_t PIN_BUF_MID = 34;
constexpr uint8_t PIN_BUF_HIGH = 35;
constexpr uint8_t PIN_TENSION = 23;
constexpr uint8_t PIN_BUF_MAX = 21;
constexpr uint8_t PIN_CYLINDER = 25;
constexpr uint8_t PIN_HOSE = 27;       // HIGH = manguera/cinta ausente

constexpr int PIN_DEREELER_STEP = 32;
constexpr int PIN_DEREELER_DIR = 33;
constexpr uint8_t DEREELER_CW_LEVEL = LOW;
constexpr uint8_t DEREELER_CCW_LEVEL = HIGH;

constexpr int PIN_FEEDER_STEP = 18;
constexpr uint8_t PIN_HOLGURA = 22;       // HIGH = holgura OK (ON)
constexpr uint8_t HOLGURA_ON_LEVEL = HIGH;
constexpr uint16_t HOLGURA_SETTLE_MS_DEFAULT = 150;
constexpr uint16_t HOLGURA_SETTLE_MS_MIN = 10;
constexpr uint16_t HOLGURA_SETTLE_MS_MAX = 500;
constexpr uint8_t PIN_SERVO = 26;

constexpr uint16_t SERVO_MIN_US = 500;
constexpr uint16_t SERVO_MAX_US = 2500;
constexpr uint16_t SERVO_NEUTRAL_US = 1500;
constexpr uint8_t SERVO_LEDC_CH = 7;
constexpr uint8_t SERVO_LEDC_BITS = 14;

constexpr uint32_t DEREELER_LEAD_MS = 100;

// Red
static const char* const WIFI_SSID = "R&D_TCM";
static const char* const WIFI_PASS = "TCM2026!r&d";
#define STA_GW_0 10
#define STA_GW_1 10
#define STA_GW_2 32
#define STA_GW_3 72
#define STA_MASK_0 255
#define STA_MASK_1 255
#define STA_MASK_2 255
#define STA_MASK_3 0
constexpr uint16_t PEER_TCP_PORT = 8765;
constexpr uint32_t PEER_STATUS_MS = 2000;
constexpr uint32_t PEER_STATUS_FAST_MS = 350;

// Motores
constexpr uint16_t MOTOR_MICROSTEP = 16;
constexpr float MOTOR_ACCEL = 1000.0f;
constexpr float MOTOR_DECEL = 1000.0f;
constexpr float RPM_MIN = 1.0f;
constexpr float RPM_MAX = 600.0f;

// Filtros sensores
constexpr uint32_t BUF_FULL_ON_MS = 50;
constexpr uint32_t BUF_FULL_OFF_MS = 100;

// Firmware
#define FW_VERSION "2.0.0"
#define OTA_PASSWORD "TCM_PF2026"
#define PREFS_NS "pf_v2"
