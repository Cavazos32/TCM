#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include "Asda.h"
#include "FeederCan.h"

// =============================================================================
// Stage 2 — un solo MOVE ABS al target (ASDA)
//
//   PREPARE → ABS(target) → WAIT REACHED → SETTLE → DONE_OK
//
// Sin Approach / Fine / OM / remaining / correcciones.
// OM en status = diagnóstico opcional al terminar.
// =============================================================================

#ifndef STAGE2_MOVE_RPM_DEFAULT
#define STAGE2_MOVE_RPM_DEFAULT      3000.0f
#endif
// Compat NVS/UI legada (ya no controlan la FSM)
#ifndef STAGE2_APPROACH_PCT_DEFAULT
#define STAGE2_APPROACH_PCT_DEFAULT  FEED_APPROACH_PCT_DEFAULT
#endif
#ifndef STAGE2_FAST_RPM_DEFAULT
#define STAGE2_FAST_RPM_DEFAULT      STAGE2_MOVE_RPM_DEFAULT
#endif
#ifndef STAGE2_FINE_RPM_DEFAULT
#define STAGE2_FINE_RPM_DEFAULT      800.0f
#endif

#define STAGE2_APPROACH_PCT_MIN  FEED_APPROACH_PCT_MIN
#define STAGE2_APPROACH_PCT_MAX  FEED_APPROACH_PCT_MAX

#ifndef STAGE2_GLOBAL_TIMEOUT_MS
#define STAGE2_GLOBAL_TIMEOUT_MS     180000u
#endif
#ifndef STAGE2_MOVE_TIMEOUT_MS
#define STAGE2_MOVE_TIMEOUT_MS       60000u
#endif
#ifndef STAGE2_SETTLE_MS
#define STAGE2_SETTLE_MS             FEED_OM_SETTLE_MS
#endif

#define STAGE2_FAULT_REASON_MAX  96
#define STAGE2_PREFS_NS          ASDA_PREFS_NS

enum Stage2Phase : uint8_t {
  S2_IDLE = 0,
  S2_PREPARE,
  S2_MOVE_ISSUE,
  S2_WAIT_REACHED,
  S2_SETTLE,
  S2_DONE_OK,
  S2_DONE_NG
};

enum Stage2Result : uint8_t {
  S2R_NONE = 0,
  S2R_OK,
  S2R_NG
};

// RPM del único MOVE (NVS key histórica s2FastRpm)
extern volatile float stage2ApproachPct;  // legado UI; no usa FSM
extern volatile float stage2FastRpm;
extern volatile float stage2FineRpm;      // legado UI; no usa FSM

void stage2Init();
void stage2LoadConfig();
void stage2SaveConfig();
void stage2Loop();
void stage2RegisterHttpRoutes(WebServer& server);

bool stage2IsActive();
bool stage2QueueStart(float pieceLengthMm, String& err);
bool stage2QueueStartSides(float pieceLengthMm, bool useOmL, bool useOmR, String& err);
bool stage2QueueStartAbsSides(float absTargetMm, bool useOmL, bool useOmR, String& err);
uint32_t stage2CurrentGen();
bool stage2ResetRuntime();

float clampStage2ApproachPct(float pct);
float clampStage2Rpm(float rpm);

String stage2StatusJson();
void stage2AppendConfigJson(String& j);
bool stage2ApplyConfigFromJson(const String& body, bool& changed);

bool stage2HostIsOccupied();
bool stage2HostStartAbsMm(float mmAbs, float rpm, String& err);
bool stage2HostCurrentMm(float* mmOut);
bool stage2HostStop(String& err);
bool stage2HostLastJobOk();
