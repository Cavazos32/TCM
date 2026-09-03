#include "Servo_Feed.h"
#include "FeederCan.h"
#include "driver/twai.h"
#include <Preferences.h>
#include <math.h>
#include <WebServer.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

extern WebServer server;

static void feedNotifyTcpResult(bool ok, int8_t onlySide);

static SemaphoreHandle_t canMutex = NULL;

bool canInitialized = false;
bool servoCanReady = false;

static bool isServoOperationEnabled(uint16_t statusWord)
{
  if (statusWord & SERVO_SW_FAULT_BIT) return false;
  return (statusWord & SERVO_SW_MASK_OP) == SERVO_SW_OP_ENABLED;
}

static bool canBusMotionBlocked()
{
  if (feedCalibrationTest) return false;
  return false;
}

static void canTwaiShutdown()
{
  twai_stop();
  twai_driver_uninstall();
}

static bool canTwaiTransmit(uint32_t id, uint8_t len, const byte* data)
{
  if (len > 8) len = 8;
  twai_message_t message = {};
  message.identifier = id;
  message.extd = 0;
  message.rtr = 0;
  message.data_length_code = len;
  for (uint8_t i = 0; i < len; i++)
    message.data[i] = data[i];
  return twai_transmit(&message, pdMS_TO_TICKS(CAN_TX_TIMEOUT_MS)) == ESP_OK;
}

static bool canTwaiReceive(uint32_t& idOut, uint8_t& lenOut, byte* dataOut, uint32_t timeoutMs)
{
  twai_message_t message;
  const TickType_t ticks = timeoutMs ? pdMS_TO_TICKS(timeoutMs) : 0;
  if (twai_receive(&message, ticks) != ESP_OK)
    return false;
  idOut = message.identifier & 0x7FFu;
  lenOut = message.data_length_code;
  if (lenOut > 8) lenOut = 8;
  for (uint8_t i = 0; i < lenOut; i++)
    dataOut[i] = message.data[i];
  return true;
}

bool sendCANMessage(unsigned long id, byte len, byte* data, const String& desc, bool logSuccess)
{
  if (!canInitialized) return false;
  if (canMutex) xSemaphoreTake(canMutex, portMAX_DELAY);
  const bool ok = canTwaiTransmit((uint32_t)id, len, data);
  if (canMutex) xSemaphoreGive(canMutex);
  if (!ok)
  {
    Serial.printf("CAN TX FAIL 0x%lX (%s)\n", id, desc.c_str());
    return false;
  }
  if (logSuccess)
    Serial.printf("CAN TX OK 0x%lX (%s)\n", id, desc.c_str());
  return true;
}

bool sendCanHaltImmediate(bool sideR)
{
  if (!canInitialized) return false;
  byte data[8] = {0x2B, 0x40, 0x60, 0x00, 0x0F, 0x01, 0x00, 0x00};
  const unsigned long id = sideR ? SERVO_CAN_TX_R : SERVO_CAN_TX_L;
  if (canMutex) xSemaphoreTake(canMutex, portMAX_DELAY);
  bool ok = false;
  for (uint8_t i = 0; i < CAN_HALT_TX_RETRIES; i++)
  {
    if (canTwaiTransmit((uint32_t)id, 8, data))
    {
      ok = true;
      break;
    }
  }
  if (canMutex) xSemaphoreGive(canMutex);
  return ok;
}

bool canReadStatusWord(uint8_t nodeId, uint16_t& statusOut, uint32_t timeoutMs)
{
  if (!canInitialized) return false;
  unsigned char data[] = {0x40, 0x41, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00};
  const unsigned long txId = CAN_SDO_TX_BASE + nodeId;
  sendCANMessage(txId, 8, data, "Poll Status", false);
  const unsigned long expectedRx = CAN_SDO_RX_BASE + nodeId;
  const uint32_t start = millis();
  while (millis() - start < timeoutMs)
  {
    uint32_t rxId = 0;
    uint8_t len = 0;
    byte rxBuf[8];
    if (canMutex) xSemaphoreTake(canMutex, portMAX_DELAY);
    const bool got = canTwaiReceive(rxId, len, rxBuf, CAN_STATUS_RX_TIMEOUT_MS);
    if (canMutex) xSemaphoreGive(canMutex);
    if (!got)
    {
      delay(CAN_STATUS_RETRY_DELAY_MS);
      continue;
    }
    if (rxId == expectedRx && len >= 6 && rxBuf[1] == 0x41 && rxBuf[2] == 0x60)
    {
      statusOut = (uint16_t)(rxBuf[4] | (rxBuf[5] << 8));
      return true;
    }
  }
  return false;
}

bool canReadSdoI32(uint8_t nodeId, uint16_t index, uint8_t subIndex, int32_t& valOut, uint32_t timeoutMs)
{
  if (!canInitialized) return false;
  unsigned char data[] = {
    0x40,
    (byte)(index & 0xFF), (byte)((index >> 8) & 0xFF), subIndex,
    0x00, 0x00, 0x00, 0x00
  };
  const unsigned long txId = CAN_SDO_TX_BASE + nodeId;
  sendCANMessage(txId, 8, data, "SDO read", false);
  const unsigned long expectedRx = CAN_SDO_RX_BASE + nodeId;
  const uint32_t start = millis();
  while (millis() - start < timeoutMs)
  {
    uint32_t rxId = 0;
    uint8_t len = 0;
    byte rxBuf[8];
    if (canMutex) xSemaphoreTake(canMutex, portMAX_DELAY);
    const bool got = canTwaiReceive(rxId, len, rxBuf, CAN_SDO_RX_TIMEOUT_MS);
    if (canMutex) xSemaphoreGive(canMutex);
    if (!got) continue;
    if (rxId != expectedRx || len < 8) continue;
    if (rxBuf[0] != 0x43) continue;
    if (rxBuf[1] != (byte)(index & 0xFF) || rxBuf[2] != (byte)((index >> 8) & 0xFF) || rxBuf[3] != subIndex)
      continue;
    valOut = (int32_t)((uint32_t)rxBuf[4] | ((uint32_t)rxBuf[5] << 8) | ((uint32_t)rxBuf[6] << 16) | ((uint32_t)rxBuf[7] << 24));
    return true;
  }
  return false;
}

bool verifyServoEnergized()
{
  uint16_t sw1 = 0, sw2 = 0;
  bool ok1 = canReadStatusWord(SERVO_NODE_R, sw1, CAN_STATUS_VERIFY_MS);
  bool ok2 = canReadStatusWord(SERVO_NODE_L, sw2, CAN_STATUS_VERIFY_MS);
  bool ready = ok1 && ok2 && isServoOperationEnabled(sw1) && isServoOperationEnabled(sw2);
  servoCanReady = ready;
  Serial.printf("SERVO CAN: R/0x%03X=0x%X/%s L/0x%03X=0x%X/%s ready=%d\n",
                (unsigned)SERVO_CAN_TX_R, sw1, ok1 ? "ok" : "timeout",
                (unsigned)SERVO_CAN_TX_L, sw2, ok2 ? "ok" : "timeout", (int)ready);
  return ready;
}

bool initCANBus(uint8_t maxRetries)
{
  if (!canMutex) canMutex = xSemaphoreCreateMutex();
  canInitialized = false;
  servoCanReady = false;
  if (maxRetries == 0) maxRetries = 1;

  canTwaiShutdown();

  const twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
      (gpio_num_t)CAN_TX_PIN, (gpio_num_t)CAN_RX_PIN, TWAI_MODE_NORMAL);
  const twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
  const twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  Serial.printf("[CAN] TWAI TX=%d RX=%d %ukbps\n",
                (int)CAN_TX_PIN, (int)CAN_RX_PIN, (unsigned)CAN_BITRATE_KBPS);

  for (uint8_t attempt = 1; attempt <= maxRetries; attempt++)
  {
    esp_err_t err = twai_driver_install(&g_config, &t_config, &f_config);
    if (err != ESP_OK)
    {
      Serial.printf("[CAN] driver_install FAIL intento %u/%u err=%d\n",
                    (unsigned)attempt, (unsigned)maxRetries, (int)err);
      canTwaiShutdown();
      delay(CAN_INIT_RETRY_MS);
      continue;
    }
    err = twai_start();
    if (err != ESP_OK)
    {
      Serial.printf("[CAN] start FAIL intento %u/%u err=%d\n",
                    (unsigned)attempt, (unsigned)maxRetries, (int)err);
      canTwaiShutdown();
      delay(CAN_INIT_RETRY_MS);
      continue;
    }
    canInitialized = true;
    Serial.printf("[CAN] TWAI OK intento %u/%u\n", (unsigned)attempt, (unsigned)maxRetries);
    return true;
  }

  Serial.println("[CAN] TWAI FAIL — revisar TX/RX, transceiver y terminacion 120R");
  return false;
}

void feedCanSetVelAcc(uint32_t velL, uint32_t velR);

static void setupServoNodeCiA402(uint32_t canId, const char* label)
{
  unsigned char data3[] = {0x2B, 0x40, 0x60, 0x00, 0x06, 0x00, 0x00, 0x00};
  sendCANMessage(canId, 8, data3, String("Shutdown ") + label, false);
  delay(CAN_CIA402_STEP_DELAY_MS);
  unsigned char data4[] = {0x2B, 0x40, 0x60, 0x00, 0x07, 0x00, 0x00, 0x00};
  sendCANMessage(canId, 8, data4, String("Switch On ") + label, false);
  delay(CAN_CIA402_STEP_DELAY_MS);
  unsigned char data5[] = {0x2B, 0x40, 0x60, 0x00, 0x0F, 0x00, 0x00, 0x00};
  sendCANMessage(canId, 8, data5, String("Enable Op ") + label, false);
  delay(CAN_CIA402_STEP_DELAY_MS);
  unsigned char data6[] = {0x2F, 0x60, 0x60, 0x00, 0x01, 0x00, 0x00, 0x00};
  sendCANMessage(canId, 8, data6, String("Mode PP ") + label, false);
  delay(CAN_CIA402_STEP_DELAY_MS);
  {
    uint32_t maxPps = SERVO_MAX_PPS;
    byte maxVel[8] = {
      0x23, 0x7F, 0x60, 0x00,
      (byte)(maxPps & 0xFF), (byte)((maxPps >> 8) & 0xFF),
      (byte)((maxPps >> 16) & 0xFF), (byte)((maxPps >> 24) & 0xFF)
    };
    sendCANMessage(canId, 8, maxVel, String("MaxVel ") + label, false);
  }
  delay(CAN_CIA402_SHORT_DELAY_MS);
  {
    byte haltOpt[8] = {0x2B, 0x5D, 0x60, 0x00, (byte)FEED_HALT_OPTION_CODE, 0x00, 0x00, 0x00};
    sendCANMessage(canId, 8, haltOpt, String("HaltOpt ") + label, false);
    uint32_t hd = FEED_HALT_DECEL_PP;
    byte qdec[8] = {
      0x23, 0x85, 0x60, 0x00,
      (byte)(hd & 0xFF), (byte)((hd >> 8) & 0xFF),
      (byte)((hd >> 16) & 0xFF), (byte)((hd >> 24) & 0xFF)
    };
    sendCANMessage(canId, 8, qdec, String("QSDec ") + label, false);
  }
  delay(CAN_CIA402_MED_DELAY_MS);
}

static void setupServoNodeHome(uint32_t canId, const char* label, uint32_t homePp)
{
  unsigned char data10[] = {
    0x23, 0x7A, 0x60, 0x00,
    (byte)(homePp & 0xFF), (byte)((homePp >> 8) & 0xFF),
    (byte)((homePp >> 16) & 0xFF), (byte)((homePp >> 24) & 0xFF)
  };
  sendCANMessage(canId, 8, data10, String("Pos ") + label, false);
  delay(CAN_CIA402_STEP_DELAY_MS);
  unsigned char data11[] = {0x2B, 0x40, 0x60, 0x00, 0x2F, 0x00, 0x00, 0x00};
  sendCANMessage(canId, 8, data11, String("Reset SP ") + label, false);
  delay(CAN_CIA402_STEP_DELAY_MS);
}

bool setupServoFeeder()
{
  servoCanReady = false;
  Serial.printf("[CAN] setup nodos R=0x%03X L=0x%03X\n",
                (unsigned)SERVO_CAN_TX_R, (unsigned)SERVO_CAN_TX_L);

  unsigned char faultReset[] = {0x2B, 0x40, 0x60, 0x00, 0x80, 0x00, 0x00, 0x00};
  sendCANMessage(SERVO_CAN_TX_R, 8, faultReset, "Fault Reset R", false);
  sendCANMessage(SERVO_CAN_TX_L, 8, faultReset, "Fault Reset L", false);
  delay(CAN_FAULT_RESET_DELAY_MS);

  unsigned char data1[] = {0x80, 0x00};
  sendCANMessage(CAN_NMT_ID, 2, data1, "Pre-Op", false);
  delay(CAN_NMT_PREOP_DELAY_MS);
  unsigned char data2[] = {0x01, 0x00};
  sendCANMessage(CAN_NMT_ID, 2, data2, "Operational", false);
  delay(CAN_NMT_OP_DELAY_MS);

  setupServoNodeCiA402(SERVO_CAN_TX_R, "R");
  setupServoNodeCiA402(SERVO_CAN_TX_L, "L");

  const uint32_t val = (uint32_t)SERVO_HOME_PP;
  feedCanSetVelAcc(val, val);
  delay(CAN_HOME_VEL_DELAY_MS);

  setupServoNodeHome(SERVO_CAN_TX_R, "R", val);
  setupServoNodeHome(SERVO_CAN_TX_L, "L", val);
  delay(CAN_HOME_DONE_DELAY_MS);

  return verifyServoEnergized();
}

bool reconnectCANBus(bool fullServoSetup)
{
  if (!initCANBus(CAN_INIT_MAX_RETRIES))
  {
    servoCanReady = false;
    return false;
  }
  if (fullServoSetup) setupServoFeeder();
  return true;
}

void serviceCANRx()
{
  if (!canInitialized) return;
  for (uint8_t n = 0; n < CAN_RX_DRAIN_MAX; n++)
  {
    twai_message_t message;
    if (canMutex) xSemaphoreTake(canMutex, portMAX_DELAY);
    const esp_err_t err = twai_receive(&message, 0);
    if (canMutex) xSemaphoreGive(canMutex);
    if (err != ESP_OK)
      break;
  }
}

static void canWriteU32BothNodes(uint16_t index, uint32_t valL, uint32_t valR, const String& desc)
{
  byte dataL[8] = {
    0x23, (byte)(index & 0xFF), (byte)((index >> 8) & 0xFF), 0x00,
    (byte)(valL & 0xFF), (byte)((valL >> 8) & 0xFF), (byte)((valL >> 16) & 0xFF), (byte)((valL >> 24) & 0xFF)
  };
  sendCANMessage(SERVO_CAN_TX_L, 8, dataL, desc + " L", false);
  byte dataR[8] = {
    0x23, (byte)(index & 0xFF), (byte)((index >> 8) & 0xFF), 0x00,
    (byte)(valR & 0xFF), (byte)((valR >> 8) & 0xFF), (byte)((valR >> 16) & 0xFF), (byte)((valR >> 24) & 0xFF)
  };
  sendCANMessage(SERVO_CAN_TX_R, 8, dataR, desc + " R", false);
}

void canSetProfileVelocity(uint32_t valL, uint32_t valR) { canWriteU32BothNodes(SERVO_OD_PROFILE_VELOCITY, valL, valR, "Vel"); }
void canSetProfileAccel(uint32_t valL, uint32_t valR)    { canWriteU32BothNodes(SERVO_OD_PROFILE_ACCEL, valL, valR, "Acc"); }
void canSetProfileDecel(uint32_t valL, uint32_t valR)    { canWriteU32BothNodes(SERVO_OD_PROFILE_DECEL, valL, valR, "Dec"); }

void canSetMotionProfile(uint32_t velL, uint32_t accL, uint32_t decL, uint32_t velR, uint32_t accR, uint32_t decR)
{
  if (canBusMotionBlocked()) return;
  canSetProfileVelocity(velL, velR);
  canSetProfileAccel(accL, accR);
  canSetProfileDecel(decL, decR);
}

void feedCanSetVelAcc(uint32_t velL, uint32_t velR)
{
  auto accFor = [](uint32_t vel, uint16_t rampMs) {
    if (vel < 1) vel = 1;
    if (rampMs < 1) rampMs = 1;
    uint64_t a = ((uint64_t)vel * 1000ULL) / rampMs;
    if (a < FEED_SERVO_ACC_MIN) a = FEED_SERVO_ACC_MIN;
    if (a > FEED_SERVO_ACC_MAX) a = FEED_SERVO_ACC_MAX;
    return (uint32_t)a;
  };
  extern volatile uint16_t feedDecRampMsL;
  extern volatile uint16_t feedDecRampMsR;
  const uint32_t aL = accFor(velL, FEED_SERVO_RAMP_MS);
  const uint32_t dL = accFor(velL, feedDecRampMsL);
  const uint32_t aR = accFor(velR, FEED_SERVO_RAMP_MS);
  const uint32_t dR = accFor(velR, feedDecRampMsR);
  canSetMotionProfile(velL, aL, dL, velR, aR, dR);
}

void feedCanPrimeHaltDecel()
{
  byte haltOpt[8] = {0x2B, 0x5D, 0x60, 0x00, (byte)FEED_HALT_OPTION_CODE, 0x00, 0x00, 0x00};
  sendCANMessage(SERVO_CAN_TX_L, 8, haltOpt, "HaltOpt L", false);
  sendCANMessage(SERVO_CAN_TX_R, 8, haltOpt, "HaltOpt R", false);
  uint32_t hd = FEED_HALT_DECEL_PP;
  byte qdec[8] = {
    0x23, 0x85, 0x60, 0x00,
    (byte)(hd & 0xFF), (byte)((hd >> 8) & 0xFF),
    (byte)((hd >> 16) & 0xFF), (byte)((hd >> 24) & 0xFF)
  };
  sendCANMessage(SERVO_CAN_TX_L, 8, qdec, "QSDec L", false);
  sendCANMessage(SERVO_CAN_TX_R, 8, qdec, "QSDec R", false);
}

void canHaltSide(bool sideR) { sendCanHaltImmediate(sideR); }
void canHalt() { canHaltSide(false); canHaltSide(true); }

static void canControlWordSides(bool doL, bool doR, uint16_t cw, const char* desc)
{
  if (canBusMotionBlocked()) return;
  byte data[8] = {
    0x2B, 0x40, 0x60, 0x00,
    (byte)(cw & 0xFF), (byte)((cw >> 8) & 0xFF), 0x00, 0x00
  };
  if (doL) sendCANMessage(SERVO_CAN_TX_L, 8, data, String(desc) + " L", false);
  if (doR) sendCANMessage(SERVO_CAN_TX_R, 8, data, String(desc) + " R", false);
}

static void canWriteTargetAbs(bool doL, bool doR, int32_t absL, int32_t absR)
{
  if (canBusMotionBlocked()) return;
  if (doL)
  {
    byte dataL[8] = {0x23, 0x7A, 0x60, 0x00,
      (byte)(absL & 0xFF), (byte)((absL >> 8) & 0xFF), (byte)((absL >> 16) & 0xFF), (byte)((absL >> 24) & 0xFF)};
    sendCANMessage(SERVO_CAN_TX_L, 8, dataL, "Target L", false);
  }
  if (doR)
  {
    byte dataR[8] = {0x23, 0x7A, 0x60, 0x00,
      (byte)(absR & 0xFF), (byte)((absR >> 8) & 0xFF), (byte)((absR >> 16) & 0xFF), (byte)((absR >> 24) & 0xFF)};
    sendCANMessage(SERVO_CAN_TX_R, 8, dataR, "Target R", false);
  }
}

void canMoveRelativePP(int32_t stepsL, int32_t stepsR)
{
  if (stepsL < 0) stepsL = 0;
  if (stepsR < 0) stepsR = 0;
  if (stepsL <= 0 && stepsR <= 0) return;
  if (canBusMotionBlocked()) return;
  bool doL = stepsL > 0;
  bool doR = stepsR > 0;
  int32_t posL = 0, posR = 0;
  if (doL && !canReadSdoI32(SERVO_NODE_L, SERVO_OD_POSITION_ACTUAL, 0x00, posL, CAN_SDO_TIMEOUT_MS)) doL = false;
  if (doR && !canReadSdoI32(SERVO_NODE_R, SERVO_OD_POSITION_ACTUAL, 0x00, posR, CAN_SDO_TIMEOUT_MS)) doR = false;
  if (!doL && !doR) return;
  const int32_t absL = doL ? (posL + (int32_t)SERVO_CMD_SIGN_L * stepsL) : 0;
  const int32_t absR = doR ? (posR + (int32_t)SERVO_CMD_SIGN_R * stepsR) : 0;
  canControlWordSides(doL, doR, SERVO_CW_HALT_HOLD, "halt hold");
  canWriteTargetAbs(doL, doR, absL, absR);
  canControlWordSides(doL, doR, SERVO_CW_RESET_HALT, "reset+halt");
  canControlWordSides(doL, doR, SERVO_CW_NEW_ABS_HALT, "new+abs+halt");
  canControlWordSides(doL, doR, SERVO_CW_RUN_ABSOLUTE, "run absolute");
}

void servoCanInitMutex()
{
  if (!canMutex) canMutex = xSemaphoreCreateMutex();
}

static Preferences feedPrefs;

static FeedPhase feedPhase = FEED_IDLE;
static FeedMode feedModeThisCycle = FEED_MODE_STEPS_SENSOR;

volatile float feedTargetMmL = FEED_TARGET_MM_DEFAULT;
volatile float feedTargetMmR = FEED_TARGET_MM_DEFAULT;
volatile float feedOffsetMm = 0.0f;
volatile float feedOffsetMmB = 0.0f;
volatile float feedCalCountsPerMmL = FEED_ENC_COUNTS_PER_MM;
volatile float feedCalCountsPerMmR = FEED_ENC_COUNTS_PER_MM;
volatile uint32_t feedSsFastPpL = FEED_VELOCITY_PP_DEFAULT;
volatile uint32_t feedSsFastPpR = FEED_VELOCITY_PP_DEFAULT;
volatile uint16_t feedDecRampMsL = FEED_SERVO_DEC_RAMP_MS;
volatile uint16_t feedDecRampMsR = FEED_SERVO_DEC_RAMP_MS;
volatile bool feedSkipEncoderConfirm = false;
FeedTestReq feedTestReq = {};

bool feedCalibrationTest = false;
bool feedRuntimeActive = false;
char feedFaultReason[FEED_FAULT_REASON_MAX] = "";
uint16_t feedErrorCode = 0;
float feedOmLastOfficialMm = -1.0f;
float feedOmLastMmSigned = 0.0f;
float feedOmLastMmAbs = -1.0f;
float omPhase1Mm = 0.0f;
bool feedOmLengthMet = false;

static bool feedNeedSensorL = false;
static bool feedNeedSensorR = false;
static bool feedSensorConfirmedL = true;
static bool feedSensorConfirmedR = true;
static int32_t feedStepsTargetThisFeed = 0;
static int32_t feedStepsTargetB = 0;
static bool feedSsEncTrackL = false;
static bool feedSsEncTrackR = false;
static int32_t feedSsEncStartPosL = 0;
static int32_t feedSsEncStartPosR = 0;
static uint32_t feedPhaseStartMs = 0;
static uint32_t feedDelayUntilMs = 0;
static uint32_t feedSsMoveStartMs = 0;
static uint32_t feedSsAbsDueMs = 0;
static uint32_t feedSsLastTrPollMs = 0;
static uint32_t feedOmLastPollMs = 0;
static bool feedOmAwaitSettle = false;
static uint8_t feedOmSettleResumeCount = 0;
static uint8_t feedOmSettleReadMiss = 0;
static bool feedOmDirChecked = false;
static bool feedSolidTargetMet = false;
static bool feedTestExactSides = false;

template<typename T>
static T clampVal(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

float clampFeedTargetMm(float mm) { return clampVal(mm, FEED_TARGET_MM_MIN, FEED_TARGET_MM_MAX); }
float clampFeedOffsetMm(float mm) { return clampVal(mm, FEED_OFFSET_MM_MIN, FEED_OFFSET_MM_MAX); }
float clampFeedCalCountsPerMm(float spm) { return clampVal(spm, FEED_CAL_COUNTS_PER_MM_MIN, FEED_CAL_COUNTS_PER_MM_MAX); }
uint16_t clampFeedRampMs(uint32_t ms) { return (uint16_t)clampVal(ms, (uint32_t)FEED_SERVO_RAMP_MS_MIN, (uint32_t)FEED_SERVO_RAMP_MS_MAX); }
int32_t clampFeedTestChunkSteps(int32_t steps) { return clampVal(steps, (int32_t)FEED_TEST_CHUNK_MIN, (int32_t)FEED_TEST_CHUNK_MAX); }
uint32_t clampFeedSsSpeedPp(uint32_t pp) { return clampVal(pp, (uint32_t)FEED_SERVO_BASE_PP_MIN, (uint32_t)FEED_SERVO_BASE_PP_MAX); }

static float feedCalibratedCountsPerMm(bool sideR) { return sideR ? feedCalCountsPerMmR : feedCalCountsPerMmL; }
static float feedSpm(bool) { return FEED_STEPS_PER_MM_DEFAULT; }

static int32_t feedMmToEncCountsNominal(float mm)
{
  if (mm <= 0.0f) return 0;
  return (int32_t)(mm * FEED_ENC_COUNTS_PER_MM + 0.5f);
}

static int32_t feedEncCountsToCmdSteps(int32_t encCounts)
{
  if (encCounts <= 0) return 0;
  const int32_t encNom = feedMmToEncCountsNominal(FEED_NOMINAL_MM);
  if (encNom <= 0) return 0;
  return (int32_t)(((int64_t)encCounts * FEED_NOMINAL_STEPS + encNom / 2) / encNom);
}

float feedMaxMmS(bool sideR)
{
  (void)sideR;
  return (float)SERVO_MAX_PPS / FEED_ENC_COUNTS_PER_MM;
}

uint32_t feedMmSToPp(float mmS, bool sideR)
{
  if (mmS < FEED_MM_S_MIN) mmS = FEED_MM_S_MIN;
  float maxS = feedMaxMmS(sideR);
  if (mmS > maxS) mmS = maxS;
  float pp = mmS * feedSpm(sideR);
  if (pp < 1.0f) pp = 1.0f;
  if (pp > (float)FEED_SERVO_BASE_PP_MAX) pp = (float)FEED_SERVO_BASE_PP_MAX;
  return (uint32_t)(pp + 0.5f);
}

float feedPpToMmS(uint32_t pp, bool sideR) { return (float)pp / feedSpm(sideR); }

static FeedEncPlan feedPlanEncTarget(float targetMm, float offsetMm, bool sideR)
{
  FeedEncPlan p = {};
  const float eff = targetMm + offsetMm;
  if (eff < 0.0f) { p.error = "target+offset negativo"; return p; }
  p.ok = true;
  p.effectiveTargetMm = eff;
  if (eff <= 0.0f) { p.targetCounts = 0; return p; }
  p.targetCounts = (int32_t)(eff * feedCalibratedCountsPerMm(sideR) + 0.5f);
  return p;
}

static uint32_t feedProfileAccForVelMs(uint32_t vel, uint32_t rampMs)
{
  if (vel < 1) vel = 1;
  if (rampMs < 1) rampMs = 1;
  uint64_t a = ((uint64_t)vel * 1000ULL) / rampMs;
  if (a < FEED_SERVO_ACC_MIN) a = FEED_SERVO_ACC_MIN;
  if (a > FEED_SERVO_ACC_MAX) a = FEED_SERVO_ACC_MAX;
  return (uint32_t)a;
}

static FeedProfilePlan feedProfilePlanCoreRamps(float targetMm, float vReqMmS, bool sideR,
                                                 uint16_t rampMs, uint16_t decRampMs)
{
  FeedProfilePlan p = {};
  p.targetMm = targetMm;
  p.vReqMmS = vReqMmS;
  if (targetMm <= 0.0f || vReqMmS < FEED_MM_S_MIN) return p;
  if (vReqMmS > feedMaxMmS(sideR)) return p;
  p.velPp = feedMmSToPp(vReqMmS, sideR);
  p.accPp = feedProfileAccForVelMs(p.velPp, rampMs);
  p.decPp = feedProfileAccForVelMs(p.velPp, decRampMs);
  const float spm = feedSpm(sideR);
  p.accMmS2 = (float)p.accPp / spm;
  p.decMmS2 = (float)p.decPp / spm;
  p.dAccMm = (vReqMmS * vReqMmS) / (2.0f * p.accMmS2);
  p.dDecMm = (vReqMmS * vReqMmS) / (2.0f * p.decMmS2);
  p.dMinMm = p.dAccMm + p.dDecMm;
  p.marginMm = targetMm - p.dMinMm;
  p.valid = (p.marginMm >= -FEED_PROFILE_MARGIN_EPS_MM);
  if (p.valid) p.dCruiseMm = (p.marginMm > 0.0f) ? p.marginMm : 0.0f;
  return p;
}

FeedProfilePlan feedProfilePlan(float targetMm, float vReqMmS, bool sideR, uint16_t decRampMs)
{
  FeedProfilePlan p = feedProfilePlanCoreRamps(targetMm, vReqMmS, sideR, FEED_SERVO_RAMP_MS, decRampMs);
  float lo = FEED_MM_S_MIN, hi = feedMaxMmS(sideR);
  for (int i = 0; i < FEED_PROFILE_SEARCH_ITERS && hi - lo >= FEED_PROFILE_SEARCH_EPS_MM; i++)
  {
    float mid = (lo + hi) * 0.5f;
    if (feedProfilePlanCoreRamps(targetMm, mid, sideR, FEED_SERVO_RAMP_MS, decRampMs).valid) lo = mid;
    else hi = mid;
  }
  p.vMaxMmS = feedProfilePlanCoreRamps(targetMm, lo, sideR, FEED_SERVO_RAMP_MS, decRampMs).valid ? lo : 0.0f;
  return p;
}

bool feedProfileSideValid(float targetMm, float vReqMmS, bool sideR, uint16_t decRampMs)
{
  return feedProfilePlanCoreRamps(targetMm, vReqMmS, sideR, FEED_SERVO_RAMP_MS, decRampMs).valid;
}

void feedProfileAppendJson(const FeedProfilePlan& p, String& json, const char* key)
{
  json += ",\""; json += key; json += "\":{";
  json += "\"valid\":"; json += p.valid ? "true" : "false";
  json += ",\"targetMm\":" + String(p.targetMm, 2);
  json += ",\"vReqMmS\":" + String(p.vReqMmS, 1);
  json += ",\"vMaxMmS\":" + String(p.vMaxMmS, 1);
  json += ",\"accMmS2\":" + String(p.accMmS2, 1);
  json += ",\"decMmS2\":" + String(p.decMmS2, 1);
  json += ",\"dAccMm\":" + String(p.dAccMm, 2);
  json += ",\"dDecMm\":" + String(p.dDecMm, 2);
  json += ",\"dCruiseMm\":" + String(p.dCruiseMm, 2);
  json += "}";
}

static float feedOmStopTargetMm()
{
  float t = 0.0f;
  if (feedNeedSensorL && feedTargetMmL > t) t = feedTargetMmL;
  if (feedNeedSensorR && feedTargetMmR > t) t = feedTargetMmR;
  if (t <= 0.0f) t = FEED_NOMINAL_MM;
  return t;
}

static bool feedOmExactTarget(float omMm)
{
  const float target = feedOmStopTargetMm();
  return fabsf(omMm - target) <= FEED_OM_TARGET_TOL_MM + 1e-4f;
}

static bool feedOmOvershoot(float omMm)
{
  return omMm > feedOmStopTargetMm() + FEED_OM_TARGET_TOL_MM + 1e-4f;
}

static int32_t feedEncoderAbsDelta(int32_t startPos, int32_t endPos)
{
  int32_t d = endPos - startPos;
  return d < 0 ? -d : d;
}

static bool feedServoTargetReachedNode(uint8_t nodeId)
{
  uint16_t sw = 0;
  if (!canReadStatusWord(nodeId, sw, CAN_STATUS_POLL_MS)) return false;
  return (sw & SERVO_SW_TARGET_REACHED) != 0;
}

static uint32_t feedSsCmdTimeMs(int32_t steps, uint32_t vel, uint16_t decRampMs)
{
  if (steps <= 0 || vel < 1) return 0;
  const uint32_t acc = feedProfileAccForVelMs(vel, FEED_SERVO_RAMP_MS);
  const uint32_t dec = feedProfileAccForVelMs(vel, decRampMs);
  const uint64_t sMin = ((uint64_t)vel * vel) / (2ULL * acc) + ((uint64_t)vel * vel) / (2ULL * dec);
  if ((uint64_t)steps <= sMin)
  {
    const uint32_t aEff = (acc < dec) ? acc : dec;
    return (uint32_t)(1000.0 * sqrt((2.0 * steps) / (double)aEff) + 0.5);
  }
  const uint64_t cruise = (uint64_t)steps - sMin;
  return (uint32_t)(1000.0 * ((double)vel / acc + (double)cruise / vel + (double)vel / dec) + 0.5);
}

static void feedSsArmAbsDueMs(uint32_t now, uint32_t moveMs)
{
  feedSsMoveStartMs = now;
  feedSsAbsDueMs = now + moveMs + FEED_SS_ABS_MARGIN_MS;
}

static void feedAbort(const char* reason)
{
  feedPhase = FEED_ERROR;
  feedRuntimeActive = false;
  if (reason && reason[0])
  {
    strncpy(feedFaultReason, reason, sizeof(feedFaultReason) - 1);
    feedFaultReason[sizeof(feedFaultReason) - 1] = '\0';
    Serial.printf("FEED ABORT: %s\n", reason);
  }
}

static void feedMarkDone()
{
  feedPhase = FEED_DONE;
  feedOmLengthMet = true;
  feedRuntimeActive = false;
}

bool feedPhaseIsActive()
{
  return feedPhase != FEED_IDLE && feedPhase != FEED_DONE && feedPhase != FEED_ERROR;
}

bool feedThisCycleSucceeded() { return feedPhase == FEED_DONE && feedOmLengthMet; }

static void feedIssueMove(int32_t stepsL, int32_t stepsR)
{
  canMoveRelativePP(stepsL, stepsR);
}

static void feedSsBeginStepsSensor()
{
  const FeedEncPlan planL = feedPlanEncTarget(feedTargetMmL, feedOffsetMm, false);
  const FeedEncPlan planR = feedPlanEncTarget(feedTargetMmR, feedOffsetMmB, true);
  if (!planL.ok) { feedAbort("FEED: target L + offset negativo"); return; }
  if (!planR.ok) { feedAbort("FEED: target R + offset negativo"); return; }

  feedStepsTargetThisFeed = planL.targetCounts;
  feedStepsTargetB = feedTestExactSides ? planR.targetCounts : (planR.targetCounts > 0 ? planR.targetCounts : planL.targetCounts);
  feedNeedSensorL = feedStepsTargetThisFeed > 0;
  feedNeedSensorR = feedStepsTargetB > 0;
  feedSensorConfirmedL = !feedNeedSensorL;
  feedSensorConfirmedR = !feedNeedSensorR;
  feedCanPrimeHaltDecel();

  feedSsEncTrackL = feedNeedSensorL && canReadSdoI32(SERVO_NODE_L, SERVO_OD_POSITION_ACTUAL, 0x00, feedSsEncStartPosL, CAN_SDO_TIMEOUT_MS);
  feedSsEncTrackR = feedNeedSensorR && canReadSdoI32(SERVO_NODE_R, SERVO_OD_POSITION_ACTUAL, 0x00, feedSsEncStartPosR, CAN_SDO_TIMEOUT_MS);
  if (feedNeedSensorL && !feedSsEncTrackL && !feedCalibrationTest) { feedAbort("FEED: no 6064 L"); return; }
  if (feedNeedSensorR && !feedSsEncTrackR && !feedCalibrationTest) { feedAbort("FEED: no 6064 R"); return; }

  feedSolidTargetMet = false;
  feedOmLengthMet = false;
  feedOmAwaitSettle = false;
  feedOmSettleResumeCount = 0;
  feedOmDirChecked = false;
  feedPhase = FEED_SS_SOLID;
  feedPhaseStartMs = millis();
  feedSsMoveStartMs = 0;
  feedSsAbsDueMs = 0;
}

static void feedSsFinishOnOm(float omMm)
{
  omPhase1Mm = omMm;
  feedOmLengthMet = true;
  feedMarkDone();
}

static void feedSsIssueCorrMove(uint32_t now, float remainingMm)
{
  if (remainingMm <= 0.0f) return;
  if (remainingMm < FEED_CORR_MIN_MM) remainingMm = FEED_CORR_MIN_MM;
  const float target = feedOmStopTargetMm();
  const float vNomL = feedPpToMmS(feedSsFastPpL, false);
  const float vNomR = feedPpToMmS(feedSsFastPpR, true);
  const float vCorrL = vNomL * (remainingMm / target);
  const float vCorrR = vNomR * (remainingMm / target);
  if (vCorrL < FEED_MM_S_CORR_MIN) {}
  feedCanSetVelAcc(feedMmSToPp(vCorrL < FEED_MM_S_CORR_MIN ? FEED_MM_S_CORR_MIN : vCorrL, false),
                    feedMmSToPp(vCorrR < FEED_MM_S_CORR_MIN ? FEED_MM_S_CORR_MIN : vCorrR, true));
  int32_t encRem = (int32_t)(remainingMm * FEED_ENC_COUNTS_PER_MM + 0.5f);
  if (encRem < 1) encRem = 1;
  int32_t moveL = feedNeedSensorL && !feedSensorConfirmedL ? feedEncCountsToCmdSteps(encRem) : 0;
  int32_t moveR = feedNeedSensorR && !feedSensorConfirmedR ? feedEncCountsToCmdSteps(encRem) : 0;
  if (moveL <= 0 && moveR <= 0) return;
  feedIssueMove(moveL, moveR);
  uint32_t tL = moveL > 0 ? feedSsCmdTimeMs(moveL, feedSsFastPpL, feedDecRampMsL) : 0;
  uint32_t tR = moveR > 0 ? feedSsCmdTimeMs(moveR, feedSsFastPpR, feedDecRampMsR) : 0;
  feedSsArmAbsDueMs(now, tL > tR ? tL : tR);
  feedPhase = FEED_RUNNING;
}

static bool feedSsEvaluateOmAfterStop(uint32_t now)
{
  float omMm = 0.0f;
  if (!feedOmReadOfficialMm(&omMm))
  {
    if (++feedOmSettleReadMiss < FEED_OM_READ_RETRY_MAX)
    {
      feedDelayUntilMs = now + FEED_OM_READ_RETRY_DELAY_MS;
      feedOmAwaitSettle = true;
      return true;
    }
    feedAbort("FEED: sin lectura OM tras move");
    return true;
  }
  feedOmSettleReadMiss = 0;
  feedOmAwaitSettle = false;
  const float target = feedOmStopTargetMm();
  if (feedOmExactTarget(omMm)) { feedSsFinishOnOm(omMm); return true; }
  if (feedOmOvershoot(omMm))
  {
    snprintf(feedFaultReason, sizeof(feedFaultReason), "OM %.1f > %.1f+%.1f", omMm, target, FEED_OM_TARGET_TOL_MM);
    feedAbort(feedFaultReason);
    return true;
  }
  if (feedOmSettleResumeCount >= FEED_OM_CORR_RETRY_MAX)
  {
    snprintf(feedFaultReason, sizeof(feedFaultReason), "OM %.1f < %.1f-%.1f", omMm, target, FEED_OM_TARGET_TOL_MM);
    feedAbort(feedFaultReason);
    return true;
  }
  feedOmSettleResumeCount++;
  feedSensorConfirmedL = !feedNeedSensorL;
  feedSensorConfirmedR = !feedNeedSensorR;
  feedSsIssueCorrMove(now, target - omMm);
  return true;
}

static bool feedSsPollMoveChunkDone(uint32_t now)
{
  if (feedPhase != FEED_RUNNING || feedSsMoveStartMs == 0) return false;
  if (now < feedSsMoveStartMs + FEED_SS_POLL_MIN_MS) return false;
  if (feedSsAbsDueMs != 0 && now < feedSsAbsDueMs) return false;
  if (feedSsLastTrPollMs != 0 && now - feedSsLastTrPollMs < FEED_SS_TR_POLL_MS) return false;
  feedSsLastTrPollMs = now;
  if (!servoCanReady) return false;
  bool hitL = !feedNeedSensorL || feedSensorConfirmedL;
  bool hitR = !feedNeedSensorR || feedSensorConfirmedR;
  if (feedNeedSensorL && !feedSensorConfirmedL)
    hitL = feedServoTargetReachedNode(SERVO_NODE_L);
  if (feedNeedSensorR && !feedSensorConfirmedR)
    hitR = feedServoTargetReachedNode(SERVO_NODE_R);
  return hitL && hitR;
}

static void feedSsPollOmMonitor(uint32_t now)
{
  if (feedSkipEncoderConfirm) return;
  if (feedPhase == FEED_DONE || feedPhase == FEED_ERROR || feedPhase == FEED_IDLE) return;
  if (feedOmLastPollMs != 0 && now - feedOmLastPollMs < FEED_OM_FEED_POLL_MS) return;
  float mmSigned = 0.0f;
  if (!feedOmReadLiveMm(&mmSigned)) return;
  feedOmLastMmSigned = mmSigned;
  feedOmLastMmAbs = fabsf(mmSigned);
  feedOmLastPollMs = now;
#if FEED_OM_REQUIRE_NEGATIVE
  if (!feedOmDirChecked && (feedPhase == FEED_RUNNING || feedPhase == FEED_SS_SOLID)
      && fabsf(feedOmLastMmSigned) >= FEED_OM_DIR_CHECK_MM)
  {
    feedOmDirChecked = true;
    if (feedOmLastMmSigned > 0.0f)
    {
      canHalt();
      feedAbort("FEED: sentido horario (OM+); debe ser antihorario (OM-)");
    }
  }
#endif
}

static void serviceFeedStepsSensor(uint32_t now)
{
  if (!feedSkipEncoderConfirm)
    feedSsPollOmMonitor(now);
  if (feedPhase == FEED_DONE || feedPhase == FEED_ERROR) return;
  if (feedOmAwaitSettle && !feedSkipEncoderConfirm)
  {
    if (now < feedDelayUntilMs) return;
    feedSsEvaluateOmAfterStop(now);
    return;
  }
  if (feedPhase == FEED_RUNNING && feedSsPollMoveChunkDone(now))
  {
    if (feedSkipEncoderConfirm)
    {
      feedMarkDone();
      return;
    }
    feedOmAwaitSettle = true;
    feedDelayUntilMs = now + FEED_OM_SETTLE_MS;
    return;
  }
  if (feedPhase == FEED_SS_SOLID)
  {
    const uint32_t velL = clampFeedSsSpeedPp(feedSsFastPpL);
    const uint32_t velR = clampFeedSsSpeedPp(feedSsFastPpR);
    feedCanSetVelAcc(velL, velR);
    int32_t moveL = feedNeedSensorL ? feedEncCountsToCmdSteps(feedStepsTargetThisFeed) : 0;
    int32_t moveR = feedNeedSensorR ? feedEncCountsToCmdSteps(feedStepsTargetB) : 0;
    if (moveL < 0) moveL = 0;
    if (moveR < 0) moveR = 0;
    feedIssueMove(moveL, moveR);
    if (feedPhase == FEED_ERROR) return;
    uint32_t tL = moveL > 0 ? feedSsCmdTimeMs(moveL, velL, feedDecRampMsL) : 0;
    uint32_t tR = moveR > 0 ? feedSsCmdTimeMs(moveR, velR, feedDecRampMsR) : 0;
    feedSsArmAbsDueMs(now, tL > tR ? tL : tR);
    feedSolidTargetMet = true;
    feedPhase = FEED_RUNNING;
  }
}

static void serviceServoFeed()
{
  if (feedPhase == FEED_IDLE || feedPhase == FEED_DONE || feedPhase == FEED_ERROR) return;
  if (feedModeThisCycle == FEED_MODE_STEPS_SENSOR)
    serviceFeedStepsSensor(millis());
}

static void waitServoFeedDone(uint32_t timeoutMs)
{
  const uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs)
  {
    serviceCANRx();
    serviceServoFeed();
    yield();
    if (feedPhase == FEED_DONE || feedPhase == FEED_ERROR) return;
    delay(FEED_LOOP_YIELD_MS);
  }
  if (feedPhaseIsActive()) feedAbort("FEED: timeout");
}

bool runFeedCycle(bool skipEncoderConfirm, int8_t onlySide)
{
  if (!servoCanReady) return false;
  if (feedPhaseIsActive()) return false;

  const bool savedExact = feedTestExactSides;
  const bool savedSkip = feedSkipEncoderConfirm;
  feedSkipEncoderConfirm = skipEncoderConfirm;
  feedTestExactSides = (onlySide >= 0);

  if (onlySide == 0) feedTargetMmR = 0.0f;
  else if (onlySide == 1) feedTargetMmL = 0.0f;

  feedFaultReason[0] = '\0';
  feedErrorCode = 0;
  sendCanHaltImmediate(false);
  sendCanHaltImmediate(true);
  delay(FEED_HALT_SETTLE_MS);
  if (!skipEncoderConfirm)
    feedOmResetLocal();

  Serial.printf("FEED cycle: L=%.1fmm (0x%03X) R=%.1fmm (0x%03X) skipOM=%d side=%d\n",
                feedTargetMmL, (unsigned)SERVO_CAN_TX_L,
                feedTargetMmR, (unsigned)SERVO_CAN_TX_R,
                (int)skipEncoderConfirm, (int)onlySide);

  feedModeThisCycle = FEED_MODE_STEPS_SENSOR;
  feedCalibrationTest = skipEncoderConfirm;
  feedRuntimeActive = true;
  feedPhase = FEED_IDLE;
  feedSsBeginStepsSensor();
  bool ok = false;
  if (feedPhase != FEED_ERROR)
  {
    waitServoFeedDone(FEED_WAIT_TIMEOUT_MS + FEED_WAIT_EXTRA_MS);
    ok = feedThisCycleSucceeded();
  }
  feedCalibrationTest = false;
  feedSkipEncoderConfirm = savedSkip;
  feedTestExactSides = savedExact;
  if (onlySide == 0 || onlySide == 1)
    feedLoadConfig();
  return ok;
}

bool runFeedTestSolidThenSensor(float solidMmL, int32_t chunkL, float solidMmR, int32_t chunkR,
                                bool withCut, int8_t onlySide)
{
  (void)chunkL; (void)chunkR; (void)withCut;
  if (!servoCanReady) return false;
  if (feedPhaseIsActive()) return false;
  if (onlySide == 0) { solidMmR = 0.0f; }
  else if (onlySide == 1) { solidMmL = 0.0f; }
  feedTargetMmL = clampFeedTargetMm(solidMmL);
  feedTargetMmR = clampFeedTargetMm(solidMmR);
  const bool ok = runFeedCycle(false, onlySide);
  return ok;
}

void feedInit()
{
  feedLoadConfig();
  feedPhase = FEED_IDLE;
}

void feedLoadConfig()
{
  feedPrefs.begin(FEED_PREFS_NS, true);
  feedTargetMmL = clampFeedTargetMm(feedPrefs.getFloat("tgtL", FEED_TARGET_MM_DEFAULT));
  feedTargetMmR = clampFeedTargetMm(feedPrefs.getFloat("tgtR", FEED_TARGET_MM_DEFAULT));
  feedOffsetMm = clampFeedOffsetMm(feedPrefs.getFloat("offL", 0.0f));
  feedOffsetMmB = clampFeedOffsetMm(feedPrefs.getFloat("offR", 0.0f));
  feedCalCountsPerMmL = clampFeedCalCountsPerMm(feedPrefs.getFloat("calL", FEED_ENC_COUNTS_PER_MM));
  feedCalCountsPerMmR = clampFeedCalCountsPerMm(feedPrefs.getFloat("calR", FEED_ENC_COUNTS_PER_MM));
  feedSsFastPpL = clampFeedSsSpeedPp(feedPrefs.getUInt("fastL", FEED_VELOCITY_PP_DEFAULT));
  feedSsFastPpR = clampFeedSsSpeedPp(feedPrefs.getUInt("fastR", FEED_VELOCITY_PP_DEFAULT));
  const uint16_t decLegacy = clampFeedRampMs(feedPrefs.getUInt("decMs", FEED_SERVO_DEC_RAMP_MS));
  feedDecRampMsL = clampFeedRampMs(feedPrefs.getUInt("decMsL", decLegacy));
  feedDecRampMsR = clampFeedRampMs(feedPrefs.getUInt("decMsR", decLegacy));
  feedSkipEncoderConfirm = feedPrefs.getBool("skipEnc", false);
  feedPrefs.end();
}

void feedSaveConfig()
{
  feedPrefs.begin(FEED_PREFS_NS, false);
  feedPrefs.putFloat("tgtL", feedTargetMmL);
  feedPrefs.putFloat("tgtR", feedTargetMmR);
  feedPrefs.putFloat("offL", feedOffsetMm);
  feedPrefs.putFloat("offR", feedOffsetMmB);
  feedPrefs.putFloat("calL", feedCalCountsPerMmL);
  feedPrefs.putFloat("calR", feedCalCountsPerMmR);
  feedPrefs.putUInt("fastL", feedSsFastPpL);
  feedPrefs.putUInt("fastR", feedSsFastPpR);
  feedPrefs.putUInt("decMsL", feedDecRampMsL);
  feedPrefs.putUInt("decMsR", feedDecRampMsR);
  feedPrefs.putBool("skipEnc", feedSkipEncoderConfirm);
  feedPrefs.end();
}

void feedLoop()
{
  static uint32_t lastCanRetryMs = 0;
  if (!feedPhaseIsActive()
      && (millis() - lastCanRetryMs) >= FEED_CAN_RETRY_MS)
  {
    lastCanRetryMs = millis();
    if (!canInitialized)
      reconnectCANBus(true);
    else if (!servoCanReady)
      setupServoFeeder();
  }

  if (feedTestReq.pending && !feedPhaseIsActive())
  {
    FeedTestReq req = feedTestReq;
    feedTestReq.pending = false;
    const bool ok = runFeedTestSolidThenSensor(req.solidMmL, req.chunkL, req.solidMmR, req.chunkR,
                                               req.withCut, req.onlySide);
    feedNotifyTcpResult(ok, req.onlySide);
  }
  if (feedPhaseIsActive()) serviceServoFeed();
}

String feedStatusJson()
{
  String j = "{";
  j += "\"canInitialized\":"; j += canInitialized ? "true" : "false";
  j += ",\"canBitrateKbps\":"; j += CAN_BITRATE_KBPS;
  j += ",\"servoCanReady\":"; j += servoCanReady ? "true" : "false";
  j += ",\"feedActive\":"; j += feedPhaseIsActive() ? "true" : "false";
  j += ",\"feedPhase\":" + String((unsigned)feedPhase);
  j += ",\"feedOk\":"; j += feedThisCycleSucceeded() ? "true" : "false";
  j += ",\"fault\":\""; j += String(feedFaultReason); j += "\"";
  j += ",\"omOfficial\":" + String(feedOmLastOfficialMm, 2);
  j += ",\"targetMmL\":" + String(feedTargetMmL, 1);
  j += ",\"targetMmR\":" + String(feedTargetMmR, 1);
  j += ",\"skipEnc\":"; j += feedSkipEncoderConfirm ? "true" : "false";
  j += "}";
  return j;
}

bool feedQueueTest(const FeedTestReq& req, String& err)
{
  if (feedPhaseIsActive() || feedTestReq.pending)
  {
    err = "Feed ocupado";
    return false;
  }
  if (!servoCanReady)
  {
    err = "Servo CAN no listo";
    return false;
  }
  feedTestReq = req;
  feedTestReq.pending = true;
  return true;
}

bool feedQueueTestSide(int8_t onlySide, String& err)
{
  FeedTestReq req = {};
  req.solidMmL = feedTargetMmL;
  req.solidMmR = feedTargetMmR;
  req.fastL = feedSsFastPpL;
  req.fastR = feedSsFastPpR;
  req.decRampL = feedDecRampMsL;
  req.decRampR = feedDecRampMsR;
  req.onlySide = onlySide;
  return feedQueueTest(req, err);
}

bool feedResetRuntime()
{
  feedTestReq = {};
  if (feedPhaseIsActive())
  {
    canHalt();
    delay(FEED_HALT_SETTLE_MS);
  }
  feedPhase = FEED_IDLE;
  feedRuntimeActive = false;
  feedFaultReason[0] = '\0';
  feedErrorCode = 0;
  feedOmAwaitSettle = false;
  feedOmSettleResumeCount = 0;
  feedOmSettleReadMiss = 0;
  feedOmDirChecked = false;
  feedSolidTargetMet = false;
  feedOmLengthMet = false;
  return true;
}

static void feedNotifyTcpResult(bool ok, int8_t onlySide)
{
  const bool both = (onlySide < 0);
  const bool sideL = both || onlySide == 0;
  const bool sideR = both || onlySide == 1;

  if (ok)
  {
    if (sideL) motionTcpOnFeedOk(false);
    if (sideR) motionTcpOnFeedOk(true);
    return;
  }
  if (strstr(feedFaultReason, "lectura OM") != nullptr)
    motionTcpOnEncoderError();
  else
  {
    if (sideL) motionTcpOnFeedNg(false);
    if (sideR) motionTcpOnFeedNg(true);
  }
}


static void sendJson(int code, const String& body)
{
  server.send(code, "application/json", body);
}

static void feedApplyDecRampArgs(uint16_t& decRampMsL, uint16_t& decRampMsR)
{
  if (server.hasArg("decRampMsL")) decRampMsL = clampFeedRampMs((uint32_t)server.arg("decRampMsL").toInt());
  if (server.hasArg("decRampMsR")) decRampMsR = clampFeedRampMs((uint32_t)server.arg("decRampMsR").toInt());
  if (server.hasArg("decRampMs") && !server.hasArg("decRampMsL") && !server.hasArg("decRampMsR"))
  {
    const uint16_t legacy = clampFeedRampMs((uint32_t)server.arg("decRampMs").toInt());
    decRampMsL = legacy;
    decRampMsR = legacy;
  }
}

static void handleGetFeedTestConfig()
{
  float tMmL = feedTargetMmL;
  float tMmR = feedTargetMmR;
  float vMmL = feedPpToMmS(feedSsFastPpL, false);
  float vMmR = feedPpToMmS(feedSsFastPpR, true);
  uint16_t decRampMsL = feedDecRampMsL;
  uint16_t decRampMsR = feedDecRampMsR;
  if (server.hasArg("solidMm")) tMmL = server.arg("solidMm").toFloat();
  if (server.hasArg("solidMmR")) tMmR = server.arg("solidMmR").toFloat();
  if (server.hasArg("velocityMmSL")) vMmL = server.arg("velocityMmSL").toFloat();
  else if (server.hasArg("velocityMmS")) vMmL = server.arg("velocityMmS").toFloat();
  if (server.hasArg("velocityMmSR")) vMmR = server.arg("velocityMmSR").toFloat();
  feedApplyDecRampArgs(decRampMsL, decRampMsR);

  FeedProfilePlan planL = feedProfilePlan(tMmL, vMmL, false, decRampMsL);
  FeedProfilePlan planR = feedProfilePlan(tMmR, vMmR, true, decRampMsR);
  planL.valid = feedProfileSideValid(tMmL, vMmL, false, decRampMsL);
  planR.valid = feedProfileSideValid(tMmR, vMmR, true, decRampMsR);

  String response = "{";
  response += "\"ok\":true";
  response += ",\"solidMm\":" + String(tMmL, 1);
  response += ",\"solidMmR\":" + String(tMmR, 1);
  response += ",\"velocityMmSL\":" + String(vMmL, 1);
  response += ",\"velocityMmSR\":" + String(vMmR, 1);
  response += ",\"decRampMsL\":" + String(decRampMsL);
  response += ",\"decRampMsR\":" + String(decRampMsR);
  response += ",\"speedMin\":" + String(FEED_MM_S_MIN, 1);
  response += ",\"speedMaxL\":" + String(feedMaxMmS(false), 1);
  response += ",\"speedMaxR\":" + String(feedMaxMmS(true), 1);
  response += ",\"rampMs\":" + String((unsigned)FEED_SERVO_RAMP_MS);
  response += ",\"accFixed\":true";
  feedProfileAppendJson(planL, response, "planL");
  feedProfileAppendJson(planR, response, "planR");
  response += "}";
  sendJson(200, response);
}

static void handleSetFeedTestConfig()
{
  if (feedPhaseIsActive()) { sendJson(409, "{\"error\":\"Feed activo\"}"); return; }
  if (server.hasArg("solidMm")) feedTargetMmL = clampFeedTargetMm(server.arg("solidMm").toFloat());
  if (server.hasArg("solidMmR")) feedTargetMmR = clampFeedTargetMm(server.arg("solidMmR").toFloat());
  if (server.hasArg("velocityMmSL")) feedSsFastPpL = feedMmSToPp(server.arg("velocityMmSL").toFloat(), false);
  else if (server.hasArg("velocityMmS")) feedSsFastPpL = feedMmSToPp(server.arg("velocityMmS").toFloat(), false);
  if (server.hasArg("velocityMmSR")) feedSsFastPpR = feedMmSToPp(server.arg("velocityMmSR").toFloat(), true);
  {
    uint16_t decRampMsL = feedDecRampMsL;
    uint16_t decRampMsR = feedDecRampMsR;
    feedApplyDecRampArgs(decRampMsL, decRampMsR);
    feedDecRampMsL = decRampMsL;
    feedDecRampMsR = decRampMsR;
  }
  feedSaveConfig();
  handleGetFeedTestConfig();
}

static void handleGetFeedCanEncoder()
{
  float official = 0.0f, mmSigned = 0.0f, mmAbs = 0.0f;
  if (!feedOmReadOfficialMm(&official, &mmSigned, &mmAbs))
  {
    sendJson(503, "{\"ok\":false,\"error\":\"OM no disponible\"}");
    return;
  }
  const float offset = feedOmGetOffsetMm();
  char buf[280];
  snprintf(buf, sizeof(buf),
           "{\"ok\":true,\"mmAbs\":%.3f,\"mm\":%.3f,\"mmOfficial\":%.2f,\"offsetMm\":%.3f,\"settled\":%s}",
           (double)mmAbs, (double)mmSigned, (double)official, (double)offset,
           feedOmIsSettled() ? "true" : "false");
  sendJson(200, buf);
}

static void handleFeedCanEncoderZero()
{
  if (!feedOmResetLocal())
  {
    sendJson(503, "{\"ok\":false,\"error\":\"OM reset fallo\"}");
    return;
  }
  sendJson(200, "{\"ok\":true,\"mmAbs\":0,\"mm\":0,\"settled\":false}");
}

static void handleFeedTestSensor()
{
  FeedTestReq req = {};
  req.solidMmL = server.hasArg("solidMm") ? server.arg("solidMm").toFloat() : feedTargetMmL;
  req.solidMmR = server.hasArg("solidMmR") ? server.arg("solidMmR").toFloat() : feedTargetMmR;
  req.fastL = feedSsFastPpL;
  req.fastR = feedSsFastPpR;
  req.decRampL = feedDecRampMsL;
  req.decRampR = feedDecRampMsR;
  req.withCut = server.hasArg("cut");
  if (server.hasArg("side"))
  {
    const String s = server.arg("side");
    if (s == "L" || s == "l") req.onlySide = 0;
    else if (s == "R" || s == "r") req.onlySide = 1;
  }
  if (server.hasArg("velocityMmS")) feedSsFastPpL = feedMmSToPp(server.arg("velocityMmS").toFloat(), false);
  if (server.hasArg("velocityMmSR")) feedSsFastPpR = feedMmSToPp(server.arg("velocityMmSR").toFloat(), true);
  {
    uint16_t decRampMsL = feedDecRampMsL;
    uint16_t decRampMsR = feedDecRampMsR;
    feedApplyDecRampArgs(decRampMsL, decRampMsR);
    feedDecRampMsL = decRampMsL;
    feedDecRampMsR = decRampMsR;
  }
  req.fastL = feedSsFastPpL;
  req.fastR = feedSsFastPpR;
  req.decRampL = feedDecRampMsL;
  req.decRampR = feedDecRampMsR;
  String err;
  if (!feedQueueTest(req, err))
  {
    const int code = (err == "Servo CAN no listo") ? 503 : 409;
    sendJson(code, String("{\"ok\":false,\"error\":\"") + jsonEscape(err) + "\"}");
    return;
  }
  sendJson(200, "{\"ok\":true,\"queued\":true}");
}

static void handleGetFeedOffset()
{
  String r = "{\"offsetMm\":" + String(feedOffsetMm, 2) + ",\"offsetMmB\":" + String(feedOffsetMmB, 2) + "}";
  sendJson(200, r);
}

static void handleSetFeedOffset()
{
  if (server.hasArg("offsetMm")) feedOffsetMm = clampFeedOffsetMm(server.arg("offsetMm").toFloat());
  if (server.hasArg("offsetMmB")) feedOffsetMmB = clampFeedOffsetMm(server.arg("offsetMmB").toFloat());
  feedSaveConfig();
  handleGetFeedOffset();
}

static void handleGetFeedCal()
{
  String r = "{";
  r += "\"targetMmL\":" + String(feedTargetMmL, 1);
  r += ",\"targetMmR\":" + String(feedTargetMmR, 1);
  r += ",\"countsPerMmL\":" + String(feedCalCountsPerMmL, 2);
  r += ",\"countsPerMmR\":" + String(feedCalCountsPerMmR, 2);
  r += "}";
  sendJson(200, r);
}

static void handleSetFeedCal()
{
  if (server.hasArg("side") && server.hasArg("measuredMm"))
  {
    const bool sideR = server.arg("side") == "R";
    const float measured = server.arg("measuredMm").toFloat();
    const float target = sideR ? feedTargetMmR : feedTargetMmL;
    const float oldSpm = sideR ? feedCalCountsPerMmR : feedCalCountsPerMmL;
    if (measured > FEED_CAL_MEASURED_MIN_MM && target > 0.0f)
    {
      const float newSpm = clampFeedCalCountsPerMm(oldSpm * (target / measured));
      if (sideR) feedCalCountsPerMmR = newSpm;
      else feedCalCountsPerMmL = newSpm;
    }
  }
  feedSaveConfig();
  handleGetFeedCal();
}

static void handleFeedStatus() { sendJson(200, feedStatusJson()); }

void feedRegisterHttpRoutes(WebServer& srv)
{
  (void)srv;
  server.on("/getFeedTestConfig", HTTP_GET, handleGetFeedTestConfig);
  server.on("/setFeedTestConfig", HTTP_GET, handleSetFeedTestConfig);
  server.on("/api/feed/encoder", HTTP_GET, handleGetFeedCanEncoder);
  server.on("/api/feed/encoder/zero", HTTP_POST, handleFeedCanEncoderZero);
  server.on("/feedTestSensor", HTTP_GET, handleFeedTestSensor);
  server.on("/getFeedOffset", HTTP_GET, handleGetFeedOffset);
  server.on("/setFeedOffset", HTTP_GET, handleSetFeedOffset);
  server.on("/getFeedCal", HTTP_GET, handleGetFeedCal);
  server.on("/setFeedCal", HTTP_GET, handleSetFeedCal);
  server.on("/api/feed/status", HTTP_GET, handleFeedStatus);
}