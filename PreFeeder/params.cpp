#include "params.h"

static void clampF(float& v, float lo, float hi) {
  if (v < lo) v = lo;
  if (v > hi) v = hi;
}

void paramsValidate(Params& p) {
  clampF(p.dereelerRpm, RPM_MIN, RPM_MAX);
  clampF(p.reverseSec, 0.1f, 60.0f);
  clampF(p.feederRpm, RPM_MIN, RPM_MAX);
  clampF(p.triggerFeedS, 0.1f, 60.0f);
  clampF(p.tensionReverseRpm, RPM_MIN, RPM_MAX);
  clampF(p.tensionCooldownS, 0.0f, 60.0f);
  clampF(p.tensionFaultS, 1.0f, 60.0f);
  clampF(p.bufferFillTimeoutS, 1.0f, 120.0f);
  clampF(p.midBoostRpm, RPM_MIN, RPM_MAX);
  clampF(p.midBoostSec, 0.1f, 60.0f);
  clampF(p.highBoostRpm, RPM_MIN, RPM_MAX);
  clampF(p.highBoostSec, 0.1f, 60.0f);
  clampF(p.holguraOnFeedSec, 0.1f, 120.0f);
  clampF(p.holguraOffFaultSec, 0.5f, 300.0f);
  if (p.holguraSettleMs < HOLGURA_SETTLE_MS_MIN) p.holguraSettleMs = HOLGURA_SETTLE_MS_MIN;
  if (p.holguraSettleMs > HOLGURA_SETTLE_MS_MAX) p.holguraSettleMs = HOLGURA_SETTLE_MS_MAX;
  if (p.servoActiveUs < SERVO_MIN_US) p.servoActiveUs = SERVO_MIN_US;
  if (p.servoActiveUs > SERVO_MAX_US) p.servoActiveUs = SERVO_MAX_US;
  if (p.refillPulseMs < 200) p.refillPulseMs = 200;
  if (p.refillPulseMs > 10000) p.refillPulseMs = 10000;
  if (p.midServoDeltaUs < -1000) p.midServoDeltaUs = -1000;
  if (p.midServoDeltaUs > 1000) p.midServoDeltaUs = 1000;
  if (p.highServoDeltaUs < -1000) p.highServoDeltaUs = -1000;
  if (p.highServoDeltaUs > 1000) p.highServoDeltaUs = 1000;
}

void paramsLoad(Params& p) {
  Preferences prefs;
  if (!prefs.begin(PREFS_NS, true)) return;
  p.dereelerRpm = prefs.getFloat("dr_rpm", 60.0f);
  p.reverseSec = prefs.getFloat("auto_rev", 2.0f);
  p.feederRpm = prefs.getFloat("fd_rpm", 60.0f);
  p.triggerFeedS = prefs.getFloat("trig_s", 2.0f);
  p.tensionReverseRpm = prefs.getFloat("t_rev", 30.0f);
  p.tensionCooldownS = prefs.getFloat("t_cd", 0.0f);
  p.tensionFaultS = prefs.getFloat("t_fault", 10.0f);
  p.bufferFillTimeoutS = prefs.getFloat("buf_to", 10.0f);
  p.servoActiveUs = (uint16_t)prefs.getUInt("servo_us", PF_SERVO_ACTIVE_US);
  p.refillPulseMs = prefs.getUInt("refill_ms", 1000);
  p.midBoostRpm = prefs.getFloat("mid_rpm", 120.0f);
  p.midBoostSec = prefs.getFloat("mid_s", 3.0f);
  p.midServoDeltaUs = (int16_t)prefs.getInt("mid_dus", (int)(PF_SERVO_SPEED_SIGN * 100));
  p.highBoostRpm = prefs.getFloat("hi_rpm", 180.0f);
  p.highBoostSec = prefs.getFloat("hi_s", 5.0f);
  p.highServoDeltaUs = (int16_t)prefs.getInt("hi_dus", (int)(PF_SERVO_SPEED_SIGN * 200));
  p.holguraSettleMs = (uint16_t)prefs.getUInt("holg_set_ms", HOLGURA_SETTLE_MS_DEFAULT);
  p.holguraOnFeedSec = prefs.getFloat("holg_on_s", 2.0f);
  p.holguraOffFaultSec = prefs.getFloat("holg_off_s", 5.0f);
  p.hmiTfeedEnabled = prefs.getBool("tfeed_en", false);
  prefs.end();
  paramsValidate(p);
}

void paramsSave(const Params& p) {
  Params copy = p;
  paramsValidate(copy);
  Preferences prefs;
  if (!prefs.begin(PREFS_NS, false)) return;
  prefs.putFloat("dr_rpm", copy.dereelerRpm);
  prefs.putFloat("auto_rev", copy.reverseSec);
  prefs.putFloat("fd_rpm", copy.feederRpm);
  prefs.putFloat("trig_s", copy.triggerFeedS);
  prefs.putFloat("t_rev", copy.tensionReverseRpm);
  prefs.putFloat("t_cd", copy.tensionCooldownS);
  prefs.putFloat("t_fault", copy.tensionFaultS);
  prefs.putFloat("buf_to", copy.bufferFillTimeoutS);
  prefs.putUInt("servo_us", copy.servoActiveUs);
  prefs.putUInt("refill_ms", copy.refillPulseMs);
  prefs.putFloat("mid_rpm", copy.midBoostRpm);
  prefs.putFloat("mid_s", copy.midBoostSec);
  prefs.putInt("mid_dus", copy.midServoDeltaUs);
  prefs.putFloat("hi_rpm", copy.highBoostRpm);
  prefs.putFloat("hi_s", copy.highBoostSec);
  prefs.putInt("hi_dus", copy.highServoDeltaUs);
  prefs.putUInt("holg_set_ms", copy.holguraSettleMs);
  prefs.putFloat("holg_on_s", copy.holguraOnFeedSec);
  prefs.putFloat("holg_off_s", copy.holguraOffFaultSec);
  prefs.putBool("tfeed_en", copy.hmiTfeedEnabled);
  prefs.end();
}

static bool hasArg(const String& q, const char* key, String& val) {
  String needle = String(key) + "=";
  int i = q.indexOf(needle);
  if (i < 0) return false;
  int start = i + needle.length();
  int amp = q.indexOf('&', start);
  val = (amp < 0) ? q.substring(start) : q.substring(start, amp);
  return true;
}

bool paramsApplyFromQuery(const String& query, Params& p, bool saveIfTouched) {
  bool touched = false;
  String v;
  if (hasArg(query, "rpm", v)) {
    p.dereelerRpm = v.toFloat();
    touched = true;
  }
  if (hasArg(query, "reverse", v)) {
    p.reverseSec = v.toFloat();
    touched = true;
  }
  if (hasArg(query, "m2_rpm", v)) {
    p.feederRpm = v.toFloat();
    touched = true;
  } else if (hasArg(query, "feeder_rpm", v)) {
    p.feederRpm = v.toFloat();
    touched = true;
  }
  if (hasArg(query, "trigger_feed_s", v)) {
    p.triggerFeedS = v.toFloat();
    touched = true;
  }
  if (hasArg(query, "tension_reverse_rpm", v)) {
    p.tensionReverseRpm = v.toFloat();
    touched = true;
  }
  if (hasArg(query, "tension_cooldown", v)) {
    p.tensionCooldownS = v.toFloat();
    touched = true;
  }
  if (hasArg(query, "tension_fault_s", v)) {
    p.tensionFaultS = v.toFloat();
    touched = true;
  } else if (hasArg(query, "tension_fault", v)) {
    p.tensionFaultS = v.toFloat();
    touched = true;
  }
  if (hasArg(query, "buffer_refill_fault_s", v)) {
    p.bufferFillTimeoutS = v.toFloat();
    touched = true;
  }
  if (hasArg(query, "servo_pwm_us", v)) {
    p.servoActiveUs = (uint16_t)v.toInt();
    touched = true;
  } else if (hasArg(query, "servo_pwm", v)) {
    p.servoActiveUs = (uint16_t)v.toInt();
    touched = true;
  }
  if (hasArg(query, "mid_boost_rpm", v)) {
    p.midBoostRpm = v.toFloat();
    touched = true;
  }
  if (hasArg(query, "mid_boost_s", v)) {
    p.midBoostSec = v.toFloat();
    touched = true;
  }
  if (hasArg(query, "mid_servo_delta_us", v)) {
    p.midServoDeltaUs = (int16_t)v.toInt();
    touched = true;
  }
  if (hasArg(query, "high_boost_rpm", v)) {
    p.highBoostRpm = v.toFloat();
    touched = true;
  }
  if (hasArg(query, "high_boost_s", v)) {
    p.highBoostSec = v.toFloat();
    touched = true;
  }
  if (hasArg(query, "high_servo_delta_us", v)) {
    p.highServoDeltaUs = (int16_t)v.toInt();
    touched = true;
  }
  if (hasArg(query, "holgura_settle_ms", v)) {
    p.holguraSettleMs = (uint16_t)v.toInt();
    touched = true;
  }
  if (hasArg(query, "holgura_on_s", v)) {
    p.holguraOnFeedSec = v.toFloat();
    touched = true;
  }
  if (hasArg(query, "holgura_off_fault_s", v)) {
    p.holguraOffFaultSec = v.toFloat();
    touched = true;
  }
  if (hasArg(query, "hmi_tfeed_enabled", v)) {
    p.hmiTfeedEnabled = (v == "1" || v == "true" || v == "on");
    touched = true;
  }
  if (hasArg(query, "refill_pulse_s", v)) {
    p.refillPulseMs = (uint32_t)(v.toFloat() * 1000.0f);
    touched = true;
  }
  if (touched) {
    paramsValidate(p);
    if (saveIfTouched) paramsSave(p);
  }
  return touched;
}
