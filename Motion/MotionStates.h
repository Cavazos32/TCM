#pragma once

#include <stdint.h>
#include <stdio.h>

// =============================================================================
// Motion — opcodes TCP (ASDA, Encoder, Feeder, estados, errores detalle EXXX)
// Ref: Doc/TCM - D.xlsx · Opcodes / Error list
//
// Estados (MOT_ST_*) = aviso de estado; NO son códigos EXXX.
// Errores detalle (MOT_ERR_*) = EXXX hacia HMI/Main únicamente.
// Clase: C1 stop all · C2 pause · C3 finish step then stop
// =============================================================================

// --- ASDA (comandos / eventos) ---
enum MotionAsdaByte : uint8_t {
  ASDA_CMD_HOME      = 0x01,  // HomeASDA()
  ASDA_CMD_STOP      = 0x02,  // StopASDA()
  ASDA_CMD_OFF       = 0x03,  // OffASDA()
  ASDA_CMD_ON        = 0x04,  // OnASDA()
  ASDA_CMD_MOVE_ABS  = 0x05,  // ABSPositionASDA()
  ASDA_TX_REACHED    = 0x06,  // ReachedASDA()
  ASDA_CMD_MOVE_ZERO = 0x07,  // HpASDA()
  ASDA_CMD_STATUS    = 0x08,  // GetStatus()
};

// --- Estados módulo Motion (no EXXX) ---
enum MotionState : uint8_t {
  MOT_ST_INIT   = 0x09,  // InitState
  MOT_ST_IDLE   = 0x0A,  // IdleState
  MOT_ST_BUSY   = 0x0B,  // BusyState
  MOT_ST_ERROR  = 0x0C,  // ErrorState — estado, no E007
  MOT_ST_STOP   = 0x0D,  // StoprState
  MOT_ST_RETURN = 0x0E,  // ReturnState
};

#define ASDA_TX_INIT   MOT_ST_INIT
#define ASDA_TX_IDLE   MOT_ST_IDLE
#define ASDA_TX_BUSY   MOT_ST_BUSY
#define ASDA_TX_ERROR  MOT_ST_ERROR
#define ASDA_TX_STOP   MOT_ST_STOP
#define ASDA_TX_RETURN MOT_ST_RETURN
#define ASDA_CMD_RESUME MOT_ST_RETURN

// --- Encoder (comandos / eventos) ---
enum MotionEncByte : uint8_t {
  ENC_CMD_MEASURE_R = 0x0F,  // GetMeasured() R
  ENC_CMD_SET0_R    = 0x10,  // Set0() R
  ENC_TX_ERROR_R    = 0x11,  // ErrorER() — E001 C3
  ENC_CMD_MEASURE_L = 0x17,  // GetMeasured() L
  ENC_CMD_SET0_L    = 0x18,  // Set0() L
  ENC_TX_ERROR_L    = 0x78,  // ErrorEL() — E046 C3
};
#define ENC_CMD_SET0   ENC_CMD_SET0_R
#define ENC_TX_ERROR   ENC_TX_ERROR_R  // alias histórico

// --- Feeder ---
enum MotionFeedByte : uint8_t {
  FEED_CMD_FEED_R     = 0x12,  // StartFeedR()
  FEED_CMD_FEED_L     = 0x13,  // StartFeedL()
  FEED_TX_LENGTH_OK_L = 0x14,  // LenghtOK() L
  FEED_TX_LENGTH_NG_L = 0x15,  // LenghtNG_L() — E002 C3
  FEED_TX_LENGTH_OK_R = 0x4A,  // LenghtOK_R()
  FEED_TX_LENGTH_NG_R = 0x4B,  // LenghtNG_R() — E003 C3
};

// --- Reset errores módulo ---
enum MotionResetByte : uint8_t {
  MOT_CMD_RESET_ERR = 0x16,  // Reset()
};
#define ASDA_CMD_RESET_ERR MOT_CMD_RESET_ERR

// --- Errores detalle Motion → HMI (EXXX). No enviar a otros esclavos. ---
enum MotionError : uint8_t {
  // Existentes / sensores
  MOT_ERR_ENCODER_R         = 0x11,  // E001 C3 · ErrorER()
  MOT_ERR_LENGTH_NG_L       = 0x15,  // E002 C3 · LenghtNG_L()
  MOT_ERR_LENGTH_NG_R       = 0x4B,  // E003 C3 · LenghtNG_R()
  MOT_ERR_LASER_R           = 0x4D,  // E004 C2 · LaserR() — Keyence LR-X R sin material
  MOT_ERR_LASER_L           = 0x4E,  // E005 C2 · LaserL() — Keyence LR-X L sin material
  MOT_ERR_EXHAUST           = 0x4F,  // E006 C1 · Exhaust() — Safety exhaust / E-Stop
  // Actuador lineal / ASDA
  MOT_ERR_ACTUATOR_TARGET   = 0x59,  // E015 C1 · ActuatorTargetError()
  MOT_ERR_ASDA_MODBUS       = 0x5A,  // E016 C1 · AsdaModbusError()
  MOT_ERR_ACTUATOR_HOME     = 0x5B,  // E017 C1 · ActuatorHomeError()
  MOT_ERR_ACTUATOR_OUT_ZONE = 0x5C,  // E018 C1 · ActuatorOutOfZone()
  MOT_ERR_ACTUATOR_ABORTED  = 0x5D,  // E019 C1 · ActuatorAborted()
  MOT_ERR_RESET_WHILE_MOVING = 0x5E, // E020 C1 · ResetWhileMoving()
  MOT_ERR_ACTUATOR_NO_RESP  = 0x60,  // E022 C1 · ActuatorNoResponse()
  // Feeder / CAN
  MOT_ERR_FEED_CAN_NO_RESP  = 0x61,  // E023 C1 · FeedCanNoResponse()
  MOT_ERR_FEED_L_NEG_TARGET = 0x62,  // E024 C3 · FeedLNegTarget()
  MOT_ERR_FEED_R_NEG_TARGET = 0x63,  // E025 C3 · FeedRNegTarget()
  MOT_ERR_FEED_L_NO_FB      = 0x64,  // E026 C2 · FeedLNoFeedback()
  MOT_ERR_FEED_R_NO_FB      = 0x65,  // E027 C2 · FeedRNoFeedback()
  MOT_ERR_ENCODER_NO_PULSES = 0x66,  // E028 C2 · EncoderNoPulses()
  MOT_ERR_TOLERANCE_WINDOW  = 0x67,  // E029 C2 · ToleranceWindowError()
  MOT_ERR_FEED_DIR_CW       = 0x68,  // E030 C1 · FeedDirCwError()
  MOT_ERR_FEED_TIMEOUT      = 0x69,  // E031 C1 · FeedTimeout()
  MOT_ERR_ENCODER_L         = 0x78,  // E046 C3 · ErrorEL()
};
#define MOT_ERR_ENCODER MOT_ERR_ENCODER_R  // alias histórico

#define MOT_OK_LENGTH_L FEED_TX_LENGTH_OK_L
#define MOT_OK_LENGTH_R FEED_TX_LENGTH_OK_R

// Clase de error (HMI aplica política; firmware solo etiqueta)
#ifndef ERR_CLASS_DEFINED
#define ERR_CLASS_DEFINED
enum ErrClass : uint8_t {
  ERR_CLASS_NONE = 0,
  ERR_CLASS_C1   = 1,  // Stop all + reset/validate + confirm + home
  ERR_CLASS_C2   = 2,  // Pause; resume+reset → desde step 0
  ERR_CLASS_C3   = 3,  // Finish step; resume+reset → reintentar
};
#endif

static inline ErrClass motErrClass(uint8_t b) {
  switch (b) {
    case MOT_ERR_EXHAUST:
    case MOT_ERR_ACTUATOR_TARGET:
    case MOT_ERR_ASDA_MODBUS:
    case MOT_ERR_ACTUATOR_HOME:
    case MOT_ERR_ACTUATOR_OUT_ZONE:
    case MOT_ERR_ACTUATOR_ABORTED:
    case MOT_ERR_RESET_WHILE_MOVING:
    case MOT_ERR_ACTUATOR_NO_RESP:
    case MOT_ERR_FEED_CAN_NO_RESP:
    case MOT_ERR_FEED_DIR_CW:
    case MOT_ERR_FEED_TIMEOUT:
      return ERR_CLASS_C1;
    case MOT_ERR_LASER_R:
    case MOT_ERR_LASER_L:
    case MOT_ERR_FEED_L_NO_FB:
    case MOT_ERR_FEED_R_NO_FB:
    case MOT_ERR_ENCODER_NO_PULSES:
    case MOT_ERR_TOLERANCE_WINDOW:
      return ERR_CLASS_C2;
    case MOT_ERR_ENCODER_R:
    case MOT_ERR_LENGTH_NG_L:
    case MOT_ERR_LENGTH_NG_R:
    case MOT_ERR_FEED_L_NEG_TARGET:
    case MOT_ERR_FEED_R_NEG_TARGET:
    case MOT_ERR_ENCODER_L:
      return ERR_CLASS_C3;
    default:
      return ERR_CLASS_NONE;
  }
}

static inline bool motIsState(uint8_t b) {
  return b >= MOT_ST_INIT && b <= MOT_ST_RETURN;
}

static inline bool motIsAsdaCmd(uint8_t b) {
  return b >= ASDA_CMD_HOME && b <= ASDA_CMD_STATUS;
}

static inline bool motIsEncByte(uint8_t b) {
  return b == ENC_CMD_MEASURE_R || b == ENC_CMD_SET0_R || b == ENC_TX_ERROR_R
      || b == ENC_CMD_MEASURE_L || b == ENC_CMD_SET0_L || b == ENC_TX_ERROR_L;
}

static inline bool motIsFeedByte(uint8_t b) {
  return b == FEED_CMD_FEED_R || b == FEED_CMD_FEED_L
      || b == FEED_TX_LENGTH_OK_L || b == FEED_TX_LENGTH_NG_L
      || b == FEED_TX_LENGTH_OK_R || b == FEED_TX_LENGTH_NG_R;
}

static inline bool motIsDetailError(uint8_t b) {
  return motErrClass(b) != ERR_CLASS_NONE;
}

static inline const char* motErrExxx(uint8_t b) {
  switch (b) {
    case MOT_ERR_ENCODER_R: return "E001";
    case MOT_ERR_LENGTH_NG_L: return "E002";
    case MOT_ERR_LENGTH_NG_R: return "E003";
    case MOT_ERR_LASER_R: return "E004";
    case MOT_ERR_LASER_L: return "E005";
    case MOT_ERR_EXHAUST: return "E006";
    case MOT_ERR_ACTUATOR_TARGET: return "E015";
    case MOT_ERR_ASDA_MODBUS: return "E016";
    case MOT_ERR_ACTUATOR_HOME: return "E017";
    case MOT_ERR_ACTUATOR_OUT_ZONE: return "E018";
    case MOT_ERR_ACTUATOR_ABORTED: return "E019";
    case MOT_ERR_RESET_WHILE_MOVING: return "E020";
    case MOT_ERR_ACTUATOR_NO_RESP: return "E022";
    case MOT_ERR_FEED_CAN_NO_RESP: return "E023";
    case MOT_ERR_FEED_L_NEG_TARGET: return "E024";
    case MOT_ERR_FEED_R_NEG_TARGET: return "E025";
    case MOT_ERR_FEED_L_NO_FB: return "E026";
    case MOT_ERR_FEED_R_NO_FB: return "E027";
    case MOT_ERR_ENCODER_NO_PULSES: return "E028";
    case MOT_ERR_TOLERANCE_WINDOW: return "E029";
    case MOT_ERR_FEED_DIR_CW: return "E030";
    case MOT_ERR_FEED_TIMEOUT: return "E031";
    case MOT_ERR_ENCODER_L: return "E046";
    default: return "";
  }
}

static inline const char* motErrDesc(uint8_t b) {
  switch (b) {
    case MOT_ERR_ENCODER_R: return "Encoder R; no cambio de valor";
    case MOT_ERR_LENGTH_NG_L: return "Encoder L; longitud fuera de tolerancia";
    case MOT_ERR_LENGTH_NG_R: return "Encoder R; longitud fuera de tolerancia";
    case MOT_ERR_LASER_R: return "Sensor R; no detecto material";
    case MOT_ERR_LASER_L: return "Sensor L; no detecto material";
    case MOT_ERR_EXHAUST: return "Safety exhaust";
    case MOT_ERR_ACTUATOR_TARGET: return "Actuador lineal no llego al destino";
    case MOT_ERR_ASDA_MODBUS: return "Fallo de comunicacion Modbus con ASDA";
    case MOT_ERR_ACTUATOR_HOME: return "Actuador lineal no realizo homing correctamente";
    case MOT_ERR_ACTUATOR_OUT_ZONE: return "Actuador lineal fuera de zona segura";
    case MOT_ERR_ACTUATOR_ABORTED: return "Movimiento actuador lineal abortado / fallido";
    case MOT_ERR_RESET_WHILE_MOVING: return "No se puede resetear error en movimiento";
    case MOT_ERR_ACTUATOR_NO_RESP: return "Actuador lineal no responde";
    case MOT_ERR_FEED_CAN_NO_RESP: return "Driver de servo feed no responde por CAN";
    case MOT_ERR_FEED_L_NEG_TARGET: return "FEED: target L + offset negativo";
    case MOT_ERR_FEED_R_NEG_TARGET: return "FEED: target R + offset negativo";
    case MOT_ERR_FEED_L_NO_FB: return "Sin feedback de posicion Feeder 6064 L";
    case MOT_ERR_FEED_R_NO_FB: return "Sin feedback de posicion Feeder 6064 R";
    case MOT_ERR_ENCODER_NO_PULSES: return "Encoder sin incremento tras comando feed";
    case MOT_ERR_TOLERANCE_WINDOW: return "Lectura fuera de ventana de tolerancia";
    case MOT_ERR_FEED_DIR_CW: return "FEED: sentido horario";
    case MOT_ERR_FEED_TIMEOUT: return "FEED: timeout";
    case MOT_ERR_ENCODER_L: return "Encoder L; no cambio de valor";
    default: return "";
  }
}

/** UI normativa: "EXXX: Motion, Descripción" */
static inline void motErrFormatUi(char* buf, size_t n, uint8_t b) {
  if (!buf || n == 0) return;
  const char* ex = motErrExxx(b);
  const char* desc = motErrDesc(b);
  if (ex && ex[0] && desc && desc[0])
    snprintf(buf, n, "%s: Motion, %s", ex, desc);
  else
    buf[0] = '\0';
}

static inline const char* motStateName(uint8_t b) {
  switch (b) {
    case MOT_ST_INIT:   return "InitState";
    case MOT_ST_IDLE:   return "IdleState";
    case MOT_ST_BUSY:   return "BusyState";
    case MOT_ST_ERROR:  return "ErrorState";
    case MOT_ST_STOP:   return "StoprState";
    case MOT_ST_RETURN: return "ReturnState";
    default:            return "unknown";
  }
}
