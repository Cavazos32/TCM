#include "Servo_Feed.h"
#include "FeederCan.h"
#include "MotionStates.h"
#include "driver/twai.h"
#include <Preferences.h>
#include <math.h>
#include <WebServer.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

extern WebServer server;

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

// Prime halt/QSDec solo del lado activo (no tocar el otro servo en paralelo).
static void feedCanPrimeHaltDecelSide(bool sideR)
{
  const uint32_t canId = sideR ? SERVO_CAN_TX_R : SERVO_CAN_TX_L;
  byte haltOpt[8] = {0x2B, 0x5D, 0x60, 0x00, (byte)FEED_HALT_OPTION_CODE, 0x00, 0x00, 0x00};
  sendCANMessage(canId, 8, haltOpt, sideR ? "HaltOpt R" : "HaltOpt L", false);
  uint32_t hd = FEED_HALT_DECEL_PP;
  byte qdec[8] = {
    0x23, 0x85, 0x60, 0x00,
    (byte)(hd & 0xFF), (byte)((hd >> 8) & 0xFF),
    (byte)((hd >> 16) & 0xFF), (byte)((hd >> 24) & 0xFF)
  };
  sendCANMessage(canId, 8, qdec, sideR ? "QSDec R" : "QSDec L", false);
}

void canHaltSide(bool sideR) { sendCanHaltImmediate(sideR); }
void canHalt() { canHaltSide(false); canHaltSide(true); }

// Halt lo antes posible al flanco láser: re-prime QSDec + ráfaga de CW Halt.
static void feedLaserHaltNow(bool sideR)
{
  feedCanPrimeHaltDecelSide(sideR);
  for (uint8_t i = 0; i < FEED_LASER_HALT_BURST; i++)
    sendCanHaltImmediate(sideR);
}

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

// steps con signo: + = avance feed, − = retroceso (corrección). No clampear a 0.
// false = no se emitió el movimiento pedido (SDO 6064 falló o bus bloqueado).
bool canMoveRelativePP(int32_t stepsL, int32_t stepsR)
{
  if (stepsL == 0 && stepsR == 0) return true;
  if (canBusMotionBlocked()) return false;
  const bool wantL = (stepsL != 0);
  const bool wantR = (stepsR != 0);
  bool doL = wantL;
  bool doR = wantR;
  int32_t posL = 0, posR = 0;
  if (doL && !canReadSdoI32(SERVO_NODE_L, SERVO_OD_POSITION_ACTUAL, 0x00, posL, CAN_SDO_TIMEOUT_MS)) doL = false;
  if (doR && !canReadSdoI32(SERVO_NODE_R, SERVO_OD_POSITION_ACTUAL, 0x00, posR, CAN_SDO_TIMEOUT_MS)) doR = false;
  // Si cualquier lado pedido falló la lectura, no mover (evita timeout disfrazado).
  if ((wantL && !doL) || (wantR && !doR)) return false;
  const int32_t absL = doL ? (posL + (int32_t)SERVO_CMD_SIGN_L * stepsL) : 0;
  const int32_t absR = doR ? (posR + (int32_t)SERVO_CMD_SIGN_R * stepsR) : 0;
  canControlWordSides(doL, doR, SERVO_CW_HALT_HOLD, "halt hold");
  canWriteTargetAbs(doL, doR, absL, absR);
  canControlWordSides(doL, doR, SERVO_CW_RESET_HALT, "reset+halt");
  canControlWordSides(doL, doR, SERVO_CW_NEW_ABS_HALT, "new+abs+halt");
  canControlWordSides(doL, doR, SERVO_CW_RUN_ABSOLUTE, "run absolute");
  return true;
}

static void canWriteU32OneNode(bool sideR, uint16_t index, uint32_t val, const char* desc)
{
  const uint32_t canId = sideR ? SERVO_CAN_TX_R : SERVO_CAN_TX_L;
  byte data[8] = {
    0x23, (byte)(index & 0xFF), (byte)((index >> 8) & 0xFF), 0x00,
    (byte)(val & 0xFF), (byte)((val >> 8) & 0xFF),
    (byte)((val >> 16) & 0xFF), (byte)((val >> 24) & 0xFF)
  };
  sendCANMessage(canId, 8, data, String(desc) + (sideR ? " R" : " L"), false);
}

static void feedCanSetVelAccSide(bool sideR, uint32_t velPp)
{
  if (canBusMotionBlocked()) return;
  if (velPp < 1) velPp = 1;
  extern volatile uint16_t feedDecRampMsL;
  extern volatile uint16_t feedDecRampMsR;
  const uint16_t decMs = sideR ? feedDecRampMsR : feedDecRampMsL;
  auto accFor = [](uint32_t vel, uint16_t rampMs) -> uint32_t {
    if (vel < 1) vel = 1;
    if (rampMs < 1) rampMs = 1;
    uint64_t a = ((uint64_t)vel * 1000ULL) / rampMs;
    if (a < FEED_SERVO_ACC_MIN) a = FEED_SERVO_ACC_MIN;
    if (a > FEED_SERVO_ACC_MAX) a = FEED_SERVO_ACC_MAX;
    return (uint32_t)a;
  };
  const uint32_t a = accFor(velPp, FEED_SERVO_RAMP_MS);
  const uint32_t d = accFor(velPp, decMs);
  canWriteU32OneNode(sideR, SERVO_OD_PROFILE_VELOCITY, velPp, "Vel");
  canWriteU32OneNode(sideR, SERVO_OD_PROFILE_ACCEL, a, "Acc");
  canWriteU32OneNode(sideR, SERVO_OD_PROFILE_DECEL, d, "Dec");
}

void servoCanInitMutex()
{
  if (!canMutex) canMutex = xSemaphoreCreateMutex();
}

static Preferences feedPrefs;

// —— Estado por lado (índice 0=L, 1=R) ——
struct FeedSideRt {
  bool pending = false;
  bool active = false;
  // Purga/refill HMI: feed físico sin validar láser ni OM (LengthOK al fin de servo).
  bool skipValidate = false;
  // >0 solo en skipValidate (override de FEED_TARGET_FIXED_MM). 0 = 55 mm.
  float overrideTargetMm = 0.0f;
  uint32_t gen = 0;
  FeedSidePhase phase = FSP_IDLE;
  float targetMm = FEED_TARGET_FIXED_MM;
  float approachMm = 0.0f;
  float approachOmMm = -1.0f;
  float correctionMm = 0.0f;
  float finalOmMm = -1.0f;
  float errAbsBeforeCorr = 0.0f;
  bool laserState = false;
  uint8_t correctionCount = 0;
  bool laserSeekDone = false;  // seek gated: solo una vez post-corrección
  FeedValResult result = FVR_NONE;
  uint16_t errByte = 0;
  char fault[FEED_FAULT_REASON_MAX] = "";
  uint32_t opStartMs = 0;
  uint32_t moveStartMs = 0;
  uint32_t absDueMs = 0;
  uint32_t lastTrPollMs = 0;
  uint32_t lastLaserPollMs = 0;
  uint32_t seekDeadlineMs = 0;
  uint32_t settleUntilMs = 0;
  uint8_t omReadMiss = 0;
  bool dirChecked = false;
  int32_t moveSteps = 0;
};

static FeedSideRt feedSides[2];
static uint32_t feedGenCounter = 1;

// Fuente única de índice: sideR=false → 0 (L); sideR=true → 1 (R).
static inline uint8_t feedSideIx(bool sideR) { return sideR ? 1u : 0u; }
static inline FeedSideRt& feedSideAt(bool sideR) { return feedSides[feedSideIx(sideR)]; }

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
volatile float feedApproachPct = FEED_APPROACH_PCT_DEFAULT;
volatile float feedMoveSpeedPct = FEED_MOVE_SPEED_PCT_DEFAULT;
volatile uint32_t feedLaserSeekMs = FEED_LASER_SEEK_MS_DEFAULT;
FeedTestReq feedTestReq = {};  // legacy HTTP both-sides; TCP usa feedSides[].pending

bool feedCalibrationTest = false;
bool feedRuntimeActive = false;
char feedFaultReason[FEED_FAULT_REASON_MAX] = "";
uint16_t feedErrorCode = 0;
float feedOmLastOfficialMm = -1.0f;
float feedOmLastMmSigned = 0.0f;
float feedOmLastMmAbs = -1.0f;
float omPhase1Mm = 0.0f;
bool feedOmLengthMet = false;

template<typename T>
static T clampVal(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

float clampFeedTargetMm(float mm) { return clampVal(mm, FEED_TARGET_MM_MIN, FEED_TARGET_MM_MAX); }
float clampFeedOffsetMm(float mm) { return clampVal(mm, FEED_OFFSET_MM_MIN, FEED_OFFSET_MM_MAX); }
float clampFeedCalCountsPerMm(float spm) { return clampVal(spm, FEED_CAL_COUNTS_PER_MM_MIN, FEED_CAL_COUNTS_PER_MM_MAX); }
uint16_t clampFeedRampMs(uint32_t ms) { return (uint16_t)clampVal(ms, (uint32_t)FEED_SERVO_RAMP_MS_MIN, (uint32_t)FEED_SERVO_RAMP_MS_MAX); }
int32_t clampFeedTestChunkSteps(int32_t steps) { return clampVal(steps, (int32_t)FEED_TEST_CHUNK_MIN, (int32_t)FEED_TEST_CHUNK_MAX); }
uint32_t clampFeedSsSpeedPp(uint32_t pp) { return clampVal(pp, (uint32_t)FEED_SERVO_BASE_PP_MIN, (uint32_t)FEED_SERVO_BASE_PP_MAX); }
float clampFeedApproachPct(float pct) { return clampVal(pct, FEED_APPROACH_PCT_MIN, FEED_APPROACH_PCT_MAX); }
float clampFeedMoveSpeedPct(float pct) { return clampVal(pct, FEED_MOVE_SPEED_PCT_MIN, FEED_MOVE_SPEED_PCT_MAX); }
uint32_t clampFeedLaserSeekMs(uint32_t ms) {
  return clampVal(ms, (uint32_t)FEED_LASER_SEEK_MS_MIN, (uint32_t)FEED_LASER_SEEK_MS_MAX);
}

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

// mm con signo → cmd steps (+ avance, − retroceso)
static int32_t feedMmToCmdStepsSigned(float mm, bool sideR)
{
  const float spm = feedCalibratedCountsPerMm(sideR);
  const float absMm = fabsf(mm);
  int32_t enc = (int32_t)(absMm * spm + 0.5f);
  int32_t steps = feedEncCountsToCmdSteps(enc);
  if (steps < 1 && absMm >= FEED_OM_QUANTUM_MM * 0.5f) steps = 1;
  return (mm < 0.0f) ? -steps : steps;
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

static bool feedInPhysical(float om)
{
  return om >= FEED_OM_PHYS_MIN_MM - 1e-4f && om <= FEED_OM_PHYS_MAX_MM + 1e-4f;
}

static const char* feedValResultName(FeedValResult r)
{
  switch (r) {
    case FVR_OK: return "FEED_OK";
    case FVR_CORRECT: return "FEED_CORRECT";
    case FVR_NG: return "FEED_NG";
    case FVR_INCONSISTENT: return "FEED_INCONSISTENT";
    default: return "NONE";
  }
}

static const char* feedSidePhaseName(FeedSidePhase p)
{
  switch (p) {
    case FSP_APPROACH: return "APPROACH";
    case FSP_HALT_SETTLE: return "HALT_SETTLE";
    case FSP_WAIT_SERVO: return "WAIT_SERVO";
    case FSP_SETTLE: return "SETTLE";
    case FSP_VALIDATE: return "VALIDATE";
    case FSP_CORRECTION: return "CORRECTION";
    case FSP_WAIT_SERVO_CORR: return "WAIT_SERVO_CORR";
    case FSP_SETTLE_FINAL: return "SETTLE_FINAL";
    case FSP_VALIDATE_FINAL: return "VALIDATE_FINAL";
    case FSP_LASER_SEEK: return "LASER_SEEK";
    case FSP_LASER_SEEK_HALT: return "LASER_SEEK_HALT";
    case FSP_DONE_OK: return "DONE_OK";
    case FSP_DONE_NG: return "DONE_NG";
    default: return "IDLE";
  }
}

// Feed Validator: solo sobre OM oficial post-SETTLE (nunca sobre approachMm del servo).
// 1ª medida: ~44 (80%) → CORREGIR a 55. PHYS 50–58 no salta la corrección (evita que R
// con offset/overshoot se quede solo en approach). |err|≤0.5 + láser → OK ya en target.
// FINAL: PHYS 50–58 + láser ON → FEED_OK.
static FeedValResult feedValidatorEvaluate(float omOfficialMm, bool laserOn, bool isFinal,
                                           float* correctionOut)
{
  if (correctionOut) *correctionOut = 0.0f;

  const float err = FEED_TARGET_FIXED_MM - omOfficialMm;

  if (!isFinal) {
    // 1ª OM tras approach: no aplicar PHYS_MIN (evita LengthNG por OM≈approachMm).
    // Solo absurdo: sin lectura útil o por encima del techo físico.
    if (omOfficialMm <= 0.0f || omOfficialMm > FEED_OM_PHYS_MAX_MM + 1e-4f)
      return FVR_NG;
    if (fabsf(err) <= FEED_OM_QUANTUM_MM + 1e-4f) {
      if (!laserOn) return FVR_INCONSISTENT;
      return FVR_OK;
    }
    if (correctionOut) *correctionOut = err;
    return FVR_CORRECT;
  }

  // Medición FINAL: PHYS 50–58 + láser (sin ventana control 54–56)
  if (!feedInPhysical(omOfficialMm))
    return FVR_NG;
  if (!laserOn)
    return FVR_INCONSISTENT;
  return FVR_OK;
}

static uint32_t feedSideNominalPp(bool sideR)
{
  return clampFeedSsSpeedPp(sideR ? feedSsFastPpR : feedSsFastPpL);
}

static uint32_t feedSideMovePp(bool sideR)
{
  const uint32_t nom = feedSideNominalPp(sideR);
  uint32_t half = (uint32_t)((float)nom * (feedMoveSpeedPct / 100.0f) + 0.5f);
  if (half < FEED_SERVO_BASE_PP_MIN) half = FEED_SERVO_BASE_PP_MIN;
  return clampFeedSsSpeedPp(half);
}

static uint32_t feedSsCmdTimeMs(int32_t steps, uint32_t vel, uint16_t decRampMs)
{
  const int32_t absSteps = steps >= 0 ? steps : -steps;
  if (absSteps <= 0 || vel < 1) return 0;
  const uint32_t acc = feedProfileAccForVelMs(vel, FEED_SERVO_RAMP_MS);
  const uint32_t dec = feedProfileAccForVelMs(vel, decRampMs);
  const uint64_t sMin = ((uint64_t)vel * vel) / (2ULL * acc) + ((uint64_t)vel * vel) / (2ULL * dec);
  if ((uint64_t)absSteps <= sMin)
  {
    const uint32_t aEff = (acc < dec) ? acc : dec;
    return (uint32_t)(1000.0 * sqrt((2.0 * absSteps) / (double)aEff) + 0.5);
  }
  const uint64_t cruise = (uint64_t)absSteps - sMin;
  return (uint32_t)(1000.0 * ((double)vel / acc + (double)cruise / vel + (double)vel / dec) + 0.5);
}

static bool feedServoTargetReachedNode(uint8_t nodeId)
{
  uint16_t sw = 0;
  if (!canReadStatusWord(nodeId, sw, CAN_STATUS_POLL_MS)) return false;
  return (sw & SERVO_SW_TARGET_REACHED) != 0;
}

bool feedSideIsActive(bool sideR)
{
  const FeedSideRt& s = feedSideAt(sideR);
  return s.active && s.phase != FSP_IDLE && s.phase != FSP_DONE_OK && s.phase != FSP_DONE_NG;
}

bool feedPhaseIsActive()
{
  return feedSideIsActive(false) || feedSideIsActive(true)
      || feedSides[0].pending || feedSides[1].pending;
}

bool feedThisCycleSucceeded()
{
  const bool lDone = feedSides[0].phase == FSP_DONE_OK;
  const bool rDone = feedSides[1].phase == FSP_DONE_OK;
  const bool lNg = feedSides[0].phase == FSP_DONE_NG;
  const bool rNg = feedSides[1].phase == FSP_DONE_NG;
  if (lNg || rNg) return false;
  if (feedSides[0].active == false && feedSides[1].active == false) {
    // última operación: OK si al menos un lado DONE_OK y ninguno NG reciente
    return (feedSides[0].result == FVR_OK || feedSides[1].result == FVR_OK)
        && feedSides[0].result != FVR_NG && feedSides[0].result != FVR_INCONSISTENT
        && feedSides[1].result != FVR_NG && feedSides[1].result != FVR_INCONSISTENT;
  }
  return lDone || rDone;
}

static void feedSideClearDiag(FeedSideRt& s)
{
  s.approachOmMm = -1.0f;
  s.correctionMm = 0.0f;
  s.finalOmMm = -1.0f;
  s.errAbsBeforeCorr = 0.0f;
  s.laserState = false;
  s.correctionCount = 0;
  s.laserSeekDone = false;
  s.lastLaserPollMs = 0;
  s.seekDeadlineMs = 0;
  s.result = FVR_NONE;
  s.errByte = 0;
  s.fault[0] = '\0';
  s.omReadMiss = 0;
  s.dirChecked = false;
  s.moveSteps = 0;
}

// Globals = "último lado que terminó" (HTTP/overview). Fuente de verdad = feedSides[].
static void feedPublishLegacyDiag(const FeedSideRt& s, FeedValResult result)
{
  strncpy(feedFaultReason, s.fault, sizeof(feedFaultReason) - 1);
  feedFaultReason[sizeof(feedFaultReason) - 1] = '\0';
  feedErrorCode = s.errByte;
  feedOmLastOfficialMm = (s.finalOmMm >= 0.0f) ? s.finalOmMm : s.approachOmMm;
  omPhase1Mm = s.approachOmMm;
  feedOmLengthMet = (result == FVR_OK);
}

static void feedSideFinish(bool sideR, FeedValResult result, uint8_t errByte, const char* reason)
{
  FeedSideRt& s = feedSideAt(sideR);
  const uint32_t gen = s.gen;
  s.result = result;
  s.errByte = errByte;
  s.skipValidate = false;
  s.overrideTargetMm = 0.0f;
  if (reason && reason[0]) {
    strncpy(s.fault, reason, sizeof(s.fault) - 1);
    s.fault[sizeof(s.fault) - 1] = '\0';
  }
  feedPublishLegacyDiag(s, result);

  const bool ok = (result == FVR_OK);
  s.phase = ok ? FSP_DONE_OK : FSP_DONE_NG;
  s.active = false;

  // Solo notificar si la generación sigue siendo la de esta operación (anti-stale).
  // LengthNG siempre: el wait HMI no depende solo del detalle EXXX.
  if (gen != 0 && gen == feedSideAt(sideR).gen) {
    if (ok)
      motionTcpOnFeedOk(sideR);
    else {
      // LengthNG cierra el wait HMI (resultado único de Feed).
      motionTcpOnFeedNg(sideR);
      // Detalle EXXX: actuador/enlace/encoder + LR-X en ventana de validación.
      // MOT_ERR_TOLERANCE_WINDOW no se detalla (evita duplicar con LengthNG).
      if (errByte == MOT_ERR_FEED_TIMEOUT
          || errByte == MOT_ERR_FEED_CAN_NO_RESP
          || errByte == MOT_ERR_FEED_DIR_CW
          || errByte == MOT_ERR_ENCODER_NO_PULSES
          || errByte == MOT_ERR_FEED_L_NEG_TARGET
          || errByte == MOT_ERR_FEED_R_NEG_TARGET
          || errByte == MOT_ERR_FEED_L_NO_FB
          || errByte == MOT_ERR_FEED_R_NO_FB
          || errByte == MOT_ERR_LASER_R
          || errByte == MOT_ERR_LASER_L)
        motionTcpOnDetailErrorSide(sideR, errByte, sideR ? "FeedR" : "FeedL");
    }
  }

  Serial.printf("FEED %c gen=%lu result=%s omA=%.2f corr=%.2f omF=%.2f laser=%d phase=%s\n",
                sideR ? 'R' : 'L', (unsigned long)gen, feedValResultName(result),
                (double)s.approachOmMm, (double)s.correctionMm, (double)s.finalOmMm,
                (int)s.laserState, feedSidePhaseName(s.phase));
}

// Emite move solo en el servo de sideR. false = SDO posición falló (sin movimiento).
static bool feedSideIssueMove(bool sideR, int32_t steps, uint32_t now)
{
  FeedSideRt& s = feedSideAt(sideR);
  const uint32_t vel = feedSideMovePp(sideR);
  const uint16_t decMs = sideR ? feedDecRampMsR : feedDecRampMsL;
  feedCanSetVelAccSide(sideR, vel);
  const bool moved = sideR ? canMoveRelativePP(0, steps) : canMoveRelativePP(steps, 0);
  if (!moved) return false;
  s.moveSteps = steps;
  s.moveStartMs = now;
  s.absDueMs = now + feedSsCmdTimeMs(steps, vel, decMs) + FEED_SS_ABS_MARGIN_MS;
  s.lastTrPollMs = 0;
  return true;
}

static uint8_t feedSideNoFbErr(bool sideR)
{
  return sideR ? MOT_ERR_FEED_R_NO_FB : MOT_ERR_FEED_L_NO_FB;
}

static bool feedSidePollServoDone(bool sideR, uint32_t now)
{
  FeedSideRt& s = feedSideAt(sideR);
  if (s.moveStartMs == 0) return false;
  if (now < s.moveStartMs + FEED_SS_POLL_MIN_MS) return false;
  if (s.absDueMs != 0 && now < s.absDueMs) return false;
  if (s.lastTrPollMs != 0 && now - s.lastTrPollMs < FEED_SS_TR_POLL_MS) return false;
  s.lastTrPollMs = now;
  if (!servoCanReady) return false;
  const uint8_t node = sideR ? SERVO_NODE_R : SERVO_NODE_L;
  return feedServoTargetReachedNode(node);
}

static bool feedSideStartLaserSeek(bool sideR, uint32_t now, const char* why)
{
  FeedSideRt& s = feedSideAt(sideR);
  const uint8_t laserErr = sideR ? MOT_ERR_LASER_R : MOT_ERR_LASER_L;

  // Láser ya ON (GPIO crudo): no mandar seek — halt y OK.
  if (feedLaserMaterialPresentRaw(sideR)) {
    s.laserState = true;
    s.laserSeekDone = true;
    feedLaserHaltNow(sideR);
    Serial.printf("FEED %c LASER_SEEK skip (%s): laser already ON\n",
                  sideR ? 'R' : 'L', why ? why : "");
    s.settleUntilMs = now + FEED_HALT_SETTLE_MS;
    s.phase = FSP_LASER_SEEK_HALT;
    return true;
  }

  const float mmS = feedPpToMmS(feedSideMovePp(sideR), sideR);
  const float seekMm = mmS * ((float)feedLaserSeekMs / 1000.0f) * FEED_LASER_SEEK_DIST_MARGIN;
  const int32_t steps = feedMmToCmdStepsSigned(seekMm, sideR);
  if (steps <= 0) {
    feedSideFinish(sideR, FVR_INCONSISTENT, laserErr, "FEED_INCONSISTENT");
    return true;
  }
  Serial.printf("FEED %c LASER_SEEK (%s) %.1fmm timeout=%lums\n",
                sideR ? 'R' : 'L', why ? why : "?", (double)seekMm,
                (unsigned long)feedLaserSeekMs);
  feedCanPrimeHaltDecelSide(sideR);
  if (!feedSideIssueMove(sideR, steps, now)) {
    feedSideFinish(sideR, FVR_NG, feedSideNoFbErr(sideR), "FEED: sin feedback 6064");
    return true;
  }
  s.laserSeekDone = true;
  s.seekDeadlineMs = now + feedLaserSeekMs;
  s.lastLaserPollMs = 0;
  s.phase = FSP_LASER_SEEK;
  return true;
}

static void feedSideStartApproach(bool sideR)
{
  FeedSideRt& s = feedSideAt(sideR);
  feedSideClearDiag(s);
  s.targetMm = FEED_TARGET_FIXED_MM;
  if (s.skipValidate && s.overrideTargetMm >= 1.0f)
    s.targetMm = clampFeedTargetMm(s.overrideTargetMm);
  s.approachMm = s.targetMm * (feedApproachPct / 100.0f);
  // Purga: un solo movimiento a target (sin corrección OM/láser).
  if (s.skipValidate)
    s.approachMm = s.targetMm;
  // approachMm = comando servo intermedio (p.ej. 44). No es medición OM ni pasa por PHYS 50–58.
  s.gen = feedGenCounter++;
  if (s.gen == 0) s.gen = feedGenCounter++;
  // Descarta LengthOK/NG/detalle stale de una operación anterior de este lado.
  motionTcpClearFeedSidePending(sideR);
  s.opStartMs = millis();
  s.active = true;
  s.pending = false;
  s.moveStartMs = 0;
  s.absDueMs = 0;

  // Purga: un movimiento a target+offset. Ciclo: approach = % de 55, sin offset
  // (si se suma offR al 80%, R manda ~50–55 y el validador se saltaba la corrección).
  const float offset = sideR ? feedOffsetMmB : feedOffsetMm;
  const float planOffset = s.skipValidate ? offset : 0.0f;
  FeedEncPlan plan = feedPlanEncTarget(s.approachMm, planOffset, sideR);
  if (!plan.ok) {
    s.phase = FSP_APPROACH;
    feedSideFinish(sideR, FVR_NG,
                   sideR ? MOT_ERR_FEED_R_NEG_TARGET : MOT_ERR_FEED_L_NEG_TARGET,
                   plan.error ? plan.error : "target+offset neg");
    return;
  }

  const int32_t steps = feedEncCountsToCmdSteps(plan.targetCounts);
  if (steps <= 0) {
    s.phase = FSP_APPROACH;
    feedSideFinish(sideR, FVR_NG, MOT_ERR_ENCODER_NO_PULSES, "approach steps=0");
    return;
  }
  s.moveSteps = steps;  // pending approach steps tras halt settle

  sendCanHaltImmediate(sideR);
  s.settleUntilMs = millis() + FEED_HALT_SETTLE_MS;
  s.phase = FSP_HALT_SETTLE;
}

static void feedSideService(bool sideR, uint32_t now)
{
  FeedSideRt& s = feedSideAt(sideR);
  if (s.pending && !s.active) {
    feedSideStartApproach(sideR);
    return;
  }
  if (!s.active) return;

  if ((now - s.opStartMs) > (FEED_WAIT_TIMEOUT_MS + FEED_WAIT_EXTRA_MS)) {
    canHaltSide(sideR);
    feedSideFinish(sideR, FVR_NG, MOT_ERR_FEED_TIMEOUT, "FEED: timeout");
    return;
  }

  switch (s.phase) {
    case FSP_HALT_SETTLE:
      if (now < s.settleUntilMs) break;
      {
        // Misma secuencia física: halt → settle → reset OM del lado → approach move.
        feedOmResetSide(sideR, false);
        const int32_t steps = s.moveSteps;
        Serial.printf("FEED %c gen=%lu APPROACH %.1fmm (%.0f%% of %.1f) steps=%ld\n",
                      sideR ? 'R' : 'L', (unsigned long)s.gen, (double)s.approachMm,
                      (double)feedApproachPct, (double)s.targetMm, (long)steps);
        feedCanPrimeHaltDecelSide(sideR);
        if (!feedSideIssueMove(sideR, steps, now)) {
          feedSideFinish(sideR, FVR_NG, feedSideNoFbErr(sideR), "FEED: sin feedback 6064");
          break;
        }
        s.phase = FSP_WAIT_SERVO;
      }
      break;

    case FSP_WAIT_SERVO:
    case FSP_WAIT_SERVO_CORR:
      // Approach/corrección: no cortar por láser aquí (OM debe llegar a target).
      // El halt por láser es solo en LASER_SEEK (compensación de presencia).
      if (feedSidePollServoDone(sideR, now)) {
        s.phase = (s.phase == FSP_WAIT_SERVO) ? FSP_SETTLE : FSP_SETTLE_FINAL;
        s.settleUntilMs = now + FEED_OM_SETTLE_MS;
        s.omReadMiss = 0;
      }
      break;

    case FSP_SETTLE:
    case FSP_SETTLE_FINAL:
      if (now < s.settleUntilMs) break;
      {
        // Purga: servo terminó → LengthOK sin láser ni ventana OM.
        if (s.skipValidate) {
          feedSideFinish(sideR, FVR_OK, 0, "FEED_OK_PURGE");
          break;
        }
        float om = 0.0f;
        if (!feedOmReadOfficialMmSide(sideR, &om)) {
          if (++s.omReadMiss < FEED_OM_READ_RETRY_MAX) {
            s.settleUntilMs = now + FEED_OM_READ_RETRY_DELAY_MS;
            break;
          }
          feedSideFinish(sideR, FVR_NG, MOT_ERR_ENCODER_NO_PULSES, "FEED: sin lectura OM");
          break;
        }
        s.omReadMiss = 0;
        const bool isFinal = (s.phase == FSP_SETTLE_FINAL);
        if (!isFinal)
          s.approachOmMm = om;
        else
          s.finalOmMm = om;
        // Snapshot legacy (último OM leído); no pisa el diag del otro lado en feedSides[].
        feedOmLastOfficialMm = om;

#if FEED_OM_REQUIRE_NEGATIVE
        float live = 0.0f;
        if (!s.dirChecked && feedOmReadLiveMmSide(sideR, &live) && fabsf(live) >= FEED_OM_DIR_CHECK_MM) {
          s.dirChecked = true;
          // L: feed = cuentas (−). R es espejo (SERVO_CMD_SIGN_R=+1): (+) es el
          // sentido correcto. Exigir (−) en R abortaba tras approach (E030).
          const bool wrongDir = sideR ? (live < 0.0f) : (live > 0.0f);
          if (wrongDir) {
            canHaltSide(sideR);
            feedSideFinish(sideR, FVR_NG, MOT_ERR_FEED_DIR_CW, "FEED: sentido horario");
            break;
          }
        }
#endif
        s.phase = isFinal ? FSP_VALIDATE_FINAL : FSP_VALIDATE;
      }
      break;

    case FSP_VALIDATE:
    case FSP_VALIDATE_FINAL:
      {
        const bool isFinal = (s.phase == FSP_VALIDATE_FINAL);
        const float om = isFinal ? s.finalOmMm : s.approachOmMm;
        // GPIO crudo en validación: evita seek “ciego” por debounce 150 ms stale OFF.
        s.laserState = feedLaserMaterialPresentRaw(sideR);
        float corr = 0.0f;
        FeedValResult vr = feedValidatorEvaluate(om, s.laserState, isFinal, &corr);
        const uint8_t laserErr = sideR ? MOT_ERR_LASER_R : MOT_ERR_LASER_L;

        if (!isFinal) {
          if (vr == FVR_OK) {
            s.finalOmMm = om;
            feedSideFinish(sideR, FVR_OK, 0, "FEED_OK");
            break;
          }
          // Approach ~55 + láser OFF → seek (no LengthNG/laser aún).
          if (vr == FVR_INCONSISTENT && !s.laserSeekDone) {
            feedSideStartLaserSeek(sideR, now, "post-approach");
            break;
          }
          if (vr == FVR_NG || vr == FVR_INCONSISTENT) {
            s.finalOmMm = om;
            feedSideFinish(sideR, vr,
                           (vr == FVR_INCONSISTENT) ? laserErr : MOT_ERR_TOLERANCE_WINDOW,
                           (vr == FVR_INCONSISTENT) ? "FEED_INCONSISTENT" : "FEED_NG phys/OM");
            break;
          }
          // FVR_CORRECT — un solo intento
          s.correctionMm = corr;
          s.errAbsBeforeCorr = fabsf(FEED_TARGET_FIXED_MM - om);
          s.correctionCount = 1;
          s.phase = FSP_CORRECTION;
          break;
        }

        // Final tras corrección
        // Seek gated: sensor OFF → avanzar hasta ON (independiente de ventana OM).
        if (!s.laserState && s.correctionCount >= 1 && !s.laserSeekDone) {
          feedSideStartLaserSeek(sideR, now, "post-corr");
          break;
        }
        if (vr == FVR_OK) {
          const float errAbs = fabsf(FEED_TARGET_FIXED_MM - om);
          if (errAbs > s.errAbsBeforeCorr + 1e-3f) {
            feedSideFinish(sideR, FVR_NG, MOT_ERR_TOLERANCE_WINDOW, "FEED: correccion empeora");
            break;
          }
          feedSideFinish(sideR, FVR_OK, 0, "FEED_OK");
          break;
        }
        feedSideFinish(sideR, vr,
                       (vr == FVR_INCONSISTENT) ? laserErr : MOT_ERR_TOLERANCE_WINDOW,
                       feedValResultName(vr));
      }
      break;

    case FSP_LASER_SEEK:
      {
        const uint8_t laserErr = sideR ? MOT_ERR_LASER_R : MOT_ERR_LASER_L;
        // GPIO crudo cada loop (sin debounce 150 ms ni throttle) → halt al flanco.
        const bool pollDue = (FEED_LASER_SEEK_POLL_MS == 0)
            || (s.lastLaserPollMs == 0)
            || ((now - s.lastLaserPollMs) >= FEED_LASER_SEEK_POLL_MS);
        if (pollDue) {
          s.lastLaserPollMs = now;
          if (feedLaserMaterialPresentRaw(sideR)) {
            s.laserState = true;
            feedLaserHaltNow(sideR);
            Serial.printf("FEED %c LASER_SEEK hit → halt inmediato\n", sideR ? 'R' : 'L');
            s.settleUntilMs = now + FEED_HALT_SETTLE_MS;
            s.phase = FSP_LASER_SEEK_HALT;
            break;
          }
        }
        if (now >= s.seekDeadlineMs) {
          feedLaserHaltNow(sideR);
          s.laserState = false;
          Serial.printf("FEED %c LASER_SEEK timeout → %s\n",
                        sideR ? 'R' : 'L', sideR ? "E004" : "E005");
          feedSideFinish(sideR, FVR_INCONSISTENT, laserErr, "FEED_INCONSISTENT");
          break;
        }
      }
      break;

    case FSP_LASER_SEEK_HALT:
      if (now < s.settleUntilMs) break;
      // Láser ON tras seek: aceptar FEED_OK e ignorar ventana PHYS/control OM.
      // Si OM ya estaba ~55 con láser OFF, el seek empuja OM fuera de 50–58.
      {
        float om = 0.0f;
        if (feedOmReadOfficialMmSide(sideR, &om)) {
          s.finalOmMm = om;
          feedOmLastOfficialMm = om;
        }
        s.laserState = true;
        Serial.printf("FEED %c LASER_SEEK OK (OM limit ignored) omF=%.2f\n",
                      sideR ? 'R' : 'L', (double)s.finalOmMm);
        feedSideFinish(sideR, FVR_OK, 0, "FEED_OK_LASER_SEEK");
      }
      break;

    case FSP_CORRECTION:
      {
        const int32_t steps = feedMmToCmdStepsSigned(s.correctionMm, sideR);
        if (steps == 0) {
          feedSideFinish(sideR, FVR_NG, MOT_ERR_TOLERANCE_WINDOW, "FEED: corr steps=0");
          break;
        }
        Serial.printf("FEED %c CORR %.2fmm steps=%ld\n",
                      sideR ? 'R' : 'L', (double)s.correctionMm, (long)steps);
        if (!feedSideIssueMove(sideR, steps, now)) {
          feedSideFinish(sideR, FVR_NG, feedSideNoFbErr(sideR), "FEED: sin feedback 6064");
          break;
        }
        s.phase = FSP_WAIT_SERVO_CORR;
      }
      break;

    default:
      break;
  }
}

void feedInit()
{
  feedLoadConfig();
  feedSides[0] = FeedSideRt{};
  feedSides[1] = FeedSideRt{};
}

void feedLoadConfig()
{
  feedPrefs.begin(FEED_PREFS_NS, true);
  feedTargetMmL = clampFeedTargetMm(feedPrefs.getFloat("tgtL", FEED_TARGET_FIXED_MM));
  feedTargetMmR = clampFeedTargetMm(feedPrefs.getFloat("tgtR", FEED_TARGET_FIXED_MM));
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
  feedApproachPct = clampFeedApproachPct(feedPrefs.getFloat("apPct", FEED_APPROACH_PCT_DEFAULT));
  feedMoveSpeedPct = clampFeedMoveSpeedPct(feedPrefs.getFloat("mvPct", FEED_MOVE_SPEED_PCT_DEFAULT));
  feedLaserSeekMs = clampFeedLaserSeekMs(feedPrefs.getUInt("seekMs", FEED_LASER_SEEK_MS_DEFAULT));
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
  feedPrefs.putFloat("apPct", feedApproachPct);
  feedPrefs.putFloat("mvPct", feedMoveSpeedPct);
  feedPrefs.putUInt("seekMs", feedLaserSeekMs);
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

  // Legacy HTTP both-sides: encolar L y R independientes
  if (feedTestReq.pending) {
    feedTestReq.pending = false;
    if (feedTestReq.onlySide < 0) {
      if (!feedSides[0].active && !feedSides[0].pending) feedSides[0].pending = true;
      if (!feedSides[1].active && !feedSides[1].pending) feedSides[1].pending = true;
    } else if (feedTestReq.onlySide == 0) {
      if (!feedSides[0].active && !feedSides[0].pending) feedSides[0].pending = true;
    } else if (feedTestReq.onlySide == 1) {
      if (!feedSides[1].active && !feedSides[1].pending) feedSides[1].pending = true;
    }
  }

  const uint32_t now = millis();
  serviceCANRx();
  feedSideService(false, now);
  feedSideService(true, now);
  feedRuntimeActive = feedPhaseIsActive();
}

static void feedSideAppendJson(String& j, bool sideR)
{
  const FeedSideRt& s = feedSideAt(sideR);
  j += sideR ? ",\"R\":{" : ",\"L\":{";
  j += "\"gen\":" + String((unsigned long)s.gen);
  j += ",\"phase\":\""; j += feedSidePhaseName(s.phase); j += "\"";
  j += ",\"active\":"; j += s.active ? "true" : "false";
  j += ",\"pending\":"; j += s.pending ? "true" : "false";
  j += ",\"targetMm\":" + String(s.targetMm, 1);
  j += ",\"approachMm\":" + String(s.approachMm, 1);
  j += ",\"approachOmMm\":" + String(s.approachOmMm, 2);
  j += ",\"correctionMm\":" + String(s.correctionMm, 2);
  j += ",\"finalOmMm\":" + String(s.finalOmMm, 2);
  j += ",\"laser\":"; j += s.laserState ? "true" : "false";
  j += ",\"correctionCount\":" + String((unsigned)s.correctionCount);
  j += ",\"laserSeekDone\":"; j += s.laserSeekDone ? "true" : "false";
  j += ",\"result\":\""; j += feedValResultName(s.result); j += "\"";
  j += ",\"errByte\":" + String((unsigned)s.errByte);
  j += ",\"fault\":\""; j += String(s.fault); j += "\"";
  j += "}";
}

String feedStatusJson()
{
  char uiBuf[96];
  uiBuf[0] = '\0';
  if (feedErrorCode)
    motErrFormatUi(uiBuf, sizeof(uiBuf), (uint8_t)feedErrorCode);
  const char* exxx = feedErrorCode ? motErrExxx((uint8_t)feedErrorCode) : "";

  String j = "{";
  j += "\"canInitialized\":"; j += canInitialized ? "true" : "false";
  j += ",\"canBitrateKbps\":"; j += CAN_BITRATE_KBPS;
  j += ",\"servoCanReady\":"; j += servoCanReady ? "true" : "false";
  j += ",\"feedActive\":"; j += feedPhaseIsActive() ? "true" : "false";
  j += ",\"feedOk\":"; j += feedThisCycleSucceeded() ? "true" : "false";
  j += ",\"fault\":\""; j += String(feedFaultReason); j += "\"";
  j += ",\"errByte\":"; j += String((unsigned)feedErrorCode);
  j += ",\"exxx\":\""; j += String(exxx); j += "\"";
  j += ",\"ui\":\""; j += String(uiBuf); j += "\"";
  j += ",\"omOfficial\":" + String(feedOmLastOfficialMm, 2);
  j += ",\"targetMm\":" + String(FEED_TARGET_FIXED_MM, 1);
  j += ",\"approachPct\":" + String(feedApproachPct, 1);
  j += ",\"moveSpeedPct\":" + String(feedMoveSpeedPct, 1);
  j += ",\"laserSeekMs\":" + String((unsigned long)feedLaserSeekMs);
  j += ",\"targetMmL\":" + String(feedTargetMmL, 1);
  j += ",\"targetMmR\":" + String(feedTargetMmR, 1);
  j += ",\"skipEnc\":"; j += feedSkipEncoderConfirm ? "true" : "false";
  feedSideAppendJson(j, false);
  feedSideAppendJson(j, true);
  j += "}";
  return j;
}

bool feedQueueTestSide(int8_t onlySide, String& err, bool skipValidate, float targetMm)
{
  if (!servoCanReady) {
    // EXXX oficial E023 / 0x61 (no texto suelto sin código).
    char uiBuf[96];
    motErrFormatUi(uiBuf, sizeof(uiBuf), MOT_ERR_FEED_CAN_NO_RESP);
    err = uiBuf;
    return false;
  }
  const float overrideMm =
      (skipValidate && targetMm >= 1.0f) ? clampFeedTargetMm(targetMm) : 0.0f;
  if (onlySide == 0) {
    if (feedSides[0].active || feedSides[0].pending) { err = "Feed L ocupado"; return false; }
    feedSides[0].pending = true;
    feedSides[0].skipValidate = skipValidate;
    feedSides[0].overrideTargetMm = overrideMm;
    return true;
  }
  if (onlySide == 1) {
    if (feedSides[1].active || feedSides[1].pending) { err = "Feed R ocupado"; return false; }
    feedSides[1].pending = true;
    feedSides[1].skipValidate = skipValidate;
    feedSides[1].overrideTargetMm = overrideMm;
    return true;
  }
  // both
  if (feedSides[0].active || feedSides[0].pending || feedSides[1].active || feedSides[1].pending) {
    err = "Feed ocupado";
    return false;
  }
  feedSides[0].pending = true;
  feedSides[1].pending = true;
  feedSides[0].skipValidate = skipValidate;
  feedSides[1].skipValidate = skipValidate;
  feedSides[0].overrideTargetMm = overrideMm;
  feedSides[1].overrideTargetMm = overrideMm;
  return true;
}

bool feedQueueTest(const FeedTestReq& req, String& err)
{
  feedTestReq = req;
  feedTestReq.pending = false;  // cola real = feedSides[].pending
  if (req.onlySide == 0 || req.onlySide == 1)
    return feedQueueTestSide(req.onlySide, err);
  return feedQueueTestSide(-1, err);
}

bool feedResetRuntime()
{
  feedTestReq = {};
  for (int i = 0; i < 2; i++) {
    if (feedSides[i].active)
      canHaltSide(i == 1);
  }
  delay(FEED_HALT_SETTLE_MS);
  feedSides[0] = FeedSideRt{};
  feedSides[1] = FeedSideRt{};
  feedRuntimeActive = false;
  feedFaultReason[0] = '\0';
  feedErrorCode = 0;
  feedOmLengthMet = false;
  return true;
}

// Bloqueante solo para callers legacy (HTTP/cal); el ciclo TCP usa feedLoop no bloqueante.
bool runFeedCycle(bool skipEncoderConfirm, int8_t onlySide)
{
  (void)skipEncoderConfirm;
  if (!servoCanReady) return false;
  if (feedPhaseIsActive()) return false;
  String err;
  if (!feedQueueTestSide(onlySide, err)) return false;
  const uint32_t t0 = millis();
  while (millis() - t0 < FEED_WAIT_TIMEOUT_MS + FEED_WAIT_EXTRA_MS) {
    feedLoop();
    yield();
    if (onlySide == 0 && !feedSideIsActive(false) && !feedSides[0].pending)
      return feedSides[0].result == FVR_OK;
    if (onlySide == 1 && !feedSideIsActive(true) && !feedSides[1].pending)
      return feedSides[1].result == FVR_OK;
    if (onlySide < 0 && !feedPhaseIsActive())
      return feedSides[0].result == FVR_OK && feedSides[1].result == FVR_OK;
    delay(FEED_LOOP_YIELD_MS);
  }
  return false;
}

static void feedNotifyTcpResult(bool ok, int8_t onlySide)
{
  // Compat: la FSM por lado ya notifica en feedSideFinish.
  (void)ok; (void)onlySide;
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
  float apPct = feedApproachPct;
  if (server.hasArg("approachPct")) apPct = clampFeedApproachPct(server.arg("approachPct").toFloat());
  float mvPct = feedMoveSpeedPct;
  if (server.hasArg("moveSpeedPct")) mvPct = clampFeedMoveSpeedPct(server.arg("moveSpeedPct").toFloat());
  uint32_t seekMs = feedLaserSeekMs;
  if (server.hasArg("laserSeekMs")) seekMs = clampFeedLaserSeekMs((uint32_t)server.arg("laserSeekMs").toInt());

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
  response += ",\"approachPct\":" + String(apPct, 1);
  response += ",\"moveSpeedPct\":" + String(mvPct, 1);
  response += ",\"laserSeekMs\":" + String((unsigned long)seekMs);
  response += ",\"laserSeekMsMin\":" + String((unsigned)FEED_LASER_SEEK_MS_MIN);
  response += ",\"laserSeekMsMax\":" + String((unsigned)FEED_LASER_SEEK_MS_MAX);
  response += ",\"targetFixedMm\":" + String(FEED_TARGET_FIXED_MM, 1);
  response += ",\"approachMm\":" + String(FEED_TARGET_FIXED_MM * apPct / 100.0f, 1);
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
  if (server.hasArg("approachPct"))
    feedApproachPct = clampFeedApproachPct(server.arg("approachPct").toFloat());
  if (server.hasArg("moveSpeedPct"))
    feedMoveSpeedPct = clampFeedMoveSpeedPct(server.arg("moveSpeedPct").toFloat());
  if (server.hasArg("laserSeekMs"))
    feedLaserSeekMs = clampFeedLaserSeekMs((uint32_t)server.arg("laserSeekMs").toInt());
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
  // mmOfficial = lectura visible (base). mmProcessed = base + offset (Feed/Motion).
  const float displayMm = official - offset;
  char buf[320];
  snprintf(buf, sizeof(buf),
           "{\"ok\":true,\"mmAbs\":%.3f,\"mm\":%.3f,\"mmOfficial\":%.2f,"
           "\"mmProcessed\":%.2f,\"offsetMm\":%.3f,\"settled\":%s}",
           (double)mmAbs, (double)mmSigned, (double)displayMm,
           (double)official, (double)offset,
           feedOmIsSettled() ? "true" : "false");
  sendJson(200, buf);
}

static void handleFeedCanEncoderZero()
{
  String side = server.hasArg("side") ? server.arg("side") : "";
  bool ok = false;
  if (side == "L" || side == "l")
    ok = feedOmResetSide(false);
  else if (side == "R" || side == "r")
    ok = feedOmResetSide(true);
  else
    ok = feedOmResetLocal();
  if (!ok) {
    sendJson(503, "{\"ok\":false,\"error\":\"OM reset fallo\"}");
    return;
  }
  sendJson(200, String("{\"ok\":true,\"mmAbs\":0,\"mm\":0,\"settled\":true,\"side\":\"") +
           (side.length() ? side : "LR") + "\"}");
}

static void handleFeedReset()
{
  // Paralelo al reset de errores HMI (limpia FSM Feed) + cero OM L/R para re-test.
  feedResetRuntime();
  (void)feedOmResetSide(false);
  (void)feedOmResetSide(true);
  String out = feedStatusJson();
  if (out.length() >= 1 && out[0] == '{')
    out = String("{\"feedReset\":true,\"omZero\":true,") + out.substring(1);
  sendJson(200, out);
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
  if (server.hasArg("approachPct"))
    feedApproachPct = clampFeedApproachPct(server.arg("approachPct").toFloat());
  if (server.hasArg("moveSpeedPct"))
    feedMoveSpeedPct = clampFeedMoveSpeedPct(server.arg("moveSpeedPct").toFloat());
  if (server.hasArg("laserSeekMs"))
    feedLaserSeekMs = clampFeedLaserSeekMs((uint32_t)server.arg("laserSeekMs").toInt());
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
    const int code = (err.indexOf("E023") >= 0) ? 503 : 409;
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
  server.on("/api/feed/encoder/zero", HTTP_GET, handleFeedCanEncoderZero);
  server.on("/api/feed/reset", HTTP_POST, handleFeedReset);
  server.on("/api/feed/reset", HTTP_GET, handleFeedReset);
  server.on("/feedTestSensor", HTTP_GET, handleFeedTestSensor);
  server.on("/getFeedOffset", HTTP_GET, handleGetFeedOffset);
  server.on("/setFeedOffset", HTTP_GET, handleSetFeedOffset);
  server.on("/getFeedCal", HTTP_GET, handleGetFeedCal);
  server.on("/setFeedCal", HTTP_GET, handleSetFeedCal);
  server.on("/api/feed/status", HTTP_GET, handleFeedStatus);
}