#pragma once

#include <stdint.h>

// =============================================================================
// Motion — opcodes TCP (ASDA, Encoder, Feeder, estados, errores)
// Ref: Doc/opcode_reference_list.md · tabla Motion
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

// --- Estados módulo Motion ---
enum MotionState : uint8_t {
  MOT_ST_INIT   = 0x09,  // InitState
  MOT_ST_IDLE   = 0x0A,  // IdleState
  MOT_ST_BUSY   = 0x0B,  // BusyState
  MOT_ST_ERROR  = 0x0C,  // ErrorState
  MOT_ST_STOP   = 0x0D,  // StoprState
  MOT_ST_RETURN = 0x0E,  // ReturnState
};

// Aliases históricos (Motion.ino / Asda) — mismos bytes que MOT_ST_*
#define ASDA_TX_INIT   MOT_ST_INIT
#define ASDA_TX_IDLE   MOT_ST_IDLE
#define ASDA_TX_BUSY   MOT_ST_BUSY
#define ASDA_TX_ERROR  MOT_ST_ERROR
#define ASDA_TX_STOP   MOT_ST_STOP
#define ASDA_TX_RETURN MOT_ST_RETURN
#define ASDA_CMD_RESUME MOT_ST_RETURN  // reanudar tras stop (mismo 0x0E)

// --- Encoder ---
enum MotionEncByte : uint8_t {
  ENC_CMD_MEASURE_R = 0x0F,  // GetMeasured() R
  ENC_CMD_SET0_R    = 0x10,  // Set0() R
  ENC_TX_ERROR      = 0x11,  // Error() — sin cambio en lectura
  ENC_CMD_MEASURE_L = 0x17,  // GetMeasured() L
  ENC_CMD_SET0_L    = 0x18,  // Set0() L
};
#define ENC_CMD_SET0 ENC_CMD_SET0_R

// --- Feeder ---
enum MotionFeedByte : uint8_t {
  FEED_CMD_FEED_R     = 0x12,  // StartFeedR()
  FEED_CMD_FEED_L     = 0x13,  // StartFeedL()
  FEED_TX_LENGTH_OK_L = 0x14,  // LenghtOK() L
  FEED_TX_LENGTH_NG_L = 0x15,  // LenghtNG_L()
  FEED_TX_LENGTH_OK_R = 0x4A,  // LenghtOK_R()
  FEED_TX_LENGTH_NG_R = 0x4B,  // LenghtNG_R()
};

// --- Reset errores módulo ---
enum MotionResetByte : uint8_t {
  MOT_CMD_RESET_ERR = 0x16,  // Reset()
};
#define ASDA_CMD_RESET_ERR MOT_CMD_RESET_ERR

// --- Errores / sensores (aliases + láser / exhaust) ---
enum MotionError : uint8_t {
  MOT_ERR_ENCODER     = 0x11,  // ENC_TX_ERROR
  MOT_ERR_LENGTH_NG_L = 0x15,  // FEED_TX_LENGTH_NG_L
  MOT_ERR_LENGTH_NG_R = 0x4B,  // FEED_TX_LENGTH_NG_R
  MOT_ERR_LASER_R     = 0x4D,  // Pres.LaserR() / LaserR()
  MOT_ERR_LASER_L     = 0x4E,  // Pres.LaserL() / LaserL()
  MOT_ERR_EXHAUST     = 0x4F,  // Exhaust() — desfoga aire
};

#define MOT_OK_LENGTH_L FEED_TX_LENGTH_OK_L
#define MOT_OK_LENGTH_R FEED_TX_LENGTH_OK_R

static inline bool motIsState(uint8_t b) {
  return b >= MOT_ST_INIT && b <= MOT_ST_RETURN;
}

static inline bool motIsAsdaCmd(uint8_t b) {
  return b >= ASDA_CMD_HOME && b <= ASDA_CMD_STATUS;
}

static inline bool motIsEncByte(uint8_t b) {
  return b == ENC_CMD_MEASURE_R || b == ENC_CMD_SET0_R || b == ENC_TX_ERROR
      || b == ENC_CMD_MEASURE_L || b == ENC_CMD_SET0_L;
}

static inline bool motIsFeedByte(uint8_t b) {
  return b == FEED_CMD_FEED_R || b == FEED_CMD_FEED_L
      || b == FEED_TX_LENGTH_OK_L || b == FEED_TX_LENGTH_NG_L
      || b == FEED_TX_LENGTH_OK_R || b == FEED_TX_LENGTH_NG_R;
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
