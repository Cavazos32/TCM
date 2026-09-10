/*
  Motion — ASDA-B3 (RS-485) + OM Encoder + Servos Feeder CAN
  Config: Config.h · Asda.h · Encoder.h · FeederCan.h
  Modulos en este archivo: COMUN | ASDA | ENCODER | FEEDER+ENCODER | DECISIONES | OVERVIEW
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <math.h>
#include "Index.h"
#include "Config.h"
#include "Asda.h"
#include "Encoder.h"
#include "Servo_Feed.h"

// Overview hitos (antes de prototipos auto-generados por Arduino)
struct OvHit {
  float targetMm;
  float actualMm;
  bool  done;
  bool  ok;
};

// =============================================================================
// COMUN — Hardware global
// =============================================================================
HardwareSerial ServoBus(2);
WebServer server(80);
Preferences prefs;

// Sensores Láser (Keyence LR-X)
static const int LRX_LaserR = PIN_LRX_LASER_R;  // Sensor láser lado derecho (R)
static const int LRX_LaserL = PIN_LRX_LASER_L;  // Sensor láser lado izquierdo (L)

// =============================================================================
// ASDA — estado runtime
// =============================================================================
static float cfgMoveRpm       = DEFAULT_MOVE_SPEED_RPM;
static float cfgStepsPerMm    = FACTORY_STEPS_PER_MM;
static float cfgOffsetSteps   = 0.0f;
static float cfgOmOffsetMm    = 0.0f;
static float cfgOmOffsetMmR   = 0.0f;
static float cfgCalProg1Mm    = 0.0f;
static float cfgCalMeas1Mm    = 0.0f;
static float cfgCalProg2Mm    = 0.0f;
static float cfgCalMeas2Mm    = 0.0f;

static bool busyMotion = false;

static bool motionJobActive = false;
static bool motionCancelled = false;
static bool motionJobOk = false;
static bool motionAlarm = false;
static uint16_t motionJobCmd = 0;
static uint32_t motionJobStartMs = 0;
static uint32_t motionJobTimeoutMs = 0;
static int32_t motionJobPos = 0;
static String motionJobDetail;
static String motionAlarmMsg;

static uint16_t cachedP5007 = 0;
static int32_t cachedPosPuu = 0;
static bool cachedTriggerOk = false;
static bool cachedPosOk = false;
static uint32_t lastLiveStatusMs = 0;
static uint32_t lastMotionPollMs = 0;

// ASDA TCP — protocolo maestro (bytes tabla ASDA)
static WiFiServer asdaTcpServer(MOTION_TCP_PORT);
static WiFiClient asdaTcpClient;
static char asdaTcpRxLine[384];
static uint16_t asdaTcpRxLen = 0;
static bool asdaTcpServicesUp = false;
static bool asdaTcpWasConnected = false;
static bool asdaTcpNeedInit = true;
static bool asdaTcpStopPending = false;
static bool asdaTcpNotifyReached = false;
static int32_t asdaTcpReachedPos = 0;
static uint8_t asdaTcpLastStateByte = 0;
static bool motionTcpNotifyEncError = false;
static bool motionTcpNotifyFeedOkL = false;
static bool motionTcpNotifyFeedNgL = false;
static bool motionTcpNotifyFeedOkR = false;
static bool motionTcpNotifyFeedNgR = false;
static bool motionTcpNotifyLaserR = false;
static bool motionTcpNotifyLaserL = false;
static bool motionTcpNotifySafety = false;

// =============================================================================
// ENCODER — estado runtime (dos unidades físicas L / R)
// =============================================================================
struct EncSideState {
  bool installed;
  gpio_num_t pinA;
  gpio_num_t pinB;
  gpio_num_t pinZ;
#if ENC_PCNT_IDF5
  pcnt_unit_handle_t pcntUnit;
#else
  pcnt_unit_t pcntUnit;
#endif
  volatile int32_t pcntOverflow;
  int32_t zeroRef;
  int32_t lastCount;
  uint32_t lastMs;
  float rpm;
  float mmS;
  float mmSPeak;
  float freqHz;
  int8_t dir;
  bool wasMoving;
  bool settled;
  uint32_t stopMs;
  int32_t settledCount;
  volatile uint32_t zCount;
  bool pcntOk;
};

static portMUX_TYPE encPcntMux = portMUX_INITIALIZER_UNLOCKED;
static EncSideState encSide[2];
static bool motionTcpNotifyMeasureR = false;
static bool motionTcpNotifyMeasureL = false;
static float motionTcpLastPushMmR = -9999.0f;
static float motionTcpLastPushMmL = -9999.0f;

static bool encSideHwOk(bool sideR) {
  const uint8_t ix = encIxFromSideR(sideR);
  return encSide[ix].installed && encSide[ix].pcntOk;
}

static bool encAnyHwOk() {
  return encSideHwOk(false) || encSideHwOk(true);
}

static bool encFeedSide(bool& sideROut) {
  if (encSide[ENC_IX_L].installed && encSide[ENC_IX_L].pcntOk) {
    sideROut = false;
    return true;
  }
  if (encSide[ENC_IX_R].installed && encSide[ENC_IX_R].pcntOk) {
    sideROut = true;
    return true;
  }
  return false;
}

// =============================================================================
// OVERVIEW (ASDA + ENCODER) — estado
// =============================================================================
static const float OV_GRIPPER_MM_DEFAULT = 55.0f;
static const float OV_EXTRA_MM_DEFAULT   = 30.0f;
static const float OV_TOL_MM_DEFAULT     = 0.8f;

static bool  ovActive = false;
static float ovPieceMm = 0.0f;
static float ovGripperMm = OV_GRIPPER_MM_DEFAULT;
static float ovLinearActuatorMm = 0.0f;
static float ovExtraMm = OV_EXTRA_MM_DEFAULT;
static float ovTolMm = OV_TOL_MM_DEFAULT;
static bool  ovRequireAsda = true;
static bool  ovRequireOm = true;
static bool  ovLaserOk = false;
static bool  ovLaserSet = false;

static OvHit ovAsdaLinearActuator;
static OvHit ovAsdaDeposit;
static OvHit ovOmFeed;
static OvHit ovOmReset;
static OvHit ovOmFinal;

// =============================================================================
// COMUN — HTTP / JSON helpers
// =============================================================================
void sendJson(int code, const String& body) {
  server.send(code, "application/json", body);
}

String jsonEscape(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if ((uint8_t)c < 0x20) {
      out += ' ';
    } else {
      out += c;
    }
  }
  return out;
}

float jsonFloat(const String& body, const char* key, float defVal) {
  String needle = String("\"") + key + "\"";
  int k = body.indexOf(needle);
  if (k < 0) return defVal;
  int colon = body.indexOf(':', k + needle.length());
  if (colon < 0) return defVal;
  return body.substring(colon + 1).toFloat();
}

long jsonLong(const String& body, const char* key, long defVal) {
  String needle = String("\"") + key + "\"";
  int k = body.indexOf(needle);
  if (k < 0) return defVal;
  int colon = body.indexOf(':', k + needle.length());
  if (colon < 0) return defVal;
  return body.substring(colon + 1).toInt();
}

bool jsonHasKey(const String& body, const char* key) {
  return body.indexOf(String("\"") + key + "\"") >= 0;
}

bool jsonBool(const String& body, const char* key, bool defVal) {
  if (!jsonHasKey(body, key)) return defVal;
  String needle = String("\"") + key + "\"";
  int k = body.indexOf(needle);
  int colon = body.indexOf(':', k + needle.length());
  if (colon < 0) return defVal;
  String rest = body.substring(colon + 1);
  rest.trim();
  rest.toLowerCase();
  if (rest.startsWith("true") || rest.startsWith("1")) return true;
  if (rest.startsWith("false") || rest.startsWith("0")) return false;
  return defVal;
}

bool jsonWantsWait(const String& body) {
  if (server.hasArg("wait")) {
    String q = server.arg("wait");
    q.trim();
    q.toLowerCase();
    if (q == "0" || q == "false" || q == "no") return false;
  }
  String needle = "\"wait\"";
  int k = body.indexOf(needle);
  if (k < 0) return true;
  int colon = body.indexOf(':', k + needle.length());
  if (colon < 0) return true;
  String rest = body.substring(colon + 1);
  rest.trim();
  rest.toLowerCase();
  if (rest.startsWith("false") || rest.startsWith("0") || rest.startsWith("no"))
    return false;
  return true;
}

String jsonString(const String& body, const char* key, const char* defVal) {
  String needle = String("\"") + key + "\"";
  int k = body.indexOf(needle);
  if (k < 0) return String(defVal);
  int colon = body.indexOf(':', k + needle.length());
  int q1 = body.indexOf('"', colon + 1);
  int q2 = body.indexOf('"', q1 + 1);
  if (q1 < 0 || q2 < 0) return String(defVal);
  return body.substring(q1 + 1, q2);
}

String okJson(const String& message) {
  return String("{\"ok\":true,\"message\":\"") + jsonEscape(message) + "\"}";
}

String errJson(const String& message) {
  return String("{\"ok\":false,\"message\":\"") + jsonEscape(message) + "\"}";
}

// =============================================================================
// ASDA — alarmas y limites
// =============================================================================
void clearMotionAlarm() {
  motionAlarm = false;
  motionAlarmMsg = "";
}

void raiseMotionAlarm(const String& msg) {
  motionAlarm = true;
  motionAlarmMsg = msg;
  motionJobDetail = msg;
}

float clampMoveRpm(float rpm) {
  if (rpm < (float)MOVE_RPM_MIN) return (float)MOVE_RPM_MIN;
  if (rpm > (float)MOVE_RPM_MAX) return (float)MOVE_RPM_MAX;
  return rpm;
}

float clampStepsPerMm(float v) {
  if (v < STEPS_PER_MM_MIN) return STEPS_PER_MM_MIN;
  if (v > STEPS_PER_MM_MAX) return STEPS_PER_MM_MAX;
  return v;
}

float clampOffsetSteps(float v) {
  if (v < OFFSET_STEPS_MIN) return OFFSET_STEPS_MIN;
  if (v > OFFSET_STEPS_MAX) return OFFSET_STEPS_MAX;
  return v;
}

float clampOmOffsetMm(float v) {
  if (v < OM_OFFSET_MM_MIN) return OM_OFFSET_MM_MIN;
  if (v > OM_OFFSET_MM_MAX) return OM_OFFSET_MM_MAX;
  return v;
}

uint32_t calcTravelTimeoutMs(float distMm, float rpm) {
  if (rpm < 0.1f) rpm = 0.1f;
  if (distMm < 0.0f) distMm = -distMm;
  if (distMm < 0.5f) distMm = 0.5f;

  const float mmPerSec = (rpm / 60.0f) * LINEAR_MM_PER_MOTOR_REV;
  float ms = (distMm / mmPerSec) * 1000.0f + (float)TIMEOUT_MARGIN_MS;
  if (ms < (float)TIMEOUT_MIN_MS) ms = (float)TIMEOUT_MIN_MS;
  if (ms > (float)TIMEOUT_MAX_MS) ms = (float)TIMEOUT_MAX_MS;
  return (uint32_t)(ms + 0.5f);
}

// =============================================================================
// ASDA — config NVS y conversion mm/PUU
// =============================================================================
void resetCalFactory() {
  cfgStepsPerMm  = FACTORY_STEPS_PER_MM;
  cfgOffsetSteps = 0.0f;
  cfgCalProg1Mm = cfgCalMeas1Mm = cfgCalProg2Mm = cfgCalMeas2Mm = 0.0f;
}

void loadCfg() {
  prefs.begin(ASDA_PREFS_NS, true);
  cfgMoveRpm     = clampMoveRpm(prefs.getFloat("moveRpm", DEFAULT_MOVE_SPEED_RPM));
  cfgStepsPerMm  = clampStepsPerMm(prefs.getFloat("spm", FACTORY_STEPS_PER_MM));
  cfgOffsetSteps = clampOffsetSteps(prefs.getFloat("off", 0.0f));
  cfgOmOffsetMm  = clampOmOffsetMm(prefs.getFloat("omOff", 0.0f));
  cfgOmOffsetMmR = clampOmOffsetMm(prefs.getFloat("omOffR", 0.0f));
  cfgCalProg1Mm  = prefs.getFloat("p1", 0.0f);
  cfgCalMeas1Mm  = prefs.getFloat("m1", 0.0f);
  cfgCalProg2Mm  = prefs.getFloat("p2", 0.0f);
  cfgCalMeas2Mm  = prefs.getFloat("m2", 0.0f);
  prefs.end();
}

void saveCfg() {
  prefs.begin(ASDA_PREFS_NS, false);
  prefs.putFloat("moveRpm", cfgMoveRpm);
  prefs.putFloat("spm", cfgStepsPerMm);
  prefs.putFloat("off", cfgOffsetSteps);
  prefs.putFloat("omOff", cfgOmOffsetMm);
  prefs.putFloat("omOffR", cfgOmOffsetMmR);
  prefs.putFloat("p1", cfgCalProg1Mm);
  prefs.putFloat("m1", cfgCalMeas1Mm);
  prefs.putFloat("p2", cfgCalProg2Mm);
  prefs.putFloat("m2", cfgCalMeas2Mm);
  prefs.end();
}

int32_t mmToWorkPuu(float mm, bool factory) {
  if (mm < 0.0f) mm = 0.0f;
  const float spm = factory ? FACTORY_STEPS_PER_MM : cfgStepsPerMm;
  const float off = factory ? 0.0f : cfgOffsetSteps;
  float pasos = off + mm * spm;
  if (pasos < 0.0f) pasos = 0.0f;
  int64_t mag = (int64_t)llroundf(pasos * (float)LINEAR_EGEAR_N);
  if (mag > 2147483647LL) mag = 2147483647LL;
  return (int32_t)(-mag);
}

float puuToMm(int32_t puu, bool factory) {
  const float spm = factory ? FACTORY_STEPS_PER_MM : cfgStepsPerMm;
  if (spm < 0.001f) return 0.0f;
  const float off = factory ? 0.0f : cfgOffsetSteps;
  float pasos = (float)((puu < 0) ? -puu : puu) / (float)LINEAR_EGEAR_N;
  return (pasos - off) / spm;
}

String configJson() {
  char buf[420];
  snprintf(buf, sizeof(buf),
           "{\"ok\":true,\"moveRpm\":%.0f,\"moveRpmMin\":%u,\"moveRpmMax\":%u,"
           "\"stepsPerMm\":%.3f,\"offsetSteps\":%.1f,"
           "\"factoryStepsPerMm\":%.3f,\"factoryLinearActuatorMm\":%.1f,"
           "\"egearN\":%lu,\"calProg1Mm\":%.1f,\"calMeas1Mm\":%.1f,"
           "\"calProg2Mm\":%.1f,\"calMeas2Mm\":%.1f}",
           cfgMoveRpm, MOVE_RPM_MIN, MOVE_RPM_MAX,
           cfgStepsPerMm, cfgOffsetSteps,
           FACTORY_STEPS_PER_MM, FACTORY_LINEAR_ACTUATOR_MM,
           (unsigned long)LINEAR_EGEAR_N,
           cfgCalProg1Mm, cfgCalMeas1Mm,
           cfgCalProg2Mm, cfgCalMeas2Mm);
  return String(buf);
}

// =============================================================================
// ASDA — Modbus RS-485
// =============================================================================
void printHexFrame(const char* label, const uint8_t* data, size_t len) {
  Serial.print(label);
  for (size_t i = 0; i < len; i++) {
    if (data[i] < 0x10) Serial.print("0");
    Serial.print(data[i], HEX);
    Serial.print(" ");
  }
  Serial.println();
}

void clearServoRx() {
  while (ServoBus.available()) {
    ServoBus.read();
  }
}

uint16_t modbusCRC(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      if (crc & 0x0001) {
        crc = (crc >> 1) ^ 0xA001;
      } else {
        crc >>= 1;
      }
    }
  }
  return crc;
}

bool transact(const uint8_t* tx,
              size_t txLen,
              uint8_t* rx,
              size_t& rxLen,
              size_t rxMax,
              size_t expectedMinLen) {
  rxLen = 0;
  clearServoRx();
  delay(MODBUS_PRE_TX_GAP_MS);

  if (MODBUS_DEBUG) printHexFrame("TX: ", tx, txLen);

  ServoBus.write(tx, txLen);
  ServoBus.flush();

  uint32_t txDoneUs = micros();
  uint32_t lastRxUs = txDoneUs;
  uint32_t timeoutStartMs = millis();
  bool gotAnyByte = false;

  while ((millis() - timeoutStartMs) < RESPONSE_TIMEOUT_MS) {
    while (ServoBus.available() > 0) {
      int c = ServoBus.read();
      if (c >= 0) {
        uint32_t nowUs = micros();
        if (!gotAnyByte) {
          gotAnyByte = true;
          if (MODBUS_DEBUG) {
            Serial.printf("First RX after %lu us\n",
                          (unsigned long)(nowUs - txDoneUs));
          }
        }
        lastRxUs = nowUs;
        if (rxLen < rxMax) rx[rxLen++] = (uint8_t)c;
      }
    }

    if (gotAnyByte && (micros() - lastRxUs) >= MODBUS_FRAME_GAP_US) {
      break;
    }
    yield();
  }

  if (MODBUS_DEBUG) {
    printHexFrame("RX: ", rx, rxLen);
    Serial.printf("RX length: %u\n", (unsigned)rxLen);
  }

  if (!gotAnyByte || rxLen == 0) return false;
  if (rxLen < 5) return false;

  uint16_t crcReceived =
      (uint16_t)rx[rxLen - 2] | ((uint16_t)rx[rxLen - 1] << 8);
  uint16_t crcCalculated = modbusCRC(rx, rxLen - 2);
  if (crcReceived != crcCalculated) {
    if (rxLen < expectedMinLen && MODBUS_DEBUG) {
      Serial.println("DIAG: possible first-byte loss; raise P3.007.");
    }
    return false;
  }

  if (rx[0] != MODBUS_ID) return false;
  if (rx[1] & 0x80) return false;
  return true;
}

bool write16(uint16_t address, uint16_t value) {
  uint8_t tx[8];
  tx[0] = MODBUS_ID;
  tx[1] = 0x06;
  tx[2] = (address >> 8) & 0xFF;
  tx[3] = address & 0xFF;
  tx[4] = (value >> 8) & 0xFF;
  tx[5] = value & 0xFF;
  uint16_t crc = modbusCRC(tx, 6);
  tx[6] = crc & 0xFF;
  tx[7] = (crc >> 8) & 0xFF;

  uint8_t rx[32];
  size_t rxLen = 0;
  if (!transact(tx, sizeof(tx), rx, rxLen, sizeof(rx), 8)) return false;
  return (rxLen == 8 && rx[1] == 0x06);
}

bool write32(uint16_t address, int32_t value) {
  uint32_t u = (uint32_t)value;
  uint16_t lowWord  = (uint16_t)(u & 0xFFFF);
  uint16_t highWord = (uint16_t)((u >> 16) & 0xFFFF);

  uint8_t tx[13];
  tx[0] = MODBUS_ID;
  tx[1] = 0x10;
  tx[2] = (address >> 8) & 0xFF;
  tx[3] = address & 0xFF;
  tx[4] = 0x00;
  tx[5] = 0x02;
  tx[6] = 0x04;
  tx[7]  = (lowWord >> 8) & 0xFF;
  tx[8]  = lowWord & 0xFF;
  tx[9]  = (highWord >> 8) & 0xFF;
  tx[10] = highWord & 0xFF;
  uint16_t crc = modbusCRC(tx, 11);
  tx[11] = crc & 0xFF;
  tx[12] = (crc >> 8) & 0xFF;

  uint8_t rx[32];
  size_t rxLen = 0;
  if (!transact(tx, sizeof(tx), rx, rxLen, sizeof(rx), 8)) return false;
  return (rxLen == 8 && rx[1] == 0x10);
}

bool readRegisters(uint16_t address, uint16_t count, uint16_t* out) {
  uint8_t tx[8];
  tx[0] = MODBUS_ID;
  tx[1] = 0x03;
  tx[2] = (address >> 8) & 0xFF;
  tx[3] = address & 0xFF;
  tx[4] = (count >> 8) & 0xFF;
  tx[5] = count & 0xFF;
  uint16_t crc = modbusCRC(tx, 6);
  tx[6] = crc & 0xFF;
  tx[7] = (crc >> 8) & 0xFF;

  uint8_t rx[64];
  size_t rxLen = 0;
  size_t expectedLen = 5 + count * 2;
  if (!transact(tx, sizeof(tx), rx, rxLen, sizeof(rx), expectedLen)) return false;
  if (rxLen != expectedLen || rx[1] != 0x03 || rx[2] != count * 2) return false;

  for (uint16_t i = 0; i < count; i++) {
    out[i] = ((uint16_t)rx[3 + i * 2] << 8) | (uint16_t)rx[4 + i * 2];
  }
  return true;
}

bool read16(uint16_t address, uint16_t& value) {
  return readRegisters(address, 1, &value);
}

bool read32(uint16_t address, int32_t& value) {
  uint16_t regs[2];
  if (!readRegisters(address, 2, regs)) return false;
  uint32_t u = ((uint32_t)regs[1] << 16) | (uint32_t)regs[0];
  value = (int32_t)u;
  return true;
}

// =============================================================================
// ASDA — job de movimiento (P5.007)
// =============================================================================
void motionJobBegin(uint16_t command, uint32_t timeoutMs) {
  motionCancelled = false;
  motionJobOk = false;
  clearMotionAlarm();
  motionJobDetail = "";
  motionJobPos = 0;
  motionJobCmd = command;
  motionJobStartMs = millis();
  motionJobTimeoutMs = timeoutMs;
  motionJobActive = true;
  lastMotionPollMs = 0;
}

bool pollMotionOnce() {
  if (!motionJobActive) return false;

  const uint32_t now = millis();
  if (lastMotionPollMs != 0 && (now - lastMotionPollMs) < MOTION_POLL_MIN_MS)
    return false;
  lastMotionPollMs = now;

  if (motionCancelled) {
    motionJobActive = false;
    busyMotion = false;
    motionJobOk = false;
    motionJobDetail = "cancelado";
    if (read32(REG_P5_016, motionJobPos)) {
      cachedPosPuu = motionJobPos;
      cachedPosOk = true;
    }
    return true;
  }

  uint16_t trigger = 0;
  const bool got = read16(REG_P5_007, trigger);
  if (got) {
    cachedP5007 = trigger;
    cachedTriggerOk = true;
  }
  const uint16_t doneLoose  = (uint16_t)(motionJobCmd + 10000);
  const uint16_t doneTarget = (uint16_t)(motionJobCmd + 20000);

  if (got && trigger == doneTarget) {
    motionJobDetail = "completo (target reached)";
    if (read32(REG_P5_016, motionJobPos)) {
      cachedPosPuu = motionJobPos;
      cachedPosOk = true;
    }
    motionJobActive = false;
    busyMotion = false;
    motionJobOk = true;
    if (cachedPosOk) {
      overviewOnAsdaMoveDone(puuToMm(cachedPosPuu, false));
      asdaTcpReachedPos = cachedPosPuu;
      asdaTcpNotifyReached = true;
    }
    return true;
  }
  if (got && trigger == doneLoose) {
    motionJobDetail = "completo";
    if (read32(REG_P5_016, motionJobPos)) {
      cachedPosPuu = motionJobPos;
      cachedPosOk = true;
    }
    motionJobActive = false;
    busyMotion = false;
    motionJobOk = true;
    if (cachedPosOk) {
      overviewOnAsdaMoveDone(puuToMm(cachedPosPuu, false));
      asdaTcpReachedPos = cachedPosPuu;
      asdaTcpNotifyReached = true;
    }
    return true;
  }

  if ((millis() - motionJobStartMs) >= motionJobTimeoutMs) {
    const String alarm = got
        ? String("ALARMA: timeout — no llego a destino (limite ") +
              String(motionJobTimeoutMs) + " ms)"
        : String("ALARMA: Modbus timeout leyendo P5.007");
    raiseMotionAlarm(alarm);
    if (read32(REG_P5_016, motionJobPos)) {
      cachedPosPuu = motionJobPos;
      cachedPosOk = true;
    }
    motionJobActive = false;
    busyMotion = false;
    motionJobOk = false;
    return true;
  }
  return false;
}

bool waitPrComplete(uint16_t command,
                    uint32_t timeoutMs,
                    String& detail,
                    int32_t* outPos) {
  motionJobBegin(command, timeoutMs);

  while (motionJobActive) {
    if (pollMotionOnce()) break;
    server.handleClient();
    updateEncoderMotion(millis());
    delay(40);
    yield();
  }

  detail = motionJobDetail;
  if (outPos) *outPos = motionJobPos;
  return motionJobOk;
}

// =============================================================================
// ASDA — comandos servo
// =============================================================================
bool servoOn() {
  return write16(REG_P2_030, 1);
}

bool servoOff() {
  if (!write16(REG_P2_030, 0xFFFF)) return false;
  delay(20);
  write16(REG_P2_030, 0);
  return true;
}

bool stopMotion() {
  return write16(REG_P5_007, 1000);
}

bool setMoveSpeed(float rpm) {
  if (rpm < 0) rpm = -rpm;
  if (rpm > 7500.0f) rpm = 7500.0f;
  int32_t speed01rpm = (int32_t)lroundf(rpm * 10.0f);
  return write32(REG_P5_060, speed01rpm);
}

bool moveAbsolute(int32_t targetPUU, float rpm) {
  if (!setMoveSpeed(rpm)) return false;
  if (!write32(REG_P6_002, (int32_t)PR1_ABSOLUTE_DEF)) return false;
  if (!write32(REG_P6_003, targetPUU)) return false;
  if (!write16(REG_P5_007, 1)) return false;
  return true;
}

bool homeTorque(bool forward,
                uint16_t torquePct,
                uint16_t timeMs,
                float speedRpm) {
  torquePct = constrain(torquePct, 1, 300);
  timeMs    = constrain(timeMs, 2, 2000);
  if (speedRpm < 0.1f) speedRpm = 0.1f;
  if (speedRpm > 2000.0f) speedRpm = 2000.0f;

  uint16_t homingMethod = forward ? 0x0029 : 0x002A;
  int32_t speed01rpm = (int32_t)lroundf(speedRpm * 10.0f);

  if (!write16(REG_P1_087, torquePct)) return false;
  if (!write16(REG_P1_088, timeMs)) return false;
  if (!write16(REG_P5_004, homingMethod)) return false;
  if (!write32(REG_P5_005, speed01rpm)) return false;
  if (!write32(REG_P5_006, speed01rpm)) return false;
  if (!write32(REG_P6_000, 0)) return false;
  if (!write32(REG_P6_001, 0)) return false;
  if (!write16(REG_P5_007, 0)) return false;
  return true;
}

bool communicationTest(String& detail) {
  uint16_t value;
  if (!read16(REG_P3_000, value)) {
    detail = "Fallo lectura P3.000";
    return false;
  }
  if (!read16(REG_P3_001, value)) {
    detail = "Fallo lectura P3.001";
    return false;
  }
  if (!read16(REG_P3_002, value)) {
    detail = "Fallo lectura P3.002";
    return false;
  }
  detail = "Comunicacion OK";
  return true;
}

// =============================================================================
// ASDA — API interna (HTTP + TCP maestro) — opcodes en MotionStates.h
// =============================================================================

static bool asdaStartHome(bool forward, String& err) {
  if (busyMotion) {
    err = "Ocupado en movimiento";
    return false;
  }

  const uint16_t torque = DEFAULT_HOME_TORQUE_PCT;
  const uint16_t timeMs = DEFAULT_HOME_TIME_MS;
  const float speed = DEFAULT_HOME_SPEED_RPM;
  const uint32_t timeoutMs = calcTravelTimeoutMs(HOME_MAX_TRAVEL_MM, speed);

  busyMotion = true;
  clearMotionAlarm();
  if (!homeTorque(forward, torque, timeMs, speed)) {
    busyMotion = false;
    err = "No se pudo disparar homing";
    return false;
  }
  motionJobBegin(0, timeoutMs);
  return true;
}

static bool asdaStartMove(int32_t position, float speed, String& err) {
  if (busyMotion) {
    err = "Ocupado en movimiento";
    return false;
  }

  speed = clampMoveRpm(speed);

  int32_t curPuu = cachedPosOk ? cachedPosPuu : 0;
  if (read32(REG_P5_016, curPuu)) {
    cachedPosPuu = curPuu;
    cachedPosOk = true;
  }
  const float distMm =
      fabsf(puuToMm(position, false) - puuToMm(curPuu, false));
  const uint32_t timeoutMs = calcTravelTimeoutMs(distMm, speed);

  busyMotion = true;
  clearMotionAlarm();
  if (!moveAbsolute(position, speed)) {
    busyMotion = false;
    err = "No se pudo disparar MOVE";
    return false;
  }
  motionJobBegin(1, timeoutMs);
  return true;
}

static bool asdaExecStop(String& err) {
  const bool ok = stopMotion();
  motionCancelled = true;
  motionJobActive = false;
  busyMotion = false;
  clearMotionAlarm();
  asdaTcpStopPending = true;
  err = ok ? "STOP enviado" : "Fallo STOP";
  return ok;
}

static bool asdaExecOn(String& err) {
  if (busyMotion) {
    err = "Ocupado en movimiento";
    return false;
  }
  const bool ok = servoOn();
  err = ok ? "Servo ON" : "Fallo Servo ON";
  return ok;
}

static bool asdaExecOff(String& err) {
  if (busyMotion) {
    err = "Ocupado en movimiento";
    return false;
  }
  const bool ok = servoOff();
  err = ok ? "Servo OFF" : "Fallo Servo OFF";
  return ok;
}

static bool asdaExecResume(String& err) {
  if (busyMotion || motionJobActive) {
    err = "Todavia en movimiento";
    return false;
  }
  asdaTcpStopPending = false;
  clearMotionAlarm();
  err = "Reanudado";
  return true;
}

static bool asdaExecResetError(String& err) {
  if (busyMotion || motionJobActive) {
    err = "No se puede resetear error en movimiento";
    return false;
  }
  clearMotionAlarm();
  asdaTcpStopPending = false;
  err = "Error limpiado";
  return true;
}

static void asdaRefreshLiveStatus(uint16_t& trigger,
                                  int32_t& pos,
                                  bool& okT,
                                  bool& okP) {
  trigger = cachedP5007;
  pos = cachedPosPuu;
  okT = cachedTriggerOk;
  okP = cachedPosOk;

  const bool needLive = !busyMotion && !motionJobActive;
  const uint32_t now = millis();
  if (needLive && (lastLiveStatusMs == 0 || (now - lastLiveStatusMs) >= STATUS_LIVE_MIN_MS)) {
    lastLiveStatusMs = now;
    uint16_t t = 0;
    int32_t p = 0;
    okT = read16(REG_P5_007, t);
    okP = read32(REG_P5_016, p);
    if (okT) {
      trigger = t;
      cachedP5007 = t;
      cachedTriggerOk = true;
    }
    if (okP) {
      pos = p;
      cachedPosPuu = p;
      cachedPosOk = true;
    }
  } else if (busyMotion || motionJobActive) {
    trigger = cachedP5007;
    pos = cachedPosPuu;
    okT = cachedTriggerOk;
    okP = cachedPosOk;
  }
}

static String asdaStatusJsonBody() {
  uint16_t trigger = 0;
  int32_t pos = 0;
  bool okT = false;
  bool okP = false;
  asdaRefreshLiveStatus(trigger, pos, okT, okP);

  String state = "unknown";
  if (okT) {
    if (trigger >= 20000 && trigger < 20100) state = "complete_tpos";
    else if (trigger >= 10000 && trigger < 10100) state = "complete";
    else if (trigger >= 1 && trigger <= 99) state = "running_pr";
    else if (trigger == 0) state = "idle_or_homing_cmd";
    else if (trigger == 1000) state = "stop";
    else state = "other";
  }

  const float mm = okP ? puuToMm(pos, false) : 0.0f;
  const bool reportOk = busyMotion ? true : (okT && okP);
  char buf[480];
  snprintf(buf, sizeof(buf),
           "{\"ok\":%s,\"busy\":%s,\"alarm\":%s,\"message\":\"%s\","
           "\"p5007\":%u,\"state\":\"%s\","
           "\"positionPuu\":%ld,\"positionMm\":%.2f,\"ip\":\"%s\"}",
           reportOk ? "true" : "false",
           busyMotion ? "true" : "false",
           motionAlarm ? "true" : "false",
           jsonEscape(motionAlarmMsg).c_str(),
           (unsigned)trigger,
           state.c_str(),
           okP ? (long)pos : 0L,
           mm,
           WiFi.localIP().toString().c_str());
  return String(buf);
}

// =============================================================================
// ENCODER — PCNT driver (L / R)
// =============================================================================
#if ENC_PCNT_IDF5
static bool IRAM_ATTR onEncPcntReach(pcnt_unit_handle_t unit,
                                     const pcnt_watch_event_data_t *edata,
                                     void *user_ctx) {
  (void)unit;
  const int ix = (int)(uintptr_t)user_ctx;
  if (ix < 0 || ix > 1) return false;
  portENTER_CRITICAL_ISR(&encPcntMux);
  encSide[ix].pcntOverflow += edata->watch_point_value;
  portEXIT_CRITICAL_ISR(&encPcntMux);
  return false;
}
#else
static void IRAM_ATTR onEncPcntIntr(void *arg) {
  (void)arg;
  const uint32_t st = PCNT.int_st.val;
  for (uint8_t ix = 0; ix < 2; ix++) {
    if (!encSide[ix].installed) continue;
    if (!(st & BIT(encSide[ix].pcntUnit))) continue;
    portENTER_CRITICAL_ISR(&encPcntMux);
    if (PCNT.status_unit[encSide[ix].pcntUnit].h_lim_lat)
      encSide[ix].pcntOverflow += ENC_PCNT_HIGH_LIM;
    if (PCNT.status_unit[encSide[ix].pcntUnit].l_lim_lat)
      encSide[ix].pcntOverflow += ENC_PCNT_LOW_LIM;
    portEXIT_CRITICAL_ISR(&encPcntMux);
    PCNT.int_clr.val = BIT(encSide[ix].pcntUnit);
  }
}
#endif

static bool setupEncoderPcntSide(uint8_t ix) {
  EncSideState& e = encSide[ix];
  if (!e.installed) {
    e.pcntOk = false;
    return true;
  }

  pinMode(e.pinA, INPUT_PULLUP);
  pinMode(e.pinB, INPUT_PULLUP);
  if (e.pinZ >= 0) pinMode(e.pinZ, INPUT_PULLUP);

#if ENC_PCNT_IDF5
  pcnt_unit_config_t unitCfg = {};
  unitCfg.high_limit = ENC_PCNT_HIGH_LIM;
  unitCfg.low_limit  = ENC_PCNT_LOW_LIM;
  if (pcnt_new_unit(&unitCfg, &e.pcntUnit) != ESP_OK) return false;

  pcnt_glitch_filter_config_t filter = {};
  filter.max_glitch_ns = ENC_GLITCH_NS;
  if (pcnt_unit_set_glitch_filter(e.pcntUnit, &filter) != ESP_OK) return false;

  pcnt_chan_config_t chanA = {};
  chanA.edge_gpio_num  = e.pinA;
  chanA.level_gpio_num = e.pinB;
  pcnt_channel_handle_t chA = nullptr;
  if (pcnt_new_channel(e.pcntUnit, &chanA, &chA) != ESP_OK) return false;
  pcnt_channel_set_edge_action(chA, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                               PCNT_CHANNEL_EDGE_ACTION_HOLD);
  pcnt_channel_set_level_action(chA, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                PCNT_CHANNEL_LEVEL_ACTION_INVERSE);

  pcnt_unit_add_watch_point(e.pcntUnit, ENC_PCNT_HIGH_LIM);
  pcnt_unit_add_watch_point(e.pcntUnit, ENC_PCNT_LOW_LIM);
  pcnt_event_callbacks_t cbs = {};
  cbs.on_reach = onEncPcntReach;
  pcnt_unit_register_event_callbacks(e.pcntUnit, &cbs, (void*)(uintptr_t)ix);

  pcnt_unit_enable(e.pcntUnit);
  pcnt_unit_clear_count(e.pcntUnit);
  pcnt_unit_start(e.pcntUnit);
#else
  e.pcntUnit = encSideIsR(ix) ? ENC_PCNT_UNIT_R : ENC_PCNT_UNIT_L;
  pcnt_config_t cfg = {};
  cfg.pulse_gpio_num = e.pinA;
  cfg.ctrl_gpio_num  = e.pinB;
  cfg.unit           = e.pcntUnit;
  cfg.channel        = encSideIsR(ix) ? PCNT_CHANNEL_0 : PCNT_CHANNEL_1;
  cfg.pos_mode       = PCNT_COUNT_INC;
  cfg.neg_mode       = PCNT_COUNT_DIS;
  cfg.lctrl_mode     = PCNT_MODE_REVERSE;
  cfg.hctrl_mode     = PCNT_MODE_KEEP;
  cfg.counter_h_lim  = ENC_PCNT_HIGH_LIM;
  cfg.counter_l_lim  = ENC_PCNT_LOW_LIM;
  if (pcnt_unit_config(&cfg) != ESP_OK) return false;

  pcnt_set_filter_value(e.pcntUnit, ENC_FILTER_APB_TICKS);
  pcnt_filter_enable(e.pcntUnit);
  pcnt_event_enable(e.pcntUnit, PCNT_EVT_H_LIM);
  pcnt_event_enable(e.pcntUnit, PCNT_EVT_L_LIM);
  static bool isrRegistered = false;
  if (!isrRegistered) {
    pcnt_isr_register(onEncPcntIntr, nullptr, 0, nullptr);
    pcnt_intr_enable(ENC_PCNT_UNIT_R);
    pcnt_intr_enable(ENC_PCNT_UNIT_L);
    isrRegistered = true;
  }

  pcnt_counter_pause(e.pcntUnit);
  pcnt_counter_clear(e.pcntUnit);
  pcnt_counter_resume(e.pcntUnit);
#endif
  e.pcntOk = true;
  return true;
}

static void initEncodersHw() {
  encSide[ENC_IX_L] = {
    ENC_L_HW_INSTALLED, ENC_L_PIN_A, ENC_L_PIN_B, ENC_L_PIN_Z,
#if ENC_PCNT_IDF5
    nullptr,
#else
    ENC_PCNT_UNIT_L,
#endif
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, false, 0, 0, 0, false
  };
  encSide[ENC_IX_R] = {
    ENC_R_HW_INSTALLED, ENC_R_PIN_A, ENC_R_PIN_B, ENC_R_PIN_Z,
#if ENC_PCNT_IDF5
    nullptr,
#else
    ENC_PCNT_UNIT_R,
#endif
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, false, 0, 0, 0, false
  };

  for (uint8_t ix = 0; ix < 2; ix++) {
    if (!setupEncoderPcntSide(ix)) {
      encSide[ix].pcntOk = false;
      Serial.printf("ERROR: PCNT encoder %s no iniciado\n",
                    encSideIsR(ix) ? "R" : "L");
    } else if (encSide[ix].installed) {
      encSide[ix].lastMs = millis();
      Serial.printf("Encoder %s OK  A=%d B=%d Z=%d\n",
                    encSideIsR(ix) ? "R" : "L",
                    (int)encSide[ix].pinA, (int)encSide[ix].pinB,
                    (int)encSide[ix].pinZ);
    } else {
      Serial.printf("Encoder %s: no instalado (reservado A=%d B=%d Z=%d)\n",
                    encSideIsR(ix) ? "R" : "L",
                    (int)encSide[ix].pinA, (int)encSide[ix].pinB,
                    (int)encSide[ix].pinZ);
    }
  }
}

static int32_t readEncoderCountSide(uint8_t ix) {
  EncSideState& e = encSide[ix];
  if (!e.installed || !e.pcntOk) return 0;
  int16_t hw = 0;
#if ENC_PCNT_IDF5
  int count = 0;
  if (e.pcntUnit) pcnt_unit_get_count(e.pcntUnit, &count);
  hw = (int16_t)count;
#else
  pcnt_get_counter_value(e.pcntUnit, &hw);
#endif
  int32_t ov;
  portENTER_CRITICAL(&encPcntMux);
  ov = e.pcntOverflow;
  portEXIT_CRITICAL(&encPcntMux);
  return ov + (int32_t)hw;
}

static int32_t readEncoderCount(bool sideR) {
  return readEncoderCountSide(encIxFromSideR(sideR));
}

static void clearEncoderCountSide(uint8_t ix) {
  EncSideState& e = encSide[ix];
  if (!e.installed || !e.pcntOk) return;
#if ENC_PCNT_IDF5
  if (e.pcntUnit) pcnt_unit_clear_count(e.pcntUnit);
#else
  pcnt_counter_clear(e.pcntUnit);
#endif
  portENTER_CRITICAL(&encPcntMux);
  e.pcntOverflow = 0;
  portEXIT_CRITICAL(&encPcntMux);
}

static void clearEncoderCount() {
  for (uint8_t ix = 0; ix < 2; ix++)
    clearEncoderCountSide(ix);
}

static float encCountsToMm(int32_t counts) {
  return (float)counts * (100.0f / (float)ENC_COUNTS_PER_100MM);
}

// Parte decimal OM → 0 / 0.5 / 1 (solo décima .0–.9; se ignora centésima):
// .0–.2→.0; .3–.7→.5; .8–.9→1.  (mismo criterio que TCM omRoundMm)

// =============================================================================
// ENCODER — redondeo y valor oficial OM
// =============================================================================
static float omRoundMm(float mm) {
  const bool neg = mm < 0.0f;
  float a = fabsf(mm);
  float whole = floorf(a);
  float frac = a - whole;
  int t = (int)(frac * 10.0f);
  if (t > 9) t = 9;
  float outFrac;
  if (t <= 2)
    outFrac = 0.0f;
  else if (t <= 7)
    outFrac = 0.5f;
  else {
    whole += 1.0f;
    outFrac = 0.0f;
  }
  const float out = whole + outFrac;
  return neg ? -out : out;
}

// Valor oficial OM: redondeo primero, luego offset crudo (no se vuelve a redondear).

static float omOfficialMm(float rawAbsMm) {
  return omRoundMm(rawAbsMm) + cfgOmOffsetMm;
}

// Resultado final OM: mm del settle redondeado + offset (o <0 si aún no hay settle).

static float encSettleMmRounded(bool sideR) {
  const uint8_t ix = encIxFromSideR(sideR);
  EncSideState& e = encSide[ix];
  if (!e.settled) return -1.0f;
  const int32_t settledAbs =
      e.settledCount >= 0 ? e.settledCount : -e.settledCount;
  return omOfficialMmSide(encCountsToMm(settledAbs), sideR);
}

// =============================================================================
// ENCODER — movimiento y reset
// =============================================================================
static void overviewOnOmSettle();
static void overviewOnEncoderReset();

void IRAM_ATTR onEncoderZR() { encSide[ENC_IX_R].zCount++; }
void IRAM_ATTR onEncoderZL() { encSide[ENC_IX_L].zCount++; }

static void updateEncoderSide(uint8_t ix, uint32_t nowMs) {
  EncSideState& e = encSide[ix];
  if (!e.installed || !e.pcntOk) return;

  const uint32_t dt = nowMs - e.lastMs;
  if (dt < 80) return;

  const int32_t count = readEncoderCountSide(ix);
  const int32_t delta = count - e.lastCount;
  e.lastCount = count;
  e.lastMs = nowMs;

  const float revs = (float)delta / (float)ENC_CPR;
  e.rpm    = revs * (60000.0f / (float)dt);
  e.freqHz = fabsf((float)delta / (float)ENC_QUAD) * (1000.0f / (float)dt);
  e.mmS    = fabsf((float)delta) * (100.0f / (float)ENC_COUNTS_PER_100MM)
           * (1000.0f / (float)dt);

  if (delta > 0) e.dir = 1;
  else if (delta < 0) e.dir = -1;
  else {
    e.dir = 0;
    e.rpm = 0.0f;
    e.mmS = 0.0f;
  }

  if (e.mmS >= ENC_MOVE_MMS) {
    if (e.settled) e.mmSPeak = e.mmS;
    else if (e.mmS > e.mmSPeak) e.mmSPeak = e.mmS;
    e.wasMoving = true;
    e.settled = false;
    e.stopMs = 0;
  } else {
    if (e.mmS > e.mmSPeak) e.mmSPeak = e.mmS;
    if (e.wasMoving) {
      if (e.stopMs == 0) e.stopMs = nowMs;
      else if ((nowMs - e.stopMs) >= ENC_SETTLE_MS) {
        e.settled = true;
        e.settledCount = count;
        e.wasMoving = false;
        e.stopMs = 0;
        if (ix == ENC_IX_L || (ix == ENC_IX_R && !encSide[ENC_IX_L].installed))
          overviewOnOmSettle();
        motionTcpQueueMeasurePush(encSideIsR(ix));
      }
    }
  }
}

void updateEncoderMotion(uint32_t nowMs) {
  updateEncoderSide(ENC_IX_L, nowMs);
  updateEncoderSide(ENC_IX_R, nowMs);
}

static void resetEncoderSide(uint8_t ix) {
  EncSideState& e = encSide[ix];
  if (!e.installed) return;
  clearEncoderCountSide(ix);
  e.zCount = 0;
  e.lastCount = 0;
  e.rpm = 0.0f;
  e.mmS = 0.0f;
  e.mmSPeak = 0.0f;
  e.freqHz = 0.0f;
  e.dir = 0;
  e.wasMoving = false;
  e.settled = false;
  e.stopMs = 0;
  e.settledCount = 0;
  e.zeroRef = 0;
}

void doEncoderReset() {
  resetEncoderSide(ENC_IX_L);
  resetEncoderSide(ENC_IX_R);
  overviewOnEncoderReset();
}

static float omOfficialMmSide(float rawAbsMm, bool sideR) {
  const float off = sideR ? cfgOmOffsetMmR : cfgOmOffsetMm;
  return omRoundMm(rawAbsMm) + off;
}

static int32_t encCountForSide(bool sideR) {
  const uint8_t ix = encIxFromSideR(sideR);
  return readEncoderCountSide(ix) - encSide[ix].zeroRef;
}

static float encSideLiveMm(bool sideR) {
  return encCountsToMm(encCountForSide(sideR));
}

static float encSideOfficialMm(bool sideR, bool useSettle) {
  const uint8_t ix = encIxFromSideR(sideR);
  EncSideState& e = encSide[ix];
  if (useSettle && e.settled) {
    const int32_t settledSide = e.settledCount - e.zeroRef;
    const int32_t absC = settledSide >= 0 ? settledSide : -settledSide;
    return omOfficialMmSide(encCountsToMm(absC), sideR);
  }
  const int32_t c = encCountForSide(sideR);
  const int32_t absC = c >= 0 ? c : -c;
  return omOfficialMmSide(encCountsToMm(absC), sideR);
}

static void motionTcpQueueMeasurePush(bool sideR) {
  if (sideR)
    motionTcpNotifyMeasureR = true;
  else
    motionTcpNotifyMeasureL = true;
}

static bool motionExecGetMeasured(bool sideR, float& mmOfficial, float& mmSigned,
                                  bool& settledOut, String& err) {
  if (!encSideHwOk(sideR)) {
    err = sideR ? "Encoder R no instalado" : "Encoder L no instalado";
    return false;
  }
  const uint8_t ix = encIxFromSideR(sideR);
  mmSigned = encSideLiveMm(sideR);
  settledOut = encSide[ix].settled;
  mmOfficial = encSideOfficialMm(sideR, encSide[ix].settled);
  return true;
}

static bool motionExecSet0Side(bool sideR, String& err) {
  if (!encSideHwOk(sideR)) {
    err = sideR ? "Encoder R no instalado" : "Encoder L no instalado";
    return false;
  }
  const uint8_t ix = encIxFromSideR(sideR);
  encSide[ix].zeroRef = readEncoderCountSide(ix);
  err = sideR ? "Set0 R OK" : "Set0 L OK";
  return true;
}

static bool motionExecSet0(String& err) {
  return motionExecSet0Side(false, err);
}

static bool motionExecResetErrors(String& err) {
  feedResetRuntime();

  if (busyMotion || motionJobActive) {
    stopMotion();
    motionCancelled = true;
    motionJobActive = false;
    busyMotion = false;
  }
  clearMotionAlarm();
  asdaTcpStopPending = false;
  err = "Errores limpiados";
  return true;
}

static bool motionExecResetAll(String& err) {
  if (!motionExecResetErrors(err)) return false;
  bool any = false;
  if (encSideHwOk(false)) {
    any = true;
    if (!motionExecSet0Side(false, err)) return false;
  }
  if (encSideHwOk(true)) {
    any = true;
    if (!motionExecSet0Side(true, err)) return false;
  }
  if (!any) {
    err = "Sin encoder instalado";
    return false;
  }

  feedOmLastMmAbs = 0.0f;
  feedOmLastMmSigned = 0.0f;
  bool feedR = false;
  if (encFeedSide(feedR))
    feedOmLastOfficialMm = encSideOfficialMm(feedR, false);
  else
    feedOmLastOfficialMm = cfgOmOffsetMm;
  omPhase1Mm = 0.0f;
  err = "Reset Motion OK";
  return true;
}

// —— Puente OM local para feed_engine (sin HTTP loopback) ——

static float encSettleMmRoundedFeed() {
  bool sideR = false;
  if (!encFeedSide(sideR)) return -1.0f;
  return encSettleMmRounded(sideR);
}

// =============================================================================
// ENCODER — JSON de estado (lado R si está instalado; si no, L)
// =============================================================================
String encoderStatusJson() {
  const EncSideState* ep = nullptr;
  bool sideR = true;
  if (encSide[ENC_IX_R].installed && encSide[ENC_IX_R].pcntOk)
    ep = &encSide[ENC_IX_R];
  else if (encSide[ENC_IX_L].installed && encSide[ENC_IX_L].pcntOk) {
    ep = &encSide[ENC_IX_L];
    sideR = false;
  }
  if (!ep) {
    return String("{\"ok\":false,\"error\":\"sin encoder OM instalado\"}");
  }

  const int32_t count = readEncoderCountSide(encIxFromSideR(sideR));
  const uint32_t zCount = ep->zCount;
  const int a = gpio_get_level(ep->pinA);
  const int b = gpio_get_level(ep->pinB);
  const int z = gpio_get_level(ep->pinZ);

  const int32_t absC = count >= 0 ? count : -count;
  const int32_t settledAbs =
      ep->settledCount >= 0 ? ep->settledCount : -ep->settledCount;
  const int32_t wrapped =
      ((count % (int32_t)ENC_CPR) + (int32_t)ENC_CPR) % (int32_t)ENC_CPR;
  const float angle = (360.0f * (float)wrapped) / (float)ENC_CPR;
  const float revs  = (float)count / (float)ENC_CPR;
  const float mm    = encCountsToMm(count);
  const float mmAbs = encCountsToMm(absC);
  const float mmSettleRaw = encCountsToMm(settledAbs);
  const float mmSettleRound = omRoundMm(mmSettleRaw);
  const float off = sideR ? cfgOmOffsetMmR : cfgOmOffsetMm;
  const float mmSettle = ep->settled ? (mmSettleRound + off) : mmSettleRaw;
  const int32_t expectZ = (absC + ENC_CPR / 2) / (int32_t)ENC_CPR;

  char buf[520];
  snprintf(buf, sizeof(buf),
           "{\"ok\":true,\"side\":\"%c\",\"c\":%ld,\"r\":%.4f,\"ang\":%.2f,"
           "\"rpm\":%.1f,\"mms\":%.1f,\"mmsPeak\":%.1f,\"f\":%.1f,\"d\":%d,"
           "\"z\":%lu,\"a\":%d,\"b\":%d,\"iz\":%d,\"mm\":%.2f,\"mmAbs\":%.2f,"
           "\"settled\":%s,\"cSettle\":%ld,\"mmSettle\":%.2f,"
           "\"mmSettleRound\":%.2f,\"offsetMm\":%.3f,"
           "\"hwL\":%s,\"hwR\":%s,"
           "\"ref100\":%ld,\"cpr\":%u,\"quad\":%u,\"expectZ\":%ld,\"pulleyMm\":%.0f}",
           sideR ? 'R' : 'L',
           (long)count, revs, angle, ep->rpm, ep->mmS, ep->mmSPeak, ep->freqHz,
           (int)ep->dir, (unsigned long)zCount, a, b, z,
           mm, mmAbs, ep->settled ? "true" : "false",
           (long)ep->settledCount, mmSettle,
           ep->settled ? mmSettleRound : mmSettleRaw, off,
           encSideHwOk(false) ? "true" : "false",
           encSideHwOk(true) ? "true" : "false",
           (long)ENC_COUNTS_PER_100MM, (unsigned)ENC_CPR, (unsigned)ENC_QUAD,
           (long)expectZ, ENC_PULLEY_DIAM_MM);
  return String(buf);
}

// =============================================================================
// FEEDER + ENCODER — puente OM local
// =============================================================================
bool feedOmReadLiveMm(float* mmSignedOut)
{
  bool sideR = false;
  if (!encFeedSide(sideR)) return false;
  const int32_t count = readEncoderCountSide(encIxFromSideR(sideR));
  if (mmSignedOut) *mmSignedOut = encCountsToMm(count);
  return true;
}

bool feedOmReadOfficialMm(float* officialOut, float* mmSignedOut, float* mmAbsOut)
{
  bool sideR = false;
  if (!encFeedSide(sideR)) return false;
  const uint8_t ix = encIxFromSideR(sideR);
  EncSideState& e = encSide[ix];
  if (!e.settled) return false;
  const int32_t settledAbs =
      e.settledCount >= 0 ? e.settledCount : -e.settledCount;
  const float mmAbs = encCountsToMm(settledAbs);
  const float mmSigned = encCountsToMm(e.settledCount);
  const float official = encSettleMmRounded(sideR);
  feedOmLastMmAbs = mmAbs;
  feedOmLastMmSigned = mmSigned;
  feedOmLastOfficialMm = official;
  if (officialOut) *officialOut = official;
  if (mmSignedOut) *mmSignedOut = mmSigned;
  if (mmAbsOut) *mmAbsOut = mmAbs;
  return true;
}

bool feedOmResetLocal()
{
  doEncoderReset();
  feedOmLastMmAbs = 0.0f;
  feedOmLastMmSigned = 0.0f;
  bool sideR = false;
  if (encFeedSide(sideR))
    feedOmLastOfficialMm = encSideOfficialMm(sideR, false);
  else
    feedOmLastOfficialMm = cfgOmOffsetMm;
  omPhase1Mm = 0.0f;
  return true;
}

float feedOmGetOffsetMm() {
  bool sideR = false;
  if (!encFeedSide(sideR)) return cfgOmOffsetMm;
  return sideR ? cfgOmOffsetMmR : cfgOmOffsetMm;
}

bool feedOmIsSettled() {
  bool sideR = false;
  if (!encFeedSide(sideR)) return false;
  return encSide[encIxFromSideR(sideR)].settled;
}

float feedOmOfficialFromRaw(float mmAbs) { return omOfficialMm(mmAbs); }

// Tolerancia simetrica |actual - target| <= tol
static bool decWithinTol(float actual, float target, float tol) {
  return fabsf(actual - target) <= tol;
}

// Longitud feed / omFeed55: dentro de tol O actual >= target - tol (no penaliza overshoot leve)
static bool decFeedLengthMet(float actual, float target, float tol) {
  return decWithinTol(actual, target, tol) || (actual >= (target - tol));
}

// OM oficial vs target con tol fija de feed (misma regla que feedOmExactTarget en Servo_Feed.cpp)
static bool decFeedOmAtTarget(float omOfficialMm, float targetMm) {
  return fabsf(omOfficialMm - targetMm) <= FEED_OM_TARGET_TOL_MM + 1e-4f;
}

static bool decFeedOmOvershoot(float omOfficialMm, float targetMm) {
  return omOfficialMm > targetMm + FEED_OM_TARGET_TOL_MM + 1e-4f;
}

// ASDA P5.007 — movimiento PR terminado (implementado en pollMotionOnce)
static bool decAsdaPrDoneLoose(uint16_t trigger, uint16_t jobCmd) {
  return trigger == (uint16_t)(jobCmd + 10000);
}

static bool decAsdaPrDoneTarget(uint16_t trigger, uint16_t jobCmd) {
  return trigger == (uint16_t)(jobCmd + 20000);
}

static bool decAsdaPrDone(uint16_t trigger, uint16_t jobCmd) {
  return decAsdaPrDoneTarget(trigger, jobCmd) || decAsdaPrDoneLoose(trigger, jobCmd);
}

// Ocupado / disponibilidad
static bool decBusyMotion() { return busyMotion || motionJobActive; }

static bool decFeedBusy() { return feedPhaseIsActive(); }

static bool decFeedCycleOk() { return feedThisCycleSucceeded(); }

static bool decServoCanReady() { return servoCanReady; }

static bool decMotionAlarm() { return motionAlarm; }

// --- Overview: evaluacion de hitos (usa decWithinTol / decFeedLengthMet) ---
static bool ovWithinTol(float actual, float target, float tol) {
  return decWithinTol(actual, target, tol);
}

static bool ovFeedMet(float actual, float target, float tol) {
  return decFeedLengthMet(actual, target, tol);
}

static void ovEvalHit(OvHit& h, bool feedStyle) {
  h.ok = feedStyle ? ovFeedMet(h.actualMm, h.targetMm, ovTolMm)
                   : ovWithinTol(h.actualMm, h.targetMm, ovTolMm);
}

static bool ovOmAllOk() {
  return ovOmFeed.done && ovOmFeed.ok
      && ovOmReset.done && ovOmReset.ok
      && ovOmFinal.done && ovOmFinal.ok;
}

static bool ovAsdaAllOk() {
  return ovAsdaLinearActuator.done && ovAsdaLinearActuator.ok
      && ovAsdaDeposit.done && ovAsdaDeposit.ok;
}

static bool ovStepOkNow() {
  if (!ovActive) return false;
  bool ok = true;
  if (ovRequireOm) {
    ok = ok && ovOmFeed.done && ovOmFeed.ok;
  }
  if (ovRequireAsda) {
    if (ovAsdaLinearActuator.done && !ovAsdaLinearActuator.ok) ok = false;
    if (ovAsdaDeposit.done && !ovAsdaDeposit.ok) ok = false;
  }
  return ok;
}

static bool ovPieceOkNow() {
  if (!ovActive) return false;
  bool ok = true;
  if (ovRequireOm && !ovOmAllOk()) ok = false;
  if (ovRequireAsda && !ovAsdaAllOk()) ok = false;
  if (ovLaserSet && !ovLaserOk) ok = false;
  return ok;
}

// =============================================================================
// OVERVIEW (ASDA + ENCODER) — comparacion de valores
// =============================================================================
static void ovHitClear(OvHit& h, float target) {
  h.targetMm = target;
  h.actualMm = 0.0f;
  h.done = false;
  h.ok = false;
}

static float ovAsdaMmNow() {
  if (cachedPosOk) return puuToMm(cachedPosPuu, false);
  int32_t pos = 0;
  if (read32(REG_P5_016, pos)) {
    cachedPosPuu = pos;
    cachedPosOk = true;
    return puuToMm(pos, false);
  }
  return 0.0f;
}

static void overviewOnOmSettle() {
  if (!ovActive) return;
  const float encMm = encSettleMmRoundedFeed();
  if (encMm < 0.0f) return;
  if (!ovOmFeed.done) ovOmFeed.actualMm = encMm;
  if (!ovOmFinal.done) ovOmFinal.actualMm = encMm;
}

static void overviewStart(float pieceMm, float extraMm, float tolMm,
                          bool requireAsda, bool requireOm) {
  if (pieceMm < 1.0f) pieceMm = 1.0f;
  if (extraMm < 0.0f) extraMm = 0.0f;
  if (tolMm < 0.05f) tolMm = 0.05f;
  if (tolMm > 10.0f) tolMm = 10.0f;

  ovActive = true;
  ovPieceMm = pieceMm;
  ovGripperMm = OV_GRIPPER_MM_DEFAULT;
  ovLinearActuatorMm = pieceMm - ovGripperMm;
  if (ovLinearActuatorMm < 0.0f) ovLinearActuatorMm = 0.0f;
  ovExtraMm = extraMm;
  ovTolMm = tolMm;
  ovRequireAsda = requireAsda;
  ovRequireOm = requireOm;
  ovLaserOk = false;
  ovLaserSet = false;

  ovHitClear(ovAsdaLinearActuator, ovLinearActuatorMm);
  ovHitClear(ovAsdaDeposit, ovGripperMm + ovLinearActuatorMm + ovExtraMm);
  ovHitClear(ovOmFeed, ovGripperMm);
  ovHitClear(ovOmReset, 0.0f);
  ovHitClear(ovOmFinal, ovLinearActuatorMm);
}

static void overviewOnEncoderReset() {
  if (!ovActive) return;
  if (!ovOmFeed.done) {
    if (!ovOmFinal.done) ovOmFinal.actualMm = 0.0f;
    return;
  }
  ovOmReset.actualMm = 0.0f;
  ovOmReset.done = true;
  ovOmReset.ok = true;
  if (!ovOmFinal.done) ovOmFinal.actualMm = 0.0f;
}

static void overviewOnAsdaMoveDone(float positionMm) {
  if (!ovActive) return;
  if (!ovAsdaLinearActuator.done && ovWithinTol(positionMm, ovAsdaLinearActuator.targetMm, ovTolMm * 2.0f)) {
    ovAsdaLinearActuator.actualMm = positionMm;
    ovAsdaLinearActuator.done = true;
    ovEvalHit(ovAsdaLinearActuator, false);
  } else if (!ovAsdaDeposit.done &&
             ovWithinTol(positionMm, ovAsdaDeposit.targetMm, ovTolMm * 2.0f)) {
    ovAsdaDeposit.actualMm = positionMm;
    ovAsdaDeposit.done = true;
    ovEvalHit(ovAsdaDeposit, false);
  }
}

static bool overviewMark(const String& event, String& err) {
  if (!ovActive) {
    err = "overview inactivo (POST /api/overview/start)";
    return false;
  }

  if (event == "omFeed" || event == "omFeed55") {
    const float settle = encSettleMmRoundedFeed();
    if (settle < 0.0f) {
      err = "OM sin settle (esperar parada)";
      return false;
    }
    ovOmFeed.actualMm = settle;
    ovOmFeed.done = true;
    ovEvalHit(ovOmFeed, true);
    return true;
  }
  if (event == "omReset") {
    overviewOnEncoderReset();
    return true;
  }
  if (event == "omFinal") {
    const float settle = encSettleMmRoundedFeed();
    if (settle < 0.0f) {
      err = "OM sin settle (esperar parada)";
      return false;
    }
    ovOmFinal.actualMm = settle;
    ovOmFinal.done = true;
    ovEvalHit(ovOmFinal, false);
    return true;
  }
  if (event == "asdaLinearActuator") {
    ovAsdaLinearActuator.actualMm = ovAsdaMmNow();
    ovAsdaLinearActuator.done = true;
    ovEvalHit(ovAsdaLinearActuator, false);
    return true;
  }
  if (event == "asdaDeposit") {
    ovAsdaDeposit.actualMm = ovAsdaMmNow();
    ovAsdaDeposit.done = true;
    ovEvalHit(ovAsdaDeposit, false);
    return true;
  }
  if (event == "laserOk" || event == "laser") {
    ovLaserSet = true;
    ovLaserOk = true;
    return true;
  }
  if (event == "laserBad") {
    ovLaserSet = true;
    ovLaserOk = false;
    return true;
  }
  err = "event desconocido";
  return false;
}

static void appendOvHitJson(char* buf, size_t n, const char* key, const OvHit& h,
                            bool feedStyle) {
  const bool met = feedStyle ? ovFeedMet(h.actualMm, h.targetMm, ovTolMm)
                             : ovWithinTol(h.actualMm, h.targetMm, ovTolMm);
  snprintf(buf, n,
           "\"%s\":{\"targetMm\":%.2f,\"actualMm\":%.2f,\"done\":%s,\"ok\":%s,\"met\":%s}",
           key, h.targetMm, h.actualMm,
           h.done ? "true" : "false",
           h.ok ? "true" : "false",
           met ? "true" : "false");
}

static String overviewJson() {
  char h1[160], h2[160], h3[160], h4[140], h5[160];
  appendOvHitJson(h1, sizeof(h1), "asdaLinearActuator", ovAsdaLinearActuator, false);
  appendOvHitJson(h2, sizeof(h2), "asdaDeposit", ovAsdaDeposit, false);
  appendOvHitJson(h3, sizeof(h3), "omFeed55", ovOmFeed, true);
  snprintf(h4, sizeof(h4),
           "\"omReset\":{\"targetMm\":0,\"actualMm\":%.2f,\"done\":%s,\"ok\":%s,\"met\":%s}",
           ovOmReset.actualMm,
           ovOmReset.done ? "true" : "false",
           ovOmReset.ok ? "true" : "false",
           ovOmReset.done && ovOmReset.ok ? "true" : "false");
  appendOvHitJson(h5, sizeof(h5), "omFinal", ovOmFinal, false);

  const bool stepOk = ovStepOkNow();
  const bool pieceOk = ovPieceOkNow();
  char buf[1200];
  snprintf(buf, sizeof(buf),
           "{\"ok\":true,\"active\":%s,"
           "\"pieceMm\":%.2f,\"gripperMm\":%.2f,\"linearActuatorMm\":%.2f,"
           "\"extraMm\":%.2f,\"tolMm\":%.2f,"
           "\"requireAsda\":%s,\"requireOm\":%s,"
           "\"laserOk\":%s,\"laserSet\":%s,"
           "\"stepOk\":%s,\"pieceOk\":%s,"
           "%s,%s,%s,%s,%s}",
           ovActive ? "true" : "false",
           ovPieceMm, ovGripperMm, ovLinearActuatorMm, ovExtraMm, ovTolMm,
           ovRequireAsda ? "true" : "false",
           ovRequireOm ? "true" : "false",
           ovLaserOk ? "true" : "false",
           ovLaserSet ? "true" : "false",
           stepOk ? "true" : "false",
           pieceOk ? "true" : "false",
           h1, h2, h3, h4, h5);
  return String(buf);
}

// =============================================================================
// HTTP — comun
// =============================================================================
void handleRoot() {
  server.send_P(200, "text/html", index_html);
}

void handleNotFound() {
  sendJson(404, errJson("Ruta no encontrada. Ver GET /"));
}

// =============================================================================
// HTTP — ASDA
// =============================================================================
void handleStatus() {
  const String body = asdaStatusJsonBody();
  const bool reportOk = body.indexOf("\"ok\":true") >= 0;
  sendJson(reportOk ? 200 : 503, body);
}

void handlePos() {
  int32_t pos = 0;
  if (!read32(REG_P5_016, pos)) {
    sendJson(503, errJson("No se pudo leer posicion"));
    return;
  }
  char buf[96];
  snprintf(buf, sizeof(buf),
           "{\"ok\":true,\"positionPuu\":%ld}", (long)pos);
  sendJson(200, String(buf));
}

void handleTest() {
  String detail;
  bool ok = communicationTest(detail);
  sendJson(ok ? 200 : 503, ok ? okJson(detail) : errJson(detail));
}

void handleOn() {
  if (busyMotion) {
    sendJson(409, errJson("Ocupado en movimiento"));
    return;
  }
  bool ok = servoOn();
  sendJson(ok ? 200 : 500, ok ? okJson("Servo ON") : errJson("Fallo Servo ON"));
}

void handleOff() {
  if (busyMotion) {
    sendJson(409, errJson("Ocupado en movimiento"));
    return;
  }
  bool ok = servoOff();
  sendJson(ok ? 200 : 500, ok ? okJson("Servo OFF") : errJson("Fallo Servo OFF"));
}

void handleStop() {
  bool ok = stopMotion();
  motionCancelled = true;
  motionJobActive = false;
  busyMotion = false;
  clearMotionAlarm();
  sendJson(ok ? 200 : 500, ok ? okJson("STOP enviado") : errJson("Fallo STOP"));
}

void handleHome() {
  if (busyMotion) {
    sendJson(409, errJson("Ocupado en movimiento"));
    return;
  }

  String body = server.hasArg("plain") ? server.arg("plain") : "";
  String dir = jsonString(body, "direction", DEFAULT_HOME_DIRECTION);
  dir.toUpperCase();
  bool forward = (dir == "F");
  if (dir != "F" && dir != "R") {
    sendJson(400, errJson("direction debe ser F o R"));
    return;
  }

  uint16_t torque = (uint16_t)jsonLong(body, "torque", DEFAULT_HOME_TORQUE_PCT);
  const uint16_t timeMs = DEFAULT_HOME_TIME_MS;
  float speed = jsonFloat(body, "speedRpm", DEFAULT_HOME_SPEED_RPM);
  const uint32_t timeoutMs = calcTravelTimeoutMs(HOME_MAX_TRAVEL_MM, speed);

  busyMotion = true;
  clearMotionAlarm();
  Serial.printf("API HOME %s torque=%u hold=%u speed=%.1f timeout=%lu\n",
                forward ? "F" : "R", torque, timeMs, speed,
                (unsigned long)timeoutMs);

  if (!homeTorque(forward, torque, timeMs, speed)) {
    busyMotion = false;
    sendJson(500, errJson("No se pudo disparar homing"));
    return;
  }

  const bool blocking = jsonWantsWait(body);
  if (!blocking) {
    motionJobBegin(0, timeoutMs);
    char buf[240];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"accepted\":true,\"busy\":true,\"direction\":\"%s\","
             "\"timeoutMs\":%lu}",
             forward ? "F" : "R", (unsigned long)timeoutMs);
    sendJson(202, String(buf));
    return;
  }

  String detail;
  int32_t pos = 0;
  bool done = waitPrComplete(0, timeoutMs, detail, &pos);
  busyMotion = false;

  char buf[280];
  snprintf(buf, sizeof(buf),
           "{\"ok\":%s,\"alarm\":%s,\"message\":\"%s\",\"positionPuu\":%ld,"
           "\"direction\":\"%s\",\"timeoutMs\":%lu}",
           done ? "true" : "false",
           done ? "false" : "true",
           jsonEscape(detail).c_str(),
           (long)pos,
           forward ? "F" : "R",
           (unsigned long)timeoutMs);
  sendJson(done ? 200 : 504, String(buf));
}

void handleConfigGet() {
  sendJson(200, configJson());
}

void handleConfigPost() {
  if (busyMotion) {
    sendJson(409, errJson("Ocupado en movimiento"));
    return;
  }

  String body = server.hasArg("plain") ? server.arg("plain") : "";
  bool changed = false;

  if (jsonHasKey(body, "moveRpm")) {
    cfgMoveRpm = clampMoveRpm(jsonFloat(body, "moveRpm", cfgMoveRpm));
    changed = true;
  }

  if (jsonBool(body, "resetCal", false)) {
    resetCalFactory();
    changed = true;
  } else if (jsonHasKey(body, "prog1") && jsonHasKey(body, "meas1") &&
             jsonHasKey(body, "prog2") && jsonHasKey(body, "meas2")) {
    float c1 = jsonFloat(body, "prog1", 0);
    float m1 = jsonFloat(body, "meas1", 0);
    float c2 = jsonFloat(body, "prog2", 0);
    float m2 = jsonFloat(body, "meas2", 0);
    if (c1 <= 0.0f || c2 <= 0.0f || m1 <= 0.0f || m2 <= 0.0f) {
      sendJson(400, errJson("Recorridos y medidas deben ser > 0"));
      return;
    }
    if (fabsf(m2 - m1) < 0.05f) {
      sendJson(400, errJson("Los dos puntos deben tener medidas distintas"));
      return;
    }
    float s1 = c1 * FACTORY_STEPS_PER_MM;
    float s2 = c2 * FACTORY_STEPS_PER_MM;
    cfgStepsPerMm = clampStepsPerMm((s2 - s1) / (m2 - m1));
    cfgOffsetSteps = clampOffsetSteps(s1 - cfgStepsPerMm * m1);
    cfgCalProg1Mm = c1;
    cfgCalMeas1Mm = m1;
    cfgCalProg2Mm = c2;
    cfgCalMeas2Mm = m2;
    changed = true;
  } else if (jsonHasKey(body, "stepsPerMm")) {
    cfgStepsPerMm = clampStepsPerMm(jsonFloat(body, "stepsPerMm", cfgStepsPerMm));
    if (jsonHasKey(body, "offsetSteps"))
      cfgOffsetSteps = clampOffsetSteps(jsonFloat(body, "offsetSteps", cfgOffsetSteps));
    changed = true;
  }

  if (!changed) {
    sendJson(400, errJson("Nada que guardar (moveRpm, 2 puntos o resetCal)"));
    return;
  }

  saveCfg();
  Serial.printf("CFG rpm=%.0f spm=%.3f off=%.1f\n",
                cfgMoveRpm, cfgStepsPerMm, cfgOffsetSteps);
  sendJson(200, configJson());
}

void handleMove() {
  if (busyMotion) {
    sendJson(409, errJson("Ocupado en movimiento"));
    return;
  }

  String body = server.hasArg("plain") ? server.arg("plain") : "";
  const bool hasMm = body.indexOf("\"mm\":") >= 0;
  const bool hasPos = body.indexOf("\"position\":") >= 0;
  if (!hasMm && !hasPos) {
    sendJson(400, errJson("Falta mm o position (PUU)"));
    return;
  }

  int32_t position;
  float cmdMm = 0.0f;
  bool factory = jsonBool(body, "factory", false);
  if (hasMm) {
    cmdMm = jsonFloat(body, "mm", 0);
    position = mmToWorkPuu(cmdMm, factory);
  } else {
    position = (int32_t)jsonLong(body, "position", 0);
  }

  float speed = jsonHasKey(body, "speedRpm")
                  ? jsonFloat(body, "speedRpm", cfgMoveRpm)
                  : cfgMoveRpm;
  speed = clampMoveRpm(speed);

  int32_t curPuu = cachedPosOk ? cachedPosPuu : 0;
  if (read32(REG_P5_016, curPuu)) {
    cachedPosPuu = curPuu;
    cachedPosOk = true;
  }
  const float distMm =
      fabsf(puuToMm(position, factory) - puuToMm(curPuu, factory));
  const uint32_t timeoutMs = calcTravelTimeoutMs(distMm, speed);

  busyMotion = true;
  clearMotionAlarm();
  Serial.printf("API MOVE %ld PUU (mm=%.2f d=%.2f) @ %.1f rpm timeout=%lu\n",
                (long)position, hasMm ? cmdMm : puuToMm(position, factory),
                distMm, speed, (unsigned long)timeoutMs);

  if (!moveAbsolute(position, speed)) {
    busyMotion = false;
    sendJson(500, errJson("No se pudo disparar MOVE"));
    return;
  }

  const bool blocking = jsonWantsWait(body);
  if (!blocking) {
    motionJobBegin(1, timeoutMs);
    char buf[320];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"accepted\":true,\"busy\":true,\"targetPuu\":%ld,"
             "\"targetMm\":%.2f,\"distMm\":%.2f,\"timeoutMs\":%lu}",
             (long)position, hasMm ? cmdMm : puuToMm(position, factory),
             distMm, (unsigned long)timeoutMs);
    sendJson(202, String(buf));
    return;
  }

  String detail;
  int32_t pos = 0;
  bool done = waitPrComplete(1, timeoutMs, detail, &pos);
  busyMotion = false;
  if (done) overviewOnAsdaMoveDone(puuToMm(pos, factory));

  char buf[360];
  snprintf(buf, sizeof(buf),
           "{\"ok\":%s,\"alarm\":%s,\"message\":\"%s\",\"targetPuu\":%ld,"
           "\"targetMm\":%.2f,\"positionPuu\":%ld,\"distMm\":%.2f,\"timeoutMs\":%lu}",
           done ? "true" : "false",
           done ? "false" : "true",
           jsonEscape(detail).c_str(),
           (long)position,
           hasMm ? cmdMm : puuToMm(position, factory),
           (long)pos, distMm, (unsigned long)timeoutMs);
  sendJson(done ? 200 : 504, String(buf));
}

// =============================================================================
// HTTP — ENCODER
// =============================================================================
void handleEncoderGet() {
  sendJson(200, encoderStatusJson());
}

void handleEncoderPost() {
  String body = server.hasArg("plain") ? server.arg("plain") : "";
  if (jsonHasKey(body, "offsetMm")) {
    cfgOmOffsetMm = clampOmOffsetMm(jsonFloat(body, "offsetMm", cfgOmOffsetMm));
    saveCfg();
    Serial.printf("OM offsetMm=%.3f (post-round)\n", cfgOmOffsetMm);
  }
  sendJson(200, encoderStatusJson());
}

void handleEncoderReset() {
  String err;
  if (!motionExecSet0(err)) {
    sendJson(503, errJson(err));
    return;
  }
  sendJson(200, encoderStatusJson());
}

// =============================================================================
// HTTP — OVERVIEW
// =============================================================================
void handleOverviewGet() {
  sendJson(200, overviewJson());
}

void handleOverviewStart() {
  String body = server.hasArg("plain") ? server.arg("plain") : "";
  const float pieceMm = jsonFloat(body, "pieceMm", 100.0f);
  const float extraMm = jsonHasKey(body, "extraMm")
      ? jsonFloat(body, "extraMm", OV_EXTRA_MM_DEFAULT)
      : OV_EXTRA_MM_DEFAULT;
  const float tolMm = jsonHasKey(body, "tolMm")
      ? jsonFloat(body, "tolMm", OV_TOL_MM_DEFAULT)
      : OV_TOL_MM_DEFAULT;
  const bool requireAsda = jsonBool(body, "requireAsda", true);
  const bool requireOm = jsonBool(body, "requireOm", true);
  overviewStart(pieceMm, extraMm, tolMm, requireAsda, requireOm);
  Serial.printf("OVERVIEW start L=%.1f linearActuator=%.1f extra=%.1f tol=%.2f\n",
                ovPieceMm, ovLinearActuatorMm, ovExtraMm, ovTolMm);
  sendJson(200, overviewJson());
}

void handleOverviewMark() {
  String body = server.hasArg("plain") ? server.arg("plain") : "";
  String event = jsonString(body, "event", "");
  if (event.length() == 0 && server.hasArg("event"))
    event = server.arg("event");
  String err;
  if (!overviewMark(event, err)) {
    sendJson(400, errJson(err));
    return;
  }
  sendJson(200, overviewJson());
}

// =============================================================================
// ASDA TCP — esclavo maestro (JSON + newline, puerto MOTION_TCP_PORT)
// =============================================================================
static bool asdaTcpLinkOk() {
  return asdaTcpClient && asdaTcpClient.connected();
}

static bool asdaTcpTx(const String& m) {
  if (!asdaTcpLinkOk()) return false;
  bool ok = asdaTcpClient.print(m) > 0;
  if (!m.endsWith("\n"))
    ok = (asdaTcpClient.print("\n") > 0) && ok;
  if (!ok)
    asdaTcpClient.stop();
  return ok;
}

static int asdaTcpJInt(const char* j, const char* k, int d) {
  String n = String("\"") + k + "\":";
  int i = String(j).indexOf(n);
  return i < 0 ? d : String(j).substring(i + n.length()).toInt();
}

static String asdaTcpJStr(const char* j, const char* k) {
  String n = String("\"") + k + "\":\"";
  String s(j);
  int i = s.indexOf(n);
  if (i < 0) return "";
  int a = i + n.length(), b = s.indexOf('"', a);
  return b < 0 ? "" : s.substring(a, b);
}

static void asdaTcpTxState(uint8_t byteCode, const char* name) {
  char buf[96];
  snprintf(buf, sizeof(buf),
           "{\"ver\":%u,\"type\":\"state\",\"actuator\":\"motion\",\"byte\":%u,\"name\":\"%s\"}",
           (unsigned)MOTION_PROTO_VER, (unsigned)byteCode, name);
  asdaTcpTx(String(buf));
  asdaTcpLastStateByte = byteCode;
}

static void motionTcpTxEvent(const char* actuator, uint8_t byteCode, const char* name,
                             int32_t extra = INT32_MIN) {
  char buf[192];
  if (extra != INT32_MIN) {
    snprintf(buf, sizeof(buf),
             "{\"ver\":%u,\"type\":\"event\",\"actuator\":\"%s\",\"byte\":%u,"
             "\"name\":\"%s\",\"positionPuu\":%ld}",
             (unsigned)MOTION_PROTO_VER, actuator, (unsigned)byteCode, name, (long)extra);
  } else {
    snprintf(buf, sizeof(buf),
             "{\"ver\":%u,\"type\":\"event\",\"actuator\":\"%s\",\"byte\":%u,\"name\":\"%s\"}",
             (unsigned)MOTION_PROTO_VER, actuator, (unsigned)byteCode, name);
  }
  asdaTcpTx(String(buf));
}

static void asdaTcpTxEvent(uint8_t byteCode, const char* name, int32_t posPuu) {
  motionTcpTxEvent("asda", byteCode, name, posPuu);
}

void motionTcpOnEncoderError() { motionTcpNotifyEncError = true; }

void motionTcpOnFeedOk(bool sideR) {
  if (sideR) motionTcpNotifyFeedOkR = true;
  else motionTcpNotifyFeedOkL = true;
}

void motionTcpOnFeedNg(bool sideR) {
  if (sideR) motionTcpNotifyFeedNgR = true;
  else motionTcpNotifyFeedNgL = true;
}

static void motionTcpTxMeasured(uint8_t byteCode, bool sideR,
                                  float mmOfficial, float mmSigned, bool settled) {
  char buf[240];
  snprintf(buf, sizeof(buf),
           "{\"ver\":%u,\"type\":\"event\",\"actuator\":\"encoder\",\"byte\":%u,"
           "\"name\":\"GetMeasured\",\"side\":\"%c\",\"mmOfficial\":%.2f,"
           "\"mm\":%.3f,\"settled\":%s}",
           (unsigned)MOTION_PROTO_VER, (unsigned)byteCode, sideR ? 'R' : 'L',
           (double)mmOfficial, (double)mmSigned, settled ? "true" : "false");
  asdaTcpTx(String(buf));
}

static bool encoderTcpDoByte(uint8_t cmdByte, const char* line) {
  (void)line;
  String err;
  bool ok = false;

  switch (cmdByte) {
    case ENC_CMD_MEASURE_R: {
      float mmOff = 0.0f, mmSigned = 0.0f;
      bool settled = false;
      ok = motionExecGetMeasured(true, mmOff, mmSigned, settled, err);
      if (ok)
        motionTcpTxMeasured(cmdByte, true, mmOff, mmSigned, settled);
      break;
    }
    case ENC_CMD_MEASURE_L: {
      float mmOff = 0.0f, mmSigned = 0.0f;
      bool settled = false;
      ok = motionExecGetMeasured(false, mmOff, mmSigned, settled, err);
      if (ok)
        motionTcpTxMeasured(cmdByte, false, mmOff, mmSigned, settled);
      break;
    }
    case ENC_CMD_SET0_R:
      ok = motionExecSet0Side(true, err);
      break;
    case ENC_CMD_SET0_L:
      ok = motionExecSet0Side(false, err);
      break;
    default:
      err = "byte/cmd encoder desconocido";
      break;
  }

  char buf[256];
  snprintf(buf, sizeof(buf),
           "{\"ver\":%u,\"type\":\"ack\",\"actuator\":\"encoder\",\"byte\":%u,"
           "\"ok\":%s,\"message\":\"%s\"}",
           (unsigned)MOTION_PROTO_VER, (unsigned)cmdByte,
           ok ? "true" : "false", jsonEscape(err).c_str());
  asdaTcpTx(String(buf));
  return ok;
}

static bool feederTcpDoByte(uint8_t cmdByte, const char* line) {
  (void)line;
  String err;
  bool ok = false;

  switch (cmdByte) {
    case FEED_CMD_FEED_R:
      ok = feedQueueTestSide(1, err);
      break;
    case FEED_CMD_FEED_L:
      ok = feedQueueTestSide(0, err);
      break;
    default:
      err = "byte/cmd feeder desconocido";
      break;
  }

  char buf[256];
  snprintf(buf, sizeof(buf),
           "{\"ver\":%u,\"type\":\"ack\",\"actuator\":\"feeder\",\"byte\":%u,"
           "\"ok\":%s,\"message\":\"%s\"}",
           (unsigned)MOTION_PROTO_VER, (unsigned)cmdByte,
           ok ? "true" : "false", jsonEscape(err).c_str());
  asdaTcpTx(String(buf));
  return ok;
}

static void motionTcpDoResetAll() {
  String err;
  const bool ok = motionExecResetAll(err);
  char buf[256];
  snprintf(buf, sizeof(buf),
           "{\"ver\":%u,\"type\":\"ack\",\"actuator\":\"motion\",\"name\":\"ResetAll\","
           "\"ok\":%s,\"message\":\"%s\"}",
           (unsigned)MOTION_PROTO_VER,
           ok ? "true" : "false", jsonEscape(err).c_str());
  asdaTcpTx(String(buf));
  if (ok)
    asdaTcpPushStateIfChanged();
}

static bool motionTcpDoByte(uint8_t cmdByte, const char* line) {
  (void)line;
  String err;
  bool ok = false;

  switch (cmdByte) {
    case MOT_CMD_RESET_ERR:
      ok = motionExecResetErrors(err);
      break;
    default:
      err = "byte/cmd motion desconocido";
      break;
  }

  char buf[256];
  snprintf(buf, sizeof(buf),
           "{\"ver\":%u,\"type\":\"ack\",\"actuator\":\"motion\",\"byte\":%u,"
           "\"ok\":%s,\"message\":\"%s\"}",
           (unsigned)MOTION_PROTO_VER, (unsigned)cmdByte,
           ok ? "true" : "false", jsonEscape(err).c_str());
  asdaTcpTx(String(buf));
  if (ok)
    asdaTcpPushStateIfChanged();
  return ok;
}

static uint8_t asdaTcpResolveStateByte() {
  if (motionAlarm) return ASDA_TX_ERROR;
  if (busyMotion || motionJobActive) return ASDA_TX_BUSY;
  if (asdaTcpStopPending) return ASDA_TX_RETURN;
  return ASDA_TX_IDLE;
}

static const char* asdaTcpStateName(uint8_t byteCode) {
  switch (byteCode) {
    case ASDA_TX_INIT: return "InitState";
    case ASDA_TX_IDLE: return "IdleState";
    case ASDA_TX_BUSY: return "BusyState";
    case ASDA_TX_ERROR: return "ErrorState";
    case ASDA_TX_STOP: return "StoprState";
    case ASDA_TX_RETURN: return "ReturnState";
    default: return "unknown";
  }
}

static void asdaTcpPushStateIfChanged() {
  const uint8_t st = asdaTcpResolveStateByte();
  if (st == asdaTcpLastStateByte) return;
  asdaTcpTxState(st, asdaTcpStateName(st));
  if (st == ASDA_TX_RETURN)
    asdaTcpStopPending = false;
}

static void asdaTcpTxStatus() {
  String j = "{\"ver\":";
  j += MOTION_PROTO_VER;
  j += ",\"type\":\"status\",\"actuator\":\"asda\",\"byte\":";
  j += (unsigned)ASDA_CMD_STATUS;
  j += ",";
  j += asdaStatusJsonBody().substring(1);
  asdaTcpTx(j);
}

static bool asdaTcpDoByte(uint8_t cmdByte, const char* line) {
  String err;
  bool ok = false;

  switch (cmdByte) {
    case ASDA_CMD_HOME: {
      String dir = asdaTcpJStr(line, "direction");
      if (dir.length() == 0) dir = DEFAULT_HOME_DIRECTION;
      dir.toUpperCase();
      const bool forward = (dir != "R");
      if (dir != "F" && dir != "R") {
        err = "direction debe ser F o R";
        break;
      }
      ok = asdaStartHome(forward, err);
      break;
    }
    case ASDA_CMD_STOP:
      ok = asdaExecStop(err);
      if (ok)
        asdaTcpTxState(ASDA_TX_STOP, "StoprState");
      break;
    case ASDA_CMD_OFF:
      ok = asdaExecOff(err);
      break;
    case ASDA_CMD_ON:
      ok = asdaExecOn(err);
      break;
    case ASDA_CMD_MOVE_ABS: {
      const bool hasMm = strstr(line, "\"mm\":") != nullptr;
      int32_t position = 0;
      if (hasMm) {
        const float mm = jsonFloat(String(line), "mm", 0);
        const bool factory = strstr(line, "\"factory\":true") != nullptr;
        position = mmToWorkPuu(mm, factory);
      } else {
        position = (int32_t)asdaTcpJInt(line, "position", 0);
      }
      float speed = jsonHasKey(String(line), "speedRpm")
                      ? jsonFloat(String(line), "speedRpm", cfgMoveRpm)
                      : cfgMoveRpm;
      ok = asdaStartMove(position, speed, err);
      break;
    }
    case ASDA_CMD_MOVE_ZERO: {
      float speed = jsonHasKey(String(line), "speedRpm")
                      ? jsonFloat(String(line), "speedRpm", cfgMoveRpm)
                      : cfgMoveRpm;
      ok = asdaStartMove(0, speed, err);
      break;
    }
    case ASDA_CMD_RESUME:
      ok = asdaExecResume(err);
      break;
    case ASDA_CMD_RESET_ERR:
      ok = asdaExecResetError(err);
      break;
    case ASDA_CMD_STATUS:
      asdaTcpTxStatus();
      return true;
    default:
      err = "byte/cmd ASDA desconocido";
      break;
  }

  char buf[256];
  snprintf(buf, sizeof(buf),
           "{\"ver\":%u,\"type\":\"ack\",\"actuator\":\"asda\",\"byte\":%u,"
           "\"ok\":%s,\"message\":\"%s\"}",
           (unsigned)MOTION_PROTO_VER, (unsigned)cmdByte,
           ok ? "true" : "false", jsonEscape(err).c_str());
  asdaTcpTx(String(buf));
  if (ok)
    asdaTcpPushStateIfChanged();
  return ok;
}

static uint8_t asdaTcpResolveCmdByte(const char* line) {
  const int b = asdaTcpJInt(line, "byte", -1);
  if (b > 0) return (uint8_t)b;

  const String cmd = asdaTcpJStr(line, "command");
  if (cmd == "home" || cmd == "HomeASDA") return ASDA_CMD_HOME;
  if (cmd == "stop" || cmd == "StopASDA") return ASDA_CMD_STOP;
  if (cmd == "off" || cmd == "OffASDA") return ASDA_CMD_OFF;
  if (cmd == "on" || cmd == "OnASDA") return ASDA_CMD_ON;
  if (cmd == "move" || cmd == "ABSPositionASDA") return ASDA_CMD_MOVE_ABS;
  if (cmd == "homePos" || cmd == "HpASDA") return ASDA_CMD_MOVE_ZERO;
  if (cmd == "status" || cmd == "GetStatus") return ASDA_CMD_STATUS;
  if (cmd == "resume" || cmd == "ReturnState") return ASDA_CMD_RESUME;
  if (cmd == "resetError" || cmd == "ResetError") return MOT_CMD_RESET_ERR;
  return 0;
}

static void asdaTcpOnLine(const char* line) {
  if (!strstr(line, "\"type\":\"command\"")) return;

  const String cmd = asdaTcpJStr(line, "command");
  if (cmd == "resetAll" || cmd == "ResetMotion") {
    motionTcpDoResetAll();
    return;
  }

  const int rawByte = asdaTcpJInt(line, "byte", -1);
  if (rawByte > 0) {
    const uint8_t cmdByte = (uint8_t)rawByte;
    if (cmdByte == ENC_CMD_MEASURE_R || cmdByte == ENC_CMD_SET0_R
        || cmdByte == ENC_TX_ERROR
        || cmdByte == ENC_CMD_MEASURE_L || cmdByte == ENC_CMD_SET0_L) {
      encoderTcpDoByte(cmdByte, line);
      return;
    }
    if (cmdByte >= FEED_CMD_FEED_R && cmdByte <= FEED_CMD_FEED_L) {
      feederTcpDoByte(cmdByte, line);
      return;
    }
    if (cmdByte == MOT_CMD_RESET_ERR) {
      motionTcpDoByte(cmdByte, line);
      return;
    }
    if (cmdByte >= 0x01 && cmdByte <= 0x0E) {
      asdaTcpDoByte(cmdByte, line);
      return;
    }
    asdaTcpTx(String("{\"ver\":") + MOTION_PROTO_VER
              + ",\"type\":\"ack\",\"ok\":false,"
              "\"message\":\"byte/cmd desconocido\"}");
    return;
  }

  const uint8_t cmdByte = asdaTcpResolveCmdByte(line);
  if (!cmdByte) {
    asdaTcpTx(String("{\"ver\":") + MOTION_PROTO_VER
              + ",\"type\":\"ack\",\"actuator\":\"asda\",\"ok\":false,"
              "\"message\":\"Falta byte o command valido\"}");
    return;
  }
  if (cmdByte == MOT_CMD_RESET_ERR) {
    motionTcpDoByte(cmdByte, line);
    return;
  }
  asdaTcpDoByte(cmdByte, line);
}

static void asdaTcpRxDrain() {
  while (asdaTcpClient.available()) {
    char c = (char)asdaTcpClient.read();
    if (c == '\n' || c == '\r') {
      if (asdaTcpRxLen) {
        asdaTcpRxLine[asdaTcpRxLen] = 0;
        asdaTcpOnLine(asdaTcpRxLine);
        asdaTcpRxLen = 0;
      }
    } else if (asdaTcpRxLen < sizeof(asdaTcpRxLine) - 1) {
      asdaTcpRxLine[asdaTcpRxLen++] = c;
    }
  }
}

static void asdaTcpOnClientAccepted() {
  asdaTcpClient.setNoDelay(true);
  asdaTcpRxLen = 0;
  asdaTcpLastStateByte = 0;
  asdaTcpNeedInit = true;
  asdaTcpTx(String("{\"ver\":") + MOTION_PROTO_VER
            + ",\"type\":\"hello\",\"role\":\"motion\"}");
  Serial.printf("[ASDA-TCP] On desde %s\n",
                asdaTcpClient.remoteIP().toString().c_str());
  asdaTcpWasConnected = true;
}

static bool asdaTcpAcceptIncoming() {
  if (!asdaTcpServicesUp || !asdaTcpServer.hasClient()) return false;
  if (asdaTcpClient.connected())
    asdaTcpClient.stop();
  asdaTcpClient = asdaTcpServer.available();
  if (!asdaTcpClient) return false;
  asdaTcpOnClientAccepted();
  return true;
}

static void asdaTcpEnsureServices() {
  if (asdaTcpServicesUp || WiFi.status() != WL_CONNECTED) return;
  asdaTcpServer.end();
  delay(20);
  asdaTcpServer.begin();
  asdaTcpServer.setNoDelay(true);
  asdaTcpServicesUp = true;
  Serial.printf("[ASDA-TCP] Servicios On %s:%u\n",
                WiFi.localIP().toString().c_str(), (unsigned)MOTION_TCP_PORT);
}

static void asdaTcpStopServices() {
  if (asdaTcpClient.connected()) asdaTcpClient.stop();
  asdaTcpWasConnected = false;
  if (!asdaTcpServicesUp) return;
  asdaTcpServer.end();
  asdaTcpServicesUp = false;
  Serial.println("[ASDA-TCP] Servicios Off");
}

static void motionTcpPollMeasureEvents() {
  if (!asdaTcpLinkOk()) return;

  if (motionTcpNotifyMeasureR) {
    motionTcpNotifyMeasureR = false;
    float mmOff = 0.0f, mmSigned = 0.0f;
    bool settled = false;
    String err;
    if (motionExecGetMeasured(true, mmOff, mmSigned, settled, err)
        && (fabsf(mmOff - motionTcpLastPushMmR) >= 0.05f || settled)) {
      motionTcpLastPushMmR = mmOff;
      motionTcpTxMeasured(ENC_CMD_MEASURE_R, true, mmOff, mmSigned, settled);
    }
  }
  if (motionTcpNotifyMeasureL) {
    motionTcpNotifyMeasureL = false;
    float mmOff = 0.0f, mmSigned = 0.0f;
    bool settled = false;
    String err;
    if (motionExecGetMeasured(false, mmOff, mmSigned, settled, err)
        && (fabsf(mmOff - motionTcpLastPushMmL) >= 0.05f || settled)) {
      motionTcpLastPushMmL = mmOff;
      motionTcpTxMeasured(ENC_CMD_MEASURE_L, false, mmOff, mmSigned, settled);
    }
  }
}

static void motionTcpPollFeedEvents() {
  if (!asdaTcpLinkOk()) return;

  if (motionTcpNotifyEncError) {
    motionTcpNotifyEncError = false;
    motionTcpTxEvent("encoder", ENC_TX_ERROR, "Error");
  }
  if (motionTcpNotifyFeedOkL) {
    motionTcpNotifyFeedOkL = false;
    motionTcpTxEvent("feeder", FEED_TX_LENGTH_OK_L, "LenghtOK_L");
  }
  if (motionTcpNotifyFeedNgL) {
    motionTcpNotifyFeedNgL = false;
    motionTcpTxEvent("feeder", FEED_TX_LENGTH_NG_L, "LenghtNG_L");
  }
  if (motionTcpNotifyFeedOkR) {
    motionTcpNotifyFeedOkR = false;
    motionTcpTxEvent("feeder", FEED_TX_LENGTH_OK_R, "LenghtOK_R");
  }
  if (motionTcpNotifyFeedNgR) {
    motionTcpNotifyFeedNgR = false;
    motionTcpTxEvent("feeder", FEED_TX_LENGTH_NG_R, "LenghtNG_R");
  }
}

// Láseres / safety exhaust — stubs + poll opcional (MOT_IO_SENSOR_EVENTS)
static void LaserR() {
  // TODO: TX MOT_ERR_LASER_R (0x4D)
}
static void LaserL() {
  // TODO: TX MOT_ERR_LASER_L (0x4E)
}
static void Exhaust() {
  // TODO: TX MOT_ERR_EXHAUST (0x4F) — Error desfoga el aire
}

static bool motionSensorActiveLaser(uint8_t pin) {
  return digitalRead(pin) == LOW;  // INPUT_PULLUP: activo en LOW
}

static bool motionSensorActiveSafety() {
#if PIN_ESTOP_ACTIVE_HIGH
  return digitalRead(PIN_SAFETY_EXHAUST) == HIGH;
#else
  return digitalRead(PIN_SAFETY_EXHAUST) == LOW;
#endif
}

static void motionPollIoSensors() {
#if !MOT_IO_SENSOR_EVENTS
  return;
#else
  static bool lastLaserR = false, lastLaserL = false, lastSafety = false;
  static bool rawLaserR = false, rawLaserL = false, rawSafety = false;
  static uint32_t tLaserR = 0, tLaserL = 0, tSafety = 0;
  const uint32_t now = millis();

  const bool rLaserR = motionSensorActiveLaser(LRX_LaserR);
  if (rLaserR != rawLaserR) { rawLaserR = rLaserR; tLaserR = now; }
  else if ((now - tLaserR) >= MOT_SENSOR_DEBOUNCE_MS && rLaserR != lastLaserR) {
    lastLaserR = rLaserR;
    if (rLaserR) { motionTcpNotifyLaserR = true; LaserR(); }
  }

  const bool rLaserL = motionSensorActiveLaser(LRX_LaserL);
  if (rLaserL != rawLaserL) { rawLaserL = rLaserL; tLaserL = now; }
  else if ((now - tLaserL) >= MOT_SENSOR_DEBOUNCE_MS && rLaserL != lastLaserL) {
    lastLaserL = rLaserL;
    if (rLaserL) { motionTcpNotifyLaserL = true; LaserL(); }
  }

  const bool rSafety = motionSensorActiveSafety();
  if (rSafety != rawSafety) { rawSafety = rSafety; tSafety = now; }
  else if ((now - tSafety) >= MOT_SENSOR_DEBOUNCE_MS && rSafety != lastSafety) {
    lastSafety = rSafety;
    if (rSafety) { motionTcpNotifySafety = true; Exhaust(); }
  }
#endif
}

static void motionTcpPollIoSensorEvents() {
#if !MOT_IO_SENSOR_EVENTS
  return;
#else
  if (!asdaTcpLinkOk()) return;
  if (motionTcpNotifyLaserR) {
    motionTcpNotifyLaserR = false;
    motionTcpTxEvent("laser", MOT_ERR_LASER_R, "LaserR");
  }
  if (motionTcpNotifyLaserL) {
    motionTcpNotifyLaserL = false;
    motionTcpTxEvent("laser", MOT_ERR_LASER_L, "LaserL");
  }
  if (motionTcpNotifySafety) {
    motionTcpNotifySafety = false;
    motionTcpTxEvent("safety", MOT_ERR_EXHAUST, "Exhaust");
  }
#endif
}

static void asdaTcpPollEvents() {
  if (!asdaTcpLinkOk()) return;

  if (asdaTcpNeedInit) {
    asdaTcpTxState(ASDA_TX_INIT, "InitState");
    asdaTcpNeedInit = false;
  }

  if (asdaTcpNotifyReached) {
    asdaTcpNotifyReached = false;
    const float reachedMm = puuToMm(asdaTcpReachedPos, false);
    char buf[240];
    snprintf(buf, sizeof(buf),
             "{\"ver\":%u,\"type\":\"event\",\"actuator\":\"asda\",\"byte\":%u,"
             "\"name\":\"ReachedASDA\",\"positionPuu\":%ld,\"positionMm\":%.2f}",
             (unsigned)MOTION_PROTO_VER, (unsigned)ASDA_TX_REACHED,
             (long)asdaTcpReachedPos, (double)reachedMm);
    asdaTcpTx(String(buf));
  }

  asdaTcpPushStateIfChanged();
  motionTcpPollMeasureEvents();
  motionTcpPollFeedEvents();
  motionTcpPollIoSensorEvents();
}

static void serviceAsdaTcp() {
  if (WiFi.status() != WL_CONNECTED) {
    asdaTcpStopServices();
    return;
  }

  asdaTcpEnsureServices();

  if (asdaTcpAcceptIncoming())
    return;

  if (!asdaTcpLinkOk()) {
    if (asdaTcpWasConnected) {
      Serial.println("[ASDA-TCP] Off");
      asdaTcpWasConnected = false;
    }
    return;
  }

  asdaTcpWasConnected = true;
  asdaTcpRxDrain();
  asdaTcpPollEvents();
}

// =============================================================================
// WIFI
// =============================================================================
bool connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("motion");

  if (!WiFi.config(STA_IP, STA_GW, STA_MASK, STA_DNS)) {
    Serial.println("WiFi.config() fallo (IP estatica)");
  }

  Serial.printf("Conectando a WiFi '%s'...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < 30000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("ERROR: no se pudo conectar al WiFi");
    return false;
  }

  Serial.printf("WiFi OK  IP=%s  RSSI=%d dBm\n",
                WiFi.localIP().toString().c_str(),
                WiFi.RSSI());
  return true;
}

// =============================================================================
// SETUP / LOOP
// =============================================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  ServoBus.begin(MODBUS_BAUD, SERIAL_8N2, RS485_RX_PIN, RS485_TX_PIN);
  clearServoRx();

  pinMode(LRX_LaserR, INPUT_PULLUP);
  pinMode(LRX_LaserL, INPUT_PULLUP);
  pinMode(PIN_E_STOP, INPUT_PULLUP);

  Serial.println();
  Serial.println("ESP32 ASDA-B3 + OM Encoder RE30AJ2000F");
  Serial.printf("Modbus ID=%u baud=%lu 8N2 RX=%d TX=%d\n",
                MODBUS_ID, (unsigned long)MODBUS_BAUD,
                RS485_RX_PIN, RS485_TX_PIN);

  initEncodersHw();
  if (encSide[ENC_IX_R].installed && encSide[ENC_IX_R].pinZ >= 0)
    attachInterrupt(digitalPinToInterrupt(encSide[ENC_IX_R].pinZ), onEncoderZR, FALLING);
  if (encSide[ENC_IX_L].installed && encSide[ENC_IX_L].pinZ >= 0)
    attachInterrupt(digitalPinToInterrupt(encSide[ENC_IX_L].pinZ), onEncoderZL, FALLING);
#if ENC_PCNT_IDF5
  Serial.printf("Encoder PCNT: IDF5 pulse_cnt · cuadratura x%u\n", (unsigned)ENC_QUAD);
#else
  Serial.printf("Encoder PCNT: driver legacy · cuadratura x%u\n", (unsigned)ENC_QUAD);
#endif
  Serial.println("Tip: si mm dispersos en rapido → pull-up 1-2.2k a 3.3V en A/B");

  loadCfg();
  Serial.printf("CFG rpm=%.0f spm=%.3f off=%.1f omOff=%.3f\n",
                cfgMoveRpm, cfgStepsPerMm, cfgOffsetSteps, cfgOmOffsetMm);

  overviewStart(100.0f, OV_EXTRA_MM_DEFAULT, OV_TOL_MM_DEFAULT, true, true);

  if (!connectWifi()) {
    Serial.println("Reintentando WiFi en loop...");
  }

  asdaTcpEnsureServices();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/pos", HTTP_GET, handlePos);
  server.on("/api/config", HTTP_GET, handleConfigGet);
  server.on("/api/config", HTTP_POST, handleConfigPost);
  server.on("/api/test", HTTP_POST, handleTest);
  server.on("/api/on", HTTP_POST, handleOn);
  server.on("/api/off", HTTP_POST, handleOff);
  server.on("/api/stop", HTTP_POST, handleStop);
  server.on("/api/home", HTTP_POST, handleHome);
  server.on("/api/move", HTTP_POST, handleMove);
  server.on("/api/encoder", HTTP_GET, handleEncoderGet);
  server.on("/api/encoder", HTTP_POST, handleEncoderPost);
  server.on("/api/encoder/reset", HTTP_POST, handleEncoderReset);
  server.on("/api/overview", HTTP_GET, handleOverviewGet);
  server.on("/api/overview/start", HTTP_POST, handleOverviewStart);
  server.on("/api/overview/mark", HTTP_POST, handleOverviewMark);
  feedRegisterHttpRoutes(server);
  server.on("/feed", HTTP_GET, []() {
    server.send_P(200, "text/html", feed_index_html);
  });
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.printf("API lista en http://%s/\n", STA_IP.toString().c_str());
  Serial.printf("ASDA TCP maestro en %s:%u\n",
                STA_IP.toString().c_str(), (unsigned)MOTION_TCP_PORT);
  Serial.println("UI Motion: Vista general | ASDA B3 | OM | Alimentacion (/feed)");

  servoCanInitMutex();
  delay(SERVO_POWERUP_MS);
  if (initCANBus(CAN_INIT_MAX_RETRIES))
  {
    for (uint8_t attempt = 1; attempt <= SERVO_SETUP_MAX_RETRIES; attempt++)
    {
      if (setupServoFeeder()) break;
      delay(500);
      if (attempt < SERVO_SETUP_MAX_RETRIES) initCANBus(2);
    }
  }
  feedInit();
  Serial.printf("Motion CAN: can=%d servo=%d\n", (int)canInitialized, (int)servoCanReady);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    static uint32_t lastTry = 0;
    if (millis() - lastTry > 5000) {
      lastTry = millis();
      Serial.println("WiFi perdido — reconectando...");
      connectWifi();
    }
  }
  server.handleClient();
  motionPollIoSensors();
  serviceAsdaTcp();
  if (motionJobActive)
    pollMotionOnce();
  updateEncoderMotion(millis());
  serviceCANRx();
  feedLoop();
}

