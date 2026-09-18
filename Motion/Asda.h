#pragma once

#include <Arduino.h>

// ====================== Modbus RS-485 (Delta ASDA-B3) ======================
static const uint8_t  MODBUS_ID   = 0x01;
static const uint32_t MODBUS_BAUD = 9600;

static const int RS485_RX_PIN = 16;
static const int RS485_TX_PIN = 17;
static const int RS485_DE_RE_PIN = -1;

static const uint32_t MODBUS_PRE_TX_GAP_MS = 10;
static const uint32_t MODBUS_FRAME_GAP_US  = 5000;
static const uint32_t RESPONSE_TIMEOUT_MS  = 300;

static const bool MODBUS_DEBUG = false;

// ====================== Movimiento / homing ======================
static const char*    DEFAULT_HOME_DIRECTION  = "F";
static const uint16_t DEFAULT_HOME_TORQUE_PCT = 10;
static const uint16_t DEFAULT_HOME_TIME_MS    = 150;
static const float    DEFAULT_HOME_SPEED_RPM  = 10.0f;
static const float    DEFAULT_MOVE_SPEED_RPM  = 1200.0f;
static const uint16_t MOVE_RPM_MIN            = 400;
static const uint16_t MOVE_RPM_MAX            = 3000;

static const uint32_t TIMEOUT_MARGIN_MS       = 1000;
static const uint32_t TIMEOUT_MIN_MS          = 2000;
static const uint32_t TIMEOUT_MAX_MS          = 600000;
static const float    LINEAR_MM_PER_MOTOR_REV = 5.0f;
static const float    HOME_MAX_TRAVEL_MM      = 120.0f;

static const uint32_t MOTION_POLL_MIN_MS = 40;
static const uint32_t STATUS_LIVE_MIN_MS = 200;
// Tras P2.030=1 (Servo ON) el drive necesita asentar antes de aceptar PR/home.
// Si el tiempo es muy corto y no entra HOME, incrementar estos ms.
static const uint32_t ASDA_SERVO_ON_SETTLE_MS = 2000;

// ====================== Calibración lineal ======================
static const uint32_t LINEAR_EGEAR_N          = 1;
static const uint32_t FACTORY_REF_STEPS       = 770;
static const float    FACTORY_LINEAR_ACTUATOR_MM = 45.0f;
static const float    FACTORY_STEPS_PER_MM    =
    ((float)FACTORY_REF_STEPS / FACTORY_LINEAR_ACTUATOR_MM);
static const float    STEPS_PER_MM_MIN        = 1.0f;
static const float    STEPS_PER_MM_MAX        = 200.0f;
static const float    OFFSET_STEPS_MIN        = -5000.0f;
static const float    OFFSET_STEPS_MAX        = 5000.0f;

static const char*    ASDA_PREFS_NS           = "asda";

// ====================== Registros ASDA-B3 ======================
static const uint16_t REG_P1_087 = 0x01AE;
static const uint16_t REG_P1_088 = 0x01B0;
static const uint16_t REG_P2_030 = 0x023C;
static const uint16_t REG_P3_000 = 0x0300;
static const uint16_t REG_P3_001 = 0x0302;
static const uint16_t REG_P3_002 = 0x0304;
static const uint16_t REG_P3_007 = 0x030E;
static const uint16_t REG_P5_004 = 0x0508;
static const uint16_t REG_P5_005 = 0x050A;
static const uint16_t REG_P5_006 = 0x050C;
static const uint16_t REG_P5_007 = 0x050E;
static const uint16_t REG_P5_016 = 0x0520;
static const uint16_t REG_P5_060 = 0x0578;
static const uint16_t REG_P6_000 = 0x0600;
static const uint16_t REG_P6_001 = 0x0602;
static const uint16_t REG_P6_002 = 0x0604;
static const uint16_t REG_P6_003 = 0x0606;

static const uint32_t PR1_ABSOLUTE_DEF = 0x00000002UL;

// ====================== Protocolo TCP: MotionStates.h ======================
#include "MotionStates.h"
