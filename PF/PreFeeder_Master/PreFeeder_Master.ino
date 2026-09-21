// PreFeeder Master — orquesta L/R: lee estados, agrega errores, despacha comandos.
// Globales (control manual) → L+R | Volátiles (trigger, settings) → un lado (side=L|R).
// HMI TCM: TCP JSON+newline (opcodes en ../PfStates.h).
#include <WiFi.h>
#include <WebServer.h>
#include "../PF_LR/Status_Mode.h"
#include "../PfStates.h"
#include "Config.h"
#include "master_cmds.h"
#include "master_html.h"

static const char* WIFI_SSID = PF_MASTER_WIFI_SSID;
static const char* WIFI_PASS = PF_MASTER_WIFI_PASS;
static const IPAddress STA_IP = PF_MASTER_STA_IP;
static const IPAddress STA_GW = PF_MASTER_STA_GW;
static const IPAddress STA_MASK = PF_MASTER_STA_MASK;
static const IPAddress PF_L_IP(10, 10, 32, 101);
static const IPAddress PF_R_IP(10, 10, 32, 102);
static const uint16_t PF_PORT = 8765;
static const uint16_t PF_UI_PORT = 80;
static const uint32_t RECONNECT_MS = 2500;
static const uint32_t CONNECT_TIMEOUT_MS = 2000;
static const uint32_t HEARTBEAT_WARN_MS = 8000;
static const uint32_t STALE_LINK_MS = 30000;
static const uint32_t KEEPALIVE_PING_MS = 4000;
static const uint32_t WIFI_RETRY_MS = 8000;
static const uint32_t STATUS_LOG_MS = 10000;
static const uint8_t  HTTP_SERVICE_PASSES = 12;
static const uint8_t PROTO_VER = PF_MASTER_PROTO_VER;

WebServer server(80);
WiFiServer pfTcpServer(PF_MASTER_TCP_PORT);
WiFiClient pfTcpClient;
static char pfTcpRxLine[512];
static uint16_t pfTcpRxLen = 0;
static bool pfTcpServicesUp = false;
static bool pfTcpWasConnected = false;
static bool pfTcpNeedInit = true;
static bool pfTcpStopPending = false;
static uint8_t pfTcpLastStateByte = 0;
static bool pfTcpLastErrR[6] = {};
static bool pfTcpLastErrL[6] = {};
WiFiClient pfClientL;
WiFiClient pfClientR;

static char rxLineL[2048];
static char rxLineR[2048];
static uint16_t rxLenL = 0;
static uint16_t rxLenR = 0;
static uint32_t lastReconnectMsL = 0;
static uint32_t lastReconnectMsR = 0;
static uint32_t lastRxMsL = 0;
static uint32_t lastRxMsR = 0;
static uint32_t connectMsL = 0;
static uint32_t connectMsR = 0;
static uint32_t lastKeepaliveMsL = 0;
static uint32_t lastKeepaliveMsR = 0;
static uint32_t lastWifiRetryMs = 0;
static uint32_t lastStatusLogMs = 0;
static uint8_t failCountL = 0;
static uint8_t failCountR = 0;
static bool wifiReconnecting = false;
static uint32_t wifiReconnectStartedMs = 0;
static uint16_t cmdId = 1;

struct SideView {
  bool home = false;
  bool endstop = false;
  bool tension = false;
  bool cylinderOpen = false;
  bool hoseAbsent = false;
  bool holgura = false;
  bool autoEnabled = false;
  String autoState = "off";
  bool error = false;
  uint8_t errorCode = 0;
  String errorReason = "none";
  bool idleMode = false;
  bool inProcess = false;
  bool sensorsArmed = false;
  bool triggerActive = false;  // Motor2 Tfeed / helper holgura en curso
  String machineState = "idle";
  uint32_t lastMsAgo = 99999;
};

struct MasterErrorVar {
  bool active = false;
  uint8_t level = 0;
  uint8_t code = 0;
  String tag = "";      // EXXX oficial (E052…)
  String ui = "";        // "EXXX: Pre-Feeder, Descripción"
  char side = '-';
  String reason = "none";
  uint32_t seq = 0;
};

static MasterErrorVar pfError;
static SideView sideL;
static SideView sideR;
static bool machinePaused = false;
static uint32_t resetGraceUntilMs = 0;

static String pfUiUrl(const IPAddress& ip)
{
  return String("http://") + ip.toString() + ":" + PF_UI_PORT + "/";
}

static int jInt(const char* j, const char* k, int d)
{
  String n = String("\"") + k + "\":";
  String s(j);
  int i = s.indexOf(n);
  if (i < 0) return d;
  return s.substring(i + n.length()).toInt();
}

static bool jBool(const char* j, const char* k, bool d)
{
  String n = String("\"") + k + "\":";
  String s(j);
  int i = s.indexOf(n);
  if (i < 0) return d;
  String tail = s.substring(i + n.length());
  if (tail.startsWith("true")) return true;
  if (tail.startsWith("false")) return false;
  return d;
}

static String jStr(const char* j, const char* k)
{
  String n = String("\"") + k + "\":\"";
  String s(j);
  int i = s.indexOf(n);
  if (i < 0) return "";
  int a = i + n.length();
  int b = s.indexOf('"', a);
  return b < 0 ? "" : s.substring(a, b);
}

static char jSideChar(const char* j, char fallback)
{
  String s = jStr(j, "side");
  if (s.length()) return s.charAt(0);
  return fallback;
}

static void pfTx(WiFiClient& client, const String& m)
{
  if (!client.connected()) return;
  client.print(m);
  if (!m.endsWith("\n")) client.print("\n");
}

static void pfSendCmd(WiFiClient& client, const String& cmd, const String& val)
{
  pfTx(client, String("{\"ver\":") + PROTO_VER + ",\"type\":\"command\",\"command\":\"" + cmd
       + "\",\"value\":\"" + val + "\",\"id\":" + String(cmdId++) + "}");
}

static void pfBroadcastCmd(const String& cmd, const String& val)
{
  if (pfClientL.connected()) pfSendCmd(pfClientL, cmd, val);
  if (pfClientR.connected()) pfSendCmd(pfClientR, cmd, val);
}

static bool pfSendSideCmd(char side, const String& cmd, const String& val)
{
  WiFiClient& client = (side == 'R') ? pfClientR : pfClientL;
  if (!client.connected()) return false;
  pfSendCmd(client, cmd, val);
  return true;
}

// Devuelve false si el comando no pudo enviarse (sin enlace, side inválido, etc.).
static bool pfDispatchCmd(const String& cmd, const String& val, char sideArg)
{
  const PfCmdScope scope = pfMasterCmdScope(cmd);
  if (scope == PfCmdScope::Global)
  {
    if (!pfClientL.connected() && !pfClientR.connected()) return false;
    pfBroadcastCmd(cmd, val);
    return true;
  }
  if (scope == PfCmdScope::Side)
  {
    if (sideArg != 'L' && sideArg != 'R') return false;
    return pfSendSideCmd(sideArg, cmd, val);
  }
  return false;
}

static void applyMasterErrorVar(const MasterErrorVar& next)
{
  if (next.active == pfError.active && next.level == pfError.level
      && next.code == pfError.code && next.side == pfError.side
      && next.reason == pfError.reason)
    return;

  pfError = next;
  pfError.seq++;

  if (pfError.active && pfError.level >= 2)
  {
    machinePaused = true;
    Serial.printf("[MASTER] N%d %s code=%u side=%c %s\n",
                  (int)pfError.level, pfError.tag.c_str(), (unsigned)pfError.code,
                  pfError.side, pfError.reason.c_str());
  }
  else if (!pfError.active)
  {
    machinePaused = false;
    Serial.println("[MASTER] masterError OK");
  }
}

static void clearMasterFaultView()
{
  sideL.error = false;
  sideL.errorCode = 0;
  sideL.errorReason = "none";
  sideR.error = false;
  sideR.errorCode = 0;
  sideR.errorReason = "none";
  MasterErrorVar cleared;
  applyMasterErrorVar(cleared);
}

static void recomputeMasterError()
{
  if (millis() < resetGraceUntilMs)
    return;

  MasterErrorVar next;
  next.active = false;
  next.level = 0;
  next.code = 0;
  next.side = '-';
  next.reason = "none";
  next.tag = "";
  next.ui = "";

  if (sideL.error)
  {
    next.active = true;
    next.side = 'L';
    next.code = sideL.errorCode;
    next.reason = sideL.errorReason;
    next.tag = String(pfErrorExxxFromWireCode(next.code));
    char uiBuf[96];
    pfErrorFormatUi(uiBuf, sizeof(uiBuf), pfErrorIdFromWireCode(next.code), 'L');
    next.ui = String(uiBuf);
    if (!next.tag.length())
      next.tag = String(pfErrorTagFromWireCode(next.code));
    const PfErrorEntry* e = pfErrorFindById(pfErrorIdFromWireCode(next.code));
    next.level = e ? e->level : 2;
  }
  else if (sideR.error)
  {
    next.active = true;
    next.side = 'R';
    next.code = sideR.errorCode;
    next.reason = sideR.errorReason;
    next.tag = String(pfErrorExxxFromWireCode(next.code));
    char uiBuf[96];
    pfErrorFormatUi(uiBuf, sizeof(uiBuf), pfErrorIdFromWireCode(next.code), 'R');
    next.ui = String(uiBuf);
    if (!next.tag.length())
      next.tag = String(pfErrorTagFromWireCode(next.code));
    const PfErrorEntry* e = pfErrorFindById(pfErrorIdFromWireCode(next.code));
    next.level = e ? e->level : 2;
  }

  applyMasterErrorVar(next);
}

static void applySideError(SideView& s, bool active, uint8_t code, const String& reason)
{
  s.error = active;
  if (active)
  {
    s.errorCode = code;
    s.errorReason = reason.length() ? reason : "none";
  }
  else
  {
    s.errorCode = 0;
    s.errorReason = "none";
  }
}

static void parseSideSnapshot(const char* j, SideView& s)
{
  s.home = jBool(j, "home", s.home);
  s.endstop = jBool(j, "endstop", s.endstop);
  s.tension = jBool(j, "tension", s.tension);
  s.cylinderOpen = jBool(j, "cylinderOpen", s.cylinderOpen);
  s.hoseAbsent = jBool(j, "hoseAbsent", s.hoseAbsent);
  s.holgura = jBool(j, "holgura", s.holgura);
  s.autoEnabled = jBool(j, "autoEnabled", s.autoEnabled);
  String st = jStr(j, "autoState");
  if (st.length()) s.autoState = st;
  s.idleMode = jBool(j, "idleMode", s.idleMode);
  s.inProcess = jBool(j, "inProcess", s.inProcess);
  s.sensorsArmed = jBool(j, "sensorsArmed", s.sensorsArmed);
  s.triggerActive = jBool(j, "triggerActive", s.triggerActive);
  String ms = jStr(j, "machineState");
  if (ms.length()) s.machineState = ms;
  applySideError(s, jBool(j, "error", s.error),
                 (uint8_t)jInt(j, "errorCode", s.errorCode),
                 jStr(j, "errorReason").length() ? jStr(j, "errorReason") : s.errorReason);
}

static void parseSideEvent(const char* line, SideView& s)
{
  String field = jStr(line, "field");
  // error: solo desde status periódico (evita parpadeo PAUSE por eventos delta)
  if (field == "error") return;
  if (field == "home") s.home = jBool(line, "value", s.home);
  else if (field == "endstop") s.endstop = jBool(line, "value", s.endstop);
  else if (field == "tension") s.tension = jBool(line, "value", s.tension);
  else if (field == "cylinderOpen") s.cylinderOpen = jBool(line, "value", s.cylinderOpen);
  else if (field == "hoseAbsent") s.hoseAbsent = jBool(line, "value", s.hoseAbsent);
  else if (field == "holgura") s.holgura = jBool(line, "value", s.holgura);
  else if (field == "idleMode") s.idleMode = jBool(line, "value", s.idleMode);
  else if (field == "inProcess") s.inProcess = jBool(line, "value", s.inProcess);
  else if (field == "sensorsArmed") s.sensorsArmed = jBool(line, "value", s.sensorsArmed);
  else if (field == "triggerActive") s.triggerActive = jBool(line, "value", s.triggerActive);
  else if (field == "machineState")
  {
    String ms = jStr(line, "value");
    if (ms.length()) s.machineState = ms;
  }
  else if (field == "auto")
  {
    s.autoEnabled = jBool(line, "autoEnabled", s.autoEnabled);
    String st = jStr(line, "autoState");
    if (st.length()) s.autoState = st;
  }
}

static void pfParseLine(const char* line, char defaultSide)
{
  const char sideCh = jSideChar(line, defaultSide);
  SideView& s = (sideCh == 'R') ? sideR : sideL;
  uint32_t& lastRx = (sideCh == 'R') ? lastRxMsR : lastRxMsL;
  lastRx = millis();
  s.lastMsAgo = 0;

  if (strstr(line, "\"type\":\"status\""))
  {
    parseSideSnapshot(line, s);
    recomputeMasterError();
    return;
  }

  if (strstr(line, "\"type\":\"event\""))
  {
    if (strstr(line, "\"field\":\"masterError\""))
    {
      MasterErrorVar next;
      next.active = jBool(line, "active", jBool(line, "value", false));
      next.level = (uint8_t)jInt(line, "level", next.active ? 2 : 0);
      next.code = (uint8_t)jInt(line, "code", jInt(line, "errorCode", 0));
      next.side = jSideChar(line, '-');
      next.reason = jStr(line, "reason");
      if (!next.reason.length()) next.reason = jStr(line, "errorReason");
      if (!next.reason.length()) next.reason = "none";
      String tag = jStr(line, "tag");
      if (!tag.length()) tag = jStr(line, "errorTag");
      next.tag = tag.length() ? tag : String(pfErrorTagFromWireCode(next.code));
      applyMasterErrorVar(next);
      return;
    }
    parseSideEvent(line, s);
  }
}

static void pfOnLine(const char* line, char defaultSide)
{
  if (strstr(line, "\"type\":\"hello\""))
  {
    Serial.printf("[MASTER] hello %c\n", defaultSide);
    return;
  }
  pfParseLine(line, defaultSide);
}

static const uint16_t RX_LINE_MAX = 2047;

static void pfRx(WiFiClient& client, char* rxLine, uint16_t& rxLen, char defaultSide)
{
  while (client.available())
  {
    char c = client.read();
    if (c == '\n' || c == '\r')
    {
      if (rxLen)
      {
        rxLine[rxLen] = 0;
        pfOnLine(rxLine, defaultSide);
        rxLen = 0;
      }
    }
    else if (rxLen < RX_LINE_MAX)
      rxLine[rxLen++] = c;
  }
}

static void pfTryConnect(WiFiClient& client, const IPAddress& ip, uint32_t& lastReconnectMs,
                         char* rxLine, uint16_t& rxLen, char sideTag,
                         uint32_t& connectMs, uint32_t& lastRxMs, uint8_t& failCount)
{
  if (client.connected() || WiFi.status() != WL_CONNECTED) return;
  // Backoff: no martillar L si está inestable (2.5s, 5s, 7.5s… máx 12s).
  uint32_t waitMs = RECONNECT_MS;
  if (failCount > 1) {
    const uint32_t backed = RECONNECT_MS * (uint32_t)failCount;
    waitMs = (backed < 12000u) ? backed : 12000u;
  }
  if (millis() - lastReconnectMs < waitMs) return;
  lastReconnectMs = millis();

  client.stop();
  delay(20);
  client.setNoDelay(true);
  client.setTimeout(CONNECT_TIMEOUT_MS);
  if (client.connect(ip, PF_PORT, CONNECT_TIMEOUT_MS))
  {
    rxLen = 0;
    rxLine[0] = 0;
    connectMs = millis();
    lastRxMs = 0;
    failCount = 0;
    if (sideTag == 'L') lastKeepaliveMsL = millis();
    else lastKeepaliveMsR = millis();
    pfSendCmd(client, "ping", "");
    Serial.printf("[MASTER] TCP %c OK -> %s:%u\n", sideTag, ip.toString().c_str(), PF_PORT);
    return;
  }

  client.stop();
  failCount++;
  if (failCount == 1 || (failCount % 5) == 0)
  {
    Serial.printf("[MASTER] TCP %c fallo (%u) -> %s:%u (esclavo encendido? misma WiFi?)\n",
                  sideTag, (unsigned)failCount, ip.toString().c_str(), PF_PORT);
  }
}

static void pfWatchLink(WiFiClient& client, uint32_t& lastRxMs, uint32_t& connectMs,
                        uint32_t& lastReconnectMs, char sideTag)
{
  if (!client.connected())
  {
    connectMs = 0;
    return;
  }
  if (!connectMs) connectMs = millis();

  const uint32_t ago = lastRxMs ? (millis() - lastRxMs) : (millis() - connectMs);
  if (ago <= STALE_LINK_MS) return;

  Serial.printf("[MASTER] TCP %c sin datos %ums — reconectando\n", sideTag, (unsigned)ago);
  client.stop();
  connectMs = 0;
  lastRxMs = 0;
  // Esperar RECONNECT_MS (no inmediato): evita flapping connect/disconnect.
  lastReconnectMs = millis();
}

static void pfKeepalive(WiFiClient& client, uint32_t& lastKeepaliveMs)
{
  if (!client.connected()) return;
  if (millis() - lastKeepaliveMs < KEEPALIVE_PING_MS) return;
  lastKeepaliveMs = millis();
  pfSendCmd(client, "ping", "");
}

static void masterServiceHttp()
{
  for (uint8_t i = 0; i < HTTP_SERVICE_PASSES; i++)
  {
    server.handleClient();
    yield();
  }
}

static void masterServiceWiFi()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    wifiReconnecting = false;
    return;
  }

  if (!wifiReconnecting)
  {
    if (millis() - lastWifiRetryMs < WIFI_RETRY_MS) return;
    lastWifiRetryMs = millis();
    wifiReconnecting = true;
    wifiReconnectStartedMs = millis();
    Serial.println("[MASTER] WiFi perdido — reintentando (no bloquea HTTP)…");
    WiFi.disconnect(true);
    delay(50);
    WiFi.config(STA_IP, STA_GW, STA_MASK, STA_GW);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    return;
  }

  if (millis() - wifiReconnectStartedMs > 15000)
  {
    Serial.println("[MASTER] WiFi sigue OFF — otro intento en breve");
    wifiReconnecting = false;
  }
}

static void pfServiceSide(WiFiClient& client, const IPAddress& ip, uint32_t& lastReconnectMs,
                          char* rxLine, uint16_t& rxLen, char sideTag,
                          uint32_t& connectMs, uint32_t& lastRxMs, uint8_t& failCount,
                          uint32_t& lastKeepaliveMs)
{
  if (client.connected())
  {
    pfRx(client, rxLine, rxLen, sideTag);
    pfKeepalive(client, lastKeepaliveMs);
    pfWatchLink(client, lastRxMs, connectMs, lastReconnectMs, sideTag);
    return;
  }
  pfTryConnect(client, ip, lastReconnectMs, rxLine, rxLen, sideTag,
               connectMs, lastRxMs, failCount);
  masterServiceHttp();
}

static void masterLogStatus()
{
  if (millis() - lastStatusLogMs < STATUS_LOG_MS) return;
  lastStatusLogMs = millis();

  Serial.printf("[MASTER] WiFi=%s L:tcp=%d rx=%lums R:tcp=%d rx=%lums\n",
                WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "OFF",
                (int)pfClientL.connected(),
                pfClientL.connected() && lastRxMsL ? (long)(millis() - lastRxMsL) : -1L,
                (int)pfClientR.connected(),
                pfClientR.connected() && lastRxMsR ? (long)(millis() - lastRxMsR) : -1L);
}

static void jsonAppendStr(String& j, const String& s)
{
  j += '"';
  for (unsigned i = 0; i < s.length(); i++)
  {
    const char c = s.charAt(i);
    if (c == '"' || c == '\\') j += '\\';
    if (c != '\n' && c != '\r') j += c;
  }
  j += '"';
}

static void appendSideJson(String& j, const char* key, const SideView& s)
{
  j += ",\""; j += key; j += "\":{";
  j += "\"home\":"; j += s.home ? "true" : "false";
  j += ",\"endstop\":"; j += s.endstop ? "true" : "false";
  j += ",\"tension\":"; j += s.tension ? "true" : "false";
  j += ",\"cylinderOpen\":"; j += s.cylinderOpen ? "true" : "false";
  j += ",\"hoseAbsent\":"; j += s.hoseAbsent ? "true" : "false";
  j += ",\"holgura\":"; j += s.holgura ? "true" : "false";
  j += ",\"autoEnabled\":"; j += s.autoEnabled ? "true" : "false";
  j += ",\"autoState\":"; jsonAppendStr(j, s.autoState);
  j += ",\"idleMode\":"; j += s.idleMode ? "true" : "false";
  j += ",\"inProcess\":"; j += s.inProcess ? "true" : "false";
  j += ",\"sensorsArmed\":"; j += s.sensorsArmed ? "true" : "false";
  j += ",\"triggerActive\":"; j += s.triggerActive ? "true" : "false";
  j += ",\"machineState\":"; jsonAppendStr(j, s.machineState);
  j += ",\"error\":"; j += s.error ? "true" : "false";
  j += ",\"errorCode\":"; j += s.errorCode;
  j += ",\"errorReason\":"; jsonAppendStr(j, s.errorReason);
  j += "}";
}

static void appendLinkJson(String& j, const char* key, bool tcpOk, uint32_t lastRxMs, uint32_t connectMs)
{
  const uint32_t ago = lastRxMs ? (millis() - lastRxMs)
                                : (connectMs ? (millis() - connectMs) : 99999);
  const bool waiting = tcpOk && !lastRxMs;
  const bool warn = tcpOk && lastRxMs && (ago > HEARTBEAT_WARN_MS);
  const bool ok = tcpOk && lastRxMs && !warn;
  j += "\""; j += key; j += "\":{\"tcp\":";
  j += tcpOk ? "true" : "false";
  j += ",\"ok\":";
  j += ok ? "true" : "false";
  j += ",\"warn\":";
  j += warn ? "true" : "false";
  j += ",\"waiting\":";
  j += waiting ? "true" : "false";
  j += ",\"msAgo\":";
  j += (int)ago;
  j += "}";
}

// Misma semántica que PF_LR handleAuto() + peerDoCmd() (TCP :8765 a L y R).
static String pfPeerOnOffVal(const String& raw)
{
  return (raw == "1" || raw == "true" || raw == "on") ? String("1") : String("0");
}

static bool pfGlobalLinkUp()
{
  return pfClientL.connected() || pfClientR.connected();
}

// Start/Stop/Reset globales: misma semántica que HTML Master → ambos MCU.
static bool pfBothSidesLinked()
{
  return pfClientL.connected() && pfClientR.connected();
}

static String pfMissingSideErr(const char* op)
{
  const bool lOk = pfClientL.connected();
  const bool rOk = pfClientR.connected();
  if (!lOk && !rOk) return String(op) + " sin enlace L/R";
  if (!lOk) return String(op) + " sin enlace L";
  if (!rOk) return String(op) + " sin enlace R";
  return String("");
}

static bool pfBroadcastAutoFromQuery()
{
  if (!pfGlobalLinkUp())
    return false;

  // enable/reset requieren L+R (no dejar un lado desincronizado).
  if ((server.hasArg("enable") || server.hasArg("reset")) && !pfBothSidesLinked())
    return false;

  if (server.hasArg("all_cfg"))
    pfBroadcastCmd("setAllCfg", server.arg("all_cfg"));

  if (server.hasArg("reset"))
  {
    pfBroadcastCmd("reset", "");
    resetGraceUntilMs = millis() + 4000;
    clearMasterFaultView();
  }

  if (server.hasArg("enable"))
  {
    if (server.arg("enable") == "1")
      pfBroadcastCmd("start", "");   // peerDoCmd("start") ↔ /api/auto?enable=1
    else
      pfBroadcastCmd("stop", "");    // peerDoCmd("stop") ↔ enable=0
  }

  if (server.hasArg("idle_mode"))
    pfBroadcastCmd("setIdleMode", pfPeerOnOffVal(server.arg("idle_mode")));
  else if (server.hasArg("test_mode"))
    pfBroadcastCmd("setIdleMode", pfPeerOnOffVal(server.arg("test_mode")));

  if (server.hasArg("in_process"))
    pfBroadcastCmd("setInProcess", pfPeerOnOffVal(server.arg("in_process")));
  else if (server.hasArg("inProcess"))
    pfBroadcastCmd("setInProcess", pfPeerOnOffVal(server.arg("inProcess")));

  if (server.hasArg("refill_pulse_s"))
    pfBroadcastCmd("setRefillPulseS", server.arg("refill_pulse_s"));

  return true;
}

void handleApiAuto()
{
  if (!pfBroadcastAutoFromQuery())
  {
    const bool needBoth = server.hasArg("enable") || server.hasArg("reset");
    const String err = needBoth ? pfMissingSideErr("auto") : String("no_link");
    server.send(503, "application/json",
                String("{\"ok\":false,\"error\":\"") + err + "\"}");
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", "{\"ok\":true,\"scope\":\"global\"}");
}

void handleRoot()
{
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", master_html);
}

static void handleUiRedirect(const IPAddress& ip)
{
  const String url = pfUiUrl(ip);
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Location", url);
  server.send(302, "text/plain", "");
}

void handleUiL() { handleUiRedirect(PF_L_IP); }
void handleUiR() { handleUiRedirect(PF_R_IP); }

void handlePing()
{
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain", "pong");
}

void handleApiHealth()
{
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleApiStatus()
{
  const bool tcpL = pfClientL.connected();
  const bool tcpR = pfClientR.connected();
  const uint32_t agoL = lastRxMsL ? (millis() - lastRxMsL) : 99999;
  const uint32_t agoR = lastRxMsR ? (millis() - lastRxMsR) : 99999;
  const bool dataL = tcpL && lastRxMsL;
  const bool dataR = tcpR && lastRxMsR;
  const bool warnL = dataL && (agoL > HEARTBEAT_WARN_MS);
  const bool warnR = dataR && (agoR > HEARTBEAT_WARN_MS);
  sideL.lastMsAgo = agoL;
  sideR.lastMsAgo = agoR;

  String j;
  j.reserve(1400);
  j += "{\"wifiOk\":";
  j += (WiFi.status() == WL_CONNECTED) ? "true" : "false";
  j += ",\"wifiIp\":\"";
  j += WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
  j += "\",\"masterLinkL\":";
  j += (dataL && !warnL) ? "true" : "false";
  j += ",\"masterLinkR\":";
  j += (dataR && !warnR) ? "true" : "false";
  j += ",\"linkWarnL\":";
  j += warnL ? "true" : "false";
  j += ",\"linkWarnR\":";
  j += warnR ? "true" : "false";
  j += ",\"uiL\":\"";
  j += pfUiUrl(PF_L_IP);
  j += "\",\"uiR\":\"";
  j += pfUiUrl(PF_R_IP);
  j += "\",\"paused\":";
  j += machinePaused ? "true" : "false";
  j += ",\"errorAny\":";
  j += pfError.active ? "true" : "false";
  j += ",\"masterError\":{\"active\":";
  j += pfError.active ? "true" : "false";
  j += ",\"level\":"; j += pfError.level;
  j += ",\"code\":"; j += pfError.code;
  j += ",\"tag\":"; jsonAppendStr(j, pfError.tag);
  j += ",\"exxx\":"; jsonAppendStr(j, pfError.tag);
  j += ",\"ui\":"; jsonAppendStr(j, pfError.ui);
  j += ",\"side\":\""; j += String(pfError.side); j += "\"";
  j += ",\"reason\":"; jsonAppendStr(j, pfError.reason);
  j += ",\"seq\":"; j += pfError.seq;
  j += "}";
  j += ",\"comm\":{";
  appendLinkJson(j, "masterL", tcpL, lastRxMsL, connectMsL);
  j += ",";
  appendLinkJson(j, "masterR", tcpR, lastRxMsR, connectMsR);
  j += "}";
  appendSideJson(j, "l", sideL);
  appendSideJson(j, "r", sideR);
  j += "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j.c_str());
}

void handleApiCmd()
{
  if (!server.hasArg("command"))
  {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"no_command\"}");
    return;
  }

  const String cmd = server.arg("command");
  const PfCmdScope scope = pfMasterCmdScope(cmd);
  if (scope == PfCmdScope::Unknown)
  {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"unknown_command\"}");
    return;
  }

  const char sideArg = server.hasArg("side") ? pfMasterParseSideArg(server.arg("side")) : '-';
  if (scope == PfCmdScope::Side && sideArg != 'L' && sideArg != 'R')
  {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"side_required\"}");
    return;
  }

  const String val = server.hasArg("value") ? server.arg("value") : "";
  String peerCmd = cmd;
  String peerVal = val;

  if (scope == PfCmdScope::Global)
  {
    if (cmd == "setIdleMode" || cmd == "idleMode")
    {
      peerCmd = "setIdleMode";
      peerVal = pfPeerOnOffVal(val);
    }
    else if (cmd == "setInProcess" || cmd == "inProcess")
    {
      peerCmd = "setInProcess";
      peerVal = pfPeerOnOffVal(val);
    }
    else if (cmd == "start" || cmd == "stop" || cmd == "reset")
      peerVal = "";
  }

  if (!pfDispatchCmd(peerCmd, peerVal, sideArg))
  {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"no_link\"}");
    return;
  }

  if (cmd == "reset")
  {
    resetGraceUntilMs = millis() + 4000;
    clearMasterFaultView();
  }

  String j = "{\"ok\":true,\"scope\":\"";
  j += (scope == PfCmdScope::Global) ? "global" : "side";
  j += "\"";
  if (scope == PfCmdScope::Side)
  {
    j += ",\"side\":\"";
    j += sideArg;
    j += "\"";
  }
  j += "}";
  server.send(200, "application/json", j);
}

// =============================================================================
// TCP HMI (TCM) — servidor :8768, JSON + newline (mismo patrón Motion/PLCA)
// =============================================================================
static bool pfTcpLinkOk()
{
  return pfTcpClient && pfTcpClient.connected();
}

static bool pfTcpTx(const String& m)
{
  if (!pfTcpLinkOk()) return false;
  bool ok = pfTcpClient.print(m) > 0;
  if (!m.endsWith("\n"))
    ok = (pfTcpClient.print("\n") > 0) && ok;
  if (!ok)
    pfTcpClient.stop();
  return ok;
}

static String pfTcpJsonEscape(const String& in)
{
  String out;
  out.reserve(in.length() + 8);
  for (unsigned i = 0; i < in.length(); i++)
  {
    const char c = in.charAt(i);
    if (c == '\\') out += "\\\\";
    else if (c == '"') out += "\\\"";
    else if (c != '\n' && c != '\r') out += c;
  }
  return out;
}

static void pfTcpTxState(uint8_t byteCode, const char* name)
{
  char buf[128];
  snprintf(buf, sizeof(buf),
           "{\"ver\":%u,\"type\":\"state\",\"actuator\":\"prefeeder\",\"byte\":%u,\"name\":\"%s\"}",
           (unsigned)PF_MASTER_PROTO_VER, (unsigned)byteCode, name);
  pfTcpTx(String(buf));
  pfTcpLastStateByte = byteCode;
}

static void pfTcpTxEvent(uint8_t byteCode, const char* name, char side, bool active)
{
  char buf[192];
  snprintf(buf, sizeof(buf),
           "{\"ver\":%u,\"type\":\"event\",\"actuator\":\"prefeeder\",\"byte\":%u,"
           "\"name\":\"%s\",\"side\":\"%c\",\"active\":%s}",
           (unsigned)PF_MASTER_PROTO_VER, (unsigned)byteCode, name, side,
           active ? "true" : "false");
  pfTcpTx(String(buf));
}

static bool pfSideBusy(const SideView& s)
{
  return s.inProcess || s.autoEnabled || s.autoState == "cw"
      || s.autoState == "servo_lead" || s.autoState == "home_hold";
}

static uint8_t pfTcpResolveStateByte()
{
  if (pfError.active && pfError.level >= 2)
    return PF_ST_ERROR;
  if (pfTcpStopPending)
  {
    if (pfSideBusy(sideL) || pfSideBusy(sideR))
      return PF_ST_STOP;
    return PF_ST_RETURN;
  }
  if (machinePaused)
    return PF_ST_STOP;
  if (pfSideBusy(sideL) || pfSideBusy(sideR))
    return PF_ST_BUSY;
  if (sideL.autoEnabled || sideR.autoEnabled)
    return PF_ST_BUSY;
  return PF_ST_IDLE;
}

static void pfTcpPushStateIfChanged()
{
  const uint8_t st = pfTcpResolveStateByte();
  if (st == pfTcpLastStateByte) return;
  pfTcpTxState(st, pfTcpStateName(st));
  if (st == PF_ST_RETURN)
    pfTcpStopPending = false;
}

static bool pfSideErrFlag(const SideView& s, uint8_t idx)
{
  switch (idx) {
    // Buffer Full / Holgura: sensor ON = OK. EXXX solo con fallo enclavado
    // (timeout). No publicar s.home / s.holgura como active del opcode E052/E058/E057/E063.
    case 0: return s.error && pfErrorIdFromWireCode(s.errorCode) == PF_ERR_BUFFER;
    case 5: return s.error && pfErrorIdFromWireCode(s.errorCode) == PF_ERR_HOLGURA;
    // Resto: sensor activo = condición de fallo (misma polaridad que EXXX)
    case 1: return s.endstop || (s.error && pfErrorIdFromWireCode(s.errorCode) == PF_ERR_ENDSTOP);
    case 2: return s.tension || (s.error && pfErrorIdFromWireCode(s.errorCode) == PF_ERR_TENSION);
    case 3: return s.cylinderOpen || (s.error && pfErrorIdFromWireCode(s.errorCode) == PF_ERR_CYLINDER);
    case 4: return s.hoseAbsent || (s.error && pfErrorIdFromWireCode(s.errorCode) == PF_ERR_HOSE);
    default: return false;
  }
}

static void pfTcpPushSideErrors(char side, SideView& s, bool* lastFlags, bool force = false)
{
  for (uint8_t i = 0; i < 6; i++)
  {
    const bool now = pfSideErrFlag(s, i);
    if (!force && now == lastFlags[i]) continue;
    lastFlags[i] = now;
    const uint8_t errByte = (side == 'R')
                                ? (uint8_t)(PF_ERR_BUFFER_FULL_R + i)
                                : (uint8_t)(PF_ERR_BUFFER_FULL_L + i);
    pfTcpTxEvent(errByte, pfTcpErrorName(errByte), side, now);
  }
}

static void pfTcpPushErrorEvents(bool force = false)
{
  pfTcpPushSideErrors('L', sideL, pfTcpLastErrL, force);
  pfTcpPushSideErrors('R', sideR, pfTcpLastErrR, force);
}

static String pfTcpStatusJson()
{
  String j;
  j.reserve(900);
  j += "{\"ver\":";
  j += PF_MASTER_PROTO_VER;
  j += ",\"type\":\"status\",\"actuator\":\"prefeeder\",\"byte\":";
  j += (unsigned)pfTcpResolveStateByte();
  j += ",\"paused\":";
  j += machinePaused ? "true" : "false";
  j += ",\"errorAny\":";
  j += pfError.active ? "true" : "false";
  j += ",\"masterError\":{\"active\":";
  j += pfError.active ? "true" : "false";
  j += ",\"level\":";
  j += pfError.level;
  j += ",\"code\":";
  j += pfError.code;
  j += ",\"tag\":";
  jsonAppendStr(j, pfError.tag);
  j += ",\"side\":\"";
  j += String(pfError.side);
  j += "\",\"reason\":";
  jsonAppendStr(j, pfError.reason);
  j += "}";
  appendSideJson(j, "l", sideL);
  appendSideJson(j, "r", sideR);
  j += "}";
  return j;
}

static void pfTcpPushFullSnapshot()
{
  // Orden: eventos + status antes de state. Así la HMI aplica status.error
  // (fallo enclavado real) y puede descartar espejos viejos de sensor Buffer/Holgura
  // antes de intentar Set al ver PF_ST_ERROR.
  pfTcpPushErrorEvents(true);
  pfTcpTx(pfTcpStatusJson());
  const uint8_t st = pfTcpResolveStateByte();
  pfTcpTxState(st, pfTcpStateName(st));
}

// Valor ON/OFF del último command TCP HMI (Materialist / InProcess).
static String pfTcpCmdValue;

static uint8_t pfTcpResolveCmdByte(const char* line)
{
  const int b = jInt(line, "byte", -1);
  if (b > 0) return (uint8_t)b;

  const String cmd = jStr(line, "command");
  if (cmd == "Start" || cmd == "start") return PF_CMD_START;
  if (cmd == "Stop" || cmd == "stop") return PF_CMD_STOP;
  if (cmd == "ResetPF" || cmd == "reset" || cmd == "Reset") return PF_CMD_RESET;
  if (cmd == "Materialist" || cmd == "materialist" || cmd == "materialistaCall")
    return PF_CMD_MATERIALIST;
  if (cmd == "TriggerR" || cmd == "triggerR" || cmd == "TriggerFeedR")
    return PF_CMD_TRIGGER_R;
  if (cmd == "TriggerL" || cmd == "triggerL" || cmd == "TriggerFeedL")
    return PF_CMD_TRIGGER_L;
  // Legacy: Trigger sin lado → R (tabla histórica 0x4C)
  if (cmd == "Trigger" || cmd == "trigger" || cmd == "TriggerFeed")
    return PF_CMD_TRIGGER_R;
  return 0;
}

static void pfTcpTxAck(uint8_t cmdByte, bool ok, const String& err)
{
  char buf[256];
  snprintf(buf, sizeof(buf),
           "{\"ver\":%u,\"type\":\"ack\",\"actuator\":\"prefeeder\",\"byte\":%u,"
           "\"ok\":%s,\"message\":\"%s\"}",
           (unsigned)PF_MASTER_PROTO_VER, (unsigned)cmdByte,
           ok ? "true" : "false", pfTcpJsonEscape(err).c_str());
  pfTcpTx(String(buf));
  if (ok)
    pfTcpPushStateIfChanged();
}

// InProcess vía command string (misma ruta peer que /api/auto?in_process=).
// Sin opcode Excel dedicado: no inventar byte; HMI manda command+value.
static bool pfTcpDoInProcess()
{
  String err;
  bool ok = false;
  const String v = pfPeerOnOffVal(
      pfTcpCmdValue.length() ? pfTcpCmdValue : String("1"));
  if (!pfGlobalLinkUp()) {
    err = "InProcess sin enlace L/R";
  } else {
    ok = pfDispatchCmd("setInProcess", v, '-');
    if (!ok) err = "InProcess no enviado a L/R";
  }
  pfTcpTxAck(0, ok, err);
  return ok;
}

static bool pfTcpDoByte(uint8_t cmdByte)
{
  String err;
  bool ok = false;

  switch (cmdByte) {
    case PF_CMD_START:
      // Global L+R — misma ruta que Master /api/auto?enable=1 → peerDoCmd("start").
      if (!pfBothSidesLinked()) {
        err = pfMissingSideErr("Start");
        ok = false;
      } else {
        ok = pfDispatchCmd("start", "", '-');
        if (ok) pfTcpStopPending = false;
        else err = "Start no enviado a L/R";
      }
      break;
    case PF_CMD_STOP:
      if (!pfBothSidesLinked()) {
        err = pfMissingSideErr("Stop");
        ok = false;
      } else {
        ok = pfDispatchCmd("stop", "", '-');
        if (ok) {
          pfTcpStopPending = true;
          pfTcpTxState(PF_ST_STOP, "StoprState");
        } else err = "Stop no enviado a L/R";
      }
      break;
    case PF_CMD_RESET:
      if (!pfBothSidesLinked()) {
        err = pfMissingSideErr("Reset");
        ok = false;
      } else {
        ok = pfDispatchCmd("reset", "", '-');
        if (ok) {
          resetGraceUntilMs = millis() + 4000;
          clearMasterFaultView();
          pfTcpStopPending = false;
        } else err = "Reset no enviado a L/R";
      }
      break;
    case PF_CMD_MATERIALIST: {
      // value ausente → ON (compat HMI legacy). value 0/false/off → salir Materialista.
      const String v = pfPeerOnOffVal(
          pfTcpCmdValue.length() ? pfTcpCmdValue : String("1"));
      ok = pfDispatchCmd("materialistaCall", v, '-');
      if (!ok) err = "Materialist no enviado a L/R";
      break;
    }
    case PF_CMD_TRIGGER_R:
      ok = pfDispatchCmd("trigger", "", 'R');
      if (!ok) err = "TriggerR sin enlace R";
      break;
    case PF_CMD_TRIGGER_L:
      ok = pfDispatchCmd("trigger", "", 'L');
      if (!ok) err = "TriggerL sin enlace L";
      break;
    default:
      if (pfTcpIsErrorByte(cmdByte))
        err = "byte error es solo TX (0x2D-0x38)";
      else if (pfTcpIsStateByte(cmdByte)) {
        // Poll/heartbeat HMI: estado + status con L/R (Python actualiza indicadores).
        const uint8_t st = pfTcpResolveStateByte();
        pfTcpTxState(st, pfTcpStateName(st));
        pfTcpTx(pfTcpStatusJson());
        return true;
      }
      else
        err = "byte/cmd PreFeeder desconocido";
      break;
  }

  pfTcpTxAck(cmdByte, ok, err);
  return ok;
}

static void pfTcpOnLine(const char* line)
{
  if (!line || !line[0]) return;
  if (!strstr(line, "\"type\":\"command\"")) return;

  pfTcpCmdValue = jStr(line, "value");

  const String cmdName = jStr(line, "command");
  if (cmdName == "setInProcess" || cmdName == "inProcess" || cmdName == "InProcess") {
    pfTcpDoInProcess();
    return;
  }

  const uint8_t cmdByte = pfTcpResolveCmdByte(line);
  if (!cmdByte) {
    pfTcpTx(String("{\"ver\":") + PF_MASTER_PROTO_VER
            + ",\"type\":\"ack\",\"actuator\":\"prefeeder\",\"ok\":false,"
            "\"message\":\"Falta byte o command valido\"}");
    return;
  }

  if (pfTcpIsCmdByte(cmdByte) || pfTcpIsStateByte(cmdByte) || pfTcpIsErrorByte(cmdByte)) {
    pfTcpDoByte(cmdByte);
    return;
  }

  pfTcpTx(String("{\"ver\":") + PF_MASTER_PROTO_VER
          + ",\"type\":\"ack\",\"actuator\":\"prefeeder\",\"ok\":false,"
          "\"message\":\"byte/cmd PreFeeder desconocido\"}");
}

static void pfTcpRxDrain()
{
  while (pfTcpClient.available())
  {
    const char c = (char)pfTcpClient.read();
    if (c == '\n' || c == '\r')
    {
      if (pfTcpRxLen)
      {
        pfTcpRxLine[pfTcpRxLen] = 0;
        pfTcpOnLine(pfTcpRxLine);
        pfTcpRxLen = 0;
      }
    }
    else if (pfTcpRxLen < sizeof(pfTcpRxLine) - 1)
      pfTcpRxLine[pfTcpRxLen++] = c;
  }
}

static void pfTcpOnClientAccepted()
{
  pfTcpClient.setNoDelay(true);
  pfTcpRxLen = 0;
  pfTcpLastStateByte = 0;
  pfTcpNeedInit = true;
  pfTcpStopPending = false;
  for (uint8_t i = 0; i < 6; i++)
  {
    pfTcpLastErrL[i] = false;
    pfTcpLastErrR[i] = false;
  }
  // Hello inmediato (igual Motion/PLCA) para que la HMI verifique el enlace
  // antes del primer poll/probe — evita "Sin enlace PreFeeder".
  pfTcpTx(String("{\"ver\":") + PF_MASTER_PROTO_VER
          + ",\"type\":\"hello\",\"role\":\"prefeeder\"}");
  // Snapshot completo: la HMI solo veía deltas y los indicadores quedaban stale.
  pfTcpNeedInit = false;
  pfTcpPushFullSnapshot();
  Serial.printf("[MASTER] TCP HMI On desde %s\n",
                pfTcpClient.remoteIP().toString().c_str());
  pfTcpWasConnected = true;
}

static bool pfTcpAcceptIncoming()
{
  if (!pfTcpServicesUp || !pfTcpServer.hasClient()) return false;
  WiFiClient incoming = pfTcpServer.available();
  if (!incoming) return false;

  // Igual que Motion/PLC: sustituir siempre (zombies LWIP).
  if (pfTcpClient)
    pfTcpClient.stop();
  pfTcpClient = incoming;
  pfTcpOnClientAccepted();
  return true;
}

static void pfTcpEnsureServices()
{
  if (pfTcpServicesUp || WiFi.status() != WL_CONNECTED) return;
  const IPAddress ip = WiFi.localIP();
  if (ip == IPAddress(0, 0, 0, 0)) return;
  pfTcpServer.end();
  delay(20);
  pfTcpServer.begin();
  pfTcpServer.setNoDelay(true);
  pfTcpServicesUp = true;
  Serial.printf("[MASTER] TCP HMI On %s:%u\n",
                ip.toString().c_str(), (unsigned)PF_MASTER_TCP_PORT);
}

static void pfTcpStopServices()
{
  if (pfTcpClient.connected())
    pfTcpClient.stop();
  pfTcpWasConnected = false;
  if (!pfTcpServicesUp) return;
  pfTcpServer.end();
  pfTcpServicesUp = false;
  Serial.println("[MASTER] TCP HMI Off");
}

static void pfTcpPollStateEvents()
{
  if (!pfTcpLinkOk()) return;

  if (pfTcpNeedInit) {
    pfTcpTxState(PF_ST_INIT, "InitState");
    pfTcpNeedInit = false;
  }

  pfTcpPushErrorEvents();
  pfTcpPushStateIfChanged();
}

static void masterServiceTcpHmi()
{
  // Mismo ciclo de vida que Motion/PLCA: el listener solo vive con WiFi OK.
  // Arrancar :8768 antes de STA deja el puerto sordo y la HMI no conecta.
  if (WiFi.status() != WL_CONNECTED)
  {
    pfTcpStopServices();
    return;
  }

  pfTcpEnsureServices();

  if (pfTcpAcceptIncoming())
    return;

  if (!pfTcpLinkOk())
  {
    if (pfTcpWasConnected)
    {
      Serial.println("[MASTER] TCP HMI desconectado");
      pfTcpWasConnected = false;
    }
    return;
  }

  pfTcpWasConnected = true;
  pfTcpRxDrain();
  pfTcpPollStateEvents();
}

static void setupWiFiBegin()
{
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.config(STA_IP, STA_GW, STA_MASK, STA_GW);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("[MASTER] WiFi iniciado \"%s\" -> %s\n", WIFI_SSID, STA_IP.toString().c_str());
}

static void masterLogWifiOnce()
{
  static bool logged = false;
  if (logged || WiFi.status() != WL_CONNECTED) return;
  logged = true;
  Serial.printf("[MASTER] WiFi OK %s\n", WiFi.localIP().toString().c_str());
  if (WiFi.localIP() != STA_IP)
  {
    Serial.printf("[MASTER] AVISO: IP real %s != fija %s\n",
                  WiFi.localIP().toString().c_str(), STA_IP.toString().c_str());
  }
  Serial.printf("[MASTER] Esclavos L=%s R=%s puerto %u\n",
                PF_L_IP.toString().c_str(), PF_R_IP.toString().c_str(), PF_PORT);
}

void setup()
{
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("[MASTER] PreFeeder Master boot");
  setupWiFiBegin();
  server.on("/", handleRoot);
  server.on("/ui/l", handleUiL);
  server.on("/ui/r", handleUiR);
  server.on("/l", handleUiL);
  server.on("/r", handleUiR);
  server.on("/ping", handlePing);
  server.on("/api/health", handleApiHealth);
  server.on("/api/auto", handleApiAuto);
  server.on("/api/status", handleApiStatus);
  server.on("/api/cmd", handleApiCmd);
  server.begin();
  Serial.printf("[MASTER] HTTP :80 listo — http://%s/ping\n", STA_IP.toString().c_str());
  Serial.printf("[MASTER] TCP HMI :%u — arranca al tener WiFi (hello al aceptar)\n",
                (unsigned)PF_MASTER_TCP_PORT);
  Serial.printf("[MASTER] UI L → %s  |  UI R → %s\n",
                pfUiUrl(PF_L_IP).c_str(), pfUiUrl(PF_R_IP).c_str());
  Serial.printf("[MASTER] Atajos Master → /ui/l  /ui/r  (también /l  /r)\n");
  Serial.printf("[MASTER] heap libre %u\n", (unsigned)ESP.getFreeHeap());
  const uint32_t now = millis();
  lastReconnectMsL = now - RECONNECT_MS;
  lastReconnectMsR = now - RECONNECT_MS;
  lastWifiRetryMs = now;
}

void loop()
{
  masterServiceHttp();
  masterServiceWiFi();
  masterLogWifiOnce();
  masterServiceTcpHmi();

  pfServiceSide(pfClientL, PF_L_IP, lastReconnectMsL, rxLineL, rxLenL, 'L',
                connectMsL, lastRxMsL, failCountL, lastKeepaliveMsL);
  masterServiceHttp();

  pfServiceSide(pfClientR, PF_R_IP, lastReconnectMsR, rxLineR, rxLenR, 'R',
                connectMsR, lastRxMsR, failCountR, lastKeepaliveMsR);

  masterLogStatus();
  masterServiceHttp();
}
