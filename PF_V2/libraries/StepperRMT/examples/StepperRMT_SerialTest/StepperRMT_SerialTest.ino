/*
  StepperRMT Serial UI Test
  - ESP32-D0WD-V3, Arduino core 3.3.3
*/

#include <StepperRMT.h>

static constexpr int PIN_STEP = 25;
static constexpr int PIN_DIR  = 26;
static constexpr int PIN_EN   = 27;
static constexpr int PIN_MS1  = 14;
static constexpr int PIN_MS2  = 12;
static constexpr int PIN_MS3  = 13;
static constexpr int PIN_RST  = -1;

static constexpr StepperDriverType DRIVER = DRIVER_DM556T; // or DRIVER_DRV8825

StepperRMT::Pins pins = { PIN_STEP, PIN_DIR, PIN_EN, PIN_MS1, PIN_MS2, PIN_MS3, PIN_RST };
StepperRMT myStepper(DRIVER, pins);

static void printHelp() {
  Serial.println(F("\n--- StepperRMT Serial UI ---"));
  Serial.println(F("help           : show this help"));
  Serial.println(F("def m a d     : set default microstep, accel(RPM/s), decel(RPM/s)"));
  Serial.println(F("step m        : set microstep (DRV8825 only)"));
  Serial.println(F("s rpm [accel] : set target speed RPM (+CW, -CCW), optional accel override"));
  Serial.println(F("stop          : immediate stop (disable coils)"));
  Serial.println(F("brake         : immediate stop (hold position)"));
  Serial.println(F("rst           : pulse RESET if available (DRV8825)"));
  Serial.println(F("info          : show current status\n"));
}

void setup() {
  Serial.begin(115200);
  while(!Serial) {}
  delay(100);

  if (!myStepper.begin()) {
    Serial.println(F("StepperRMT begin() failed"));
    while(true) delay(1000);
  }

  // IMPORTANT for DM556T: mirror the DIP value here
  myStepper.setDefault(STEPPERRMT_DEFAULT_MICROSTEP,
                       STEPPERRMT_DEFAULT_ACCEL_RPM_S,
                       STEPPERRMT_DEFAULT_DECEL_RPM_S);

  printHelp();
}

static String rdln() {
  String s;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c=='\r') continue;
    if (c=='\n') break;
    s += c;
  }
  return s;
}

void loop() {
  String line = rdln();
  if (line.length()==0) { delay(5); return; }

  line.trim();
  if (line.equalsIgnoreCase("help")) { printHelp(); return; }
  if (line.equalsIgnoreCase("stop")) { myStepper.Stop(); Serial.println(F("STOP: coils disabled.")); return; }
  if (line.equalsIgnoreCase("brake")){ myStepper.Brake(); Serial.println(F("BRAKE: holding.")); return; }
  if (line.equalsIgnoreCase("rst")) {
    if (PIN_RST >= 0) { pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, LOW); delay(2); digitalWrite(PIN_RST, HIGH); }
    Serial.println(F("RESET pulse sent (if wired)."));
    return;
  }
  if (line.equalsIgnoreCase("info")) {
    Serial.printf("Driver=%s  Microstep=%u  Enabled=%s\n",
      (DRIVER==DRIVER_DM556T?"DM556T(ULN2003)":"DRV8825"),
      myStepper.microstep(), myStepper.isEnabled()?"YES":"NO");
    Serial.printf("RPM cur/tgt=%.2f / %.2f  Accel=%.1f  Decel=%.1f\n",
      myStepper.currentRPM(), myStepper.targetRPM(), myStepper.defaultAccel(), myStepper.defaultDecel());
    return;
  }

  if (line.startsWith("def")) {
    int m; float a,d;
    if (sscanf(line.c_str(), "def %d %f %f", &m, &a, &d)==3) {
      myStepper.setDefault((uint16_t)m, a, d);
      Serial.println(F("Default set."));
    } else {
      Serial.println(F("Usage: def <microstep> <accel> <decel>"));
    }
    return;
  }

  if (line.startsWith("step")) {
    int m;
    if (sscanf(line.c_str(), "step %d", &m)==1) {
      if (DRIVER==DRIVER_DRV8825) {
        if (myStepper.setMicrostep((uint16_t)m)) Serial.println(F("Microstep updated."));
        else Serial.println(F("Microstep set failed."));
      } else {
        Serial.println(F("Ignored (DM556T DIP sets microstep)."));
      }
    } else {
      Serial.println(F("Usage: step <1|2|4|8|16|32>"));
    }
    return;
  }

  if (line.startsWith("s")) {
    float rpm, acc;
    int n = sscanf(line.c_str(), "s %f %f", &rpm, &acc);
    bool ok = (n==1) ? myStepper.setSpeed(rpm) : myStepper.setSpeed(rpm, acc);
    if (!ok) Serial.println(F("Refused: direction flip only allowed when speed=0."));
    return;
  }

  Serial.println(F("Unknown command. Type 'help'."));
}
