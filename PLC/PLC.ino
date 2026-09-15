// PLCA — esclavo I/O TubeCut: sensores de seguridad + válvulas.
// WiFi STA 10.10.32.50 | TCP servidor :8766 | JSON + newline (PreFeeder / TCM).
//
// RESUMEN:
//   · Lee 6 entradas (pinzas, sujetador, cortador, manguera A/B, bandeja) con debounce.
//   · Válvulas por impulso (cutter R/L, grippers, holder, encoder, reset):
//     comando on = un pulso; off = otro pulso. El GPIO no queda enclavado.
//   · Blower: nivel ON durante durationSec (HMI), luego OFF automático.
//   · Compatible con protocolo sensor_tubecut (snapshot/alert/poll).
//   · Status extendido para PLCA_Master (válvulas lógicas en JSON type=status).
//
// ÍNDICE:
//   01  Estado global
//   02  Sensores
//   03  Válvulas
//   04  TCP TX
//   05  TCP RX / WiFi
//   06  Setup / Loop

#include <WiFi.h>
#include "Config.h"
#include "PlcStates.h"

WiFiServer tcpServer(PLCA_TCP_PORT);
WiFiClient tcpClient;

// ============================================================
// SECCION 01 — Estado global
// ============================================================
bool wifiReady = false;
bool tcpServicesUp = false;
bool tcpWasConnected = false;
bool peerLinkOk = false;

uint8_t lastBitmask = 0xFF;
uint8_t rawCandidateMask = 0xFF;
unsigned long rawCandidateSinceMs = 0;
unsigned long lastSnapshotTxMs = 0;
unsigned long lastStatusPushMs = 0;
uint8_t sequence = 0;

bool stCutterR   = false;
bool stCutterL   = false;
bool stCutters   = false;
bool stGrippers  = false;
bool stHolder    = false;  // no auto-ON: lo activa rutina / manual
bool stEncoder   = false;  // válvula encoder (0x1D)
bool stBlower    = false;
bool stReset     = false;
static bool blowerTimedActive = false;
static unsigned long blowerStartMs = 0;
static unsigned long blowerHoldMs = 0;

char tcpRxLine[512];
uint16_t tcpRxLen = 0;
unsigned long wifiConnectStartedMs = 0;
bool staWasConnected = false;

static bool lastPeerCutterR = false;
static bool lastPeerCutterL = false;
static bool lastPeerCutters = false;
static bool lastPeerGrippers = false;
static bool lastPeerHolder = false;
static bool lastPeerEncoder = false;
static bool lastPeerBlower = false;
static bool lastPeerReset = false;
static uint8_t lastPeerMask = 0xFF;

static uint8_t plcTcpLastStateByte = 0;
static bool plcTcpNeedInit = false;
static bool plcTcpStopPending = false;

// ============================================================
// SECCION 02 — Sensores
// ============================================================
static uint8_t readSensorBitmask()
{
  uint8_t mask = 0;
  if (!digitalRead(PIN_CUTTER))  mask |= BIT_CUTTER;
  if (!digitalRead(PIN_GRIPPER)) mask |= BIT_GRIPPER;
  if (!digitalRead(PIN_HOLDER))  mask |= BIT_HOLDER;
  if (!digitalRead(PIN_ENCODER)) mask |= BIT_ENCODER;
  return mask;
}

static uint8_t currentStableMask()
{
  return (lastBitmask == 0xFF) ? 0 : lastBitmask;
}

static bool updateDebouncedMask(uint8_t raw, uint8_t& stableOut, unsigned long now)
{
  if (raw != rawCandidateMask)
  {
    rawCandidateMask = raw;
    rawCandidateSinceMs = now;
  }

  if ((now - rawCandidateSinceMs) < SENSOR_DEBOUNCE_MS)
  {
    stableOut = (lastBitmask == 0xFF) ? 0 : lastBitmask;
    return false;
  }

  stableOut = rawCandidateMask;
  if (lastBitmask == 0xFF)
    return true;
  return stableOut != lastBitmask;
}

static void logSensorMask(uint8_t mask)
{
  Serial.print("SENSORS mask=0x");
  if (mask < 0x10) Serial.print('0');
  Serial.print(mask, HEX);
  Serial.print(" G=");
  Serial.print((mask & BIT_GRIPPER) ? 1 : 0);
  Serial.print(" H=");
  Serial.print((mask & BIT_HOLDER) ? 1 : 0);
  Serial.print(" C=");
  Serial.print((mask & BIT_CUTTER) ? 1 : 0);
  Serial.print(" HA=");
  Serial.print((mask & BIT_HOSE_A) ? 1 : 0);
  Serial.print(" HB=");
  Serial.print((mask & BIT_HOSE_B) ? 1 : 0);
  Serial.print(" E=");
  Serial.println((mask & BIT_ENCODER) ? 1 : 0);
}

// ============================================================
// SECCION 03 — Válvulas (pulso ON / pulso OFF, sin enclavado)
// ============================================================
static void valveWritePin(uint8_t pin, bool on)
{
#if VALVE_ACTIVE_HIGH
  digitalWrite(pin, on ? HIGH : LOW);
#else
  digitalWrite(pin, on ? LOW : HIGH);
#endif
}

static void valveIdleAll()
{
  valveWritePin(PIN_OUT_CUTTER_R, false);
  valveWritePin(PIN_OUT_CUTTER_L, false);
  valveWritePin(PIN_OUT_GRIPPERS, false);
  valveWritePin(PIN_OUT_HOLDER,   false);
  valveWritePin(PIN_OUT_ENCODER,  false);
  valveWritePin(PIN_OUT_BLOWER,   false);
  valveWritePin(PIN_OUT_RESET,    false);
}

static void valvePulsePin(uint8_t pin)
{
  valveWritePin(pin, true);
  delay(VALVE_PULSE_MS);
  valveWritePin(pin, false);
}

static void syncCuttersFlag()
{
  stCutters = stCutterR || stCutterL;
}

// KEEP lógico (Set/Res): pulso solo en transición. Mismo pin = Set o Res
// según wantOn; el PLC neumático enclava. No pulsar si ya está en el estado
// pretendido (un pulso de más invertiría un KEEP tipo toggle).
static bool valveSetPulse(bool& st, uint8_t pin, bool wantOn)
{
  if (st == wantOn) return false;
  st = wantOn;
  valvePulsePin(pin);
  syncCuttersFlag();
  return true;
}

static void blowerStop()
{
  blowerTimedActive = false;
  blowerHoldMs = 0;
  valveWritePin(PIN_OUT_BLOWER, false);
  stBlower = false;
}

static void blowerStart(unsigned long durationMs)
{
  if (durationMs < BLOWER_MIN_MS) durationMs = BLOWER_MIN_MS;
  if (durationMs > BLOWER_MAX_MS) durationMs = BLOWER_MAX_MS;
  valveWritePin(PIN_OUT_BLOWER, true);
  stBlower = true;
  blowerTimedActive = true;
  blowerStartMs = millis();
  blowerHoldMs = durationMs;
#if PLCA_DEBUG
  Serial.printf("BLOWER ON %lums (%.1fs)\n", durationMs, durationMs / 1000.0);
#endif
}

static bool blowerSet(bool wantOn, unsigned long durationMs)
{
  if (!wantOn) {
    if (!stBlower && !blowerTimedActive) return false;
    blowerStop();
    return true;
  }
  blowerStart(durationMs);
  return true;
}

static void logValveLogical()
{
#if PLCA_DEBUG
  Serial.printf("VALVE(logic) cutterR=%d cutterL=%d grippers=%d holder=%d encoder=%d blower=%d\n",
                (int)stCutterR, (int)stCutterL, (int)stGrippers, (int)stHolder,
                (int)stEncoder, (int)stBlower);
#endif
}

static bool setPlcOutputByName(const String& outName, bool state)
{
  String n = outName;
  n.trim();
  n.toUpperCase();
  bool changed = false;
  if (n == "CUTTER" || n == "CUTTERS") {
    changed |= valveSetPulse(stCutterR, PIN_OUT_CUTTER_R, state);
    changed |= valveSetPulse(stCutterL, PIN_OUT_CUTTER_L, state);
  }
  else if (n == "CUTTER_R" || n == "CUTTERS_R")
    changed = valveSetPulse(stCutterR, PIN_OUT_CUTTER_R, state);
  else if (n == "CUTTER_L" || n == "CUTTERS_L")
    changed = valveSetPulse(stCutterL, PIN_OUT_CUTTER_L, state);
  else if (n == "GRIPPER" || n == "GRIPPERS")
    changed = valveSetPulse(stGrippers, PIN_OUT_GRIPPERS, state);
  else if (n == "HOLDER")
    changed = valveSetPulse(stHolder, PIN_OUT_HOLDER, state);
  else if (n == "FGTRAY" || n == "TRAY" || n == "ENCODER")
    changed = valveSetPulse(stEncoder, PIN_OUT_ENCODER, state);
  else if (n == "BLOWER")
    changed = blowerSet(state, BLOWER_DEFAULT_SEC * 1000UL);
  else if (n == "RESET") {
    valvePulsePin(PIN_OUT_RESET);
    stReset = false;
    changed = true;
  }
  else return false;
  if (changed) logValveLogical();
  return true;
}

// HOME / All Off: todas las válvulas OFF (holder incluido; lo activa rutina/manual).
static void plcGoHomePulse()
{
  valveSetPulse(stCutterR,  PIN_OUT_CUTTER_R, false);
  valveSetPulse(stCutterL,  PIN_OUT_CUTTER_L, false);
  valveSetPulse(stGrippers, PIN_OUT_GRIPPERS, false);
  valveSetPulse(stEncoder,  PIN_OUT_ENCODER,  false);
  blowerStop();
  valveSetPulse(stHolder,   PIN_OUT_HOLDER,   false);
  stReset = false;
  logValveLogical();
}

static bool parseOnOffVal(const String& val)
{
  return val == "1" || val == "true" || val == "on";
}

// ============================================================
// SECCION 04 — TCP TX
// ============================================================
static bool tcpLinkOk()
{
  return tcpClient && tcpClient.connected();
}

static bool tcpTx(const String& m)
{
  if (!tcpLinkOk()) return false;
  bool ok = tcpClient.print(m) > 0;
  if (!m.endsWith("\n"))
    ok = (tcpClient.print("\n") > 0) && ok;
  if (!ok)
  {
    tcpClient.stop();
    peerLinkOk = false;
    return false;
  }
  return true;
}

static bool plcHasSensorError()
{
  if (millis() < BOOT_GRACE_MS) return false;
  return currentStableMask() != 0;
}

static bool plcHasValveBusy()
{
  return stCutterR || stCutterL || stGrippers || stEncoder || stBlower || stReset;
}

static uint8_t plcTcpResolveStateByte()
{
  if (plcHasSensorError()) return PLC_TX_ERROR;
  if (plcTcpStopPending) {
    if (plcHasValveBusy()) return PLC_TX_STOP;
    return PLC_TX_RETURN;
  }
  if (plcHasValveBusy()) return PLC_TX_BUSY;
  return PLC_TX_IDLE;
}

static const char* plcTcpStateName(uint8_t byteCode)
{
  switch (byteCode) {
    case PLC_TX_INIT:   return "InitState";
    case PLC_TX_IDLE:   return "IdleState";
    case PLC_TX_BUSY:   return "BusyState";
    case PLC_TX_ERROR:  return "ErrorState";
    case PLC_TX_STOP:   return "StoprState";
    case PLC_TX_RETURN: return "ReturnState";
    default:            return "unknown";
  }
}

static void plcTcpTxState(uint8_t byteCode, const char* name)
{
  char buf[128];
  snprintf(buf, sizeof(buf),
           "{\"ver\":%u,\"type\":\"state\",\"actuator\":\"plc\",\"byte\":%u,\"name\":\"%s\"}",
           (unsigned)PLCA_PROTO_VER, (unsigned)byteCode, name);
  tcpTx(String(buf));
  plcTcpLastStateByte = byteCode;
}

static void plcTcpPushStateIfChanged()
{
  const uint8_t st = plcTcpResolveStateByte();
  if (st == plcTcpLastStateByte) return;
  plcTcpTxState(st, plcTcpStateName(st));
  if (st == PLC_TX_RETURN)
    plcTcpStopPending = false;
}

static void plcTcpPollStateEvents()
{
  if (!tcpLinkOk()) return;

  if (plcTcpNeedInit) {
    plcTcpTxState(PLC_TX_INIT, "InitState");
    plcTcpNeedInit = false;
  }

  plcTcpPushStateIfChanged();
}

static String statusJson(const char* type)
{
  const uint8_t mask = currentStableMask();
  String j;
  j.reserve(560);
  j += "{\"ver\":";
  j += PLCA_PROTO_VER;
  j += ",\"type\":\"";
  j += type;
  j += "\",\"role\":\"plca\",\"mask\":";
  j += mask;
  j += ",\"seq\":";
  j += sequence;
  j += ",\"sensorGripper\":";
  j += (mask & BIT_GRIPPER) ? "true" : "false";
  j += ",\"sensorHolder\":";
  j += (mask & BIT_HOLDER) ? "true" : "false";
  j += ",\"sensorCutter\":";
  j += (mask & BIT_CUTTER) ? "true" : "false";
  j += ",\"sensorHoseA\":";
  j += (mask & BIT_HOSE_A) ? "true" : "false";
  j += ",\"sensorHoseB\":";
  j += (mask & BIT_HOSE_B) ? "true" : "false";
  j += ",\"sensorTray\":";
  j += (mask & BIT_ENCODER) ? "true" : "false";
  j += ",\"cutterR\":";
  j += stCutterR ? "true" : "false";
  j += ",\"cutterL\":";
  j += stCutterL ? "true" : "false";
  j += ",\"cutters\":";
  j += stCutters ? "true" : "false";
  j += ",\"grippers\":";
  j += stGrippers ? "true" : "false";
  j += ",\"holder\":";
  j += stHolder ? "true" : "false";
  j += ",\"encoder\":";
  j += stEncoder ? "true" : "false";
  j += ",\"fgtray\":";
  j += stEncoder ? "true" : "false";
  j += ",\"blower\":";
  j += stBlower ? "true" : "false";
  j += ",\"reset\":";
  j += stReset ? "true" : "false";
  j += "}";
  return j;
}

static bool sendSnapshot(uint8_t bitmask)
{
  String j = "{\"type\":\"event\",\"field\":\"snapshot\",\"mask\":";
  j += String(bitmask);
  j += ",\"seq\":";
  j += String(sequence);
  j += "}";
  if (!tcpTx(j))
    return false;
  sequence++;
  lastSnapshotTxMs = millis();
  return true;
}

static void sendAlert(uint8_t code)
{
  String j = "{\"type\":\"event\",\"field\":\"alert\",\"code\":";
  j += String(code);
  j += "}";
  tcpTx(j);
}

static void plcTcpTxEvent(uint8_t byteCode, const char* name)
{
  char buf[160];
  snprintf(buf, sizeof(buf),
           "{\"ver\":%u,\"type\":\"event\",\"actuator\":\"plc\",\"byte\":%u,\"name\":\"%s\"}",
           (unsigned)PLCA_PROTO_VER, (unsigned)byteCode, name);
  tcpTx(String(buf));
}

static void plcTcpTxValve(uint8_t byteCode, const char* name, bool on)
{
  char buf[192];
  snprintf(buf, sizeof(buf),
           "{\"ver\":%u,\"type\":\"event\",\"actuator\":\"plc\",\"byte\":%u,"
           "\"name\":\"%s\",\"on\":%s}",
           (unsigned)PLCA_PROTO_VER, (unsigned)byteCode, name, on ? "true" : "false");
  tcpTx(String(buf));
}

static void sendAlertsForNewBits(uint8_t prev, uint8_t now)
{
  uint8_t risen = (uint8_t)(now & ~prev);
  if (!risen) return;
  if (risen & BIT_GRIPPER) { sendAlert(1); plcTcpTxEvent(PLC_TX_GRIPPER_ERR, "GripperE"); }  // falla gripper / aire
  if (risen & BIT_HOLDER)  { sendAlert(2); plcTcpTxEvent(PLC_TX_HOLDER_ERR, "HolderE"); }
  if (risen & BIT_CUTTER)  { sendAlert(3); plcTcpTxEvent(PLC_TX_CUTTER_ERR, "CutterE"); }
  if (risen & (BIT_HOSE_A | BIT_HOSE_B)) sendAlert(4);
  if (risen & BIT_ENCODER) { sendAlert(5); plcTcpTxEvent(PLC_TX_ENCODER_ERR, "EncoderE"); }  // aire / manguera / cilindro
  plcTcpPushStateIfChanged();
}

static void peerTxSensorBitEvents(uint8_t prev, uint8_t now)
{
  struct { uint8_t bit; const char* field; } map[] = {
    { BIT_GRIPPER, "sensorGripper" },
    { BIT_HOLDER,  "sensorHolder" },
    { BIT_CUTTER,  "sensorCutter" },
    { BIT_HOSE_A,  "sensorHoseA" },
    { BIT_HOSE_B,  "sensorHoseB" },
    { BIT_ENCODER, "sensorTray" },
  };
  for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
  {
    const bool was = (prev & map[i].bit) != 0;
    const bool nowOn = (now & map[i].bit) != 0;
    if (was == nowOn) continue;
    tcpTx(String("{\"type\":\"event\",\"field\":\"") + map[i].field
          + "\",\"value\":" + (nowOn ? "true" : "false") + "}");
  }
}

static void peerTxValveIfChanged(bool& last, bool now, uint8_t byteCode, const char* name)
{
  if (now == last) return;
  last = now;
  plcTcpTxValve(byteCode, name, now);
}

static void peerTxEvents()
{
  const uint8_t mask = currentStableMask();
  if (lastPeerMask != 0xFF && mask != lastPeerMask)
    peerTxSensorBitEvents(lastPeerMask, mask);
  if (mask != lastPeerMask)
  {
    lastPeerMask = mask;
    tcpTx(String("{\"type\":\"event\",\"field\":\"sensorMask\",\"value\":") + mask + "}");
  }
  peerTxValveIfChanged(lastPeerCutterR, stCutterR, PLC_CMD_CUTTER_R, "CutterR");
  peerTxValveIfChanged(lastPeerCutterL, stCutterL, PLC_CMD_CUTTER_L, "CutterL");
  if (stCutters != lastPeerCutters)
  {
    lastPeerCutters = stCutters;
    tcpTx(String("{\"type\":\"event\",\"field\":\"cutters\",\"value\":") + (stCutters ? "true" : "false") + "}");
  }
  peerTxValveIfChanged(lastPeerGrippers, stGrippers, PLC_CMD_GRIPPER, "Gripper");
  peerTxValveIfChanged(lastPeerHolder, stHolder, PLC_CMD_HOLDER, "Holder");
  peerTxValveIfChanged(lastPeerEncoder, stEncoder, PLC_CMD_ENCODER, "Encoder");
  peerTxValveIfChanged(lastPeerBlower, stBlower, PLC_CMD_BLOWER, "Blower");
  if (stReset != lastPeerReset)
  {
    lastPeerReset = stReset;
    tcpTx(String("{\"type\":\"event\",\"field\":\"reset\",\"value\":") + (stReset ? "true" : "false") + "}");
  }
}

static void peerPushNow(bool fullStatus = true)
{
  peerTxEvents();
  if (fullStatus)
    tcpTx(statusJson("status"));
  lastStatusPushMs = millis();
}

static void replyPollSnapshot()
{
  sendSnapshot(currentStableMask());
  tcpTx(statusJson("status"));
  lastStatusPushMs = millis();
#if PLCA_DEBUG
  Serial.print("POLL -> mask=0x");
  Serial.println(currentStableMask(), HEX);
#endif
}

static void blowerService()
{
  if (!blowerTimedActive) return;
  if ((millis() - blowerStartMs) < blowerHoldMs) return;
  blowerStop();
#if PLCA_DEBUG
  Serial.println("BLOWER OFF (timeout)");
#endif
  if (tcpLinkOk()) {
    peerTxEvents();
    tcpTx(statusJson("status"));
    plcTcpPushStateIfChanged();
    lastStatusPushMs = millis();
  }
}

// ============================================================
// SECCION 05 — TCP RX / WiFi
// ============================================================
static int jInt(const char* j, const char* k, int d)
{
  String n = String("\"") + k + "\":";
  int i = String(j).indexOf(n);
  return i < 0 ? d : String(j).substring(i + n.length()).toInt();
}

static unsigned long blowerParseDurationMs(const char* line)
{
  String s(line);
  auto readNum = [&s](const char* key) -> float {
    String n = String("\"") + key + "\":";
    int i = s.indexOf(n);
    if (i < 0) return -1.0f;
    return s.substring(i + n.length()).toFloat();
  };
  float sec = readNum("durationSec");
  if (sec < 0) sec = readNum("sec");
  if (sec > 0) return (unsigned long)(sec * 1000.0f + 0.5f);
  float ms = readNum("durationMs");
  if (ms > 0) return (unsigned long)(ms + 0.5f);
  return BLOWER_DEFAULT_SEC * 1000UL;
}

static String jStr(const char* j, const char* k)
{
  String n = String("\"") + k + "\":\"";
  String s(j);
  int i = s.indexOf(n);
  if (i < 0) return "";
  int a = i + n.length(), b = s.indexOf('"', a);
  return b < 0 ? "" : s.substring(a, b);
}

static bool jBool(const char* j, const char* k, bool defVal)
{
  String s(j);
  String n = String("\"") + k + "\":";
  int i = s.indexOf(n);
  if (i < 0) return defVal;
  String rest = s.substring(i + n.length());
  rest.trim();
  if (rest.startsWith("true") || rest.startsWith("1")) return true;
  if (rest.startsWith("false") || rest.startsWith("0")) return false;
  return defVal;
}

static String jsonEscape(const String& in)
{
  String out;
  out.reserve(in.length() + 8);
  for (unsigned i = 0; i < in.length(); i++)
  {
    const char c = in.charAt(i);
    if (c == '\\') out += "\\\\";
    else if (c == '"') out += "\\\"";
    else if (c == '\n') out += "\\n";
    else out += c;
  }
  return out;
}

static bool plcTcpIsCmdByte(uint8_t b)
{
  return (b >= PLC_CMD_CUTTER_R && b <= PLC_CMD_RESET) || b == PLC_CMD_BLOWER;
}

static bool plcTcpParseOn(const char* line, uint8_t cmdByte)
{
  if (cmdByte == PLC_CMD_RESET) return true;
  const String val = jStr(line, "value");
  if (val.length())
    return parseOnOffVal(val);
  return jBool(line, "on", jBool(line, "value", true));
}

static bool plcTcpDoByte(uint8_t cmdByte, const char* line)
{
  String err;
  bool ok = false;
  const bool wantOn = plcTcpParseOn(line, cmdByte);

  switch (cmdByte) {
    case PLC_CMD_CUTTER_R:
      valveSetPulse(stCutterR, PIN_OUT_CUTTER_R, wantOn);
      ok = true;
      break;
    case PLC_CMD_CUTTER_L:
      valveSetPulse(stCutterL, PIN_OUT_CUTTER_L, wantOn);
      ok = true;
      break;
    case PLC_CMD_GRIPPER:
      valveSetPulse(stGrippers, PIN_OUT_GRIPPERS, wantOn);
      ok = true;
      break;
    case PLC_CMD_HOLDER:
      valveSetPulse(stHolder, PIN_OUT_HOLDER, wantOn);
      ok = true;
      break;
    case PLC_CMD_ENCODER:
      valveSetPulse(stEncoder, PIN_OUT_ENCODER, wantOn);
      ok = true;
      break;
    case PLC_CMD_BLOWER:
      blowerSet(wantOn, wantOn ? blowerParseDurationMs(line) : 0);
      ok = true;
      break;
    case PLC_CMD_RESET:
      valvePulsePin(PIN_OUT_RESET);
      plcGoHomePulse();
      ok = true;
      break;
    default:
      if (cmdByte >= PLC_TX_CUTTER_ERR && cmdByte <= PLC_TX_ENCODER_ERR)
        err = "byte error es solo TX (0x1F-0x22)";
      else
        err = "byte/cmd PLC desconocido";
      break;
  }

  Serial.printf("[PLC-CMD] byte=0x%02X on=%d cutterR=%d cutterL=%d grip=%d hold=%d enc=%d blow=%d\n",
                (unsigned)cmdByte, (int)wantOn,
                (int)stCutterR, (int)stCutterL, (int)stGrippers,
                (int)stHolder, (int)stEncoder, (int)stBlower);

  if (ok) {
    plcTcpStopPending = false;
    peerTxEvents();
    plcTcpPushStateIfChanged();
  }

  char buf[256];
  snprintf(buf, sizeof(buf),
           "{\"ver\":%u,\"type\":\"ack\",\"actuator\":\"plc\",\"byte\":%u,"
           "\"ok\":%s,\"message\":\"%s\"}",
           (unsigned)PLCA_PROTO_VER, (unsigned)cmdByte,
           ok ? "true" : "false", jsonEscape(err).c_str());
  tcpTx(String(buf));
  return ok;
}

static uint8_t plcTcpResolveCmdByte(const char* line)
{
  const int b = jInt(line, "byte", -1);
  if (b > 0) return (uint8_t)b;

  const String cmd = jStr(line, "command");
  if (cmd == "CutterR" || cmd == "cutterR") return PLC_CMD_CUTTER_R;
  if (cmd == "CutterL" || cmd == "cutterL") return PLC_CMD_CUTTER_L;
  if (cmd == "Gripper" || cmd == "gripper") return PLC_CMD_GRIPPER;
  if (cmd == "Holder" || cmd == "holder") return PLC_CMD_HOLDER;
  if (cmd == "Encoder" || cmd == "encoder") return PLC_CMD_ENCODER;
  if (cmd == "ResetPLC" || cmd == "resetPLC" || cmd == "reset") return PLC_CMD_RESET;
  if (cmd == "Blower" || cmd == "blower") return PLC_CMD_BLOWER;
  return 0;
}

static void tcpOnLine(const char* line)
{
  if (!line || !line[0]) return;
  if (!strstr(line, "\"type\":\"command\"")) return;

  const int rawByte = jInt(line, "byte", -1);
  if (rawByte > 0) {
    const uint8_t cmdByte = (uint8_t)rawByte;
    if (plcTcpIsCmdByte(cmdByte)) {
      plcTcpDoByte(cmdByte, line);
      return;
    }
    if (cmdByte >= PLC_TX_CUTTER_ERR && cmdByte <= PLC_TX_ENCODER_ERR) {
      tcpTx(String("{\"ver\":") + PLCA_PROTO_VER
            + ",\"type\":\"ack\",\"actuator\":\"plc\",\"ok\":false,"
            "\"message\":\"byte error es solo TX (0x1F-0x22)\"}");
      return;
    }
    if (cmdByte >= PLC_TX_INIT && cmdByte <= PLC_TX_RETURN) {
      const uint8_t st = plcTcpResolveStateByte();
      plcTcpTxState(st, plcTcpStateName(st));
      return;
    }
    tcpTx(String("{\"ver\":") + PLCA_PROTO_VER
          + ",\"type\":\"ack\",\"actuator\":\"plc\",\"ok\":false,"
          "\"message\":\"byte/cmd PLC desconocido\"}");
    return;
  }

  const uint8_t cmdByte = plcTcpResolveCmdByte(line);
  if (cmdByte && plcTcpIsCmdByte(cmdByte)) {
    plcTcpDoByte(cmdByte, line);
    return;
  }

  const String cmd = jStr(line, "command");
  const String val = jStr(line, "value");
  const String out = jStr(line, "out");

  if (cmd == "setOut" || cmd == "setPlc" || cmd == "plcSet")
  {
    const String outName = out.length() ? out : val;
    const String stateRaw = out.length() ? val : String(jInt(line, "value", 0));
    const bool wantOn = parseOnOffVal(stateRaw);
    String n = outName;
    n.trim();
    n.toUpperCase();
#if PLCA_DEBUG
    Serial.printf("TCP setOut %s -> %s\n", outName.c_str(), wantOn ? "ON" : "OFF");
#endif
    bool changed = false;
    if (n == "BLOWER") {
      changed = blowerSet(wantOn, wantOn ? blowerParseDurationMs(line) : 0);
      if (changed) logValveLogical();
    } else if (!setPlcOutputByName(outName, wantOn)) {
#if PLCA_DEBUG
      Serial.printf("TCP setOut RECHAZADO: \"%s\"\n", outName.c_str());
#endif
      return;
    } else {
      changed = true;
    }
    if (!changed && n == "BLOWER") {
      // ya OFF: igual ack con status
    }
    peerTxEvents();
    tcpTx(statusJson("status"));
    plcTcpPushStateIfChanged();
    lastStatusPushMs = millis();
    return;
  }
  if (cmd == "allOff")
  {
    const bool alreadyHome = !stCutterR && !stCutterL && !stGrippers
                             && !stEncoder && !stBlower && !stReset && !stHolder;
    if (!alreadyHome) {
      plcGoHomePulse();
      peerTxEvents();
      plcTcpStopPending = true;
      plcTcpTxState(PLC_TX_STOP, "StoprState");
    }
    tcpTx(statusJson("status"));
    lastStatusPushMs = millis();
    return;
  }
  if (cmd == "poll")
  {
    // Solo bajo demanda / debug — no usar como heartbeat (regla C1).
    replyPollSnapshot();
    return;
  }
  if (cmd == "ping")
  {
    // Keepalive de enlace: ack mínimo, sin snapshot de sensores.
    tcpTx("{\"type\":\"pong\"}");
    return;
  }
}

static void tcpRxDrain()
{
  while (tcpClient.available())
  {
    char c = (char)tcpClient.read();
    if (c == '\n' || c == '\r')
    {
      if (tcpRxLen)
      {
        tcpRxLine[tcpRxLen] = 0;
        tcpOnLine(tcpRxLine);
        tcpRxLen = 0;
      }
    }
    else if (tcpRxLen < sizeof(tcpRxLine) - 1)
      tcpRxLine[tcpRxLen++] = c;
  }
}

static void tcpOnClientAccepted()
{
  tcpClient.setNoDelay(true);
  tcpRxLen = 0;
  lastPeerMask = 0xFF;
  lastPeerCutterR = !stCutterR;
  lastPeerCutterL = !stCutterL;
  lastPeerCutters = !stCutters;
  lastPeerGrippers = !stGrippers;
  lastPeerHolder = !stHolder;
  lastPeerEncoder = !stEncoder;
  lastPeerBlower = !stBlower;
  lastPeerReset = !stReset;
  plcTcpLastStateByte = 0;
  plcTcpNeedInit = true;
  plcTcpStopPending = false;
  peerLinkOk = true;
  tcpTx(String("{\"ver\":") + PLCA_PROTO_VER + ",\"type\":\"hello\",\"role\":\"plca\"}");
  peerPushNow(true);
  Serial.printf("[TCP] On desde %s\n", tcpClient.remoteIP().toString().c_str());
  tcpWasConnected = true;
}

static bool tcpAcceptIncoming()
{
  if (!tcpServicesUp || !tcpServer.hasClient()) return false;
  WiFiClient incoming = tcpServer.available();
  if (!incoming) return false;

  // Un solo maestro. Zombies WiFi a menudo quedan con connected()==false
  // (o true) y bloquean el pool LWIP → HMI “a veces conecta”.
  if (tcpClient)
    tcpClient.stop();
  tcpClient = incoming;
  tcpOnClientAccepted();
  return true;
}

static void tcpEnsureServices()
{
  if (tcpServicesUp || WiFi.status() != WL_CONNECTED) return;
  IPAddress ip = WiFi.localIP();
  if (ip == IPAddress(0, 0, 0, 0)) return;
  tcpServer.end();
  delay(20);
  tcpServer.begin();
  tcpServer.setNoDelay(true);
  tcpServicesUp = true;
  wifiReady = true;
  Serial.printf("[TCP] Servicios On %s:%u\n", ip.toString().c_str(), PLCA_TCP_PORT);
}

static void tcpStopServices()
{
  if (tcpClient.connected()) tcpClient.stop();
  peerLinkOk = false;
  tcpWasConnected = false;
  if (!tcpServicesUp) return;
  tcpServer.end();
  tcpServicesUp = false;
  wifiReady = false;
  Serial.println("[TCP] Servicios Off");
}

static void wifiLogStatus()
{
  wl_status_t s = WiFi.status();
  bool on = (s == WL_CONNECTED);
  if (on == staWasConnected) return;
  staWasConnected = on;
  if (on)
  {
    wifiConnectStartedMs = 0;
    Serial.printf("[WiFi] On %s\n", WiFi.localIP().toString().c_str());
    tcpEnsureServices();
  }
  else
  {
    tcpStopServices();
    Serial.println("[WiFi] Off");
  }
}

static bool wifiNeedsRetry()
{
  wl_status_t s = WiFi.status();
  if (s == WL_CONNECTED) return false;
  if (s == WL_NO_SSID_AVAIL || s == WL_CONNECT_FAILED || s == WL_CONNECTION_LOST) return true;
  return wifiConnectStartedMs && (millis() - wifiConnectStartedMs > 15000);
}

static void wifiRetry()
{
  tcpStopServices();
  wifiConnectStartedMs = millis();
  WiFi.disconnect(true);
  delay(500);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.config(STA_IP, STA_GW, STA_MASK, STA_GW);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.println("[WiFi] Reintentando STA...");
}

static void serviceTcp()
{
  wifiLogStatus();
  if (wifiNeedsRetry())
  {
    static unsigned long lastRetryMs = 0;
    if (millis() - lastRetryMs >= 8000)
    {
      lastRetryMs = millis();
      wifiRetry();
    }
  }
  tcpEnsureServices();

  if (tcpAcceptIncoming())
    return;

  if (!tcpLinkOk())
  {
    if (tcpWasConnected)
    {
      Serial.println("[TCP] Off");
      tcpWasConnected = false;
      peerLinkOk = false;
    }
    return;
  }

  tcpWasConnected = true;
  peerLinkOk = true;
  tcpRxDrain();
  plcTcpPollStateEvents();

  const unsigned long iv = STATUS_PUSH_MS;
  if (millis() - lastStatusPushMs >= iv)
  {
    peerTxEvents();
    tcpTx(statusJson("status"));
    plcTcpPushStateIfChanged();
    lastStatusPushMs = millis();
  }
}

// ============================================================
// SECCION 06 — Setup / Loop
// ============================================================
void setup()
{
  Serial.begin(115200);
  Serial.println();
  Serial.println("PLCA esclavo — sensores + válvulas — 10.10.32.50:8766");

  pinMode(PIN_CUTTER, INPUT_PULLUP);
  pinMode(PIN_GRIPPER, INPUT_PULLUP);
  pinMode(PIN_HOLDER, INPUT_PULLUP);
  pinMode(PIN_ENCODER, INPUT_PULLUP);

  pinMode(PIN_OUT_CUTTER_R, OUTPUT);
  pinMode(PIN_OUT_CUTTER_L, OUTPUT);
  pinMode(PIN_OUT_GRIPPERS, OUTPUT);
  pinMode(PIN_OUT_HOLDER, OUTPUT);
  pinMode(PIN_OUT_ENCODER, OUTPUT);
  pinMode(PIN_OUT_BLOWER, OUTPUT);
  pinMode(PIN_OUT_RESET, OUTPUT);
  valveIdleAll();
  // Reposo: todas OFF (sin pulso). Holder no se auto-activa; rutina/manual.
  stCutterR = stCutterL = stGrippers = stHolder = stEncoder = stBlower = stReset = false;
  syncCuttersFlag();
  Serial.printf("Valvulas: modo pulso %lums (on=pulso, off=pulso); boot=all OFF\n", VALVE_PULSE_MS);

  wifiConnectStartedMs = millis();
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  delay(100);
  if (!WiFi.config(STA_IP, STA_GW, STA_MASK, STA_GW))
    Serial.println("WiFi.config() fallo.");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Buscando router \"%s\"...\n", WIFI_SSID);
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - wifiStart < WIFI_CONNECT_TIMEOUT_MS))
  {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  wifiLogStatus();
  tcpEnsureServices();
  if (WiFi.status() != WL_CONNECTED)
    Serial.println("WiFi pendiente — reintentos en loop");
}

void loop()
{
  serviceTcp();
  blowerService();

  unsigned long now = millis();
  uint8_t raw = readSensorBitmask();
  uint8_t stable = lastBitmask;
  bool changed = updateDebouncedMask(raw, stable, now);

  if (!changed)
  {
    if (tcpLinkOk() && lastBitmask != 0xFF
        && (now - lastSnapshotTxMs) >= SNAPSHOT_HEARTBEAT_MS)
      sendSnapshot(currentStableMask());
    yield();
    return;
  }

  uint8_t previousMask = (lastBitmask == 0xFF) ? 0 : lastBitmask;
  const bool inBootGrace = (millis() < BOOT_GRACE_MS);
  logSensorMask(stable);
  if (tcpLinkOk())
  {
    sendSnapshot(stable);
    if (!inBootGrace)
      sendAlertsForNewBits(previousMask, stable);
    peerTxEvents();
    tcpTx(statusJson("status"));
    plcTcpPushStateIfChanged();
    lastStatusPushMs = millis();
  }
  else
    Serial.println("TCP offline — sensores/válvulas solo locales");

  lastBitmask = stable;
  serviceTcp();
}
