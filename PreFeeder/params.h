#pragma once

#include "config.h"
#include <Preferences.h>

struct Params {
  float dereelerRpm = 60.0f;
  float reverseSec = 2.0f;
  float feederRpm = 60.0f;
  float triggerFeedS = 2.0f;
  float tensionReverseRpm = 30.0f;
  float tensionCooldownS = 0.0f;
  float tensionFaultS = 10.0f;
  float bufferFillTimeoutS = 10.0f;
  uint16_t servoActiveUs = PF_SERVO_ACTIVE_US;
  uint32_t refillPulseMs = 1000;
  float midBoostRpm = 120.0f;
  float midBoostSec = 3.0f;
  int16_t midServoDeltaUs = (int16_t)(PF_SERVO_SPEED_SIGN * 100);
  float highBoostRpm = 180.0f;
  float highBoostSec = 5.0f;
  int16_t highServoDeltaUs = (int16_t)(PF_SERVO_SPEED_SIGN * 200);
  uint16_t lidarActivateCm = 50;
  float lidarFeedMaxSec = 3.0f;
  bool lidarHolguraEnabled = true;
};

void paramsLoad(Params& p);
void paramsSave(const Params& p);
void paramsValidate(Params& p);
bool paramsApplyFromQuery(const String& query, Params& p, bool saveIfTouched);
