#pragma once

#include <Arduino.h>

// ====================== LADO L / R ======================
// Comentada = L | descomentada = R
//#define PREFEEDER_SIDE_RIGHT

#ifndef PREFEEDER_SIDE_RIGHT
#define PREFEEDER_SIDE_TAG  "L"
#define PREFEEDER_SIDE_ROLE "prefeeder_L"
// .30 estaba ocupada / conflictiva en la LAN (ping sin TCP). L usa .101.
static const IPAddress STA_IP(10, 10, 32, 101);
constexpr uint16_t SERVO_PWM_ACTIVE_US = 800;
constexpr uint8_t  FAULT_CODE_BASE     = 20;  // wire: base+PfErrorId → ver Status_Mode.h
#else
#define PREFEEDER_SIDE_TAG  "R"
#define PREFEEDER_SIDE_ROLE "prefeeder_R"
// .40 intermitente (posible conflicto DHCP/ARP). R usa .102.
static const IPAddress STA_IP(10, 10, 32, 102);
constexpr uint16_t SERVO_PWM_ACTIVE_US = 2000;
constexpr uint8_t  FAULT_CODE_BASE     = 30;  // wire: base+PfErrorId → ver Status_Mode.h
#endif

// ====================== DEBUG ======================
#ifndef SERIAL_VERBOSE
#define SERIAL_VERBOSE 0
#endif
#if SERIAL_VERBOSE
#define DBG_PRINT(...)   Serial.print(__VA_ARGS__)
#define DBG_PRINTLN(...) Serial.println(__VA_ARGS__)
#define DBG_PRINTF(...)  Serial.printf(__VA_ARGS__)
#else
#define DBG_PRINT(...)
#define DBG_PRINTLN(...)
#define DBG_PRINTF(...)
#endif

// ====================== GPIO ======================
// Holgura GPIO22: HIGH=OK; LOW=sin holgura → helper. Cilindro: opto 24V→3.3V HIGH=abierto.
constexpr uint8_t PIN_SENSOR_BUFFER_FULL = 19;  // HIGH = activo
constexpr uint8_t PIN_SENSOR_TENSION     = 23;  // HIGH = activo
constexpr uint8_t PIN_SENSOR_BUFFER_MAX  = 21;  // HIGH = activo
constexpr uint8_t PIN_SENSOR_HOLGURA     = 22;  // HIGH = OK; LOW = helper
constexpr uint8_t PIN_SENSOR_CILINDRO    = 25;
constexpr uint8_t PIN_SENSOR_HOSE_BELT   = 27;  // HIGH = cinta/manguera ausente
constexpr int PIN_DEREELER_PUL           = 32;
constexpr int PIN_DEREELER_MOSFET        = 33;  // DIR; EN fijo en driver
constexpr int PIN_FEEDER_PUL             = 18;  // DIR/EN fijos en driver
constexpr uint8_t PIN_SERVO_PWM          = 26;

// ====================== SERVO ======================
constexpr uint16_t SERVO_PWM_MIN_US       = 500;
constexpr uint16_t SERVO_PWM_MAX_US       = 2500;
constexpr uint16_t SERVO_PWM_NEUTRAL_US     = 1500;
constexpr uint8_t  SERVO_LEDC_BITS        = 14;
constexpr uint32_t DEREELER_START_DELAY_MS = 100;
// Tras Stop() RMT el LEDC se suelta: reafirmar 1500 cada frame RC, no cada 250 ms.
constexpr uint32_t SERVO_STOP_REASSERT_MS = 20;

// ====================== RED / WiFi ======================
// L: http://10.10.32.101 | R: http://10.10.32.102 | Master: http://10.10.32.100
// Master TCP → L/R :8765 (esclavos independientes; ver PreFeeder_Master/master_cmds.h)
//   Globales (Master→L+R): start/stop/reset, Materialista, In process, refill manual, …
//   Volátiles (Master→L o R): trigger, settings, Tfeed, …
static const char* const WIFI_SSID = "R&D_TCM";
static const char* const WIFI_PASS = "TCM2026!r&d";
static const IPAddress STA_GW(10, 10, 32, 72);
static const IPAddress STA_MASK(255, 255, 255, 0);
static const IPAddress TCM_IP(10, 10, 32, 100);
constexpr uint16_t PEER_PORT = 8765;
constexpr uint32_t PEER_STATUS_MS = 2000;
constexpr uint32_t PEER_STATUS_FAST_MS = 350;
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 30000;

// ====================== MOTORES ======================
constexpr uint16_t MOTOR_MICROSTEP    = 16;  // DM556 @ 3200 pulsos/rev (200×16)
constexpr float    MOTOR_ACCEL        = 1000.0f;
constexpr float    MOTOR_DECEL        = 1000.0f;
constexpr float    MOTOR_RPM_MIN      = 1.0f;
constexpr float    MOTOR_RPM_MAX      = 600.0f;
constexpr float    MOTOR_RPM_DEFAULT  = 60.0f;
constexpr float    MOTOR2_ACCEL       = 1000.0f;
constexpr float    MOTOR2_DECEL       = 1000.0f;
constexpr uint32_t MOTOR_DIR_SETUP_MS = 15;

// ====================== AUTO (defaults NVS) ======================
constexpr float AUTO_RPM_DEFAULT         = 60.0f;
constexpr float AUTO_REVERSE_DEFAULT     = 2.0f;
constexpr float AUTO_REVERSE_MIN         = 0.1f;
constexpr float AUTO_REVERSE_MAX         = 60.0f;
constexpr float TENSION_COOLDOWN_DEFAULT = 0.0f;
constexpr float TENSION_COOLDOWN_MIN     = 0.0f;
constexpr float TENSION_COOLDOWN_MAX     = 60.0f;
constexpr float TENSION_FAULT_SEC        = 10.0f;
constexpr float TENSION_BOOST_RPM_OFFSET = 30.0f;

// ====================== BUFFER / HOLGURA ======================
// Legacy UI/JSON: el monitor ya no enclava FAULT_BUFFER_TIMEOUT.
constexpr float BUFFER_REFILL_FAULT_SEC = 10.0f;
constexpr float M2_HOLGURA_FAULT_SEC      = 1.5f;   // ausencia acumulada tras Full visto → PF_ERR_HOLGURA
constexpr float M2_HOLGURA_FAULT_SEC_MIN  = 0.3f;
constexpr float M2_HOLGURA_FAULT_SEC_MAX  = 30.0f;
constexpr uint32_t M2_HOLGURA_FILTER_MS   = 80;
constexpr uint32_t M2_HOLGURA_HELPER_ABSENT_MS     = 100;  // ausente ≥ esto → helper feed
constexpr uint32_t M2_HOLGURA_HELPER_ABSENT_MS_MIN = 20;
constexpr uint32_t M2_HOLGURA_HELPER_ABSENT_MS_MAX = 5000;
constexpr float M2_HOLGURA_HELPER_RPM_DEFAULT = MOTOR_RPM_DEFAULT;
constexpr float M2_HOLGURA_HELPER_SEC_DEFAULT = 1.0f;
constexpr float M2_HOLGURA_HELPER_SEC_MIN     = 0.05f;
constexpr float M2_HOLGURA_HELPER_SEC_MAX     = 60.0f;
constexpr uint32_t BUFFER_FULL_ON_FILTER_MS  = 80;
constexpr uint32_t BUFFER_FULL_OFF_FILTER_MS = 200;
constexpr uint32_t BUFFER_FULL_GLITCH_MS     = 40;
constexpr uint32_t TENSION_REVERSE_FILTER_MS = 80;

// ====================== FEEDER M2 / Tfeed ======================
constexpr float M2_TRIGGER_FEED_DEFAULT = 2.0f;
constexpr float M2_TRIGGER_FEED_MIN     = 0.1f;
constexpr float M2_TRIGGER_FEED_MAX     = 60.0f;
constexpr uint8_t M2_TRIGGER_CORE          = 1;

// ====================== REFILL / NVS / PEER ======================
constexpr uint32_t REFILL_PULSE_MS_DEFAULT = 1000;
constexpr uint32_t REFILL_PULSE_MS_MIN     = 200;
constexpr uint32_t REFILL_PULSE_MS_MAX     = 10000;
constexpr uint8_t  TRIGGER_ID_HIST         = 8;
constexpr const char* PREFS_NS             = "prefeeder2";
