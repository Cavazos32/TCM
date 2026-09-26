// Andon — torreta + presión FRL.
// HMI manda ANDON_RX_* (0x40–0x49). Presión: pin local → torreta Error + TX 0x50.

#include "Config.h"
#include "AndonStates.h"
#include <WiFi.h>

static WiFiServer tcpServer(ANDON_TCP_PORT);
static WiFiClient tcpClient;
static char tcpRxLine[384];
static uint16_t tcpRxLen = 0;
static bool tcpServicesUp = false;
static bool tcpWasConnected = false;

// WiFi — reconexión no bloqueante (mismo patrón que Motion).
static bool wifiStaWasConnected = false;
static uint32_t wifiConnectStartedMs = 0;
static uint32_t wifiLastKickMs = 0;

static uint8_t lastMachineByte = ANDON_RX_IDLE;
static bool pressureFaultLatched = false;
static bool buzzerMuted = false;
static bool manualOverride = false;
static bool stGreen = false;
static bool stYellow = false;
static bool stRed = false;
static bool stBuzzerWant = false;

// Fin de WO (0x47): secuencia temporal R→Y→G + buzzer (no bloqueante).
static bool finishSeqActive = false;
static uint8_t finishSeqPhase = 0;   // 0=R, 1=Y, 2=G por ciclo
static uint8_t finishSeqCycle = 0;
static uint32_t finishSeqPhaseMs = 0;

static bool tcpLinkOk();
static bool tcpTx(const String& m);
static void andonTxStatus();
static void andonFinishSeqStop(bool settleIdle);
static void andonFinishSeqStart();
static void andonApplyStaticTower(uint8_t byteCode);

static void andonWriteOut(uint8_t pin, bool on)
{
#if ANDON_ACTIVE_HIGH
  digitalWrite(pin, on ? HIGH : LOW);
#else
  digitalWrite(pin, on ? LOW : HIGH);
#endif
}

void andonSetGreen(bool on)
{
  stGreen = on;
  andonWriteOut(PIN_GREEN_LED, on);
}
void andonSetYellow(bool on)
{
  stYellow = on;
  andonWriteOut(PIN_YELLOW_LED, on);
}
void andonSetRed(bool on)
{
  stRed = on;
  andonWriteOut(PIN_RED_LED, on);
}
void andonSetBuzzer(bool on)
{
  stBuzzerWant = on;
  if (buzzerMuted) on = false;
  andonWriteOut(PIN_BUZZER, on);
}

void andonTowerAllOff()
{
  andonSetGreen(false);
  andonSetYellow(false);
  andonSetRed(false);
  andonSetBuzzer(false);
}

static void andonTxStatus()
{
  if (!tcpLinkOk()) return;
  char buf[280];
  snprintf(buf, sizeof(buf),
           "{\"ver\":%u,\"type\":\"status\",\"actuator\":\"andon\","
           "\"green\":%s,\"yellow\":%s,\"red\":%s,\"buzzer\":%s,"
           "\"manual\":%s,\"mute\":%s,\"byte\":%u,\"pressure\":%s}",
           (unsigned)ANDON_PROTO_VER,
           stGreen ? "true" : "false",
           stYellow ? "true" : "false",
           stRed ? "true" : "false",
           (stBuzzerWant && !buzzerMuted) ? "true" : "false",
           manualOverride ? "true" : "false",
           buzzerMuted ? "true" : "false",
           (unsigned)lastMachineByte,
           pressureFaultLatched ? "true" : "false");
  tcpTx(String(buf));
}

static bool andonIsNaByte(uint8_t byteCode)
{
  // Start / Reset → N/A (no cambian torreta)
  return byteCode == ANDON_RX_START || byteCode == ANDON_RX_RESET;
}

static void andonFinishSeqStop(bool settleIdle)
{
  finishSeqActive = false;
  finishSeqPhase = 0;
  finishSeqCycle = 0;
  finishSeqPhaseMs = 0;
  if (settleIdle) {
    lastMachineByte = ANDON_RX_IDLE;
    andonTowerAllOff();
    andonSetGreen(true);
    andonTxStatus();
  }
}

static void andonFinishSeqStart()
{
  finishSeqActive = true;
  finishSeqPhase = 0;
  finishSeqCycle = 0;
  finishSeqPhaseMs = millis();
  andonTowerAllOff();
  andonSetRed(true);
  andonSetBuzzer(true);
  andonTxStatus();
}

void andonServiceFinishSequence()
{
  if (!finishSeqActive || pressureFaultLatched || manualOverride) return;

  const uint32_t now = millis();
  if ((now - finishSeqPhaseMs) < ANDON_FINISH_STEP_MS) return;

  finishSeqPhaseMs = now;
  finishSeqPhase++;

  if (finishSeqPhase >= 3) {
    finishSeqPhase = 0;
    finishSeqCycle++;
    if (finishSeqCycle >= ANDON_FINISH_CYCLES) {
      andonFinishSeqStop(true);
      return;
    }
  }

  andonTowerAllOff();
  andonSetBuzzer(true);
  if (finishSeqPhase == 0) andonSetRed(true);
  else if (finishSeqPhase == 1) andonSetYellow(true);
  else andonSetGreen(true);
  andonTxStatus();
}

static void andonApplyStaticTower(uint8_t byteCode)
{
  andonTowerAllOff();
  switch (byteCode) {
    case ANDON_RX_INIT:
    case ANDON_RX_IDLE:
    case ANDON_RX_BUSY:
      andonSetGreen(true);
      break;
    case ANDON_RX_STOP:
      andonSetRed(true);
      break;
    case ANDON_RX_ERROR:
      andonSetRed(true);
      andonSetBuzzer(true);
      break;
    case ANDON_RX_PAUSE:
      andonSetYellow(true);
      break;
    case ANDON_RX_MATERIALIST:
      andonSetYellow(true);
      andonSetBuzzer(true);
      break;
    default:
      break;
  }
}

void andonApplyMachineByte(uint8_t byteCode)
{
  if (!andonIsMachineByte(byteCode)) return;
  if (andonIsNaByte(byteCode)) return;
  // T1: presión reporta 0x50 a Main; no se bloquea el RX de estado/manual.

  manualOverride = false;
  andonFinishSeqStop(false);
  lastMachineByte = byteCode;

  if (byteCode == ANDON_RX_FINISH) {
    andonFinishSeqStart();
    return;
  }

  andonApplyStaticTower(byteCode);
  andonTxStatus();
}

void andonSetBuzzerMute(bool mute)
{
  buzzerMuted = mute;
  if (mute) {
    andonWriteOut(PIN_BUZZER, false);
  } else if (!pressureFaultLatched) {
    if (manualOverride) {
      andonSetBuzzer(stBuzzerWant);
    } else if (finishSeqActive) {
      andonSetBuzzer(true);
    } else {
      andonApplyStaticTower(lastMachineByte);
    }
  } else {
    andonSetBuzzer(true);
  }
  andonTxStatus();
}

static void andonManualSetOut(const String& outName, bool wantOn)
{
  andonFinishSeqStop(false);
  manualOverride = true;
  String n = outName;
  n.trim();
  n.toLowerCase();
  if (n == "green" || n == "verde") andonSetGreen(wantOn);
  else if (n == "yellow" || n == "amarillo") andonSetYellow(wantOn);
  else if (n == "red" || n == "rojo") andonSetRed(wantOn);
  else if (n == "buzzer" || n == "beep") andonSetBuzzer(wantOn);
  andonTxStatus();
}

static void andonManualAllOff()
{
  andonFinishSeqStop(false);
  manualOverride = true;
  andonTowerAllOff();
  andonTxStatus();
}

static void andonResumeAuto()
{
  manualOverride = false;
  andonApplyMachineByte(lastMachineByte);
}

void PressureError()
{
  if (!tcpLinkOk()) return;
  char buf[128];
  snprintf(buf, sizeof(buf),
           "{\"ver\":%u,\"type\":\"event\",\"actuator\":\"andon\","
           "\"byte\":%u,\"name\":\"PressureError\"}",
           (unsigned)ANDON_PROTO_VER, (unsigned)ANDON_ERR_PRESSURE);
  tcpTx(String(buf));
}

bool andonPressureFaultRaw()
{
  return digitalRead(PIN_PRESSURE_FRL) == LOW;
}

void andonServicePressure()
{
  static bool raw = false, stable = false;
  static uint32_t tChange = 0;
  const uint32_t now = millis();
  const bool r = andonPressureFaultRaw();

  if (r != raw) {
    raw = r;
    tChange = now;
  } else if ((now - tChange) >= ANDON_PRESSURE_DEBOUNCE_MS && r != stable) {
    stable = r;
    if (stable) {
      pressureFaultLatched = true;
      andonFinishSeqStop(false);
      andonTowerAllOff();
      andonSetRed(true);
      andonSetBuzzer(true);
      PressureError();
      andonTxStatus();
    } else {
      pressureFaultLatched = false;
      andonApplyMachineByte(lastMachineByte);
    }
  }
}

// --- TCP helpers ---
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
  if (!ok) {
    tcpClient.stop();
    return false;
  }
  return true;
}

static int jInt(const char* j, const char* k, int d)
{
  String n = String("\"") + k + "\":";
  int i = String(j).indexOf(n);
  return i < 0 ? d : String(j).substring(i + n.length()).toInt();
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

static bool jBool(const char* j, const char* k, bool d)
{
  String n = String("\"") + k + "\":";
  String s(j);
  int i = s.indexOf(n);
  if (i < 0) return d;
  String rest = s.substring(i + n.length());
  rest.trim();
  if (rest.startsWith("true") || rest.startsWith("1")) return true;
  if (rest.startsWith("false") || rest.startsWith("0")) return false;
  return d;
}

static void tcpAck(uint8_t byteCode, bool ok, const char* msg)
{
  char buf[192];
  snprintf(buf, sizeof(buf),
           "{\"ver\":%u,\"type\":\"ack\",\"actuator\":\"andon\",\"byte\":%u,"
           "\"ok\":%s,\"message\":\"%s\"}",
           (unsigned)ANDON_PROTO_VER, (unsigned)byteCode,
           ok ? "true" : "false", msg);
  tcpTx(String(buf));
}

static void tcpOnLine(const char* line)
{
  if (!line || !line[0]) return;
  if (!strstr(line, "\"type\":\"command\"")) return;

  const String cmd = jStr(line, "command");
  if (cmd == "ping") {
    tcpTx("{\"type\":\"pong\"}");
    return;
  }
  if (cmd == "buzzerMute") {
    const bool mute = jBool(line, "mute", jBool(line, "on", false));
    andonSetBuzzerMute(mute);
    tcpAck(0, true, mute ? "buzzer muted" : "buzzer on");
    return;
  }
  if (cmd == "setOut" || cmd == "setAndon") {
    const String outName = jStr(line, "out");
    const bool wantOn = jBool(line, "on", true);
    if (!outName.length()) {
      tcpAck(0, false, "out requerido");
      return;
    }
    andonManualSetOut(outName, wantOn);
    tcpAck(0, true, "setOut");
    return;
  }
  if (cmd == "allOff") {
    andonManualAllOff();
    tcpAck(0, true, "allOff");
    return;
  }
  if (cmd == "resumeAuto" || cmd == "auto") {
    andonResumeAuto();
    tcpAck(0, true, "resumeAuto");
    return;
  }

  const int rawByte = jInt(line, "byte", -1);
  if (rawByte < 0) {
    tcpAck(0, false, "byte/cmd Andon desconocido");
    return;
  }
  const uint8_t byteCode = (uint8_t)rawByte;
  if (!andonIsMachineByte(byteCode)) {
    tcpAck(byteCode, false, "byte fuera de 0x40-0x49");
    return;
  }
  andonApplyMachineByte(byteCode);
  tcpAck(byteCode, true, andonRxName(byteCode));
}

static void tcpRxDrain()
{
  while (tcpClient.available()) {
    char c = (char)tcpClient.read();
    if (c == '\n' || c == '\r') {
      if (tcpRxLen) {
        tcpRxLine[tcpRxLen] = 0;
        tcpOnLine(tcpRxLine);
        tcpRxLen = 0;
      }
    } else if (tcpRxLen < sizeof(tcpRxLine) - 1) {
      tcpRxLine[tcpRxLen++] = c;
    }
  }
}

static void tcpOnClientAccepted()
{
  tcpClient.setNoDelay(true);
  tcpRxLen = 0;
  tcpTx(String("{\"ver\":") + ANDON_PROTO_VER + ",\"type\":\"hello\",\"role\":\"andon\"}");
  andonTxStatus();
#if ANDON_DEBUG
  Serial.printf("[TCP] On desde %s\n", tcpClient.remoteIP().toString().c_str());
#endif
  tcpWasConnected = true;
}

static bool tcpAcceptIncoming()
{
  if (!tcpServicesUp || !tcpServer.hasClient()) return false;
  WiFiClient incoming = tcpServer.available();
  if (!incoming) return false;
  if (tcpClient)
    tcpClient.stop();
  tcpClient = incoming;
  tcpOnClientAccepted();
  return true;
}

static void tcpEnsureServices()
{
  if (tcpServicesUp || WiFi.status() != WL_CONNECTED) return;
  const IPAddress ip = WiFi.localIP();
  if (ip == IPAddress(0, 0, 0, 0)) return;
  tcpServer.end();
  delay(20);
  tcpServer.begin();
  tcpServer.setNoDelay(true);
  tcpServicesUp = true;
  Serial.printf("[TCP] Servicios On %s:%u\n",
                ip.toString().c_str(), (unsigned)ANDON_TCP_PORT);
}

static void tcpStopServices()
{
  // Invalidar siempre el socket (también zombies con connected()==false).
  if (tcpClient) tcpClient.stop();
  tcpWasConnected = false;
  tcpRxLen = 0;
  if (!tcpServicesUp) return;
  tcpServer.end();
  tcpServicesUp = false;
  Serial.println("[TCP] Off");
}

// =============================================================================
// WIFI — reconexión no bloqueante (millis / estados; sin delay en loop)
// Patrón: Motion wifiKickConnect / serviceWifi
// =============================================================================
static void wifiKickConnect(bool isRetry)
{
  if (isRetry) {
    // Sin delay: el loop debe seguir atendiendo torreta / presión / TCP teardown.
    WiFi.disconnect(false);
  }

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("andon");
  WiFi.setSleep(false);

  if (!WiFi.config(STA_IP, STA_GW, STA_MASK, STA_DNS)) {
    Serial.println("WiFi.config() fallo (IP estatica)");
  }

  const uint32_t now = millis();
  wifiConnectStartedMs = now;
  wifiLastKickMs = now;
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  if (isRetry)
    Serial.println("[WiFi] Reintentando STA...");
  else
    Serial.printf("Conectando a WiFi '%s' -> %s...\n",
                  WIFI_SSID, STA_IP.toString().c_str());
}

static void serviceWifi()
{
  const wl_status_t st = WiFi.status();

  if (st == WL_CONNECTED) {
    if (!wifiStaWasConnected) {
      wifiStaWasConnected = true;
      wifiConnectStartedMs = 0;
      const IPAddress ip = WiFi.localIP();
      Serial.printf("WiFi OK  IP=%s  RSSI=%d dBm\n",
                    ip.toString().c_str(), WiFi.RSSI());
      if (ip != STA_IP) {
        Serial.printf("[WiFi] AVISO: IP real %s != fija %s\n",
                      ip.toString().c_str(), STA_IP.toString().c_str());
      }
    }
    return;
  }

  // STA caído: invalidar TCP de inmediato; luego rearmar WiFi sin bloquear.
  if (wifiStaWasConnected) {
    wifiStaWasConnected = false;
    tcpStopServices();
    wifiConnectStartedMs = 0;
    wifiLastKickMs = 0;  // permitir reintento inmediato
    Serial.println("WiFi perdido — reconectando...");
  }

  const uint32_t now = millis();
  if (wifiLastKickMs != 0 && (now - wifiLastKickMs) < WIFI_RETRY_INTERVAL_MS)
    return;

  const bool attemptExpired =
      wifiConnectStartedMs != 0 &&
      (now - wifiConnectStartedMs) >= WIFI_CONNECT_TIMEOUT_MS;
  const bool hardFail =
      (st == WL_NO_SSID_AVAIL || st == WL_CONNECT_FAILED || st == WL_CONNECTION_LOST);
  const bool idle =
      (wifiConnectStartedMs == 0);

  if (idle || attemptExpired || hardFail)
    wifiKickConnect(/*isRetry=*/true);
}

static void serviceTcp()
{
  if (WiFi.status() != WL_CONNECTED) {
    tcpStopServices();
    return;
  }

  tcpEnsureServices();

  if (tcpAcceptIncoming())
    return;

  if (!tcpLinkOk()) {
    if (tcpWasConnected) {
      Serial.println("[TCP] Off");
      tcpWasConnected = false;
    }
    return;
  }

  tcpWasConnected = true;
  tcpRxDrain();
}

void setup()
{
  Serial.begin(115200);
  delay(500);

  pinMode(PIN_RED_LED, OUTPUT);
  pinMode(PIN_YELLOW_LED, OUTPUT);
  pinMode(PIN_GREEN_LED, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_PRESSURE_FRL, INPUT_PULLUP);

  pinMode(PIN_ETH_CS, OUTPUT);
  pinMode(PIN_ETH_CLK, OUTPUT);
  pinMode(PIN_ETH_RST, OUTPUT);
  pinMode(PIN_ETH_MOSI, OUTPUT);
  pinMode(PIN_ETH_INT, INPUT);

  andonTowerAllOff();
  andonApplyMachineByte(ANDON_RX_INIT);

  wifiKickConnect(/*isRetry=*/false);
  if (WiFi.status() != WL_CONNECTED)
    Serial.println("WiFi pendiente — reconexion no bloqueante en loop...");
  tcpEnsureServices();

  Serial.printf("Andon TCP en %s:%u\n",
                STA_IP.toString().c_str(), (unsigned)ANDON_TCP_PORT);
}

void loop()
{
  serviceWifi();
  serviceTcp();
  andonServiceFinishSequence();
  andonServicePressure();
}
