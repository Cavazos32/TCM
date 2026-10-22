# StepperRMT

RMT-based stepper control library for **ESP32 (Arduino core 3.3.3+)**.  
Supports:

- **DM556T** (via **ULN2003** sink driver for PUL-/DIR-/ENA-, DIP sets microstep)
- **DRV8825** (STEP/DIR/EN direct from ESP32; microstep set by MS1/MS2/MS3 pins)

Features:

- Speed in **RPM**
- Accel/Decel ramps in **RPM/s**
- `Stop()` (free) and `Brake()` (hold)
- Direction by sign: `+rpm = CW`, `-rpm = CCW` (no invert command)
- **RMT new driver** backend (`rmt_new_tx_channel` + copy encoder) for Arduino-ESP32 3.x / ESP-IDF v5.

## DM556T DIP quick rule
If you change the microstep DIP on DM556T, tell the library the same value so RPM math stays absolute:
```cpp
// In setup(), after begin():
myStepper.setDefault(8, 3000, 3000);  // 1/8 DIP -> 8 here
```
(From v1.0.3, `setDefault()` updates the effective microstep for BOTH DM556T and DRV8825.)

## API

```cpp
StepperRMT::StepperRMT(StepperDriverType driver, const StepperRMT::Pins& pins)
bool begin();

void setDefault(uint16_t microstep, float accel_rpm_s, float decel_rpm_s);
// DRV8825 only:
bool setMicrostep(uint16_t microstep);

bool setSpeed(float rpm, float accel_override_rpm_s = -1);
// setSpeed(0) => ramp to zero using accel_override (or default decel) and HOLD (Brake)

void Stop();  // immediate: speed=0, disable coils
void Brake(); // immediate: speed=0, enable coils (hold)
```

### Pins
Default pins (change in example or your sketch):

- STEP 25, DIR 26, EN 27, MS1 14, MS2 12, MS3 13, RST (optional)

## Notes
- **Direction flip** is ignored while moving; allowed only when current speed is zero (or after `Stop()`/`Brake()`).
- RMT resolution is **312.5 kHz** (3.2 µs ticks) so very slow RPM works without overflowing symbol durations. Pulse width defaults to **10 µs** (safe for DM/DRV).  
- NEMA23 assumed **200 full-steps/rev**.
