#pragma once

#include <stdint.h>

// ====================== CAN servos feeder (ESP32 TWAI) ======================
#define CAN_TX_PIN                  4
#define CAN_RX_PIN                  5
#define CAN_BITRATE_KBPS            500
#define CAN_RX_DRAIN_MAX            32
#define CAN_INIT_MAX_RETRIES        3
#define CAN_INIT_RETRY_MS           400
#define SERVO_POWERUP_MS            1000
#define SERVO_SETUP_MAX_RETRIES     2
#define SERVO_HOME_PP               42500

// ====================== CiA402 COB-IDs ======================
#define CAN_NMT_ID                  0x000u
#define CAN_SDO_TX_BASE             0x600u
#define CAN_SDO_RX_BASE             0x580u

#define SERVO_NODE_L                2
#define SERVO_NODE_R                1
#define SERVO_CAN_TX_L              (CAN_SDO_TX_BASE + SERVO_NODE_L)
#define SERVO_CAN_TX_R              (CAN_SDO_TX_BASE + SERVO_NODE_R)
#define SERVO_CMD_SIGN_L            (-1)
#define SERVO_CMD_SIGN_R            (1)

#define SERVO_ENC_INC_PER_REV       131072u
#define SERVO_MAX_RPM               3000u
#define SERVO_MAX_PPS               ((uint32_t)((uint64_t)SERVO_MAX_RPM * SERVO_ENC_INC_PER_REV / 60u))

// ====================== CiA402 objetos (OD) ======================
#define SERVO_OD_CONTROL_WORD       0x6040u
#define SERVO_OD_STATUS_WORD        0x6041u
#define SERVO_OD_MODE_OF_OPERATION  0x6060u
#define SERVO_OD_POSITION_ACTUAL    0x6064u
#define SERVO_OD_TARGET_POSITION    0x607Au
#define SERVO_OD_MAX_PROFILE_VEL    0x607Fu
#define SERVO_OD_PROFILE_VELOCITY   0x6081u
#define SERVO_OD_PROFILE_ACCEL      0x6083u
#define SERVO_OD_PROFILE_DECEL      0x6084u
#define SERVO_OD_QUICK_STOP_DECEL   0x6085u
#define SERVO_OD_HALT_OPTION        0x605Du

// ====================== CiA402 control / status word ======================
#define SERVO_CW_HALT_HOLD          0x010Fu
#define SERVO_CW_RESET_HALT         0x012Fu
#define SERVO_CW_NEW_ABS_HALT       0x013Fu
#define SERVO_CW_RUN_ABSOLUTE       0x003Fu
#define SERVO_SW_FAULT_BIT          0x0008u
#define SERVO_SW_MASK_OP            0x006Fu
#define SERVO_SW_OP_ENABLED         0x0027u
#define SERVO_SW_TARGET_REACHED     0x0400u

// ====================== TWAI / CAN timeouts ======================
#define CAN_TX_TIMEOUT_MS           50
#define CAN_STATUS_POLL_MS          80
#define CAN_STATUS_TIMEOUT_MS       500
#define CAN_STATUS_VERIFY_MS        600
#define CAN_STATUS_RETRY_DELAY_MS   5
#define CAN_SDO_TIMEOUT_MS          150
#define CAN_SDO_RX_TIMEOUT_MS       2
#define CAN_STATUS_RX_TIMEOUT_MS    5
#define CAN_HALT_TX_RETRIES         3

// ====================== Setup / NMT delays ======================
#define CAN_FAULT_RESET_DELAY_MS    300
#define CAN_NMT_PREOP_DELAY_MS      500
#define CAN_NMT_OP_DELAY_MS         500
#define CAN_CIA402_STEP_DELAY_MS    200
#define CAN_CIA402_SHORT_DELAY_MS   50
#define CAN_CIA402_MED_DELAY_MS     100
#define CAN_HOME_VEL_DELAY_MS       500
#define CAN_HOME_DONE_DELAY_MS      300

// ====================== Feed / alimentación ======================
#define FEED_NOMINAL_STEPS          3600
#define FEED_NOMINAL_MM             55.0f
#define FEED_STEPS_PER_MM_DEFAULT   ((float)FEED_NOMINAL_STEPS / FEED_NOMINAL_MM)

#define FEED_PULLEY_DIAM_MM         50.0f
#define FEED_ENC_COUNTS_PER_MM      ((float)SERVO_ENC_INC_PER_REV / (3.14159265358979323846f * FEED_PULLEY_DIAM_MM))

#define FEED_MM_S_MIN               10.0f
#define FEED_MM_S_DEFAULT           650.0f
#define FEED_MM_S_CORR_MIN          1.0f
#define FEED_SERVO_BASE_PP_DEFAULT  ((uint32_t)(FEED_MM_S_DEFAULT * FEED_STEPS_PER_MM_DEFAULT + 0.5f))
#define FEED_SERVO_BASE_PP_MIN      500
#define FEED_SERVO_BASE_PP_MAX      ((uint32_t)(((float)SERVO_MAX_PPS * FEED_STEPS_PER_MM_DEFAULT / FEED_ENC_COUNTS_PER_MM) + 0.5f))

#define FEED_TARGET_MM_DEFAULT      FEED_NOMINAL_MM
#define FEED_TARGET_MM_MIN          0.0f
#define FEED_TARGET_MM_MAX          200.0f
#define FEED_OFFSET_MM_MIN          -50.0f
#define FEED_OFFSET_MM_MAX          50.0f
#define FEED_CAL_COUNTS_PER_MM_MIN  (FEED_ENC_COUNTS_PER_MM * 0.5f)
#define FEED_CAL_COUNTS_PER_MM_MAX  (FEED_ENC_COUNTS_PER_MM * 1.5f)

#define FEED_TEST_CHUNK_DEFAULT     200
#define FEED_TEST_CHUNK_MIN         20
#define FEED_TEST_CHUNK_MAX         2000

#define FEED_SERVO_RAMP_MS          50
#define FEED_SERVO_DEC_RAMP_MS      100
#define FEED_SERVO_RAMP_MS_MIN      10u
#define FEED_SERVO_RAMP_MS_MAX      1000u
#define FEED_SERVO_ACC_MIN          100000u
#define FEED_SERVO_ACC_MAX          2000000u
#define FEED_HALT_OPTION_CODE       2
#define FEED_HALT_DECEL_PP          8000000u

// ≥ ENC_SETTLE_MS (250): si es menor, R (y a veces L) lee OM antes de settle → solo approach.
#define FEED_OM_SETTLE_MS           280
#define FEED_OM_TARGET_TOL_MM       0.5f   // legacy overview; Feed usa ventanas abajo
#define FEED_OM_CORR_RETRY_MAX      1      // 1 corrección (aprox. no cuenta)
#define FEED_OM_READ_RETRY_MAX      3
#define FEED_WAIT_TIMEOUT_MS        5000
#define FEED_OM_FEED_POLL_MS        40
#define FEED_OM_REQUIRE_NEGATIVE    1
#define FEED_OM_DIR_CHECK_MM        0.8f

// Ventanas Feed (TARGET fijo; no mezclar)
#define FEED_TARGET_FIXED_MM        55.0f
#define FEED_CONTROL_TOL_MM         1.0f   // 54–56 banda ideal (corrección apunta a 55)
#define FEED_GOOD_TOL_MM            2.0f   // 53–57 (referencia; aceptación = PHYS)
#define FEED_OM_PHYS_MIN_MM         50.0f  // aceptación producción + láser ON
#define FEED_OM_PHYS_MAX_MM         58.0f
#define FEED_OM_QUANTUM_MM          0.5f   // paso oficial omRoundMm; umbral OK |err|<=0.5 (no re-cuantizar)
#define FEED_APPROACH_PCT_DEFAULT   80.0f
#define FEED_APPROACH_PCT_MIN       50.0f
#define FEED_APPROACH_PCT_MAX       95.0f
#define FEED_MOVE_SPEED_PCT_DEFAULT 50.0f  // approach + corrección = % de vel nominal
#define FEED_MOVE_SPEED_PCT_MIN     10.0f
#define FEED_MOVE_SPEED_PCT_MAX    100.0f

// Tras corrección #1, si láser OFF: avanzar hasta ON o timeout (modo gated).
#define FEED_LASER_SEEK_MS_DEFAULT  1000u
#define FEED_LASER_SEEK_MS_MIN      200u
#define FEED_LASER_SEEK_MS_MAX      5000u
#define FEED_LASER_SEEK_POLL_MS     0u     // 0 = cada feedLoop (halt lo antes posible)
#define FEED_LASER_SEEK_DIST_MARGIN 1.25f  // recorrido ≥ vel×t × margen
#define FEED_LASER_HALT_BURST       2u     // reintentos CW Halt al flanco ON

#define FEED_VELOCITY_PP_DEFAULT    FEED_SERVO_BASE_PP_DEFAULT
#define FEED_PREFS_NS               "motion_feed"

// ====================== Feed runtime timing ======================
#define FEED_FAULT_REASON_MAX       96
#define FEED_CAN_RETRY_MS           15000
#define FEED_HALT_SETTLE_MS         30
#define FEED_WAIT_EXTRA_MS          30000
#define FEED_LOOP_YIELD_MS          2
#define FEED_SS_ABS_MARGIN_MS       80
#define FEED_SS_POLL_MIN_MS         40
#define FEED_SS_TR_POLL_MS          80
#define FEED_OM_READ_RETRY_DELAY_MS 50
#define FEED_CORR_MIN_MM            0.15f
#define FEED_PROFILE_SEARCH_ITERS   32
#define FEED_PROFILE_SEARCH_EPS_MM  0.05f
#define FEED_PROFILE_MARGIN_EPS_MM  0.001f
#define FEED_CAL_MEASURED_MIN_MM    0.1f

// ====================== Tipos feed ======================
enum FeedMode : uint8_t {
  FEED_MODE_BYPASS = 0,
  FEED_MODE_STEPS_SENSOR = 2
};

// FSM por lado (L y R independientes)
enum FeedSidePhase : uint8_t {
  FSP_IDLE = 0,
  FSP_APPROACH,
  FSP_HALT_SETTLE,  // halt emitido; espera FEED_HALT_SETTLE_MS sin delay()
  FSP_WAIT_SERVO,
  FSP_SETTLE,
  FSP_VALIDATE,
  FSP_CORRECTION,
  FSP_WAIT_SERVO_CORR,
  FSP_SETTLE_FINAL,
  FSP_VALIDATE_FINAL,
  FSP_LASER_SEEK,       // post-corr: avance hasta láser ON o timeout
  FSP_LASER_SEEK_HALT,  // halt por flanco ON; settle corto → SETTLE_FINAL
  FSP_DONE_OK,
  FSP_DONE_NG
};

// Resultado Validator (FEED_CORRECT solo interno; no se emite a HMI)
enum FeedValResult : uint8_t {
  FVR_NONE = 0,
  FVR_OK,
  FVR_CORRECT,
  FVR_NG,
  FVR_INCONSISTENT
};

// Alias legacy (status JSON / HTTP)
enum FeedPhase : uint8_t {
  FEED_IDLE = 0,
  FEED_RUNNING,
  FEED_CHUNK_DELAY,
  FEED_HOSE_DELAY,
  FEED_PARALLEL_START_DELAY,
  FEED_BYPASS,
  FEED_SS_SOLID,
  FEED_SS_TRANSITION,
  FEED_SS_SLOW,
  FEED_DONE,
  FEED_ERROR
};

struct FeedProfilePlan {
  float targetMm = 0.0f;
  float vReqMmS = 0.0f;
  float vMaxMmS = 0.0f;
  float accMmS2 = 0.0f;
  float decMmS2 = 0.0f;
  float dAccMm = 0.0f;
  float dDecMm = 0.0f;
  float dMinMm = 0.0f;
  float dCruiseMm = 0.0f;
  float marginMm = 0.0f;
  uint32_t velPp = 0;
  uint32_t accPp = 0;
  uint32_t decPp = 0;
  bool valid = false;
};

struct FeedEncPlan {
  bool ok = false;
  const char* error = nullptr;
  float effectiveTargetMm = 0.0f;
  int32_t targetCounts = 0;
};

// Protocolo TCP feeder: MotionStates.h
#include "MotionStates.h"

struct FeedTestReq {
  bool pending = false;
  float solidMmL = 0.0f;
  float solidMmR = 0.0f;
  int32_t chunkL = 0;
  int32_t chunkR = 0;
  bool withCut = false;
  int8_t onlySide = -1;
  uint32_t fastL = 0;
  uint32_t fastR = 0;
  uint16_t decRampL = FEED_SERVO_DEC_RAMP_MS;
  uint16_t decRampR = FEED_SERVO_DEC_RAMP_MS;
};
