#include "StepperRMT.h"

static inline uint32_t u32max(uint32_t a, uint32_t b){ return (a>b)?a:b; }

// ----- ctor ------------------------------------------------------
StepperRMT::StepperRMT(StepperDriverType driver, const Pins& pins)
: _driver(driver), _pins(pins)
{
  const bool dm = (_driver == DRIVER_DM556T);
  _step_active_high = !dm;
  _dir_active_high  = !dm;
  _en_active_high   = false; // EN active LOW
  _microstep_eff    = _microstep; 
}

// ----- begin -----------------------------------------------------
bool StepperRMT::begin() {
  pinMode(_pins.step, OUTPUT);
  pinMode(_pins.dir,  OUTPUT);
  if (_pins.en >= 0) pinMode(_pins.en, OUTPUT);
  if (_pins.ms1 >= 0) pinMode(_pins.ms1, OUTPUT);
  if (_pins.ms2 >= 0) pinMode(_pins.ms2, OUTPUT);
  if (_pins.ms3 >= 0) pinMode(_pins.ms3, OUTPUT);
  if (_pins.rst >= 0) { pinMode(_pins.rst, OUTPUT); digitalWrite(_pins.rst, HIGH); }

  _enable(false);
  _setDirLevel(true);

  _microstep_eff = _microstep;

  if (_driver == DRIVER_DRV8825) _applyMicrostepPins();
  else {
    if (_pins.ms1 >= 0) digitalWrite(_pins.ms1, LOW);
    if (_pins.ms2 >= 0) digitalWrite(_pins.ms2, LOW);
    if (_pins.ms3 >= 0) digitalWrite(_pins.ms3, LOW);
  }

  rmt_tx_channel_config_t cfg = {};
  cfg.gpio_num           = (gpio_num_t)_pins.step;
  cfg.clk_src            = RMT_CLK_SRC_DEFAULT;
  cfg.resolution_hz      = STEPPERRMT_RMT_RES_HZ;
  cfg.mem_block_symbols  = 64;
  cfg.trans_queue_depth  = 4;

  if (rmt_new_tx_channel(&cfg, &_rmt) != ESP_OK) return false;
  if (rmt_enable(_rmt) != ESP_OK) return false;

  rmt_copy_encoder_config_t e = {};
  if (rmt_new_copy_encoder(&e, &_enc) != ESP_OK) return false;

  const esp_timer_create_args_t targs = {
    .callback = &StepperRMT::_rampTimerThunk,
    .arg = this,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "rpm_ramp"
  };
  esp_timer_create(&targs, &_rampTimer);
  esp_timer_start_periodic(_rampTimer, 5000);

  xTaskCreatePinnedToCore(_pulseTaskThunk, "stepper_rmt", 4096, this, 10, &_pulseTaskHandle, 1);
  return true;
}

// ----- public API ------------------------------------------------
void StepperRMT::setDefault(uint16_t microstep, float accel_rpm_s, float decel_rpm_s) {
  portENTER_CRITICAL(&_mux);
  _microstep     = microstep;
  _microstep_eff = _microstep;   // apply to BOTH DM556T and DRV8825
  _accel_def     = accel_rpm_s;
  _decel_def     = decel_rpm_s;
  portEXIT_CRITICAL(&_mux);
  if (_driver == DRIVER_DRV8825) _applyMicrostepPins(); // pins only for DRV
}

bool StepperRMT::setMicrostep(uint16_t microstep) {
  if (_driver != DRIVER_DRV8825) return false;
  portENTER_CRITICAL(&_mux);
  _microstep = microstep;
  _microstep_eff = microstep;
  portEXIT_CRITICAL(&_mux);
  _applyMicrostepPins();
  return true;
}

bool StepperRMT::setSpeed(float rpm, float accel_override_rpm_s) {
  int new_sign = (rpm > 0) ? +1 : (rpm < 0 ? -1 : _dir_sign);
  if ((new_sign != _dir_sign) && (fabsf(_cur_rpm) > 0.01f)) {
    return false;
  }

  portENTER_CRITICAL(&_mux);
  _positioning = false;            // velocity command cancels any positioning move
  _tgt_rpm = fabsf(rpm);
  _accel_override = accel_override_rpm_s;
  _hold_on_zero = (fabsf(rpm) < 0.01f);
  if (fabsf(rpm) > 0.01f) {
    _dir_sign = new_sign;
    _setDirLevel(_dir_sign > 0);
    if (!_enabled) _enable(true);
  } else {
    if (!_enabled) _enable(true);
  }
  portEXIT_CRITICAL(&_mux);
  return true;
}

// ----- positioning ----------------------------------------------
bool StepperRMT::moveToStep(int64_t target_step, float cruise_rpm) {
  portENTER_CRITICAL(&_mux);
  const int64_t delta = target_step - _pos_steps;
  const int new_sign = (delta > 0) ? +1 : (delta < 0 ? -1 : _dir_sign);
  const bool moving = (fabsf(_cur_rpm) > 0.01f);
  if (delta != 0 && moving && (new_sign != _dir_sign)) {
    portEXIT_CRITICAL(&_mux);
    return false;                 // refuse direction flip while moving
  }

  _target_steps   = target_step;
  _pos_cruise_rpm = fabsf(cruise_rpm);
  _accel_override = -1.0f;
  _hold_on_zero   = true;         // hold position once the move completes
  _positioning    = (delta != 0);
  if (delta != 0) {
    _dir_sign = new_sign;
    _setDirLevel(_dir_sign > 0);
  }
  if (!_enabled) _enable(true);
  portEXIT_CRITICAL(&_mux);
  return true;
}

bool StepperRMT::moveSteps(int64_t delta_step, float cruise_rpm) {
  portENTER_CRITICAL(&_mux);
  const int64_t target = _pos_steps + delta_step;
  portEXIT_CRITICAL(&_mux);
  return moveToStep(target, cruise_rpm);
}

void StepperRMT::setZero() {
  portENTER_CRITICAL(&_mux);
  _pos_steps    = 0;
  _target_steps = 0;
  _positioning  = false;
  portEXIT_CRITICAL(&_mux);
}

int64_t StepperRMT::currentStep() const {
  portENTER_CRITICAL(const_cast<portMUX_TYPE*>(&_mux));
  const int64_t v = _pos_steps;
  portEXIT_CRITICAL(const_cast<portMUX_TYPE*>(&_mux));
  return v;
}

int64_t StepperRMT::targetStep() const {
  portENTER_CRITICAL(const_cast<portMUX_TYPE*>(&_mux));
  const int64_t v = _target_steps;
  portEXIT_CRITICAL(const_cast<portMUX_TYPE*>(&_mux));
  return v;
}

int64_t StepperRMT::distanceToGo() const {
  portENTER_CRITICAL(const_cast<portMUX_TYPE*>(&_mux));
  const int64_t v = _positioning ? (_target_steps - _pos_steps) : 0;
  portEXIT_CRITICAL(const_cast<portMUX_TYPE*>(&_mux));
  return v;
}

void StepperRMT::Stop() {
  portENTER_CRITICAL(&_mux);
  _positioning = false;
  _tgt_rpm = 0.0f;
  _cur_rpm = 0.0f;
  _hold_on_zero = false;
  portEXIT_CRITICAL(&_mux);

  rmt_disable(_rmt);
  rmt_enable(_rmt);
  _enable(false);
}

void StepperRMT::Brake() {
  portENTER_CRITICAL(&_mux);
  _positioning = false;
  _tgt_rpm = 0.0f;
  _cur_rpm = 0.0f;
  _hold_on_zero = true;
  portEXIT_CRITICAL(&_mux);

  rmt_disable(_rmt);
  rmt_enable(_rmt);
  _enable(true);
}

// ----- private helpers ------------------------------------------
void StepperRMT::_enable(bool on) {
  _enabled = on;
  if (_pins.en < 0) return;
  const bool level = (_en_active_high ? on : !on);
  digitalWrite(_pins.en, level ? HIGH : LOW);
}

void StepperRMT::_setDirLevel(bool cw) {
  const bool level = _dir_active_high ? cw : !cw;
  digitalWrite(_pins.dir, level ? HIGH : LOW);
}

void StepperRMT::_applyMicrostepPins() {
  // DRV8825 only — DM556T microstep is hardware-set; we just store the value.
  if (_driver != DRIVER_DRV8825) return;

  auto setPin = [](int pin, int v){ if (pin >= 0) digitalWrite(pin, v ? HIGH : LOW); };
  uint16_t m = _microstep_eff;
  uint8_t ms1 = 0, ms2 = 0, ms3 = 0;

  switch (m) {
    case 1:  ms1=0; ms2=0; ms3=0; break;
    case 2:  ms1=1; ms2=0; ms3=0; break;
    case 4:  ms1=0; ms2=1; ms3=0; break;
    case 8:  ms1=1; ms2=1; ms3=0; break;
    case 16: ms1=0; ms2=0; ms3=1; break;
    case 32: ms1=1; ms2=0; ms3=1; break;
    default:
      // For any higher value (like DM556T 64 or 128), keep pins low and just record it.
      ms1 = 0; ms2 = 0; ms3 = 0;
      break;
  }
  setPin(_pins.ms1, ms1);
  setPin(_pins.ms2, ms2);
  setPin(_pins.ms3, ms3);
}

void StepperRMT::_rampTimerThunk(void* arg){ reinterpret_cast<StepperRMT*>(arg)->_rampTimerISR(); }

void StepperRMT::_rampTimerISR() {
  const float dt = 0.005f;

  // In positioning mode, drive _tgt_rpm from the remaining distance so the
  // motor cruises and then decelerates to stop near the target step.
  if (_positioning) {
    portENTER_CRITICAL(&_mux);
    const int64_t remaining = _target_steps - _pos_steps;
    portEXIT_CRITICAL(&_mux);
    const int64_t rem_abs = (remaining < 0) ? -remaining : remaining;

    if (rem_abs == 0) {
      _tgt_rpm = 0.0f;
    } else {
      // Max speed from which we can still stop within the remaining steps:
      //   v_allowed = sqrt(2 * a * remaining)   (a in steps/s^2)
      // This yields a trapezoidal profile for long moves and a triangular
      // one for short moves, decelerating smoothly toward the target.
      const float steps_per_rev = STEPPERRMT_BASE_STEPS_PER_REV * (float)_microstep_eff;
      const float a_sps2 = (_decel_def * steps_per_rev) / 60.0f;      // steps/s^2
      const float v_allowed_sps = sqrtf(2.0f * a_sps2 * (float)rem_abs);
      float rpm_allowed = (v_allowed_sps * 60.0f) / steps_per_rev;
      _tgt_rpm = (rpm_allowed < _pos_cruise_rpm) ? rpm_allowed : _pos_cruise_rpm;
    }
  }

  float cur = _cur_rpm;
  float tgt = _tgt_rpm;

  float useAccel = _accel_override > 0 ? _accel_override
                     : ((tgt > cur) ? _accel_def : _decel_def);
  float delta = useAccel * dt;

  if (fabsf(tgt - cur) <= delta) cur = tgt;
  else cur += (tgt > cur ? +delta : -delta);

  _cur_rpm = cur;
}

void StepperRMT::_pulseTaskThunk(void* arg){ reinterpret_cast<StepperRMT*>(arg)->_pulseTask(); }

void StepperRMT::_pulseTask() {
  const uint32_t pulse_ticks = u32max((uint32_t)1, (uint32_t)((uint64_t)STEPPERRMT_PULSE_US * STEPPERRMT_RMT_RES_HZ / 1000000ULL));
  static constexpr size_t MAX_STEPS_PER_CHUNK = 128;
  static constexpr size_t MAX_SYMBOLS = 512;
  const uint32_t DUR_MAX = 32767;
  rmt_symbol_word_t symbols[MAX_SYMBOLS];

  while (true) {
    float rpm = _cur_rpm;

    // Snapshot positioning state for this chunk.
    bool    positioning;
    int64_t remaining;
    portENTER_CRITICAL(&_mux);
    positioning = _positioning;
    remaining   = _target_steps - _pos_steps;
    portEXIT_CRITICAL(&_mux);
    int64_t rem_abs = (remaining < 0) ? -remaining : remaining;

    if (positioning) {
      if (rem_abs == 0) {
        // Target reached: hold position and clear the move.
        portENTER_CRITICAL(&_mux);
        _positioning  = false;
        _tgt_rpm      = 0.0f;
        _cur_rpm      = 0.0f;
        _hold_on_zero = true;
        portEXIT_CRITICAL(&_mux);
        if (!_enabled) _enable(true);
        vTaskDelay(pdMS_TO_TICKS(2));
        continue;
      }
      // Guarantee motion even if the ramp already reached 0 RPM.
      if (rpm < STEPPERRMT_POSITION_CREEP_RPM) rpm = STEPPERRMT_POSITION_CREEP_RPM;
    } else if (rpm <= 0.01f) {
      if (!_hold_on_zero && _enabled) _enable(false);
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    if (!_enabled) _enable(true);

    const uint32_t period_us = _rpmToPeriodUs(rpm);
    const uint32_t period_ticks = u32max(pulse_ticks + 1, (uint32_t)((uint64_t)period_us * STEPPERRMT_RMT_RES_HZ / 1000000ULL));
    const uint32_t low_ticks_total = (period_ticks > pulse_ticks) ? (period_ticks - pulse_ticks) : 1;

    uint32_t step_time_us = (period_us == 0) ? 1 : period_us;
    uint32_t steps_per_chunk = (5000 / step_time_us);
    if (steps_per_chunk < 1) steps_per_chunk = 1;
    if (steps_per_chunk > MAX_STEPS_PER_CHUNK) steps_per_chunk = MAX_STEPS_PER_CHUNK;
    // Never emit more steps than remain to the target.
    if (positioning && (uint64_t)steps_per_chunk > (uint64_t)rem_abs)
      steps_per_chunk = (uint32_t)rem_abs;

    size_t sym_idx = 0;
    uint32_t steps_emitted = 0;
    for (uint32_t s = 0; s < steps_per_chunk && sym_idx + 1 < MAX_SYMBOLS; ++s) {
      uint32_t rem = low_ticks_total;

      if (_step_active_high) {
        symbols[sym_idx].level0    = 1;
        symbols[sym_idx].duration0 = pulse_ticks;
        symbols[sym_idx].level1    = 0;
        symbols[sym_idx].duration1 = (rem > DUR_MAX) ? DUR_MAX : rem;
      } else {
        symbols[sym_idx].level0    = 0;
        symbols[sym_idx].duration0 = pulse_ticks;
        symbols[sym_idx].level1    = 1;
        symbols[sym_idx].duration1 = (rem > DUR_MAX) ? DUR_MAX : rem;
      }
      rem -= (rem > DUR_MAX) ? DUR_MAX : rem;
      ++sym_idx;
      ++steps_emitted;          // one STEP pulse committed

      while (rem > 0 && sym_idx < MAX_SYMBOLS) {
        uint32_t d0 = (rem > DUR_MAX) ? DUR_MAX : rem; rem -= d0;
        uint32_t d1 = (rem > DUR_MAX) ? DUR_MAX : rem; rem -= d1;
        if (_step_active_high) {
          symbols[sym_idx].level0 = 0; symbols[sym_idx].duration0 = d0;
          symbols[sym_idx].level1 = 0; symbols[sym_idx].duration1 = d1;
        } else {
          symbols[sym_idx].level0 = 1; symbols[sym_idx].duration0 = d0;
          symbols[sym_idx].level1 = 1; symbols[sym_idx].duration1 = d1;
        }
        ++sym_idx;
      }
    }

    if (rmt_transmit(_rmt, _enc, symbols, sym_idx * sizeof(rmt_symbol_word_t), &_txcfg) == ESP_OK) {
      rmt_tx_wait_all_done(_rmt, -1);
      // Account for the steps actually emitted (sign = current direction).
      portENTER_CRITICAL(&_mux);
      _pos_steps += (int64_t)_dir_sign * (int64_t)steps_emitted;
      portEXIT_CRITICAL(&_mux);
    } else {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
}
