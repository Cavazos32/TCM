#include "Stage2.h"
#include "Servo_Feed.h"
#include <Preferences.h>
#include <math.h>

static Preferences stage2Prefs;

volatile float stage2ApproachPct = STAGE2_APPROACH_PCT_DEFAULT;
volatile float stage2FastRpm = STAGE2_FAST_RPM_DEFAULT;
volatile float stage2FineRpm = STAGE2_FINE_RPM_DEFAULT;

struct Stage2Rt {
  bool pending = false;
  bool active = false;
  uint32_t gen = 0;
  Stage2Phase phase = S2_IDLE;
  Stage2Result result = S2R_NONE;
  uint8_t errByte = 0;
  char fault[STAGE2_FAULT_REASON_MAX] = {0};

  float pieceMm = 0.0f;
  float targetMm = 0.0f;
  float asdaMm = 0.0f;

  // Solo diagnóstico (no control)
  float omL = 0.0f;
  float omR = 0.0f;
  float omAvg = 0.0f;

  uint32_t opStartMs = 0;
  uint32_t moveStartMs = 0;
  uint32_t settleUntilMs = 0;
  bool moveIssued = false;
  bool sawBusy = false;
};

static Stage2Rt s2;
static uint32_t stage2GenCounter = 1;
static WebServer* stage2Server = nullptr;

float clampStage2ApproachPct(float pct) {
  return clampFeedApproachPct(pct);
}

float clampStage2Rpm(float rpm) {
  if (rpm < (float)MOVE_RPM_MIN) return (float)MOVE_RPM_MIN;
  if (rpm > (float)MOVE_RPM_MAX) return (float)MOVE_RPM_MAX;
  return rpm;
}

static const char* stage2PhaseName(Stage2Phase p) {
  switch (p) {
    case S2_IDLE: return "IDLE";
    case S2_PREPARE: return "PREPARE";
    case S2_ZERO_ISSUE: return "ZERO_ISSUE";
    case S2_WAIT_ZERO: return "WAIT_ZERO";
    case S2_MOVE_ISSUE: return "MOVE_ISSUE";
    case S2_WAIT_REACHED: return "WAIT_REACHED";
    case S2_SETTLE: return "SETTLE";
    case S2_DONE_OK: return "DONE_OK";
    case S2_DONE_NG: return "DONE_NG";
    default: return "?";
  }
}

static void stage2SetFault(const char* msg, uint8_t errByte = 0) {
  s2.errByte = errByte;
  if (!msg) {
    s2.fault[0] = '\0';
    return;
  }
  strncpy(s2.fault, msg, STAGE2_FAULT_REASON_MAX - 1);
  s2.fault[STAGE2_FAULT_REASON_MAX - 1] = '\0';
}

static void stage2Finish(Stage2Result r, const char* fault, uint8_t errByte) {
  s2.result = r;
  stage2SetFault(fault, errByte);
  s2.active = false;
  s2.pending = false;
  s2.moveIssued = false;
  s2.sawBusy = false;
  s2.phase = (r == S2R_OK) ? S2_DONE_OK : S2_DONE_NG;
  Serial.printf("STAGE2 gen=%lu %s target=%.1f asda=%.2f omAvg=%.2f %s\n",
                (unsigned long)s2.gen, stage2PhaseName(s2.phase),
                (double)s2.targetMm, (double)s2.asdaMm, (double)s2.omAvg,
                s2.fault[0] ? s2.fault : "");
}

static void stage2BeginMoveWait(uint32_t now) {
  s2.moveIssued = true;
  // asdaStartMove ya marca ocupado; no exigir ver Busy en un poll posterior
  // (si el job termina entre loops, WAIT_REACHED se colgaba hasta timeout).
  s2.sawBusy = true;
  s2.moveStartMs = now;
}

static bool stage2MoveTimedOut(uint32_t now) {
  return s2.moveIssued && (now - s2.moveStartMs) > STAGE2_MOVE_TIMEOUT_MS;
}

static bool stage2PollMoveDone(bool* okOut) {
  if (!s2.moveIssued) {
    if (okOut) *okOut = false;
    return true;
  }
  if (stage2HostIsOccupied()) {
    s2.sawBusy = true;
    return false;
  }
  if (!s2.sawBusy)
    return false;
  s2.moveIssued = false;
  s2.sawBusy = false;
  if (okOut) *okOut = stage2HostLastJobOk();
  return true;
}

/** OM opcional para log — no afecta FSM ni movimiento. */
static void stage2SampleOmDiag() {
  float l = 0.0f, r = 0.0f;
  bool got = false;
  if (feedOmIsSettledSide(false) && feedOmReadOfficialMmSide(false, &l))
    got = true;
  else
    l = 0.0f;
  if (feedOmIsSettledSide(true) && feedOmReadOfficialMmSide(true, &r))
    got = true;
  else
    r = 0.0f;
  if (!got) {
    s2.omL = s2.omR = s2.omAvg = 0.0f;
    return;
  }
  if (l == 0.0f && r != 0.0f) l = r;
  if (r == 0.0f && l != 0.0f) r = l;
  s2.omL = l;
  s2.omR = r;
  s2.omAvg = 0.5f * (l + r);
}

static void stage2StartActive(uint32_t now) {
  s2.active = true;
  s2.pending = false;
  s2.result = S2R_NONE;
  s2.errByte = 0;
  s2.fault[0] = '\0';
  s2.asdaMm = 0.0f;
  s2.omL = s2.omR = s2.omAvg = 0.0f;
  s2.moveIssued = false;
  s2.sawBusy = false;
  s2.moveStartMs = 0;
  s2.opStartMs = now;
  if (s2.targetMm < 0.0f) s2.targetMm = 0.0f;
  s2.phase = S2_PREPARE;

  Serial.printf("STAGE2 START gen=%lu ABS target=%.1f rpm=%.0f (zero-then-target)\n",
                (unsigned long)s2.gen, (double)s2.targetMm,
                (double)clampStage2Rpm(stage2FastRpm));
}

static void stage2Service(uint32_t now) {
  if (s2.pending && !s2.active) {
    if (stage2HostIsOccupied()) {
      stage2Finish(S2R_NG, "ASDA ocupado", MOT_ERR_ACTUATOR_ABORTED);
      return;
    }
    stage2StartActive(now);
  }

  if (!s2.active)
    return;

  if ((now - s2.opStartMs) > STAGE2_GLOBAL_TIMEOUT_MS) {
    String stopErr;
    (void)stage2HostStop(stopErr);
    stage2Finish(S2R_NG, "STAGE2: timeout global", MOT_ERR_ACTUATOR_TARGET);
    return;
  }

  String err;

  switch (s2.phase) {
    case S2_PREPARE: {
      // Referencia conocida: si no está en 0, ir a 0 antes del ABS(target).
      float cur = 0.0f;
      if (!stage2HostCurrentMm(&cur)) {
        Serial.printf("STAGE2 PREPARE: sin lectura pos — FORCE ZERO\n");
        s2.phase = S2_ZERO_ISSUE;
        break;
      }
      s2.asdaMm = cur;
      if (fabsf(cur) <= STAGE2_AT_ZERO_MM) {
        Serial.printf("STAGE2 PREPARE: ya en 0 (%.2f mm) — ABS target\n",
                      (double)cur);
        s2.phase = S2_MOVE_ISSUE;
      } else {
        Serial.printf("STAGE2 PREPARE: pos=%.2f mm ≠ 0 — ZERO primero\n",
                      (double)cur);
        s2.phase = S2_ZERO_ISSUE;
      }
      break;
    }

    case S2_ZERO_ISSUE: {
      const float rpm = clampStage2Rpm(stage2FastRpm);
      Serial.printf("STAGE2 ZERO ABS=0 @ %.0f RPM\n", (double)rpm);
      if (!stage2HostStartAbsMm(0.0f, rpm, err)) {
        stage2Finish(S2R_NG,
                     err.length() ? err.c_str() : "STAGE2: ZERO fallo",
                     MOT_ERR_ASDA_MODBUS);
        break;
      }
      stage2BeginMoveWait(now);
      s2.phase = S2_WAIT_ZERO;
      break;
    }

    case S2_WAIT_ZERO: {
      if (stage2MoveTimedOut(now)) {
        String stopErr;
        (void)stage2HostStop(stopErr);
        stage2Finish(S2R_NG, "STAGE2: timeout ZERO", MOT_ERR_ACTUATOR_TARGET);
        break;
      }
      bool ok = false;
      if (!stage2PollMoveDone(&ok))
        break;
      if (!ok) {
        stage2Finish(S2R_NG, "STAGE2: ZERO no Reached", MOT_ERR_ACTUATOR_TARGET);
        break;
      }
      (void)stage2HostCurrentMm(&s2.asdaMm);
      if (fabsf(s2.asdaMm) > STAGE2_AT_ZERO_MM) {
        stage2Finish(S2R_NG, "STAGE2: ZERO no confirmado", MOT_ERR_ACTUATOR_TARGET);
        break;
      }
      Serial.printf("STAGE2 ZERO OK asda=%.2f — continuar ABS target\n",
                    (double)s2.asdaMm);
      s2.phase = S2_MOVE_ISSUE;
      break;
    }

    case S2_MOVE_ISSUE: {
      const float rpm = clampStage2Rpm(stage2FastRpm);
      if (s2.targetMm < 0.05f) {
        stage2Finish(S2R_NG, "STAGE2: target invalido", MOT_ERR_ACTUATOR_OUT_ZONE);
        break;
      }
      Serial.printf("STAGE2 MOVE ABS=%.2f @ %.0f RPM\n",
                    (double)s2.targetMm, (double)rpm);
      if (!stage2HostStartAbsMm(s2.targetMm, rpm, err)) {
        stage2Finish(S2R_NG,
                     err.length() ? err.c_str() : "STAGE2: MOVE fallo",
                     MOT_ERR_ASDA_MODBUS);
        break;
      }
      stage2BeginMoveWait(now);
      s2.phase = S2_WAIT_REACHED;
      break;
    }

    case S2_WAIT_REACHED: {
      if (stage2MoveTimedOut(now)) {
        String stopErr;
        (void)stage2HostStop(stopErr);
        stage2Finish(S2R_NG, "STAGE2: timeout movimiento", MOT_ERR_ACTUATOR_TARGET);
        break;
      }
      bool ok = false;
      if (!stage2PollMoveDone(&ok))
        break;
      if (!ok) {
        stage2Finish(S2R_NG, "STAGE2: no Reached", MOT_ERR_ACTUATOR_TARGET);
        break;
      }
      s2.settleUntilMs = now + STAGE2_SETTLE_MS;
      s2.phase = S2_SETTLE;
      break;
    }

    case S2_SETTLE:
      if ((int32_t)(now - s2.settleUntilMs) < 0)
        break;
      (void)stage2HostCurrentMm(&s2.asdaMm);
      stage2SampleOmDiag();
      Serial.printf("STAGE2 DONE target=%.2f asda=%.2f omAvg=%.2f (Reached OK)\n",
                    (double)s2.targetMm, (double)s2.asdaMm, (double)s2.omAvg);
      stage2Finish(S2R_OK, "", 0);
      break;

    default:
      break;
  }
}

void stage2LoadConfig() {
  stage2Prefs.begin(STAGE2_PREFS_NS, true);
  stage2ApproachPct = clampStage2ApproachPct(
      stage2Prefs.getFloat("s2ApPct", STAGE2_APPROACH_PCT_DEFAULT));
  stage2FastRpm = clampStage2Rpm(
      stage2Prefs.getFloat("s2FastRpm", STAGE2_FAST_RPM_DEFAULT));
  stage2FineRpm = clampStage2Rpm(
      stage2Prefs.getFloat("s2FineRpm", STAGE2_FINE_RPM_DEFAULT));
  stage2Prefs.end();
}

void stage2SaveConfig() {
  stage2Prefs.begin(STAGE2_PREFS_NS, false);
  stage2Prefs.putFloat("s2ApPct", clampStage2ApproachPct(stage2ApproachPct));
  stage2Prefs.putFloat("s2FastRpm", clampStage2Rpm(stage2FastRpm));
  stage2Prefs.putFloat("s2FineRpm", clampStage2Rpm(stage2FineRpm));
  stage2Prefs.end();
}

void stage2Init() {
  stage2LoadConfig();
  s2 = Stage2Rt{};
}

void stage2Loop() {
  stage2Service(millis());
}

bool stage2IsActive() {
  return s2.active && s2.phase != S2_IDLE && s2.phase != S2_DONE_OK &&
         s2.phase != S2_DONE_NG;
}

bool stage2QueueStart(float pieceLengthMm, String& err) {
  return stage2QueueStartSides(pieceLengthMm, true, true, err);
}

bool stage2QueueStartSides(float pieceLengthMm, bool /*useOmL*/, bool /*useOmR*/,
                           String& err) {
  if (stage2IsActive() || s2.pending) {
    err = "Stage2 ocupado";
    return false;
  }
  if (pieceLengthMm < (FEED_TARGET_FIXED_MM + 0.5f)) {
    err = "pieceMm insuficiente (necesita > 55)";
    return false;
  }
  if (stage2HostIsOccupied()) {
    err = "ASDA ocupado";
    return false;
  }
  if (stage2GenCounter == 0) stage2GenCounter = 1;
  s2.gen = stage2GenCounter++;
  s2.pieceMm = pieceLengthMm;
  s2.targetMm = pieceLengthMm - FEED_TARGET_FIXED_MM;
  if (s2.targetMm < 0.0f) s2.targetMm = 0.0f;
  s2.pending = true;
  s2.active = false;
  s2.result = S2R_NONE;
  s2.errByte = 0;
  s2.fault[0] = '\0';
  s2.phase = S2_IDLE;
  err = "";
  return true;
}

bool stage2QueueStartAbsSides(float absTargetMm, bool /*useOmL*/, bool /*useOmR*/,
                              String& err) {
  if (stage2IsActive() || s2.pending) {
    err = "Stage2 ocupado";
    return false;
  }
  if (absTargetMm < 0.5f) {
    err = "targetAbsMm insuficiente";
    return false;
  }
  if (stage2HostIsOccupied()) {
    err = "ASDA ocupado";
    return false;
  }
  if (stage2GenCounter == 0) stage2GenCounter = 1;
  s2.gen = stage2GenCounter++;
  s2.targetMm = absTargetMm;
  s2.pieceMm = absTargetMm + FEED_TARGET_FIXED_MM;
  s2.pending = true;
  s2.active = false;
  s2.result = S2R_NONE;
  s2.errByte = 0;
  s2.fault[0] = '\0';
  s2.phase = S2_IDLE;
  err = "";
  return true;
}

uint32_t stage2CurrentGen() {
  return s2.gen;
}

bool stage2ResetRuntime() {
  if (s2.active && stage2HostIsOccupied()) {
    String e;
    (void)stage2HostStop(e);
  }
  s2 = Stage2Rt{};
  return true;
}

void stage2AppendConfigJson(String& j) {
  j += ",\"stage2ApproachPct\":";
  j += String(clampStage2ApproachPct(stage2ApproachPct), 1);
  j += ",\"stage2FastRpm\":";
  j += String(clampStage2Rpm(stage2FastRpm), 0);
  j += ",\"stage2FineRpm\":";
  j += String(clampStage2Rpm(stage2FineRpm), 0);
  j += ",\"stage2ApproachPctMin\":";
  j += String(STAGE2_APPROACH_PCT_MIN, 0);
  j += ",\"stage2ApproachPctMax\":";
  j += String(STAGE2_APPROACH_PCT_MAX, 0);
  j += ",\"stage2GlobalTimeoutMs\":";
  j += String((unsigned long)STAGE2_GLOBAL_TIMEOUT_MS);
  j += ",\"stage2MoveTimeoutMs\":";
  j += String((unsigned long)STAGE2_MOVE_TIMEOUT_MS);
  j += ",\"stage2Control\":\"ASDA_SINGLE\"";
}

static bool jsonHasKeyLocal(const String& body, const char* key) {
  String pat = String("\"") + key + "\"";
  return body.indexOf(pat) >= 0;
}

static float jsonFloatLocal(const String& body, const char* key, float def) {
  String pat = String("\"") + key + "\"";
  int i = body.indexOf(pat);
  if (i < 0) return def;
  i = body.indexOf(':', i);
  if (i < 0) return def;
  return body.substring(i + 1).toFloat();
}

bool stage2ApplyConfigFromJson(const String& body, bool& changed) {
  bool local = false;
  if (jsonHasKeyLocal(body, "stage2ApproachPct")) {
    stage2ApproachPct = clampStage2ApproachPct(
        jsonFloatLocal(body, "stage2ApproachPct", stage2ApproachPct));
    local = true;
  }
  if (jsonHasKeyLocal(body, "stage2FastRpm")) {
    stage2FastRpm = clampStage2Rpm(
        jsonFloatLocal(body, "stage2FastRpm", stage2FastRpm));
    local = true;
  }
  if (jsonHasKeyLocal(body, "stage2FineRpm")) {
    stage2FineRpm = clampStage2Rpm(
        jsonFloatLocal(body, "stage2FineRpm", stage2FineRpm));
    local = true;
  }
  if (local) {
    stage2SaveConfig();
    changed = true;
  }
  return local;
}

String stage2StatusJson() {
  String j = "{";
  j += "\"ok\":true";
  j += ",\"gen\":";
  j += String((unsigned long)s2.gen);
  j += ",\"phase\":\"";
  j += stage2PhaseName(s2.phase);
  j += "\"";
  j += ",\"active\":";
  j += stage2IsActive() ? "true" : "false";
  j += ",\"pending\":";
  j += s2.pending ? "true" : "false";
  j += ",\"pieceMm\":";
  j += String(s2.pieceMm, 1);
  j += ",\"targetMm\":";
  j += String(s2.targetMm, 1);
  j += ",\"targetAbsMm\":";
  j += String(s2.targetMm, 1);
  j += ",\"approachMm\":0";
  j += ",\"asdaMm\":";
  j += String(s2.asdaMm, 2);
  j += ",\"omL\":";
  j += String(s2.omL, 2);
  j += ",\"omR\":";
  j += String(s2.omR, 2);
  j += ",\"omAvg\":";
  j += String(s2.omAvg, 2);
  j += ",\"averageMm\":";
  j += String(s2.omAvg, 2);
  j += ",\"remainingMm\":0";
  j += ",\"finalOmL\":";
  j += String(s2.omL, 2);
  j += ",\"finalOmR\":";
  j += String(s2.omR, 2);
  j += ",\"finalAverageMm\":";
  j += String(s2.omAvg, 2);
  j += ",\"result\":\"";
  j += (s2.result == S2R_OK) ? "OK" : (s2.result == S2R_NG) ? "NG" : "NONE";
  j += "\"";
  j += ",\"errByte\":";
  j += String((unsigned)s2.errByte);
  j += ",\"fault\":\"";
  j += jsonEscape(String(s2.fault));
  j += "\"";
  stage2AppendConfigJson(j);
  j += "}";
  return j;
}

static void s2SendJson(int code, const String& body) {
  if (!stage2Server) return;
  stage2Server->send(code, "application/json", body);
}

static void handleStage2Status() {
  s2SendJson(200, stage2StatusJson());
}

static void handleStage2Start() {
  if (!stage2Server) return;
  float pieceMm = 0.0f;
  float targetAbsMm = 0.0f;
  bool hasAbs = false;
  if (stage2Server->hasArg("pieceMm"))
    pieceMm = stage2Server->arg("pieceMm").toFloat();
  if (stage2Server->hasArg("targetAbsMm")) {
    targetAbsMm = stage2Server->arg("targetAbsMm").toFloat();
    hasAbs = true;
  }
  if (stage2Server->hasArg("plain")) {
    const String body = stage2Server->arg("plain");
    pieceMm = jsonFloatLocal(body, "pieceMm", pieceMm);
    if (jsonHasKeyLocal(body, "targetAbsMm")) {
      targetAbsMm = jsonFloatLocal(body, "targetAbsMm", targetAbsMm);
      hasAbs = true;
    }
  }
  String err;
  bool ok = hasAbs ? stage2QueueStartAbsSides(targetAbsMm, true, true, err)
                   : stage2QueueStartSides(pieceMm, true, true, err);
  if (!ok) {
    s2SendJson(409, String("{\"ok\":false,\"error\":\"") + jsonEscape(err) + "\"}");
    return;
  }
  String j = "{\"ok\":true,\"queued\":true,\"gen\":";
  j += String((unsigned long)stage2CurrentGen());
  if (hasAbs) {
    j += ",\"targetAbsMm\":";
    j += String(targetAbsMm, 1);
  } else {
    j += ",\"pieceMm\":";
    j += String(pieceMm, 1);
  }
  j += ",\"control\":\"ASDA_SINGLE\"";
  j += "}";
  s2SendJson(200, j);
}

static void handleStage2Reset() {
  stage2ResetRuntime();
  s2SendJson(200, stage2StatusJson());
}

static void handleStage2ConfigGet() {
  String j = "{\"ok\":true";
  stage2AppendConfigJson(j);
  j += ",\"moveRpmMin\":";
  j += String((unsigned)MOVE_RPM_MIN);
  j += ",\"moveRpmMax\":";
  j += String((unsigned)MOVE_RPM_MAX);
  j += "}";
  s2SendJson(200, j);
}

static void handleStage2ConfigPost() {
  if (!stage2Server) return;
  if (stage2IsActive()) {
    s2SendJson(409, "{\"ok\":false,\"error\":\"Stage2 activo\"}");
    return;
  }
  bool changed = false;
  if (stage2Server->hasArg("stage2ApproachPct")) {
    stage2ApproachPct =
        clampStage2ApproachPct(stage2Server->arg("stage2ApproachPct").toFloat());
    changed = true;
  }
  if (stage2Server->hasArg("stage2FastRpm")) {
    stage2FastRpm = clampStage2Rpm(stage2Server->arg("stage2FastRpm").toFloat());
    changed = true;
  }
  if (stage2Server->hasArg("stage2FineRpm")) {
    stage2FineRpm = clampStage2Rpm(stage2Server->arg("stage2FineRpm").toFloat());
    changed = true;
  }
  if (!changed && stage2Server->hasArg("plain")) {
    (void)stage2ApplyConfigFromJson(stage2Server->arg("plain"), changed);
  }
  if (!changed) {
    s2SendJson(400, "{\"ok\":false,\"error\":\"Nada que guardar\"}");
    return;
  }
  stage2SaveConfig();
  handleStage2ConfigGet();
}

void stage2RegisterHttpRoutes(WebServer& serverRef) {
  stage2Server = &serverRef;
  serverRef.on("/api/stage2/status", HTTP_GET, handleStage2Status);
  serverRef.on("/api/stage2/start", HTTP_POST, handleStage2Start);
  serverRef.on("/api/stage2/reset", HTTP_POST, handleStage2Reset);
  serverRef.on("/api/stage2/config", HTTP_GET, handleStage2ConfigGet);
  serverRef.on("/api/stage2/config", HTTP_POST, handleStage2ConfigPost);
}
