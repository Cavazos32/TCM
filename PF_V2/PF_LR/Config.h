#pragma once

#include <Arduino.h>

// ====================== LADO L / R ======================
// Comentada = L | descomentada = R
#define PREFEEDER_SIDE_RIGHT

#ifndef PREFEEDER_SIDE_RIGHT
#define PREFEEDER_SIDE_TAG  "L"
#define PREFEEDER_SIDE_ROLE "prefeeder_L"
static const IPAddress STA_IP(10, 10, 32, 101);
constexpr uint16_t SERVO_PWM_ACTIVE_US = 800;
constexpr int8_t SERVO_SPEED_DELTA_SIGN = -1;
constexpr uint8_t  FAULT_CODE_BASE     = 20;  
#else
#define PREFEEDER_SIDE_TAG  "R"
#define PREFEEDER_SIDE_ROLE "prefeeder_R"
static const IPAddress STA_IP(10, 10, 32, 102);
constexpr uint16_t SERVO_PWM_ACTIVE_US = 2000;
constexpr int8_t SERVO_SPEED_DELTA_SIGN = 1;
constexpr uint8_t  FAULT_CODE_BASE     = 30;  
#endif

// ====================== DEBUG ======================
#ifndef SERIAL_VERBOSE
#define SERIAL_VERBOSE 1
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
constexpr uint8_t PIN_SENSOR_BUFFER_FULL = 19;  // HIGH = activo
constexpr uint8_t PIN_SENSOR_BUFFER_MID  = 34;  // HIGH = activo; media altura (solo entrada)
constexpr uint8_t PIN_SENSOR_BUFFER_HIGH = 35;  // HIGH = activo; altura superior (solo entrada)
constexpr uint8_t PIN_SENSOR_TENSION     = 23;  // HIGH = activo
constexpr uint8_t PIN_SENSOR_BUFFER_MAX  = 21;  // HIGH = activo
constexpr uint8_t PIN_SENSOR_CILINDRO    = 25;
constexpr uint8_t PIN_SENSOR_HOSE_BELT   = 27;  // HIGH = cinta/manguera ausente
constexpr int PIN_DEREELER_PUL           = 32;
constexpr int PIN_DEREELER_MOSFET        = 33;
constexpr uint8_t DEREELER_DIR_CW_LEVEL  = LOW;
constexpr uint8_t DEREELER_DIR_CCW_LEVEL = HIGH;
constexpr int PIN_FEEDER_PUL             = 18;  // DIR/EN fijos en driver
constexpr uint8_t PIN_SERVO_PWM          = 26;
constexpr int PIN_LIDAR_RX               = 16;  // UART2 RX ← TX del TFmini
constexpr int PIN_LIDAR_TX               = 17;  // UART2 TX → RX del TFmini
constexpr uint32_t LIDAR_BAUD            = 115200;
constexpr uint32_t LIDAR_READING_FRESH_MS = 400;
// 1 = tras el Tfeed seguir hasta la distancia. 0 = trigger solo por tiempo.
#define PF_LIDAR_HOLGURA 0

// ====================== SERVO ======================
constexpr uint16_t SERVO_PWM_MIN_US       = 500;
constexpr uint16_t SERVO_PWM_MAX_US       = 2500;
constexpr uint16_t SERVO_PWM_NEUTRAL_US     = 1500;
constexpr uint8_t  SERVO_LEDC_BITS        = 14;
constexpr uint8_t  SERVO_LEDC_CHANNEL     = 7;
constexpr uint32_t DEREELER_START_DELAY_MS = 100;
constexpr uint32_t SERVO_CREEP_RUN_MS_DEFAULT  = 200;
constexpr uint32_t SERVO_CREEP_STOP_MS_DEFAULT = 50;
constexpr uint32_t SERVO_CREEP_RUN_MS_MIN      = 50;
constexpr uint32_t SERVO_CREEP_RUN_MS_MAX      = 2000;
constexpr uint32_t SERVO_CREEP_STOP_MS_MIN     = 20;
constexpr uint32_t SERVO_CREEP_STOP_MS_MAX     = 500;

// ====================== RED / WiFi ======================
// L: http://10.10.32.101 | R: http://10.10.32.102 | Master: http://10.10.32.100
// Master TCP → L/R :8765 (esclavos independientes; ver PreFeeder_Master/master_cmds.h)
static const char* const WIFI_SSID = "R&D_TCM";
static const char* const WIFI_PASS = "TCM2026!r&d";
static const IPAddress STA_GW(10, 10, 32, 72);
static const IPAddress STA_MASK(255, 255, 255, 0);
// DNS: mismo criterio que un PC en la LAN (router primero). 8.8.8.8 solo como respaldo.
static const IPAddress STA_DNS1(10, 10, 32, 72);
static const IPAddress STA_DNS2(8, 8, 8, 8);
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
constexpr float TENSION_FAULT_SEC_DEFAULT = 10.0f;
constexpr float TENSION_FAULT_SEC_MIN     = 1.0f;
constexpr float TENSION_FAULT_SEC_MAX     = 60.0f;
constexpr float TENSION_REVERSE_RPM_DEFAULT = 30.0f;
constexpr float TENSION_REVERSE_RPM_MIN     = MOTOR_RPM_MIN;
constexpr float TENSION_REVERSE_RPM_MAX     = MOTOR_RPM_MAX;

// ====================== BUFFER ======================
constexpr float BUFFER_REFILL_FAULT_SEC = 10.0f;
constexpr uint32_t BUFFER_FULL_ON_FILTER_MS  = 50;
constexpr uint32_t BUFFER_FULL_OFF_FILTER_MS = 100;
constexpr uint32_t BUFFER_FULL_GLITCH_MS     = 50;
constexpr uint32_t BUFFER_MID_FILTER_MS      = 50;
constexpr uint32_t BUFFER_HIGH_FILTER_MS     = 50;
constexpr uint32_t TENSION_REVERSE_FILTER_MS = 50;

// Impulso de llenado al cruzar el sensor de media altura.
constexpr float BUFFER_MID_BOOST_RPM_DEFAULT       = 120.0f;
constexpr float BUFFER_MID_BOOST_SEC_DEFAULT       = 3.0f;
constexpr float BUFFER_MID_BOOST_SEC_MIN           = 0.1f;
constexpr float BUFFER_MID_BOOST_SEC_MAX           = 60.0f;
constexpr int16_t BUFFER_MID_SERVO_DELTA_DEFAULT_US = SERVO_SPEED_DELTA_SIGN * 100;
constexpr int16_t BUFFER_MID_SERVO_DELTA_MIN_US     = -1000;
constexpr int16_t BUFFER_MID_SERVO_DELTA_MAX_US     = 1000;

// Impulso fuerte al cruzar el segundo sensor superior.
constexpr float BUFFER_HIGH_BOOST_RPM_DEFAULT        = 180.0f;
constexpr float BUFFER_HIGH_BOOST_SEC_DEFAULT        = 5.0f;
constexpr float BUFFER_HIGH_BOOST_SEC_MIN            = 0.1f;
constexpr float BUFFER_HIGH_BOOST_SEC_MAX            = 60.0f;
constexpr int16_t BUFFER_HIGH_SERVO_DELTA_DEFAULT_US = SERVO_SPEED_DELTA_SIGN * 200;
constexpr int16_t BUFFER_HIGH_SERVO_DELTA_MIN_US     = -1000;
constexpr int16_t BUFFER_HIGH_SERVO_DELTA_MAX_US     = 1000;

// ====================== FEEDER M2 / Tfeed ======================
constexpr float M2_TRIGGER_FEED_DEFAULT = 2.0f;
constexpr float M2_TRIGGER_FEED_MIN     = 0.1f;
constexpr float M2_TRIGGER_FEED_MAX     = 60.0f;
constexpr uint8_t M2_TRIGGER_CORE          = 1;
// Tras el Tfeed: seguir con el feeder hasta distancia < umbral, o este tope.
constexpr uint16_t LIDAR_TARGET_CM_DEFAULT = 30;
constexpr uint16_t LIDAR_TARGET_CM_MIN     = 1;
constexpr uint16_t LIDAR_TARGET_CM_MAX     = 1200;
constexpr float    LIDAR_FEED_SEC_DEFAULT  = 3.0f;
constexpr float    LIDAR_FEED_SEC_MIN      = 0.1f;
constexpr float    LIDAR_FEED_SEC_MAX      = 30.0f;

// ====================== REFILL / NVS / PEER ======================
constexpr uint32_t REFILL_PULSE_MS_DEFAULT = 1000;
constexpr uint32_t REFILL_PULSE_MS_MIN     = 200;
constexpr uint32_t REFILL_PULSE_MS_MAX     = 10000;
constexpr uint8_t  TRIGGER_ID_HIST         = 8;
constexpr const char* PREFS_NS             = "prefeeder2";

// ====================== FIRMWARE / OTA ======================
// Como Home: panel → GitHub Releases (HTTPS). Release: make_release.py + tag vX.Y.Z.
// Red local IDE: ArduinoOTA (OTA_PASSWORD). Requiere Wi‑Fi con internet hacia github.com.
#define FW_VERSION "0.1.7"
#define OTA_PASSWORD "TCM_PF2026"
#ifndef PREFEEDER_SIDE_RIGHT
#define OTA_HOSTNAME "prefeeder-L"
#define OTA_MANIFEST_URL "https://github.com/Cavazos32/TCM/releases/latest/download/pf_lr_L_version.json"
#else
#define OTA_HOSTNAME "prefeeder-R"
#define OTA_MANIFEST_URL "https://github.com/Cavazos32/TCM/releases/latest/download/pf_lr_R_version.json"
#endif
#define OTA_CHECK_INTERVAL_MS (6UL * 60UL * 60UL * 1000UL)
#define OTA_BOOT_DELAY_MS (30UL * 1000UL)
#define OTA_RETRY_MS (15UL * 60UL * 1000UL)
// 1 = prueba en banco sin verificar certificado HTTPS (no dejar en producción).
#define OTA_TLS_INSECURE 0
