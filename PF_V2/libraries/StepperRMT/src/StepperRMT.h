#pragma once
#include <Arduino.h>
#include <math.h>
#include "driver/gpio.h"
#include "driver/rmt_types.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// -------- User-tweakable defaults (compile-time) -----------------
#ifndef STEPPERRMT_DEFAULT_MICROSTEP
#define STEPPERRMT_DEFAULT_MICROSTEP   32     // 1/32 step default; DM556T supports 1–128
#endif
#ifndef STEPPERRMT_DEFAULT_ACCEL_RPM_S
#define STEPPERRMT_DEFAULT_ACCEL_RPM_S 3000  // RPM per second
#endif
#ifndef STEPPERRMT_DEFAULT_DECEL_RPM_S
#define STEPPERRMT_DEFAULT_DECEL_RPM_S 3000  // RPM per second
#endif
#ifndef STEPPERRMT_PULSE_US
#define STEPPERRMT_PULSE_US            10    // STEP pulse width in microseconds
#endif
#ifndef STEPPERRMT_BASE_STEPS_PER_REV
#define STEPPERRMT_BASE_STEPS_PER_REV  200   // NEMA23 1.8° motor
#endif
#ifndef STEPPERRMT_RMT_RES_HZ
#define STEPPERRMT_RMT_RES_HZ          312500 // 80MHz/256 -> 3.2us ticks; valid divider
#endif
#ifndef STEPPERRMT_POSITION_CREEP_RPM
#define STEPPERRMT_POSITION_CREEP_RPM  1.0f   // min RPM near target so the move always finishes
#endif

// -------- Driver selection --------------------------------------
enum StepperDriverType : uint8_t {
  DRIVER_DM556T = 0,  // opto driver; we assume ULN2003 sinking to PUL-/DIR-/ENA-
  DRIVER_DRV8825 = 1  // direct STEP/DIR/EN pins (3.3V logic)
};

class StepperRMT {
public:
  struct Pins {
    int step=25, dir=26, en=27, ms1=14, ms2=12, ms3=13, rst=-1;
  };

  StepperRMT(StepperDriverType driver, const Pins& pins);

  // Call once in setup()
  bool begin();

  // Defaults: microstep (effective only for DRV8825), accel, decel
  void setDefault(uint16_t microstep, float accel_rpm_s, float decel_rpm_s);

  // DRV8825 only; ignored for DM556T
  bool setMicrostep(uint16_t microstep);

  // Set target speed in RPM (positive=CW, negative=CCW).
  // Optional accel overrides defaults for this command (used for accel or decel).
  // Returns false if a direction flip was requested while moving.
  bool setSpeed(float rpm, float accel_override_rpm_s = -1.0f);

  // Immediate: speed=0, motor disabled (free to turn)
  void Stop();

  // Immediate: speed=0, motor enabled and holding (brake)
  void Brake();

  // ---- Positioning (RMT step counting) ----
  // Move to an absolute step position (microsteps) using a trapezoidal RPM
  // profile (accel -> cruise_rpm -> decel) and stop exactly on target.
  // cruise_rpm is the magnitude of the cruise speed.
  // Returns false if a direction flip is requested while still moving
  // (caller should Stop() / wait, then retry).
  bool moveToStep(int64_t target_step, float cruise_rpm);

  // Relative move by delta microsteps (sign = direction).
  bool moveSteps(int64_t delta_step, float cruise_rpm);

  // Mark the current position as zero.
  void setZero();

  // Position queries (microsteps)
  int64_t currentStep() const;
  int64_t targetStep()  const;
  int64_t distanceToGo() const;       // target - current (signed)
  bool     isPositioning() const { return _positioning; }

  // Query
  float currentRPM() const { return _cur_rpm; }
  float targetRPM()  const { return _tgt_rpm; }
  float defaultAccel() const { return _accel_def; }
  float defaultDecel() const { return _decel_def; }
  uint16_t microstep() const { return _microstep; }
  uint32_t stepsPerRev() const { return STEPPERRMT_BASE_STEPS_PER_REV * (uint32_t)_microstep_eff; }
  StepperDriverType driver() const { return _driver; }
  bool isEnabled() const { return _enabled; }

private:
  // Core control
  void _enable(bool on);
  void _setDirLevel(bool cw);
  void _applyMicrostepPins();     // DRV8825 only
  static void _rampTimerThunk(void* arg);
  void _rampTimerISR();           // esp_timer 5ms ISR-safe numeric update
  static void _pulseTaskThunk(void* arg);
  void _pulseTask();              // generates pulses using RMT

  // Timing helpers
  inline uint32_t _rpmToPeriodUs(float rpm) const {
    if (rpm <= 0) return 0;
    const float steps_per_rev = STEPPERRMT_BASE_STEPS_PER_REV * (float)_microstep_eff;
    const float steps_per_sec = (rpm * steps_per_rev) / 60.0f;
    if (steps_per_sec < 1e-6f) return 0;
    const float period_us = 1e6f / steps_per_sec;
    return (uint32_t)(period_us + 0.5f);
  }

  // Config/stat
  StepperDriverType _driver;
  Pins _pins;

  // Polarity assumptions (ULN2003 sinks => active-low edges to driver inputs)
  bool _step_active_high;
  bool _dir_active_high;
  bool _en_active_high;     // both DM556T & DRV8825 typically enable on LOW => false

  // Motion state
  volatile float _cur_rpm = 0.0f;
  volatile float _tgt_rpm = 0.0f;
  volatile float _accel_def = STEPPERRMT_DEFAULT_ACCEL_RPM_S;
  volatile float _decel_def = STEPPERRMT_DEFAULT_DECEL_RPM_S;
  volatile float _accel_override = -1.0f; // one-shot accel override for setSpeed()

  volatile bool  _hold_on_zero = false;  // behavior when target==0
  volatile bool  _enabled = false;
  volatile int   _dir_sign = +1;         // +1 = CW, -1 = CCW

  // Positioning state (microsteps). Updated by the pulse task.
  volatile bool    _positioning   = false;
  volatile int64_t _pos_steps     = 0;   // running absolute position
  volatile int64_t _target_steps  = 0;   // target for a positioning move
  volatile float   _pos_cruise_rpm = 0.0f;

  // Microstep
  uint16_t _microstep = STEPPERRMT_DEFAULT_MICROSTEP;  // requested
  uint16_t _microstep_eff = STEPPERRMT_DEFAULT_MICROSTEP; // effective

  // RMT
  rmt_channel_handle_t  _rmt = nullptr;
  rmt_encoder_handle_t  _enc = nullptr;
  rmt_transmit_config_t _txcfg = { .loop_count = 0 };

  // Control infra
  esp_timer_handle_t _rampTimer = nullptr;
  TaskHandle_t       _pulseTaskHandle = nullptr;
  portMUX_TYPE       _mux = portMUX_INITIALIZER_UNLOCKED;
};
