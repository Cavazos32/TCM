#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include "FeederCan.h"

String jsonEscape(const String& in);

// —— OM local (implementado en Motion.ino) ——
bool feedOmReadOfficialMm(float* officialOut, float* mmSignedOut = nullptr, float* mmAbsOut = nullptr);
bool feedOmReadLiveMm(float* mmSignedOut);
bool feedOmResetLocal();
float feedOmGetOffsetMm();
bool feedOmIsSettled();
float feedOmOfficialFromRaw(float mmAbs);

// —— CAN servos ——
extern bool canInitialized;
extern bool servoCanReady;

void servoCanInitMutex();
bool initCANBus(uint8_t maxRetries = CAN_INIT_MAX_RETRIES);
bool setupServoFeeder();
void serviceCANRx();
void canMoveRelativePP(int32_t stepsL, int32_t stepsR);
void canHalt();
bool sendCanHaltImmediate(bool sideR);
bool canReadSdoI32(uint8_t nodeId, uint16_t index, uint8_t subIndex, int32_t& valOut, uint32_t timeoutMs = CAN_SDO_TIMEOUT_MS);
bool canReadStatusWord(uint8_t nodeId, uint16_t& statusOut, uint32_t timeoutMs = CAN_STATUS_TIMEOUT_MS);

// —— Feed runtime ——
extern volatile float feedTargetMmL;
extern volatile float feedTargetMmR;
extern volatile float feedOffsetMm;
extern volatile float feedOffsetMmB;
extern volatile float feedCalCountsPerMmL;
extern volatile float feedCalCountsPerMmR;
extern volatile uint32_t feedSsFastPpL;
extern volatile uint32_t feedSsFastPpR;
extern volatile uint16_t feedDecRampMsL;
extern volatile uint16_t feedDecRampMsR;
extern volatile bool feedSkipEncoderConfirm;
extern FeedTestReq feedTestReq;
extern bool feedCalibrationTest;
extern char feedFaultReason[FEED_FAULT_REASON_MAX];
extern float feedOmLastOfficialMm;
extern float feedOmLastMmSigned;
extern float feedOmLastMmAbs;
extern float omPhase1Mm;
extern bool feedOmLengthMet;

void feedInit();
void feedLoadConfig();
void feedSaveConfig();
void feedLoop();
void feedRegisterHttpRoutes(WebServer& server);

bool feedPhaseIsActive();
bool feedThisCycleSucceeded();
bool runFeedCycle(bool skipEncoderConfirm, int8_t onlySide = -1);

float feedMaxMmS(bool sideR);
uint32_t feedMmSToPp(float mmS, bool sideR);
float feedPpToMmS(uint32_t pp, bool sideR);
FeedProfilePlan feedProfilePlan(float targetMm, float vReqMmS, bool sideR, uint16_t decRampMs);
bool feedProfileSideValid(float targetMm, float vReqMmS, bool sideR, uint16_t decRampMs);
void feedProfileAppendJson(const FeedProfilePlan& p, String& json, const char* key);
float clampFeedTargetMm(float mm);
uint16_t clampFeedRampMs(uint32_t ms);
float clampFeedOffsetMm(float mm);
float clampFeedCalCountsPerMm(float spm);
String feedStatusJson();

bool feedQueueTest(const FeedTestReq& req, String& err);
bool feedQueueTestSide(int8_t onlySide, String& err);
bool feedResetRuntime();

void motionTcpOnEncoderError();
void motionTcpOnFeedOk(bool sideR);
void motionTcpOnFeedNg(bool sideR);
