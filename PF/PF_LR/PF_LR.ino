// PF_LR — PreFeeder L/R: loop principal (WiFi STA + web + motores + enlace Master)

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <StepperRMT.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "Config.h"
#include "Index.h"
#include "Types.h"
#include "Status_Mode.h"

WebServer server(80);
WiFiServer peerServer(PEER_PORT);
WiFiClient peerClient;
Preferences prefs;
StepperRMT* motor  = nullptr;
StepperRMT* motor2 = nullptr;

static volatile bool motor2TcpTriggerRequest = false;
static volatile float motor2TcpTriggerSecRequest = 0.0f;  // 0 = usar motor2TriggerFeedSec
static volatile uint16_t motor2TcpTriggerPendingId = 0;   // id del Tfeed pendiente (0 = sin id)
static volatile float motor2TriggerActiveSec = 0.0f;      // duración del trigger en curso

// Dedup Tfeed: mismo cmd id no re-alimenta; se marca al arrancar el feed.
static uint16_t triggerDoneIds[TRIGGER_ID_HIST] = {};
static uint8_t triggerDoneCount = 0;
static uint8_t triggerDoneNext = 0;

static bool triggerIdAlreadyDone(uint16_t id)
{
  if (!id) return false;
  for (uint8_t i = 0; i < triggerDoneCount; i++)
  {
    if (triggerDoneIds[i] == id) return true;
  }
  return false;
}

static void triggerIdRemember(uint16_t id)
{
  if (!id || triggerIdAlreadyDone(id)) return;
  triggerDoneIds[triggerDoneNext] = id;
  triggerDoneNext = (uint8_t)((triggerDoneNext + 1) % TRIGGER_ID_HIST);
  if (triggerDoneCount < TRIGGER_ID_HIST) triggerDoneCount++;
}

static void triggerIdClear()
{
  triggerDoneCount = 0;
  triggerDoneNext = 0;
  motor2TcpTriggerRequest = false;
  motor2TcpTriggerSecRequest = 0.0f;
  motor2TcpTriggerPendingId = 0;
}
static String peerLastCmd = "";
static uint32_t peerLastCmdMs = 0;
static bool peerLastCmdOk = false;
static String peerLastMessage = "";
static uint32_t peerLastMessageMs = 0;
static char peerLastMessageDirection = '-';
static char peerRxLine[768];
static uint16_t peerRxLen = 0;
static uint32_t peerLastStatusMs = 0;
static uint32_t lastWifiRetryMs = 0;
static uint32_t wifiConnectStartedMs = 0;
static bool staWasConnected = false;
static bool peerServicesUp = false;
static bool peerWasConnected = false;
static volatile bool peerLinkOk = false;  // false = sin enlace TCP con TCM
static volatile bool idleMode = false;      // Materialista ON (ex-Idle): bloquea buffer, omite GPIO27, solo refill manual; torre naranja
static volatile bool tcmInProcess = false;  // ciclo/settle TCM: relleno continuo mientras ON
static volatile bool fillUntilReady = false; // one-shot: rellenar hasta Buffer Full, luego congelar
static bool lastPeerHome = false, lastPeerEndstop = false, lastPeerTension = false;
static bool lastPeerCyl = false, lastPeerHose = false, lastPeerErr = false;
static bool lastPeerTrig = false, lastPeerAutoEn = false;
static bool lastPeerRefillDer = false, lastPeerRefillSrv = false, lastPeerRefillFeed = false;
static bool lastPeerIdleMode = false, lastPeerInProcess = false;
static bool lastPeerTfeedIncomplete = false;
static uint8_t lastPeerFault = 255;
static uint8_t lastPeerAutoState = 255;
static uint8_t lastPeerPhase = 255;
static uint32_t lastPeerTrigMsLeft = UINT32_MAX;

float commandedRpm  = 0.0f;
float commandedRpm2 = 0.0f;

volatile float motor2RpmSetting       = MOTOR_RPM_DEFAULT;  // RPM durante alimentación (trigger TCP)
volatile float motor2TriggerFeedSec   = M2_TRIGGER_FEED_DEFAULT;  // duración Tfeed (editable)
volatile Motor2Phase motor2Phase      = M2_PHASE_IDLE;
volatile Motor2FeedSource motor2FeedSource = M2_FEED_NONE;
volatile float motor2ActiveFeedRpm = 0.0f;   // RPM del timed feed en curso
volatile uint32_t motor2TriggerFeedEndMs = 0;
TaskHandle_t   motor2TaskHandle       = nullptr;

static void applyTriggerFeedSec(float v)
{
  motor2TriggerFeedSec = constrain(v, M2_TRIGGER_FEED_MIN, M2_TRIGGER_FEED_MAX);
}

static volatile bool bufferFullStable       = false;
static uint32_t      bufferFullHighAccumMs  = 0;
static uint32_t      bufferFullLowAccumMs   = 0;
static uint32_t      bufferFullFilterLastMs = 0;

// Diagnóstico: latencia BUFFER_FULL RAW → force stop → PWM 1500 (solo flanco).
static bool     bufferFullRawPrev = false;
static uint32_t bufferFullDetectedMs = 0;
static uint32_t servoForceStopMs = 0;
static bool     waitingServoStopMeasurement = false;
static bool     servoForceStopLogged = false;

// ====================== AUTO / FAULTS ======================
bool      autoEnabled    = true;
AutoState autoState      = AUTO_OFF;
float     autoRpm        = AUTO_RPM_DEFAULT;
float     autoReverseSec = AUTO_REVERSE_DEFAULT;
float     tensionReverseRpm = TENSION_REVERSE_RPM_DEFAULT;
float     tensionCooldownSec = TENSION_COOLDOWN_DEFAULT;
float     tensionFaultSec = TENSION_FAULT_SEC_DEFAULT;
uint32_t  servoLeadStartMs = 0;
uint32_t  tensionLastRoutineMs = 0;
uint32_t  tensionActiveSinceMs = 0;
uint32_t  tensionReverseUntilMs = 0;      // fin de pulso CCW por tensión
uint32_t  tensionReverseHighSinceMs = 0;  // debounce GPIO 23
static bool tensionReverseStable = false;
uint32_t  bufferEmptySinceMs = 0;
uint32_t  bufferFullRecoverSinceMs = 0;  // Buffer Full estable antes de cancelar timeout
SystemFault systemFault = FAULT_NONE;
volatile bool motor2AbortRequested = false;
// Pause (setInProcess OFF): Tfeed abortado antes de completar → retry parcial en Resume.
static bool motor2TfeedIncompleteLatch = false;
static float motor2TfeedResumeSec = 0.0f;

uint16_t  servoActivePwmUs = SERVO_PWM_ACTIVE_US;  // editable UI/NVS (default por lado)
bool      servoRunning      = false;
uint16_t  servoLastOutputUs = 0;
// RMT Stop/Brake puede soltar el pin LEDC del servo (marcar dirty → remux).
static volatile bool servoPinDirty = false;

static void servoMarkPinDirty()
{
  servoPinDirty = true;
}

// Refill manual (pulso); volatile: loop + motor2FeederTask.
volatile uint32_t refillPulseMs = REFILL_PULSE_MS_DEFAULT;
volatile bool refillDereelerOn = false;
volatile bool refillServoOn    = false;
volatile bool refillFeederOn   = false;
volatile uint32_t refillDereelerPulseUntilMs = 0;
volatile uint32_t refillServoPulseUntilMs    = 0;
volatile uint32_t refillFeederPulseUntilMs   = 0;

static void saveSettings()
{
  prefs.begin(PREFS_NS, false);
  prefs.putFloat("auto_rpm", autoRpm);
  prefs.putFloat("auto_rev", autoReverseSec);
  prefs.putBool("auto_en", autoEnabled);
  prefs.putFloat("m2_rpm", (float)motor2RpmSetting);
  prefs.putFloat("m2_trig_s", (float)motor2TriggerFeedSec);
  prefs.putFloat("tens_rev", tensionReverseRpm);
  prefs.putFloat("tens_cd", tensionCooldownSec);
  prefs.putFloat("tens_fault", tensionFaultSec);
  prefs.putUInt("servo_pwm", (uint32_t)servoActivePwmUs);
  prefs.putUInt("refill_ms", refillPulseMs);
  prefs.end();
}

static void loadSettings()
{
  prefs.begin(PREFS_NS, true);
  autoRpm = prefs.getFloat("auto_rpm", AUTO_RPM_DEFAULT);
  autoReverseSec = prefs.getFloat("auto_rev", AUTO_REVERSE_DEFAULT);
  autoEnabled = prefs.getBool("auto_en", true);
  motor2RpmSetting = prefs.getFloat("m2_rpm", MOTOR_RPM_DEFAULT);
  motor2TriggerFeedSec = prefs.getFloat("m2_trig_s", M2_TRIGGER_FEED_DEFAULT);
  {
    float tr = prefs.getFloat("tens_rev", -1.0f);
    if (tr < 0.0f)
      tr = prefs.getFloat("tens_boost", TENSION_REVERSE_RPM_DEFAULT);
    tensionReverseRpm = tr;
  }
  tensionCooldownSec = prefs.getFloat("tens_cd", TENSION_COOLDOWN_DEFAULT);
  tensionFaultSec = prefs.getFloat("tens_fault", TENSION_FAULT_SEC_DEFAULT);
  servoActivePwmUs = (uint16_t)prefs.getUInt("servo_pwm", (uint32_t)SERVO_PWM_ACTIVE_US);
  refillPulseMs = prefs.getUInt("refill_ms", REFILL_PULSE_MS_DEFAULT);
  if (refillPulseMs < REFILL_PULSE_MS_MIN) refillPulseMs = REFILL_PULSE_MS_MIN;
  if (refillPulseMs > REFILL_PULSE_MS_MAX) refillPulseMs = REFILL_PULSE_MS_MAX;
  prefs.end();

  // Migración: tens_fault ≈ auto_rev (builds mezclados) → default.
  if (tensionFaultSec <= (autoReverseSec + 0.2f)
      && tensionFaultSec < (TENSION_FAULT_SEC_DEFAULT - 0.5f))
  {
    Serial.printf("TENSION: migrar tens_fault %.1f → %.1f (era ≈ reverse)\n",
                  tensionFaultSec, TENSION_FAULT_SEC_DEFAULT);
    tensionFaultSec = TENSION_FAULT_SEC_DEFAULT;
    prefs.begin(PREFS_NS, false);
    prefs.putFloat("tens_fault", tensionFaultSec);
    prefs.end();
  }

  autoRpm = constrain(autoRpm, MOTOR_RPM_MIN, MOTOR_RPM_MAX);
  autoReverseSec = constrain(autoReverseSec, AUTO_REVERSE_MIN, AUTO_REVERSE_MAX);
  motor2RpmSetting = constrain(motor2RpmSetting, MOTOR_RPM_MIN, MOTOR_RPM_MAX);
  applyTriggerFeedSec((float)motor2TriggerFeedSec);
  tensionReverseRpm = constrain(tensionReverseRpm, TENSION_REVERSE_RPM_MIN, TENSION_REVERSE_RPM_MAX);
  tensionCooldownSec = constrain(tensionCooldownSec, TENSION_COOLDOWN_MIN, TENSION_COOLDOWN_MAX);
  tensionFaultSec = constrain(tensionFaultSec, TENSION_FAULT_SEC_MIN, TENSION_FAULT_SEC_MAX);
  servoActivePwmUs = constrain(servoActivePwmUs, SERVO_PWM_MIN_US, SERVO_PWM_MAX_US);
}

static uint32_t servoUsToDuty(uint16_t us)
{
  us = constrain(us, SERVO_PWM_MIN_US, SERVO_PWM_MAX_US);
  return (uint32_t)us * ((1UL << SERVO_LEDC_BITS) - 1) / 20000UL;
}

static uint16_t servoMotionPwmUs()
{
  const int delta = (int)servoActivePwmUs - (int)SERVO_PWM_NEUTRAL_US;
  if (delta > -40 && delta < 40)
    return SERVO_PWM_ACTIVE_US;
  return servoActivePwmUs;
}

static void servoTimingOnPwmNeutralWritten();

// Remux completo tras RMT: Detach → GPIO bajo → Attach. Sin el pinMode
// intermedio ledcAttachChannel puede quedar sin pulso real en GPIO26.
static void servoRemuxWriteUs(uint16_t us)
{
  us = constrain(us, SERVO_PWM_MIN_US, SERVO_PWM_MAX_US);
  const uint32_t duty = servoUsToDuty(us);
  servoPinDirty = false;
  servoLastOutputUs = us;

  ledcDetach(PIN_SERVO_PWM);
  pinMode(PIN_SERVO_PWM, OUTPUT);
  digitalWrite(PIN_SERVO_PWM, LOW);
  ledcAttachChannel(PIN_SERVO_PWM, 50, SERVO_LEDC_BITS, SERVO_LEDC_CHANNEL);
  ledcWrite(PIN_SERVO_PWM, duty);
  ledcWrite(PIN_SERVO_PWM, duty);
}

static void servoWriteUsForced(uint16_t us)
{
  us = constrain(us, SERVO_PWM_MIN_US, SERVO_PWM_MAX_US);
  const uint32_t duty = servoUsToDuty(us);
  const bool remux = servoPinDirty;

  // Remux tras RMT; sin dirty no reescribir el mismo duty (glitch RC).
  if (!remux && ledcRead(PIN_SERVO_PWM) == duty)
  {
    servoLastOutputUs = us;
    return;
  }
  if (!remux && ledcWrite(PIN_SERVO_PWM, duty))
  {
    servoLastOutputUs = us;
    return;
  }

  servoRemuxWriteUs(us);
}

// Neutro 1500. Remux solo si dirty o force con cache mentiroso.
static void servoHardStopNeutral(bool force)
{
  static uint32_t lastHardMs = 0;
  const uint32_t now = millis();
  const bool dirty = servoPinDirty;
  const bool wasRunning = servoRunning
      || servoLastOutputUs != SERVO_PWM_NEUTRAL_US;
  const bool remux = dirty || (force && !wasRunning);

  if (!remux && !wasRunning
      && (uint32_t)(now - lastHardMs) < SERVO_STOP_REASSERT_MS)
  {
    // Diagnóstico: ya neutro; no inventar latencia de parada.
    if (waitingServoStopMeasurement)
    {
      Serial.printf("[%s][SERVO_TIMING] PWM already neutral (no rewrite) @ %lu ms | +%lu ms from BUFFER_FULL\n",
                    PREFEEDER_SIDE_TAG,
                    (unsigned long)now,
                    (unsigned long)(now - bufferFullDetectedMs));
      waitingServoStopMeasurement = false;
      servoForceStopLogged = false;
    }
    return;
  }

  servoPinDirty = false;
  servoLastOutputUs = SERVO_PWM_NEUTRAL_US;
  servoRunning = false;

  if (remux)
  {
    servoRemuxWriteUs(SERVO_PWM_NEUTRAL_US);
  }
  else if (!ledcWrite(PIN_SERVO_PWM, servoUsToDuty(SERVO_PWM_NEUTRAL_US)))
  {
    servoRemuxWriteUs(SERVO_PWM_NEUTRAL_US);
  }
  lastHardMs = now;
  servoTimingOnPwmNeutralWritten();
}

static void servoAssertNeutral(bool force)
{
  static uint32_t lastNeutralMs = 0;
  const bool need = servoRunning || servoLastOutputUs != SERVO_PWM_NEUTRAL_US;
  if (!force && !servoPinDirty && !need
      && (uint32_t)(millis() - lastNeutralMs) < SERVO_STOP_REASSERT_MS)
    return;
  servoWriteUsForced(SERVO_PWM_NEUTRAL_US);
  servoRunning = false;
  lastNeutralMs = millis();
}

static bool bufferFullStopNow();

static void servoAssertRun(bool force)
{
  if (!idleMode && bufferFullStopNow())
  {
    servoHardStopNeutral(true);
    return;
  }

  static uint32_t lastRunMs = 0;
  const uint16_t us = servoMotionPwmUs();
  if (!force && !servoPinDirty && servoRunning && servoLastOutputUs == us
      && (uint32_t)(millis() - lastRunMs) < SERVO_STOP_REASSERT_MS)
    return;
  servoWriteUsForced(us);
  servoRunning = true;
  lastRunMs = millis();
}

static void servoStop()
{
  if (!servoPinDirty && !servoRunning && servoLastOutputUs == SERVO_PWM_NEUTRAL_US)
    return;
  servoAssertNeutral(true);
}

static void applyServoActivePwmUs(uint16_t us)
{
  us = constrain(us, SERVO_PWM_MIN_US, SERVO_PWM_MAX_US);
  servoActivePwmUs = us;
  if (servoRunning)
    servoWriteUsForced(servoActivePwmUs);
}

static bool sensorsMotionArmed()
{
  return !idleMode && (tcmInProcess || fillUntilReady);
}

static void resumeAutoFromSensors();
static void requestFillUntilReady();
static void clearFillUntilReady(const char* reason);
static void serviceFillUntilReady();
static bool bufferFullAllowsMotion();
static void forceStopDereelerAndServo();
static void servoServiceBufferFullCut();
static void dereelerDirCw();
static void dereelerDirReverse();
static void dereelerDirApply(bool reverse);

static bool refillMaterialAllOn()
{
  return refillDereelerOn && refillServoOn && refillFeederOn;
}

static bool servoShouldRunAuto()
{
  return systemFault == FAULT_NONE
      && autoEnabled
      && sensorsMotionArmed()
      && !bufferFullStopNow()
      && (autoState == AUTO_SERVO_LEAD || autoState == AUTO_CW || autoState == AUTO_CCW);
}

static void syncServoToAutoState()
{
  if (idleMode && systemFault == FAULT_NONE)
  {
    if (refillServoOn)
      servoAssertRun(false);
    else if (servoRunning || servoLastOutputUs != SERVO_PWM_NEUTRAL_US)
      servoStop();
    return;
  }

  if (servoShouldRunAuto())
    servoAssertRun(false);
  else if (servoRunning || servoLastOutputUs != SERVO_PWM_NEUTRAL_US)
    servoStop();
}

static void serviceServoPwm()
{
  if (idleMode)
  {
    if (refillServoOn)
      servoAssertRun(true);
    else
      servoAssertNeutral(false);
    return;
  }
  if (bufferFullStopNow())
  {
    servoServiceBufferFullCut();
    return;
  }
  if (servoShouldRunAuto())
    servoAssertRun(true);
  else
    servoAssertNeutral(false);
}

static void setupRotationServo()
{
  servoPinDirty = true;
  servoLastOutputUs = 0;
  servoRemuxWriteUs(SERVO_PWM_NEUTRAL_US);
  servoRunning = false;
}

static bool bufferFullRaw()
{
  return digitalRead(PIN_SENSOR_BUFFER_FULL) == HIGH;
}

// Flanco LOW→HIGH del sensor RAW (no filtrado). Solo log en el flanco.
static void servoTimingOnBufferFullRawEdge()
{
  const bool raw = bufferFullRaw();
  if (raw && !bufferFullRawPrev)
  {
    const uint32_t now = millis();
    bufferFullDetectedMs = now;
    const bool alreadyStopped = !servoRunning
        && servoLastOutputUs == SERVO_PWM_NEUTRAL_US;
    if (alreadyStopped)
    {
      waitingServoStopMeasurement = false;
      servoForceStopLogged = false;
      Serial.printf("[%s][SERVO_TIMING] BUFFER_FULL RAW HIGH @ %lu ms | servo=%u us | running=0 | already stopped (no timing)\n",
                    PREFEEDER_SIDE_TAG,
                    (unsigned long)now,
                    (unsigned)servoLastOutputUs);
    }
    else
    {
      waitingServoStopMeasurement = true;
      servoForceStopLogged = false;
      Serial.printf("[%s][SERVO_TIMING] BUFFER_FULL RAW HIGH @ %lu ms | servo=%u us | running=%d\n",
                    PREFEEDER_SIDE_TAG,
                    (unsigned long)now,
                    (unsigned)servoLastOutputUs,
                    (int)servoRunning);
    }
  }
  bufferFullRawPrev = raw;
}

// Solo callers de Buffer Full (no otros forceStop).
static void servoTimingOnForceStopFromBufferFull()
{
  if (!waitingServoStopMeasurement || servoForceStopLogged)
    return;
  servoForceStopLogged = true;
  servoForceStopMs = millis();
  Serial.printf("[%s][SERVO_TIMING] FORCE STOP @ %lu ms | +%lu ms from BUFFER_FULL\n",
                PREFEEDER_SIDE_TAG,
                (unsigned long)servoForceStopMs,
                (unsigned long)(servoForceStopMs - bufferFullDetectedMs));
}

static void servoTimingOnPwmNeutralWritten()
{
  if (!waitingServoStopMeasurement)
    return;
  const uint32_t now = millis();
  Serial.printf("[%s][SERVO_TIMING] PWM STOP %u us @ %lu ms | +%lu ms from BUFFER_FULL\n",
                PREFEEDER_SIDE_TAG,
                (unsigned)SERVO_PWM_NEUTRAL_US,
                (unsigned long)now,
                (unsigned long)(now - bufferFullDetectedMs));
  waitingServoStopMeasurement = false;
  servoForceStopLogged = false;
}

static bool bufferFullActive()
{
  return bufferFullStable;
}

// Parar con Full raw|stable; rearrancar solo OFF filtrado ~200 ms.
static bool bufferFullStopNow()
{
  return bufferFullStable || bufferFullRaw();
}

static void servoServiceBufferFullCut()
{
  static bool latched = false;
  if (!bufferFullStopNow())
  {
    latched = false;
    return;
  }
  const bool edge = !latched;
  latched = true;
  servoHardStopNeutral(edge);
}

static bool bufferFullAllowsMotion()
{
  return !bufferFullStable
      && !bufferFullRaw()
      && bufferFullLowAccumMs >= BUFFER_FULL_OFF_FILTER_MS;
}

static void bufferFullFilterReset()
{
  const bool raw = bufferFullRaw();
  bufferFullStable = raw;
  bufferFullHighAccumMs = raw ? BUFFER_FULL_ON_FILTER_MS : 0;
  bufferFullLowAccumMs = raw ? 0 : BUFFER_FULL_OFF_FILTER_MS;
  bufferFullFilterLastMs = millis();
}

static void updateBufferFullFilter()
{
  const uint32_t now = millis();
  uint32_t dt = (uint32_t)(now - bufferFullFilterLastMs);
  bufferFullFilterLastMs = now;
  if (dt > 50) dt = 50;  // anti-salto tras bloqueo largo del loop

  const bool raw = bufferFullRaw();
  if (raw)
  {
    bufferFullLowAccumMs = 0;
    if (bufferFullHighAccumMs < BUFFER_FULL_ON_FILTER_MS)
    {
      bufferFullHighAccumMs += dt;
      if (bufferFullHighAccumMs > BUFFER_FULL_ON_FILTER_MS)
        bufferFullHighAccumMs = BUFFER_FULL_ON_FILTER_MS;
    }
    if (bufferFullHighAccumMs >= BUFFER_FULL_ON_FILTER_MS)
      bufferFullStable = true;
  }
  else
  {
    bufferFullLowAccumMs += dt;
    if (bufferFullLowAccumMs >= BUFFER_FULL_GLITCH_MS)
      bufferFullHighAccumMs = 0;
    if (bufferFullLowAccumMs >= BUFFER_FULL_OFF_FILTER_MS)
    {
      bufferFullStable = false;
      bufferFullLowAccumMs = BUFFER_FULL_OFF_FILTER_MS;
    }
  }
}

// Corta DeReeler+servo; reintenta Stop RMT cada 250 ms; remux LEDC tras Stop.
static void forceStopDereelerAndServo()
{
  static uint32_t lastStopMs = 0;
  const bool commanded = motor && fabsf(commandedRpm) > 0.01f;
  const bool stillSpinning = motor && fabsf(motor->currentRPM()) > 1.0f;

  servoHardStopNeutral(false);

  if (commanded || (stillSpinning && (uint32_t)(millis() - lastStopMs) >= 250u))
  {
    motor->Stop();
    lastStopMs = millis();
    servoMarkPinDirty();
    servoHardStopNeutral(true);
  }
  commandedRpm = 0.0f;
  tensionReverseUntilMs = 0;
  dereelerDirCw();
}

static bool bufferMaxActive()
{
  return digitalRead(PIN_SENSOR_BUFFER_MAX) == HIGH;
}

static bool tensionSensorActive()
{
  return digitalRead(PIN_SENSOR_TENSION) == HIGH;
}

static bool cylinderOpenActive()
{
  return digitalRead(PIN_SENSOR_CILINDRO) == HIGH;
}

static bool hoseBeltAbsentActive()
{
  return digitalRead(PIN_SENSOR_HOSE_BELT) == HIGH;
}

static bool refillOverrideActive()
{
  return refillDereelerOn || refillServoOn || refillFeederOn;
}

// Materialista + pulso manual: Buffer Full / Max / falta sensor no cortan el movimiento.
static bool refillManualActive()
{
  return idleMode && refillOverrideActive();
}

static void refillClearFlags()
{
  if (refillFeederOn)
    motor2AbortRequested = true;
  refillDereelerOn = false;
  refillServoOn = false;
  refillFeederOn = false;
  refillDereelerPulseUntilMs = 0;
  refillServoPulseUntilMs = 0;
  refillFeederPulseUntilMs = 0;
}

static uint32_t refillPulseNowMs()
{
  uint32_t ms = refillPulseMs;
  if (ms < REFILL_PULSE_MS_MIN) ms = REFILL_PULSE_MS_MIN;
  if (ms > REFILL_PULSE_MS_MAX) ms = REFILL_PULSE_MS_MAX;
  return ms;
}

static void applyRefillPulseSec(float sec)
{
  if (sec < 0.2f) sec = 0.2f;
  if (sec > 10.0f) sec = 10.0f;
  refillPulseMs = (uint32_t)(sec * 1000.0f + 0.5f);
  if (refillPulseMs < REFILL_PULSE_MS_MIN) refillPulseMs = REFILL_PULSE_MS_MIN;
  if (refillPulseMs > REFILL_PULSE_MS_MAX) refillPulseMs = REFILL_PULSE_MS_MAX;
}

static void applyRefillOutputs();

static bool tensionCooldownReady()
{
  if (tensionCooldownSec <= 0.0f)
    return true;
  if (tensionLastRoutineMs == 0)
    return true;
  const uint32_t waitMs = (uint32_t)(tensionCooldownSec * 1000.0f);
  return (uint32_t)(millis() - tensionLastRoutineMs) >= waitMs;
}

static void updateTensionReverseFilter()
{
  const bool raw = tensionSensorActive();
  if (!raw)
  {
    tensionReverseHighSinceMs = 0;
    tensionReverseStable = false;
    return;
  }
  if (tensionReverseHighSinceMs == 0)
    tensionReverseHighSinceMs = millis();
  else if ((uint32_t)(millis() - tensionReverseHighSinceMs) >= TENSION_REVERSE_FILTER_MS)
    tensionReverseStable = true;
}

static bool tensionRoutineBlocked()
{
  return tensionSensorActive() && !tensionCooldownReady();
}

static bool tensionReverseIsActive()
{
  return tensionReverseUntilMs != 0;
}

// DeReeler debe estar (o querer estar) en marcha: auto, Materialista refill o jog.
static bool dereelerWantedRunning()
{
  if (systemFault != FAULT_NONE)
    return false;
  if (idleMode)
    return refillDereelerOn;
  if (!autoEnabled)
    return fabsf(commandedRpm) > 0.01f;
  if (!sensorsMotionArmed())
    return false;
  if (bufferFullStopNow())
    return false;
  return autoState == AUTO_SERVO_LEAD || autoState == AUTO_CW || autoState == AUTO_CCW
      || fabsf(commandedRpm) > 0.01f;
}

static bool dereelerIsSpinning()
{
  if (fabsf(commandedRpm) > 0.01f)
    return true;
  return motor && fabsf(motor->currentRPM()) > 1.0f;
}

static bool tensionCanTriggerReverse()
{
  return !tensionReverseIsActive()
      && tensionReverseStable
      && tensionCooldownReady()
      && dereelerWantedRunning()
      && dereelerIsSpinning();
}

static void tensionMarkReverseTriggered()
{
  tensionLastRoutineMs = millis();
}

static void stopAllMotors();
static void enterSystemFault(SystemFault fault, bool pushPeer = true);
static void peerPushNow(bool fullStatus);
static void updateTensionFaultMonitor();
static void updateBufferRefillFaultMonitor();

static StepperRMT* motorByIndex(uint8_t idx)
{
  return (idx == 0) ? motor : motor2;
}

static float* commandedRpmByIndex(uint8_t idx)
{
  return (idx == 0) ? &commandedRpm : &commandedRpm2;
}

static int motorPinStep(uint8_t idx)  { return (idx == 0) ? PIN_DEREELER_PUL  : PIN_FEEDER_PUL; }
static int motorPinDir(uint8_t idx)   { return (idx == 0) ? PIN_DEREELER_MOSFET : -1; }

// GPIO33 fuera de StepperRMT: CW=LOW; inversión por tensión=HIGH.
static bool dereelerDirIsReverse = false;

static void dereelerDirApply(bool reverse)
{
  const uint8_t level = reverse ? DEREELER_DIR_CCW_LEVEL : DEREELER_DIR_CW_LEVEL;
  pinMode(PIN_DEREELER_MOSFET, OUTPUT);
  digitalWrite(PIN_DEREELER_MOSFET, level);
  if (dereelerDirIsReverse != reverse)
  {
    dereelerDirIsReverse = reverse;
    Serial.printf("DeReeler DIR GPIO%u → %s (%s)\n",
                  (unsigned)PIN_DEREELER_MOSFET,
                  level == HIGH ? "HIGH" : "LOW",
                  reverse ? "inversion" : "CW");
  }
  else
    dereelerDirIsReverse = reverse;
}

static void dereelerDirCw()
{
  dereelerDirApply(false);
}

static void dereelerDirReverse()
{
  dereelerDirApply(true);
}

static const char* motorDirName(uint8_t idx)
{
  const float rpm = *commandedRpmByIndex(idx);
  if (fabsf(rpm) < 0.01f) return "stop";
  return rpm > 0.0f ? "cw" : "ccw";
}

static bool motorWaitStopped(uint8_t idx, uint16_t timeoutMs = 200)
{
  StepperRMT* m = motorByIndex(idx);
  const uint32_t t0 = millis();
  while (fabsf(m->currentRPM()) > 1.0f && (millis() - t0) < timeoutMs)
  {
    server.handleClient();
    delay(1);
  }
  return fabsf(m->currentRPM()) <= 1.0f;
}

// StepperRMT: signo → DIR interno. DeReeler: DIR físico lo pone GPIO33 a mano
// (pins.dir = -1); el signo de drive solo mantiene el mismo sentido de pulsos CW.
static float uiSignedToDriveRpm(float uiSigned, uint8_t idx = 1)
{
  const float mag = fabsf(uiSigned);
  if (idx == 0)
    return -mag;  // pulsos como el CW histórico; sentido = GPIO33
  return (uiSigned > 0.0f) ? -mag : mag;
}

static void motorHardStop(uint8_t idx)
{
  StepperRMT* m = motorByIndex(idx);
  m->Stop();
  servoMarkPinDirty();
  motorWaitStopped(idx);
  *commandedRpmByIndex(idx) = 0.0f;
  if (idx == 0)
    dereelerDirCw();
}

static bool motorStartSigned(uint8_t idx, float uiSignedRpm)
{
  StepperRMT* m = motorByIndex(idx);
  const float driveRpm = uiSignedToDriveRpm(uiSignedRpm, idx);

  if (fabsf(*commandedRpmByIndex(idx)) > 0.01f || fabsf(m->currentRPM()) > 1.0f)
  {
    motorHardStop(idx);
    delay(MOTOR_DIR_SETUP_MS);
  }

  // DeReeler: fijar DIR antes de pulsos (para → DIR → gira).
  if (idx == 0)
  {
    dereelerDirApply(uiSignedRpm < -0.01f);
    delay(MOTOR_DIR_SETUP_MS);
  }

  if (!m->setSpeed(driveRpm, MOTOR_ACCEL))
  {
    motorHardStop(idx);
    delay(MOTOR_DIR_SETUP_MS);
    if (idx == 0)
    {
      dereelerDirApply(uiSignedRpm < -0.01f);
      delay(MOTOR_DIR_SETUP_MS);
    }
    if (!m->setSpeed(driveRpm, MOTOR_ACCEL))
      return false;
  }

  *commandedRpmByIndex(idx) = uiSignedRpm;
  servoMarkPinDirty();
  if (servoShouldRunAuto() || (idleMode && refillServoOn))
    servoAssertRun(true);
  else
    servoAssertNeutral(true);
  if (motorPinDir(idx) >= 0)
    DBG_PRINTF("Motor%u UI=%.1f → setSpeed(%.1f) MOSFET_GPIO=%s\n",
                  (unsigned)(idx + 1), uiSignedRpm, driveRpm,
                  digitalRead(motorPinDir(idx)) ? "HIGH" : "LOW");
  else
    DBG_PRINTF("Motor%u UI=%.1f → setSpeed(%.1f)\n",
                  (unsigned)(idx + 1), uiSignedRpm, driveRpm);
  return true;
}

static bool motorRun(uint8_t idx, float signedRpm)
{
  if (fabsf(signedRpm) < 0.01f)
  {
    // Evitar Stop/Brake repetidos: el driver RMT spamea errores si ya está idle.
    if (fabsf(*commandedRpmByIndex(idx)) > 0.01f)
      motorHardStop(idx);
    else
      *commandedRpmByIndex(idx) = 0.0f;
    return true;
  }

  float rpm = fabsf(signedRpm);
  if (rpm < MOTOR_RPM_MIN) rpm = MOTOR_RPM_MIN;
  if (rpm > MOTOR_RPM_MAX) rpm = MOTOR_RPM_MAX;
  const float target = (signedRpm > 0.0f) ? rpm : -rpm;

  // Ya en régimen: no Stop()+setSpeed (auto/refill lo llamaban cada loop → arrastre).
  if (fabsf(*commandedRpmByIndex(idx) - target) <= 0.5f)
    return true;

  StepperRMT* m = motorByIndex(idx);
  const float cur = *commandedRpmByIndex(idx);
  if (fabsf(cur) > 0.01f && ((cur > 0.0f) == (target > 0.0f)))
  {
    if (m->setSpeed(uiSignedToDriveRpm(target, idx), MOTOR_ACCEL))
    {
      *commandedRpmByIndex(idx) = target;
      servoMarkPinDirty();
      if (servoShouldRunAuto() || (idleMode && refillServoOn))
        servoAssertRun(true);
      else
        servoAssertNeutral(true);
      return true;
    }
  }

  return motorStartSigned(idx, target);
}

static bool motorRun(float signedRpm)
{
  return motorRun(0, signedRpm);
}

// RPM UI de inversión (CCW = negativo). Parar→DIR→girar vía motorStartSigned.
static float autoDereelerReverseRpm()
{
  float rpm = tensionReverseRpm;
  if (rpm < MOTOR_RPM_MIN) rpm = MOTOR_RPM_MIN;
  if (rpm > MOTOR_RPM_MAX) rpm = MOTOR_RPM_MAX;
  return -rpm;
}

// Refill / CW nominal: siempre sentido normal (positivo).
static float autoDereelerTargetRpm()
{
  return autoRpm;
}

static void motorRunAutoDereeler()
{
  motorRun(autoDereelerTargetRpm());
}

static void motorRunAutoDereelerReverse()
{
  motorRun(autoDereelerReverseRpm());
}

static void beginTensionReverse()
{
  const uint32_t now = millis();
  tensionReverseUntilMs = now + (uint32_t)(autoReverseSec * 1000.0f);
  tensionMarkReverseTriggered();
  motorRunAutoDereelerReverse();
  if (!idleMode && (autoState == AUTO_CW || autoState == AUTO_SERVO_LEAD || autoState == AUTO_CCW))
    autoState = AUTO_CCW;
  Serial.printf("TENSION: inversion ON GPIO%u HIGH · %.0f RPM · %.1fs (%s)\n",
                (unsigned)PIN_DEREELER_MOSFET, tensionReverseRpm, autoReverseSec,
                idleMode ? "Materialista" : "Auto");
}

static void endTensionReverseToCw()
{
  const bool was = tensionReverseIsActive();
  tensionReverseUntilMs = 0;
  if (!dereelerWantedRunning())
  {
    if (fabsf(commandedRpm) > 0.01f)
      motorRun(0.0f);
    else
      dereelerDirCw();
    if (!idleMode && autoState == AUTO_CCW)
      autoState = AUTO_HOME_HOLD;
    if (was)
      Serial.println("TENSION: inversion OFF GPIO33 LOW (DeReeler parado)");
    return;
  }
  motorRunAutoDereeler();
  if (!idleMode && autoState == AUTO_CCW)
    autoState = AUTO_CW;
  if (was)
    Serial.println("TENSION: inversion OFF GPIO33 LOW -> CW");
}

// Independiente del modo: si DeReeler gira y hay tensión → GPIO33 HIGH / CCW.
// Fin: tiempo agotado (autoReverseSec) → GPIO33 LOW / CW (sigue girando si aplica).
// La lectura del sensor durante el pulso no acorta la inversión; el timeout E054/E060
// sigue en updateTensionFaultMonitor() si GPIO23 permanece HIGH ≥ tensionFaultSec.
static void serviceTensionReverse()
{
  if (tensionReverseIsActive())
  {
    const bool timeUp = (int32_t)(millis() - tensionReverseUntilMs) >= 0;
    if (timeUp || !dereelerWantedRunning())
    {
      endTensionReverseToCw();
      return;
    }
    motorRunAutoDereelerReverse();
    return;
  }

  if (tensionCanTriggerReverse())
    beginTensionReverse();
}

// Secuencia fija (Start / Resume / buffer vacío mid-ciclo):
//   1) servo ON ya
//   2) DeReeler CW tras DEREELER_START_DELAY_MS
// Full HIGH → forceStop al instante. Full OFF ~200 ms → otra vez esta secuencia.
// Vacío no es EXXX: se sigue alimentando. Solo falla enclavada para el auto.
static void beginAutoCwWithServoLead()
{
  // Protección local para el arranque automático que escribe PWM directamente.
  if (!idleMode && bufferFullStopNow())
  {
    autoState = AUTO_HOME_HOLD;
    servoHardStopNeutral(true);
    return;
  }

  servoLeadStartMs = millis();
  autoState = AUTO_SERVO_LEAD;
  // Arranque inmediato del RC (Forced: no depender de cache si forceStop dejó neutro).
  servoWriteUsForced(servoMotionPwmUs());
  servoRunning = true;
  DBG_PRINTLN("AUTO: Buffer Full inactivo -> servo lead");
}

// Rearme DeReeler/servo si la ventana de relleno está abierta.
static void resumeAutoFromSensors()
{
  if (!autoEnabled || systemFault != FAULT_NONE || idleMode || refillOverrideActive())
    return;
  if (!sensorsMotionArmed())
  {
    forceStopDereelerAndServo();
    autoState = AUTO_HOME_HOLD;
    syncServoToAutoState();
    return;
  }
  if (bufferMaxActive())
  {
    enterSystemFault(FAULT_ENDSTOP);
    return;
  }
  if (bufferFullActive())
  {
    forceStopDereelerAndServo();
    autoState = AUTO_HOME_HOLD;
    syncServoToAutoState();
    return;
  }
  // Ya rellenando (HTML Iniciar o Start HMI): In process no debe restart/forceStop.
  // Eso apagaba el servo y dejaba el DeReeler.
  if (autoState == AUTO_SERVO_LEAD || autoState == AUTO_CW || autoState == AUTO_CCW)
  {
    syncServoToAutoState();
    return;
  }
  if (bufferFullAllowsMotion())
    beginAutoCwWithServoLead();
  else
  {
    autoState = AUTO_HOME_HOLD;
    syncServoToAutoState();
  }
}

static void requestFillUntilReady()
{
  if (idleMode || systemFault != FAULT_NONE || !autoEnabled)
    return;
  if (bufferFullActive() && motor2Phase == M2_PHASE_IDLE)
  {
    fillUntilReady = false;
    return;
  }
  fillUntilReady = true;
  DBG_PRINTLN("FILL: open");
  resumeAutoFromSensors();
}

static void clearFillUntilReady(const char* reason)
{
  if (!fillUntilReady) return;
  fillUntilReady = false;
  DBG_PRINTF("FILL: close (%s)\n", reason ? reason : "");
}

static void serviceFillUntilReady()
{
  if (!fillUntilReady) return;
  if (idleMode || systemFault != FAULT_NONE || !autoEnabled)
  {
    clearFillUntilReady("abort");
    return;
  }
  if (tcmInProcess)
    return;
  if (bufferFullActive() && motor2Phase == M2_PHASE_IDLE)
  {
    clearFillUntilReady("ready");
    if (fabsf(commandedRpm) > 0.01f)
      motorRun(0.0f);
    autoState = AUTO_HOME_HOLD;
    motor2AbortRequested = true;
    syncServoToAutoState();
  }
}

static void refillExpireChannels(uint32_t now)
{
  const bool servoWasOn = refillServoOn;
  auto expire = [&](volatile bool& onFlag, volatile uint32_t& pulseUntilMs) {
    if (!onFlag || pulseUntilMs == 0)
      return;
    if ((int32_t)(now - pulseUntilMs) >= 0)
    {
      onFlag = false;
      pulseUntilMs = 0;
    }
  };
  expire(refillDereelerOn, refillDereelerPulseUntilMs);
  expire(refillServoOn, refillServoPulseUntilMs);
  expire(refillFeederOn, refillFeederPulseUntilMs);
  if (servoWasOn && !refillServoOn)
    Serial.println("REFILL servo OFF (pulso expirado)");
}

static void refillHaltOutputs()
{
  forceStopDereelerAndServo();
  motor2AbortRequested = true;
}

static void applyRefillOutputs()
{
  refillExpireChannels(millis());

  // RC primero: Stop/setSpeed del DeReeler suelta LEDC; el 1500 posterior se pierde.
  if (refillServoOn)
    servoAssertRun(true);
  else
    servoAssertNeutral(true);

  if (refillDereelerOn)
  {
    const float want = tensionReverseIsActive()
      ? autoDereelerReverseRpm()
      : autoDereelerTargetRpm();
    if (fabsf(commandedRpm - want) > 0.5f)
      motorRun(want);
  }
  else if (fabsf(commandedRpm) > 0.01f || tensionReverseIsActive())
  {
    tensionReverseUntilMs = 0;
    if (motor)
      motor->Stop();
    commandedRpm = 0.0f;
    dereelerDirCw();
    servoMarkPinDirty();
  }

  if (refillServoOn)
    servoAssertRun(true);
  else
    servoAssertNeutral(true);

  // Feeder se aplica en motor2FeederTask vía refillFeederOn.
  if (!refillFeederOn)
    motor2AbortRequested = true;
}

static void refillStopChannel(volatile bool& onFlag, volatile uint32_t& pulseUntilMs)
{
  onFlag = false;
  pulseUntilMs = 0;
}

// Clic = pulso de refillPulseMs (misma duración para todos). Relanza el timer.
static bool refillStartChannel(
  volatile bool& onFlag,
  volatile uint32_t& pulseUntilMs,
  bool hold
)
{
  const bool wasOn = onFlag;
  onFlag = true;
  // Hold remoto: permanece ON hasta recibir el comando OFF.
  pulseUntilMs = hold ? 0 : millis() + refillPulseNowMs();
  return !wasOn;
}

static void serviceRefillPulses()
{
  if (!idleMode)
  {
    if (refillOverrideActive())
    {
      refillClearFlags();
      refillHaltOutputs();
    }
    return;
  }

  const bool wasActive = refillOverrideActive();
  refillExpireChannels(millis());

  if (refillOverrideActive() || wasActive)
    applyRefillOutputs();
}

// which: "material" | "dereeler" | "servo" | "feeder".
// on=true → un pulso de refillPulseMs (misma duración; relanza el timer).
// on=false → apaga de inmediato.
static bool applyRefillCommand(
  const String& which, bool on, bool hold = false
)
{
  if (!idleMode)
  {
    if (systemFault != FAULT_NONE)
    {
      refillClearFlags();
      return false;
    }
    if (on)
      return false;
  }

  if (which == "material")
  {
    if (on)
    {
      refillStartChannel(refillDereelerOn, refillDereelerPulseUntilMs, hold);
      refillStartChannel(refillServoOn, refillServoPulseUntilMs, hold);
      refillStartChannel(refillFeederOn, refillFeederPulseUntilMs, hold);
    }
    else
    {
      refillStopChannel(refillDereelerOn, refillDereelerPulseUntilMs);
      refillStopChannel(refillServoOn, refillServoPulseUntilMs);
      refillStopChannel(refillFeederOn, refillFeederPulseUntilMs);
    }
  }
  else if (which == "dereeler")
  {
    if (on)
      refillStartChannel(refillDereelerOn, refillDereelerPulseUntilMs, hold);
    else
      refillStopChannel(refillDereelerOn, refillDereelerPulseUntilMs);
  }
  else if (which == "servo")
  {
    if (on)
    {
      if (refillStartChannel(refillServoOn, refillServoPulseUntilMs, hold))
      {
        Serial.printf("REFILL servo ON %u us (cfg %u, neutro %u)\n",
                      (unsigned)servoMotionPwmUs(), (unsigned)servoActivePwmUs,
                      (unsigned)SERVO_PWM_NEUTRAL_US);
      }
    }
    else
      refillStopChannel(refillServoOn, refillServoPulseUntilMs);
  }
  else if (which == "feeder")
  {
    if (on)
      refillStartChannel(refillFeederOn, refillFeederPulseUntilMs, hold);
    else
      refillStopChannel(refillFeederOn, refillFeederPulseUntilMs);
  }
  else
    return false;

  if (refillOverrideActive())
    applyRefillOutputs();
  else
  {
    refillHaltOutputs();
    if (!idleMode && autoEnabled && systemFault == FAULT_NONE && sensorsMotionArmed())
      resumeAutoFromSensors();
  }
  return true;
}

static const char* autoStateName()
{
  return pfAutoPhaseName(autoState);
}

static uint8_t systemFaultCode()
{
  return pfErrorWireCodeFromFault(systemFault, FAULT_CODE_BASE);
}

// Detener: para motores y queda Idle. No es fallo — no enclava PF-007 ni ErrorState.
// Sensor / timeout sí enclavan EXXX y piden Reset + Iniciar.
static void autoDisable()
{
  motor2ClearTfeedIncompleteLatch();
  tcmInProcess = false;
  refillClearFlags();
  clearFillUntilReady("detener");
  stopAllMotors();
  autoEnabled = false;
  if (systemFault == FAULT_NONE)
    autoState = AUTO_OFF;
  syncServoToAutoState();
  if (peerLinkOk)
    peerPushNow(true);
}

static void autoEnable(bool openFillWindow = true)
{
  if (systemFault != FAULT_NONE)
    return;

  refillClearFlags();
  autoEnabled = true;
  if (idleMode)
  {
    motorRun(0.0f);
    autoState = AUTO_HOME_HOLD;
    syncServoToAutoState();
    return;
  }
  // GPIO 27 se ignora en Materialista (refill operador ↔ prefeeder).
  if (hoseBeltAbsentActive())
  {
    enterSystemFault(FAULT_HOSE_ABSENT);
    return;
  }
  if (cylinderOpenActive())
  {
    enterSystemFault(FAULT_CYLINDER_OPEN);
    return;
  }
  if (bufferMaxActive())
  {
    enterSystemFault(FAULT_ENDSTOP);
    return;
  }

  // Idle (quieto): one-shot Iniciar o In process del TCM.
  if (tcmInProcess)
    resumeAutoFromSensors();
  else if (openFillWindow)
    requestFillUntilReady();
  else
  {
    autoState = AUTO_HOME_HOLD;
    syncServoToAutoState();
  }
}

static void applyIdleMode(bool on)
{
  if (idleMode == on)
    return;

  idleMode = on;
  if (on)
  {
    // Manual: sensores (Max/Full/…) no deben dejar el refill trabado.
    if (systemFault != FAULT_NONE && systemFault != FAULT_OPERATOR_STOP)
    {
      systemFault = FAULT_NONE;
      autoState = autoEnabled ? AUTO_HOME_HOLD : AUTO_OFF;
    }
    if (!refillOverrideActive())
    {
      if (fabsf(commandedRpm) > 0.01f)
        motorRun(0.0f);
      motor2AbortRequested = true;
      if (autoEnabled && systemFault == FAULT_NONE)
        autoState = AUTO_HOME_HOLD;
      syncServoToAutoState();
    }
    clearFillUntilReady("materialista");
    Serial.println("MODE: Materialista");
  }
  else
  {
    if (refillOverrideActive())
    {
      refillClearFlags();
      refillHaltOutputs();
    }
    Serial.println("MODE: Idle");
    if (hoseBeltAbsentActive())
      enterSystemFault(FAULT_HOSE_ABSENT);
    else if (tcmInProcess && autoEnabled && systemFault == FAULT_NONE)
      resumeAutoFromSensors();
    else if (autoEnabled && systemFault == FAULT_NONE)
    {
      autoState = AUTO_HOME_HOLD;
      syncServoToAutoState();
    }
    else
    {
      if (fabsf(commandedRpm) > 0.01f)
        motorRun(0.0f);
      motor2AbortRequested = true;
      if (autoEnabled && systemFault == FAULT_NONE)
        autoState = AUTO_HOME_HOLD;
      syncServoToAutoState();
    }
  }
  if (peerLinkOk)
    peerPushNow(true);
}

static void autoReset()
{
  // Reset desde TCM/UI: cortar refill/motores siempre; luego liberar falla si aplica.
  refillClearFlags();
  motorRun(0.0f);
  servoStop();
  motor2AbortRequested = true;
  clearFillUntilReady("reset");

  if (systemFault == FAULT_NONE)
  {
    if (autoEnabled)
      autoState = AUTO_HOME_HOLD;
    syncServoToAutoState();
    if (peerLinkOk)
      peerPushNow(true);
    return;
  }

  const SystemFault prev = systemFault;
  const char* prevName = pfErrorSlugFromFault(prev);

  if (prev == FAULT_ENDSTOP)
  {
    if (bufferMaxActive())
      return;
  }
  else if (prev == FAULT_TENSION_TIMEOUT)
  {
    if (tensionSensorActive())
      return;
  }
  else if (prev == FAULT_CYLINDER_OPEN)
  {
    if (cylinderOpenActive())
      return;
  }
  else if (prev == FAULT_HOSE_ABSENT)
  {
    if (!idleMode && hoseBeltAbsentActive())
      return;
  }
  else if (prev == FAULT_BUFFER_TIMEOUT)
  {
    // Buffer vacío enclavado: se permite Reset; Iniciar reintenta el relleno.
  }
  else if (prev == FAULT_OPERATOR_STOP)
  {
    // Parada operador (Detener): Reset libre; luego Iniciar.
  }
  else
    return;

  systemFault = FAULT_NONE;
  if (prev == FAULT_TENSION_TIMEOUT)
    tensionActiveSinceMs = 0;
  bufferEmptySinceMs = 0;
  bufferFullRecoverSinceMs = 0;

  clearFillUntilReady("reset");
  autoEnabled = false;
  autoState = AUTO_OFF;
  stopAllMotors();
  syncServoToAutoState();
  Serial.printf("RESET OK (%s)\n", prevName);
  if (peerLinkOk)
    peerPushNow(true);
}

// Logica que corre siempre en loop() cuando autoEnabled
static void serviceAuto()
{
  if (idleMode)
  {
    if (refillOverrideActive())
    {
      applyRefillOutputs();
      serviceTensionReverse();
    }
    else
    {
      if (tensionReverseIsActive())
        endTensionReverseToCw();
      if (fabsf(commandedRpm) > 0.01f)
        motorRun(0.0f);
      syncServoToAutoState();
    }
    return;
  }

  if (systemFault != FAULT_NONE)
  {
    if ((motor && fabsf(motor->currentRPM()) > 1.0f)
        || fabsf(commandedRpm) > 0.01f
        || servoRunning
        || servoLastOutputUs != SERVO_PWM_NEUTRAL_US)
      forceStopDereelerAndServo();
    else
      syncServoToAutoState();
    return;
  }

  if (!autoEnabled)
  {
    if ((motor && fabsf(motor->currentRPM()) > 1.0f)
        || fabsf(commandedRpm) > 0.01f
        || servoRunning
        || servoLastOutputUs != SERVO_PWM_NEUTRAL_US)
      forceStopDereelerAndServo();
    else
      syncServoToAutoState();
    return;
  }

  // Idle / sin ventana: quieto. forceStop: RMT a veces ignora motorRun(0)
  // y el DeReeler sigue con el servo ya en neutro.
  if (!sensorsMotionArmed())
  {
    forceStopDereelerAndServo();
    if (autoState == AUTO_SERVO_LEAD || autoState == AUTO_CW || autoState == AUTO_CCW)
      autoState = AUTO_HOME_HOLD;
    syncServoToAutoState();
    serviceFillUntilReady();
    return;
  }

  serviceFillUntilReady();
  if (!sensorsMotionArmed())
  {
    forceStopDereelerAndServo();
    if (autoState == AUTO_SERVO_LEAD || autoState == AUTO_CW || autoState == AUTO_CCW)
      autoState = AUTO_HOME_HOLD;
    syncServoToAutoState();
    return;
  }

  if (autoState == AUTO_OFF)
  {
    resumeAutoFromSensors();
    if (autoState == AUTO_OFF || systemFault != FAULT_NONE)
    {
      syncServoToAutoState();
      return;
    }
  }

  if (bufferFullStopNow())
  {
    servoTimingOnForceStopFromBufferFull();
    forceStopDereelerAndServo();
    if (autoState == AUTO_SERVO_LEAD || autoState == AUTO_CW || autoState == AUTO_CCW
        || autoState == AUTO_HOME_HOLD)
    {
      if (autoState != AUTO_HOME_HOLD)
        DBG_PRINTLN("AUTO: Buffer Full -> HOME_HOLD (force stop)");
      autoState = AUTO_HOME_HOLD;
    }
    syncServoToAutoState();
    return;
  }

  serviceTensionReverse();

  switch (autoState)
  {
    case AUTO_ENDSTOP_FAULT:
    case AUTO_TENSION_FAULT:
    case AUTO_CYLINDER_FAULT:
    case AUTO_HOSE_FAULT:
    case AUTO_BUFFER_FAULT:
    case AUTO_OPERATOR_STOP:
      forceStopDereelerAndServo();
      syncServoToAutoState();
      return;

    case AUTO_HOME_HOLD:
      if (bufferFullAllowsMotion())
        beginAutoCwWithServoLead();
      break;

    case AUTO_SERVO_LEAD:
      servoAssertRun(true);
      if ((uint32_t)(millis() - servoLeadStartMs) >= DEREELER_START_DELAY_MS)
      {
        motorRunAutoDereeler();
        autoState = AUTO_CW;
        servoAssertRun(true);
        DBG_PRINTF("AUTO: servo lead %lums -> DeReeler CW\n", (unsigned long)DEREELER_START_DELAY_MS);
      }
      break;

    case AUTO_CW:
      if (!tensionReverseIsActive())
        motorRunAutoDereeler();
      servoAssertRun(true);
      break;

    case AUTO_CCW:
      servoAssertRun(true);
      if (tensionReverseIsActive())
        motorRunAutoDereelerReverse();
      else
        motorRunAutoDereeler();
      break;

    default:
      break;
  }

  syncServoToAutoState();
}

static void jsonAppendUInt(String& j, uint32_t v)
{
  char buf[12];
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)v);
  j += buf;
}

static void jsonAppendFloat(String& j, float v, uint8_t decimals = 1)
{
  if (isnan(v) || isinf(v))
  {
    j += "0";
    return;
  }
  char buf[24];
  switch (decimals)
  {
    case 0: snprintf(buf, sizeof(buf), "%.0f", v); break;
    case 2: snprintf(buf, sizeof(buf), "%.2f", v); break;
    case 3: snprintf(buf, sizeof(buf), "%.3f", v); break;
    default: snprintf(buf, sizeof(buf), "%.1f", v); break;
  }
  if (buf[0] == '.' || buf[0] == '-')
  {
    // JSON exige dígito antes del punto (snprintf puede emitir ".100").
    if (buf[0] == '.' && buf[1])
    {
      char fixed[24];
      snprintf(fixed, sizeof(fixed), "0%s", buf);
      j += fixed;
      return;
    }
    if (buf[0] == '-' && buf[1] == '.' && buf[2])
    {
      char fixed[24];
      snprintf(fixed, sizeof(fixed), "-0%s", buf + 1);
      j += fixed;
      return;
    }
  }
  j += buf;
}

static void jsonAppendStrC(String& j, const char* s)
{
  j += '"';
  if (s)
  {
    for (const char* p = s; *p; p++)
    {
      if (*p == '"' || *p == '\\') j += '\\';
      if (*p != '\n' && *p != '\r') j += *p;
    }
  }
  j += '"';
}

static void jsonAppendDirChar(String& j, char dir)
{
  j += '"';
  j += (dir == 'E' || dir == 'R') ? dir : '-';
  j += '"';
}

static void appendMotorObject(String& json, uint8_t idx, const char* key)
{
  StepperRMT* m = motorByIndex(idx);
  const float rpmCmd = *commandedRpmByIndex(idx);

  json += "\"";
  json += key;
  json += "\":{\"dir\":\"";
  json += motorDirName(idx);
  json += "\",\"rpm\":";
  jsonAppendFloat(json, fabsf(rpmCmd), 1);
  json += ",\"signed_rpm\":";
  jsonAppendFloat(json, rpmCmd, 1);
  json += ",\"drive_rpm\":";
  jsonAppendFloat(json, uiSignedToDriveRpm(rpmCmd, idx), 1);
  json += ",\"current_rpm\":";
  jsonAppendFloat(json, m->currentRPM(), 1);
  json += ",\"target_rpm\":";
  jsonAppendFloat(json, m->targetRPM(), 1);
  json += ",\"enabled\":";
  json += m->isEnabled() ? "true" : "false";
  if (motorPinDir(idx) >= 0)
  {
    json += ",\"dir_pin_high\":";
    json += digitalRead(motorPinDir(idx)) ? "true" : "false";
  }
  json += ",\"step_pin_high\":";
  json += digitalRead(motorPinStep(idx)) ? "true" : "false";
  json += "}";
}

static const char* motor2FeedSourceName(Motor2FeedSource src)
{
  switch (src)
  {
    case M2_FEED_TCP:     return "tcp";
    default:              return "none";
  }
}

static const char* motor2PhaseName(Motor2Phase phase)
{
  switch (phase)
  {
    case M2_PHASE_TIMED_FEED: return "timed_feed";
    default:                  return "idle";
  }
}

static void appendTrigger2Object(String& json)
{
  const bool triggerActive = (motor2Phase == M2_PHASE_TIMED_FEED);
  json += "\"trigger2\":{\"trigger_active\":";
  json += triggerActive ? "true" : "false";
  json += ",\"trigger_via\":\"";
  json += motor2FeedSourceName(motor2FeedSource);
  json += "\"";
  json += ",\"phase\":\"";
  json += motor2PhaseName(motor2Phase);
  json += "\",\"feed_source\":\"";
  json += motor2FeedSourceName(motor2FeedSource);
  json += "\",\"trigger_feed_s\":";
  jsonAppendFloat(json, motor2TriggerFeedSec, 2);
  json += ",\"peer_link_ok\":";
  json += peerLinkOk ? "true" : "false";
  json += "}";
}

static float motor2ClampRpm(float signedRpm)
{
  float mag = fabsf(signedRpm);
  if (mag < MOTOR_RPM_MIN) mag = MOTOR_RPM_MIN;
  if (mag > MOTOR_RPM_MAX) mag = MOTOR_RPM_MAX;
  return (signedRpm >= 0.0f) ? mag : -mag;
}

static bool motor2WaitStoppedTask(uint16_t timeoutMs = 200)
{
  const uint32_t t0 = millis();
  while (fabsf(motor2->currentRPM()) > 1.0f && (millis() - t0) < timeoutMs)
    vTaskDelay(pdMS_TO_TICKS(1));
  return fabsf(motor2->currentRPM()) <= 1.0f;
}

static void motor2StopMotionOnly()
{
  motor2->Stop();
  motor2WaitStoppedTask();
  commandedRpm2 = 0.0f;
}

static void motor2BrakeAtRest()
{
  motor2->Brake();
  motor2WaitStoppedTask();
  commandedRpm2 = 0.0f;
}

static void motor2HardStopTask()
{
  if (fabsf(commandedRpm2) > 0.01f || motor2Phase != M2_PHASE_IDLE)
    motor2BrakeAtRest();
  else
    commandedRpm2 = 0.0f;
  motor2Phase = M2_PHASE_IDLE;
  motor2FeedSource = M2_FEED_NONE;
  motor2ActiveFeedRpm = 0.0f;
  motor2TriggerFeedEndMs = 0;
}

static void stopAllMotors()
{
  forceStopDereelerAndServo();
  motor2AbortRequested = true;
}

static void enterSystemFault(SystemFault fault, bool pushPeer)
{
  if (fault == systemFault)
    return;  // misma falla enclavada: sin re-log ni re-stop
  if (systemFault != FAULT_NONE)
    return;  // otra falla activa: esperar Reset antes de cambiar

  refillClearFlags();
  clearFillUntilReady("falla");
  systemFault = fault;
  autoEnabled = false;  // tras falla: Reset y luego Iniciar
  autoState = pfAutoPhaseFromFault(fault);
  stopAllMotors();
  syncServoToAutoState();
  bufferEmptySinceMs = 0;
  bufferFullRecoverSinceMs = 0;

  Serial.printf("FALTA [%s]: ", pfErrorTagFromFault(fault));
  if (fault == FAULT_BUFFER_TIMEOUT)
    Serial.printf("Buffer Full no rellenó en %.1fs\n", BUFFER_REFILL_FAULT_SEC);
  else if (fault == FAULT_TENSION_TIMEOUT)
    Serial.printf("Tension > %.1fs\n", tensionFaultSec);
  else if (fault == FAULT_HOSE_ABSENT)
    Serial.println("GPIO27 manguera ausente");
  else if (fault == FAULT_OPERATOR_STOP)
    Serial.println("Parada operador (Detener) — Reset + Iniciar");
  else
    Serial.println(pfErrorDescFromFault(fault));

  if (pushPeer && peerLinkOk)
    peerPushNow(true);
}

static void updateBufferMaxFaultMonitor()
{
  if (!bufferMaxActive() || !sensorsMotionArmed())
    return;

  if (systemFault != FAULT_ENDSTOP)
    enterSystemFault(FAULT_ENDSTOP);
  // Si ya está enclavado: no volver a stopAllMotors() cada loop (spam RMT).
}

static void updateCylinderFaultMonitor()
{
  if (!cylinderOpenActive() || !sensorsMotionArmed()) return;
  if (systemFault == FAULT_CYLINDER_OPEN)
    return;
  enterSystemFault(FAULT_CYLINDER_OPEN);
}

static void updateHoseBeltFaultMonitor()
{
  if (idleMode)
  {
    if (systemFault == FAULT_HOSE_ABSENT)
    {
      systemFault = FAULT_NONE;
      if (autoState == AUTO_HOSE_FAULT)
        autoState = autoEnabled ? AUTO_HOME_HOLD : AUTO_OFF;
      if (peerLinkOk)
        peerPushNow(true);
    }
    return;
  }
  if (!hoseBeltAbsentActive() || !sensorsMotionArmed()) return;
  if (systemFault == FAULT_HOSE_ABSENT)
    return;
  enterSystemFault(FAULT_HOSE_ABSENT);
}

// Timeout de falla (E054/E060): GPIO23 HIGH continuo (reloj de pared) ≥ tensionFaultSec.
// Independiente de autoReverseSec (inversión). Si NVS dejó tens_fault ≈ reverse (~2 s),
// migrar al default 10 s — evita EXXX al acabar la inversión con HTML a más segundos.
static void updateTensionFaultMonitor()
{
  if (systemFault != FAULT_NONE)
    return;

  if (!tensionSensorActive())
  {
    tensionActiveSinceMs = 0;
    return;
  }

  const uint32_t limitMs = (uint32_t)(tensionFaultSec * 1000.0f + 0.5f);
  if (limitMs < 1000u)
    return;
  if (tensionActiveSinceMs == 0)
    tensionActiveSinceMs = millis();
  else if ((uint32_t)(millis() - tensionActiveSinceMs) >= limitMs)
  {
    Serial.printf("TENSION FAULT: fault_s=%.1f reverse_s=%.1f high_ms=%lu\n",
                  tensionFaultSec, autoReverseSec,
                  (unsigned long)(millis() - tensionActiveSinceMs));
    enterSystemFault(FAULT_TENSION_TIMEOUT);
  }
}

// Buffer Full consumido: ya no enclava por timeout de relleno (E052/E058).
// DeReeler/servo paran en Full HIGH (crudo o estable) y reanudan tras OFF ~200 ms.
static void updateBufferRefillFaultMonitor()
{
  bufferEmptySinceMs = 0;
  bufferFullRecoverSinceMs = 0;
}

static bool motor2EnsureFeedingAt(float rpm)
{
  const float want = motor2ClampRpm(rpm);
  if (fabsf(commandedRpm2 - want) <= 0.5f && commandedRpm2 > 0.01f)
    return true;

  if (commandedRpm2 > 0.5f && want > 0.0f)
    return motor2ApplySpeedTask(want);

  return motor2StartSignedTask(want);
}

static bool motor2EnsureFeeding()
{
  if (motor2Phase == M2_PHASE_TIMED_FEED && motor2ActiveFeedRpm > 0.01f)
    return motor2EnsureFeedingAt(motor2ActiveFeedRpm);
  return motor2EnsureFeedingAt((float)motor2RpmSetting);
}

static bool motor2StartSignedTask(float uiSignedRpm)
{
  const float target = motor2ClampRpm(uiSignedRpm);
  const float driveRpm = uiSignedToDriveRpm(target);

  if (fabsf(commandedRpm2) > 0.01f || fabsf(motor2->currentRPM()) > 1.0f)
  {
    motor2StopMotionOnly();
    vTaskDelay(pdMS_TO_TICKS(MOTOR_DIR_SETUP_MS));
  }

  if (!motor2->setSpeed(driveRpm, MOTOR2_ACCEL))
  {
    motor2StopMotionOnly();
    vTaskDelay(pdMS_TO_TICKS(MOTOR_DIR_SETUP_MS));
    if (!motor2->setSpeed(driveRpm, MOTOR2_ACCEL))
      return false;
  }

  commandedRpm2 = target;
  servoMarkPinDirty();
  DBG_PRINTF("Motor2 arranque UI=%.1f → setSpeed(%.1f)\n", target, driveRpm);
  return true;
}

static bool motor2ApplySpeedTask(float signedRpm)
{
  const float target = motor2ClampRpm(signedRpm);
  const float driveRpm = uiSignedToDriveRpm(target);

  if (fabsf(commandedRpm2) > 0.01f && target > 0.0f && commandedRpm2 > 0.0f)
  {
    if (motor2->setSpeed(driveRpm, MOTOR2_ACCEL))
    {
      commandedRpm2 = target;
      servoMarkPinDirty();
      return true;
    }
  }

  return motor2StartSignedTask(target);
}

// Misma rutina de feeder temporizado. Diferenciador = source (TCM vs sensor).
static void motor2StartTimedFeed(uint32_t now, Motor2FeedSource src, float rpm, float sec)
{
  if (sec < 0.05f) sec = 0.05f;
  if (sec > M2_TRIGGER_FEED_MAX) sec = M2_TRIGGER_FEED_MAX;
  if (rpm < MOTOR_RPM_MIN) rpm = MOTOR_RPM_MIN;
  if (rpm > MOTOR_RPM_MAX) rpm = MOTOR_RPM_MAX;

  motor2FeedSource = src;
  motor2TriggerActiveSec = sec;
  motor2ActiveFeedRpm = rpm;
  motor2Phase = M2_PHASE_TIMED_FEED;
  motor2TriggerFeedEndMs = now + (uint32_t)(sec * 1000.0f);
  motor2EnsureFeedingAt(rpm);
  Serial.printf("M2: timed_feed src=%s %.0f RPM × %.2fs\n",
                motor2FeedSourceName(src), rpm, sec);
}

static void motor2StartTriggerFeed(uint32_t now)
{
  motor2ClearTfeedIncompleteLatch();
  float sec = (motor2TcpTriggerSecRequest > 0.05f)
                ? (float)motor2TcpTriggerSecRequest
                : (float)motor2TriggerFeedSec;
  motor2TcpTriggerSecRequest = 0.0f;
  const uint16_t trigId = motor2TcpTriggerPendingId;
  motor2TcpTriggerPendingId = 0;
  motor2StartTimedFeed(now, M2_FEED_TCP, (float)motor2RpmSetting, sec);
  triggerIdRemember(trigId);
}

static void motor2ClearPendingTrigger()
{
  motor2TcpTriggerRequest = false;
  motor2TcpTriggerSecRequest = 0.0f;
  motor2TcpTriggerPendingId = 0;
}

static void motor2ClearTfeedIncompleteLatch()
{
  motor2TfeedIncompleteLatch = false;
  motor2TfeedResumeSec = 0.0f;
}

// Solo setInProcess OFF (Pause HMI): latch para retry parcial en Resume.
static void motor2LatchTfeedIncompleteOnHoldOff()
{
  if (motor2Phase == M2_PHASE_TIMED_FEED && motor2TriggerFeedEndMs != 0)
  {
    int32_t left = (int32_t)(motor2TriggerFeedEndMs - millis());
    const uint32_t msLeft = left > 0 ? (uint32_t)left : 0;
    if (msLeft >= 50u)
    {
      motor2TfeedIncompleteLatch = true;
      motor2TfeedResumeSec = msLeft / 1000.0f;
      if (motor2TfeedResumeSec < M2_TRIGGER_FEED_MIN)
        motor2TfeedResumeSec = M2_TRIGGER_FEED_MIN;
      DBG_PRINTF("M2: Tfeed incompleto — resume %.2fs\n", motor2TfeedResumeSec);
    }
    return;
  }
  if (motor2TcpTriggerRequest)
  {
    motor2TfeedIncompleteLatch = true;
    motor2TfeedResumeSec = 0.0f;
    DBG_PRINTLN("M2: Tfeed pendiente abortado (In process OFF)");
  }
}

static void serviceTimedFeeder()
{
  if (motor2Phase != M2_PHASE_TIMED_FEED)
    return;

  const uint32_t now = millis();
  if (motor2TriggerFeedEndMs != 0
      && (int32_t)(now - motor2TriggerFeedEndMs) >= 0)
  {
    const float doneSec = motor2TriggerActiveSec;
    const Motor2FeedSource doneSrc = motor2FeedSource;
    motor2HardStopTask();
    motor2ClearTfeedIncompleteLatch();
    motor2TriggerActiveSec = 0.0f;
    Serial.printf("M2: fin timed_feed src=%s %.2fs\n",
                  motor2FeedSourceName(doneSrc), doneSec);
    return;
  }

  motor2EnsureFeeding();
}

static void motor2FeederTask(void* /*param*/)
{
  motor2HardStopTask();

  for (;;)
  {
    if (motor2AbortRequested)
    {
      motor2ClearPendingTrigger();
      motor2HardStopTask();
      motor2AbortRequested = false;
    }

    if (motor2Phase == M2_PHASE_TIMED_FEED)
      serviceTimedFeeder();

    // DeReeler/servo los mueve solo loop() — RMT no es thread-safe (dos cores = vibra).
    if (refillManualActive())
    {
      motor2ClearPendingTrigger();
      if (refillFeederOn)
        motor2EnsureFeeding();
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }

    if (systemFault != FAULT_NONE)
    {
      motor2ClearPendingTrigger();
      if (motor2Phase != M2_PHASE_IDLE || fabsf(commandedRpm2) > 0.01f
          || (motor2 && fabsf(motor2->currentRPM()) > 1.0f))
        motor2HardStopTask();
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }

    // Buffer Max: cortar feeder en curso. Conservar Tfeed pendiente — al bajar
    // Max arranca (un glitch de Max no debe borrar el relleno de la pieza).
    if (bufferMaxActive())
    {
      if (motor2Phase != M2_PHASE_IDLE || fabsf(commandedRpm2) > 0.01f
          || (motor2 && fabsf(motor2->currentRPM()) > 1.0f))
        motor2HardStopTask();
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }

    // Trigger TCP: arranque requiere ventana armada + Auto ON.
    // In process OFF aborta Tfeed (Pause); pendiente se borra en motor2AbortRequested.
    if (!sensorsMotionArmed() || !autoEnabled)
    {
      if (!sensorsMotionArmed()
          && (motor2Phase != M2_PHASE_IDLE || fabsf(commandedRpm2) > 0.01f
              || (motor2 && fabsf(motor2->currentRPM()) > 1.0f)))
        motor2HardStopTask();
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }

    if (motor2TcpTriggerRequest)
    {
      // Sin enlace: conservar pendiente (blip TCP). Buffer Max ya se filtró arriba.
      if (!peerLinkOk)
      {
        // Esperar enlace; no borrar el Tfeed de ciclo.
      }
      else if (motor2Phase == M2_PHASE_TIMED_FEED)
      {
        // Tfeed en curso: no cortar ni reiniciar timer;
        // el Tfeed pendiente arranca al terminar (evita L/R asimétrico en lote).
      }
      else
      {
        motor2TcpTriggerRequest = false;
        motor2StartTriggerFeed(millis());
      }
    }

    serviceTimedFeeder();

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

static void appendAutoObject(String& json)
{
  json += "\"auto\":{\"enabled\":";
  json += autoEnabled ? "true" : "false";
  json += ",\"state\":\"";
  json += autoStateName();
  json += "\",\"rpm\":";
  jsonAppendFloat(json, autoRpm, 1);
  json += ",\"reverse_s\":";
  jsonAppendFloat(json, autoReverseSec, 1);
  json += ",\"servo_pwm_us\":";
  jsonAppendUInt(json, servoActivePwmUs);
  json += ",\"servo_neutral_us\":";
  jsonAppendUInt(json, SERVO_PWM_NEUTRAL_US);
  json += ",\"servo_running\":";
  json += servoRunning ? "true" : "false";
  json += ",\"servo_pin\":";
  jsonAppendUInt(json, PIN_SERVO_PWM);
  json += ",\"tension_reverse_rpm\":";
  jsonAppendFloat(json, tensionReverseRpm, 1);
  json += ",\"tension_cooldown_s\":";
  jsonAppendFloat(json, tensionCooldownSec, 1);
  json += ",\"tension_fault_s\":";
  jsonAppendFloat(json, tensionFaultSec, 1);
  json += ",\"buffer_refill_fault_s\":";
  jsonAppendFloat(json, BUFFER_REFILL_FAULT_SEC, 1);
  json += ",\"dereeler_lead_ms\":";
  jsonAppendUInt(json, DEREELER_START_DELAY_MS);
  json += "}";
}

static void appendRefillObject(String& json)
{
  json += "\"refill\":{\"material\":";
  json += refillMaterialAllOn() ? "true" : "false";
  json += ",\"dereeler\":";
  json += refillDereelerOn ? "true" : "false";
  json += ",\"servo\":";
  json += refillServoOn ? "true" : "false";
  json += ",\"feeder\":";
  json += refillFeederOn ? "true" : "false";
  json += ",\"active\":";
  json += refillOverrideActive() ? "true" : "false";
  json += ",\"pulse_s\":";
  jsonAppendFloat(json, refillPulseNowMs() / 1000.0f, 1);
  json += "}";
}

static void appendErrorObject(String& json)
{
  char uiBuf[96];
  uiBuf[0] = '\0';
  const char sideCh = PREFEEDER_SIDE_TAG[0];
  if (systemFault != FAULT_NONE)
    pfErrorFormatUiFromFault(uiBuf, sizeof(uiBuf), systemFault, sideCh);

  json += "\"error\":{\"active\":";
  json += (systemFault != FAULT_NONE) ? "true" : "false";
  json += ",\"reason\":";
  jsonAppendStrC(json, pfErrorSlugFromFault(systemFault));
  json += ",\"code\":";
  jsonAppendUInt(json, systemFaultCode());
  json += ",\"tag\":";
  jsonAppendStrC(json, pfErrorTagFromFault(systemFault));
  json += ",\"exxx\":";
  jsonAppendStrC(json, pfErrorExxxFromFault(systemFault, sideCh));
  json += ",\"ui\":";
  jsonAppendStrC(json, uiBuf);
  json += ",\"side\":\"";
  json += PREFEEDER_SIDE_TAG;
  json += "\"}";
}

// ====================== WEB UI ======================
void handleRoot()
{
  String html = FPSTR(index_html);
  html.replace("SERVO_PWM_UI_DEFAULT", String((unsigned)SERVO_PWM_ACTIVE_US));
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", html);
}

void handleStatus()
{
  bool homeRaw = bufferFullRaw();
  bool endstopRaw = digitalRead(PIN_SENSOR_BUFFER_MAX);
  bool tensionRaw = digitalRead(PIN_SENSOR_TENSION);
  const bool masterLinked = peerLinkOk && peerClient.connected();

  String json;
  json.reserve(5120);
  json += "{\"home\":{\"raw\":";
  json += homeRaw ? "true" : "false";
  json += ",\"active\":";
  json += bufferFullStopNow() ? "true" : "false";
  json += ",\"stable\":";
  json += bufferFullActive() ? "true" : "false";
  json += ",\"refill_fault_s\":";
  jsonAppendFloat(json, BUFFER_REFILL_FAULT_SEC, 1);
  json += "},\"endstop\":{\"raw\":";
  json += endstopRaw ? "true" : "false";
  json += ",\"active\":";
  json += bufferMaxActive() ? "true" : "false";
  json += "},\"tension\":{\"raw\":";
  json += tensionRaw ? "true" : "false";
  json += ",\"active\":";
  json += tensionSensorActive() ? "true" : "false";
  json += ",\"blocked\":";
  json += tensionRoutineBlocked() ? "true" : "false";
  json += ",\"cooldown_s\":";
  jsonAppendFloat(json, tensionCooldownSec, 1);
  json += ",\"reverse_rpm\":";
  jsonAppendFloat(json, tensionReverseRpm, 1);
  json += ",\"fault_s\":";
  jsonAppendFloat(json, tensionFaultSec, 1);
  json += "},\"cylinder\":{\"raw\":";
  json += (digitalRead(PIN_SENSOR_CILINDRO) == HIGH) ? "true" : "false";
  json += ",\"open\":";
  json += cylinderOpenActive() ? "true" : "false";
  json += "},\"hose_belt\":{\"raw\":";
  json += (digitalRead(PIN_SENSOR_HOSE_BELT) == HIGH) ? "true" : "false";
  json += ",\"absent\":";
  json += hoseBeltAbsentActive() ? "true" : "false";
  json += "},";
  appendMotorObject(json, 0, "motor");
  json += ",";
  appendMotorObject(json, 1, "motor2");
  json += ",";
  appendTrigger2Object(json);
  json += ",";
  appendAutoObject(json);
  json += ",";
  appendRefillObject(json);
  json += ",";
  appendErrorObject(json);
  json += ",\"idleMode\":";
  json += idleMode ? "true" : "false";
  json += ",\"inProcess\":";
  json += tcmInProcess ? "true" : "false";
  json += ",\"machineState\":\"";
  json += pfMachineStateName(pfMachineStateResolve(idleMode, tcmInProcess, autoEnabled, systemFault));
  json += "\",\"sensorsArmed\":";
  json += sensorsMotionArmed() ? "true" : "false";
  json += ",\"side\":\"";
  json += PREFEEDER_SIDE_TAG;
  json += "\",\"peer\":{\"link\":";
  json += masterLinked ? "true" : "false";
  if (masterLinked)
  {
    json += ",\"master_ip\":\"";
    json += peerClient.remoteIP().toString();
    json += "\"";
  }
  json += ",\"last_cmd\":\"";
  json += peerEsc(peerLastCmd);
  json += "\",\"last_cmd_ok\":";
  json += peerLastCmdOk ? "true" : "false";
  json += ",\"last_cmd_ms_ago\":";
  jsonAppendUInt(json, peerLastCmdMs ? (millis() - peerLastCmdMs) : 0);
  json += ",\"has_cmd\":";
  json += peerLastCmdMs ? "true" : "false";
  json += ",\"last_message_direction\":";
  jsonAppendDirChar(json, peerLastMessageDirection);
  json += ",\"last_message_ms_ago\":";
  jsonAppendUInt(json, peerLastMessageMs ? (millis() - peerLastMessageMs) : 0);
  json += ",\"last_message\":\"";
  json += peerEsc(peerLastMessage);
  json += "\"}}";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json.c_str());
}

static void handleMotorAt(uint8_t idx)
{
  if (!server.hasArg("dir"))
  {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"dir required\"}");
    return;
  }

  if (systemFault != FAULT_NONE)
  {
    stopAllMotors();
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"system_fault\"}");
    return;
  }

  autoDisable();

  String dir = server.arg("dir");
  bool ok = true;

  if (dir == "stop")
  {
    ok = motorRun(idx, 0.0f);
  }
  else if (server.hasArg("signed"))
  {
    const float signedRpm = server.arg("signed").toFloat();
    DBG_PRINTF("Motor%u GET: dir=%s signed=%.1f\n", (unsigned)(idx + 1), dir.c_str(), signedRpm);
    ok = motorRun(idx, signedRpm);
  }
  else
  {
    float rpm = MOTOR_RPM_DEFAULT;
    if (server.hasArg("rpm"))
    {
      rpm = server.arg("rpm").toFloat();
      if (rpm < MOTOR_RPM_MIN) rpm = MOTOR_RPM_MIN;
      if (rpm > MOTOR_RPM_MAX) rpm = MOTOR_RPM_MAX;
    }
    if (dir == "cw")
      ok = motorRun(idx, rpm);
    else if (dir == "ccw")
    {
      if (idx == 0)
        ok = motorRun(idx, rpm);  // DeReeler: CCW ignorado → mismo sentido CW
      else
        ok = motorRun(idx, -rpm);
    }
    else
    {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid dir\"}");
      return;
    }
    DBG_PRINTF("Motor%u GET: dir=%s rpm=%.1f\n", (unsigned)(idx + 1), dir.c_str(), dir == "ccw" ? -rpm : rpm);
  }

  const char* key = (idx == 0) ? "motor" : "motor2";
  String json = "{\"ok\":";
  json += ok ? "true" : "false";
  json += ",";
  appendMotorObject(json, idx, key);
  json += "}";
  server.send(ok ? 200 : 409, "application/json", json);
}

void handleMotor1() { handleMotorAt(0); }

void handleMotor2()
{
  if (server.hasArg("rpm"))
  {
    float v = server.arg("rpm").toFloat();
    if (v < MOTOR_RPM_MIN) v = MOTOR_RPM_MIN;
    if (v > MOTOR_RPM_MAX) v = MOTOR_RPM_MAX;
    motor2RpmSetting = v;
  }
  if (server.hasArg("trigger_feed_s"))
  {
    float v = server.arg("trigger_feed_s").toFloat();
    applyTriggerFeedSec(v);
  }

  saveSettings();
  // Avisar al TCM de inmediato para que el espejo / BASE no pisen lo ajustado en .50/.51.
  if (peerLinkOk)
    peerPushNow(true);

  String json = "{\"ok\":true,";
  appendMotorObject(json, 1, "motor2");
  json += ",";
  appendTrigger2Object(json);
  json += "}";
  server.send(200, "application/json", json);
}

void handleAuto()
{
  // Parametros configurables (se aplican aunque ya este corriendo)
  if (server.hasArg("rpm"))
  {
    float v = server.arg("rpm").toFloat();
    if (v < MOTOR_RPM_MIN) v = MOTOR_RPM_MIN;
    if (v > MOTOR_RPM_MAX) v = MOTOR_RPM_MAX;
    autoRpm = v;
  }
  if (server.hasArg("reverse"))
  {
    float v = server.arg("reverse").toFloat();
    if (v < AUTO_REVERSE_MIN) v = AUTO_REVERSE_MIN;
    if (v > AUTO_REVERSE_MAX) v = AUTO_REVERSE_MAX;
    autoReverseSec = v;
  }
  if (server.hasArg("tension_reverse_rpm"))
  {
    float v = server.arg("tension_reverse_rpm").toFloat();
    if (v < TENSION_REVERSE_RPM_MIN) v = TENSION_REVERSE_RPM_MIN;
    if (v > TENSION_REVERSE_RPM_MAX) v = TENSION_REVERSE_RPM_MAX;
    tensionReverseRpm = v;
  }
  if (server.hasArg("tension_fault") || server.hasArg("tension_fault_s"))
  {
    const String& raw = server.hasArg("tension_fault_s")
      ? server.arg("tension_fault_s") : server.arg("tension_fault");
    float v = raw.toFloat();
    if (v < TENSION_FAULT_SEC_MIN) v = TENSION_FAULT_SEC_MIN;
    if (v > TENSION_FAULT_SEC_MAX) v = TENSION_FAULT_SEC_MAX;
    tensionFaultSec = v;
  }
  if (server.hasArg("tension_cooldown"))
  {
    float v = server.arg("tension_cooldown").toFloat();
    if (v < TENSION_COOLDOWN_MIN) v = TENSION_COOLDOWN_MIN;
    if (v > TENSION_COOLDOWN_MAX) v = TENSION_COOLDOWN_MAX;
    tensionCooldownSec = v;
  }
  if (server.hasArg("servo_pwm") || server.hasArg("servo_pwm_us"))
  {
    const String& raw = server.hasArg("servo_pwm")
      ? server.arg("servo_pwm") : server.arg("servo_pwm_us");
    applyServoActivePwmUs((uint16_t)raw.toInt());
  }

  if (server.hasArg("reset"))
    autoReset();

  if (server.hasArg("enable"))
  {
    if (server.arg("enable") == "1")
      autoEnable();  // siempre rearmar a inicio
    else
      autoDisable();
  }

  if (server.hasArg("idle_mode") || server.hasArg("test_mode"))
  {
    // Materialista solo desde HMI (Master TCP 0x3F → peer setIdleMode).
    // HTML local /api/auto?idle_mode= ya no activa el modo.
  }

  if (server.hasArg("refill_pulse_s"))
    applyRefillPulseSec(server.arg("refill_pulse_s").toFloat());

  saveSettings();
  // Avisar al TCM de inmediato para que el espejo / BASE no pisen lo ajustado en .50/.51.
  if (peerLinkOk)
    peerPushNow(true);

  String json = "{\"ok\":true,";
  appendAutoObject(json);
  json += ",";
  appendMotorObject(json, 0, "motor");
  json += ",";
  appendRefillObject(json);
  json += ",";
  appendErrorObject(json);
  json += ",\"idleMode\":";
  json += idleMode ? "true" : "false";
  json += ",\"inProcess\":";
  json += tcmInProcess ? "true" : "false";
  json += ",\"machineState\":\"";
  json += pfMachineStateName(pfMachineStateResolve(idleMode, tcmInProcess, autoEnabled, systemFault));
  json += "\",\"sensorsArmed\":";
  json += sensorsMotionArmed() ? "true" : "false";
  json += "}";
  server.send(200, "application/json", json);
}

void handleRefill()
{
  if (server.hasArg("pulse_s") || server.hasArg("refill_pulse_s"))
  {
    const String& raw = server.hasArg("pulse_s")
      ? server.arg("pulse_s") : server.arg("refill_pulse_s");
    applyRefillPulseSec(raw.toFloat());
    saveSettings();
    if (peerLinkOk)
      peerPushNow(true);
  }

  String which;
  bool on = false;
  if (server.hasArg("material"))
  {
    which = "material";
    on = server.arg("material") == "1";
  }
  else if (server.hasArg("dereeler"))
  {
    which = "dereeler";
    on = server.arg("dereeler") == "1";
  }
  else if (server.hasArg("servo"))
  {
    which = "servo";
    on = server.arg("servo") == "1";
  }
  else if (server.hasArg("feeder"))
  {
    which = "feeder";
    on = server.arg("feeder") == "1";
  }
  else if (server.hasArg("pulse_s") || server.hasArg("refill_pulse_s"))
  {
    String json = "{\"ok\":true,";
    appendRefillObject(json);
    json += "}";
    server.send(200, "application/json", json);
    return;
  }
  else
  {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"material|dereeler|servo|feeder required\"}");
    return;
  }

  const bool ok = applyRefillCommand(which, on);
  if (!ok && systemFault != FAULT_NONE)
  {
    stopAllMotors();
    String json = "{\"ok\":false,\"error\":\"system_fault\",";
    appendRefillObject(json);
    json += ",";
    appendErrorObject(json);
    json += "}";
    server.send(409, "application/json", json);
    return;
  }
  if (!ok)
  {
    if (!idleMode)
    {
      String json = "{\"ok\":false,\"error\":\"idle_required\",";
      appendRefillObject(json);
      json += "}";
      server.send(409, "application/json", json);
      return;
    }
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid\"}");
    return;
  }

  String json = "{\"ok\":true,";
  appendRefillObject(json);
  json += ",";
  appendMotorObject(json, 0, "motor");
  json += ",";
  appendAutoObject(json);
  json += "}";
  server.send(200, "application/json", json);
}

static void peerSetLastMessage(const String& m, char dir)
{
  peerLastMessageMs = millis();
  peerLastMessageDirection = dir;
  if (m.indexOf("\"type\":\"status\"") >= 0)
    peerLastMessage = "status";
  else if (m.indexOf("\"type\":\"hello\"") >= 0)
    peerLastMessage = "hello";
  else if (m.indexOf("\"type\":\"event\"") >= 0)
  {
    int i = m.indexOf("\"field\":\"");
    if (i >= 0)
    {
      int a = i + 9;
      int b = m.indexOf('"', a);
      peerLastMessage = (b > a) ? String("event:") + m.substring(a, b) : "event";
    }
    else
      peerLastMessage = "event";
  }
  else if (m.indexOf("\"type\":\"command\"") >= 0 || dir == 'R')
  {
    int i = m.indexOf("\"command\":\"");
    if (i >= 0)
    {
      int a = i + 11;
      int b = m.indexOf('"', a);
      peerLastMessage = (b > a) ? String("cmd:") + m.substring(a, b) : "cmd";
    }
    else
      peerLastMessage = "cmd";
  }
  else if (m.length() > 96)
    peerLastMessage = m.substring(0, 93) + "...";
  else
    peerLastMessage = m;
}

static void serviceHttp(uint8_t passes = 12)
{
  for (uint8_t i = 0; i < passes; i++)
  {
    server.handleClient();
    yield();
  }
}

// ====================== PEER TCP (esclavo del Master) ======================
static String peerEsc(const String& s)
{
  String o;
  for (unsigned i = 0; i < s.length() && o.length() < 180; i++)
  {
    char c = s.charAt(i);
    if (c == '"' || c == '\\') o += '\\';
    if (c != '\n' && c != '\r') o += c;
  }
  return o;
}

static int peerJInt(const char* j, const char* k, int d)
{
  String n = String("\"") + k + "\":";
  int i = String(j).indexOf(n);
  return i < 0 ? d : String(j).substring(i + n.length()).toInt();
}

static String peerJStr(const char* j, const char* k)
{
  String n = String("\"") + k + "\":\"";
  String s(j);
  int i = s.indexOf(n);
  if (i < 0) return "";
  int a = i + n.length(), b = s.indexOf('"', a);
  return b < 0 ? "" : s.substring(a, b);
}

static uint32_t peerTriggerMsLeft()
{
  if (motor2Phase != M2_PHASE_TIMED_FEED || motor2TriggerFeedEndMs == 0)
    return 0;
  int32_t left = (int32_t)(motor2TriggerFeedEndMs - millis());
  return left > 0 ? (uint32_t)left : 0;
}

static void peerTx(const String& m)
{
  if (!peerClient.connected()) return;
  peerSetLastMessage(m, 'E');
  bool ok = peerClient.print(m) > 0;
  if (!m.endsWith("\n"))
    ok = (peerClient.print("\n") > 0) && ok;
  if (!ok)
    peerClient.stop();
}

static String peerStatusJson(const char* type)
{
  // Flags de modo al inicio: si el RX del TCM se truncara, Idle/Production siguen llegando.
  String j;
  j.reserve(2048);
  j += "{\"ver\":1,\"type\":\"";
  j += type;
  j += "\",\"role\":\"";
  j += PREFEEDER_SIDE_ROLE;
  j += "\",\"side\":\"";
  j += PREFEEDER_SIDE_TAG;
  j += "\",\"idleMode\":";
  j += idleMode ? "true" : "false";
  j += ",\"inProcess\":";
  j += tcmInProcess ? "true" : "false";
  j += ",\"machineState\":\"";
  j += pfMachineStateName(pfMachineStateResolve(idleMode, tcmInProcess, autoEnabled, systemFault));
  j += "\",\"sensorsArmed\":";
  j += sensorsMotionArmed() ? "true" : "false";
  j += ",\"home\":";
  j += bufferFullActive() ? "true" : "false";
  j += ",\"endstop\":";
  j += bufferMaxActive() ? "true" : "false";
  j += ",\"tension\":";
  j += tensionSensorActive() ? "true" : "false";
  j += ",\"cylinderOpen\":";
  j += cylinderOpenActive() ? "true" : "false";
  j += ",\"hoseAbsent\":";
  j += hoseBeltAbsentActive() ? "true" : "false";
  j += ",\"error\":";
  j += (systemFault != FAULT_NONE) ? "true" : "false";
  j += ",\"errorCode\":";
  jsonAppendUInt(j, systemFaultCode());
  j += ",\"errorReason\":";
  jsonAppendStrC(j, pfErrorSlugFromFault(systemFault));
  j += ",\"errorTag\":";
  jsonAppendStrC(j, pfErrorTagFromFault(systemFault));
  j += ",\"autoEnabled\":";
  j += autoEnabled ? "true" : "false";
  j += ",\"autoState\":\"";
  j += autoStateName();
  j += "\",\"triggerActive\":";
  j += (motor2Phase == M2_PHASE_TIMED_FEED) ? "true" : "false";
  j += ",\"triggerPhase\":\"";
  j += motor2PhaseName(motor2Phase);
  j += "\",\"feedSource\":\"";
  j += motor2FeedSourceName(motor2FeedSource);
  j += "\",\"triggerMsLeft\":";
  j += peerTriggerMsLeft();
  j += ",\"triggerFeedS\":";
  jsonAppendFloat(j, motor2TriggerFeedSec, 2);
  j += ",\"tfeedIncomplete\":";
  j += motor2TfeedIncompleteLatch ? "true" : "false";
  j += ",\"tfeedResumeSec\":";
  jsonAppendFloat(j, motor2TfeedResumeSec, 2);
  j += ",\"autoRpm\":";
  jsonAppendFloat(j, autoRpm, 1);
  j += ",\"autoReverseS\":";
  jsonAppendFloat(j, autoReverseSec, 1);
  j += ",\"motor2Rpm\":";
  jsonAppendFloat(j, motor2RpmSetting, 1);
  j += ",\"tensionReverseRpm\":";
  jsonAppendFloat(j, tensionReverseRpm, 1);
  j += ",\"tensionBoostRpm\":";  // alias legacy
  jsonAppendFloat(j, tensionReverseRpm, 1);
  j += ",\"tensionCooldownS\":";
  jsonAppendFloat(j, tensionCooldownSec, 1);
  j += ",\"tensionFaultS\":";
  jsonAppendFloat(j, tensionFaultSec, 1);
  j += ",\"bufferRefillFaultS\":";
  jsonAppendFloat(j, BUFFER_REFILL_FAULT_SEC, 1);
  j += ",\"servoPwmUs\":";
  jsonAppendUInt(j, servoActivePwmUs);
  j += ",\"dereelerLeadMs\":";
  jsonAppendUInt(j, DEREELER_START_DELAY_MS);
  j += ",\"refillMaterial\":";
  j += refillMaterialAllOn() ? "true" : "false";
  j += ",\"refillDereeler\":";
  j += refillDereelerOn ? "true" : "false";
  j += ",\"refillServo\":";
  j += refillServoOn ? "true" : "false";
  j += ",\"refillFeeder\":";
  j += refillFeederOn ? "true" : "false";
  j += ",\"refillPulseS\":";
  jsonAppendFloat(j, refillPulseNowMs() / 1000.0f, 1);
  j += ",\"errorAny\":";
  j += (systemFault != FAULT_NONE) ? "true" : "false";
  j += "}";
  return j;
}

static void peerTxEvents()
{
  const bool home = bufferFullActive();
  const bool endstop = bufferMaxActive();
  const bool tension = tensionSensorActive();
  const bool cyl = cylinderOpenActive();
  const bool hose = hoseBeltAbsentActive();
  const bool err = (systemFault != FAULT_NONE);
  const bool trig = (motor2Phase == M2_PHASE_TIMED_FEED);
  const uint32_t msLeft = peerTriggerMsLeft();

  if (home != lastPeerHome)
  {
    lastPeerHome = home;
    peerTx(String("{\"type\":\"event\",\"field\":\"home\",\"value\":") + (home ? "true" : "false") + "}");
  }
  if (endstop != lastPeerEndstop)
  {
    lastPeerEndstop = endstop;
    peerTx(String("{\"type\":\"event\",\"field\":\"endstop\",\"value\":") + (endstop ? "true" : "false") + "}");
  }
  if (tension != lastPeerTension)
  {
    lastPeerTension = tension;
    peerTx(String("{\"type\":\"event\",\"field\":\"tension\",\"value\":") + (tension ? "true" : "false") + "}");
  }
  if (cyl != lastPeerCyl)
  {
    lastPeerCyl = cyl;
    peerTx(String("{\"type\":\"event\",\"field\":\"cylinderOpen\",\"value\":") + (cyl ? "true" : "false") + "}");
  }
  if (hose != lastPeerHose)
  {
    lastPeerHose = hose;
    peerTx(String("{\"type\":\"event\",\"field\":\"hoseAbsent\",\"value\":") + (hose ? "true" : "false") + "}");
  }
  if (refillDereelerOn != lastPeerRefillDer || refillServoOn != lastPeerRefillSrv
      || refillFeederOn != lastPeerRefillFeed)
  {
    lastPeerRefillDer = refillDereelerOn;
    lastPeerRefillSrv = refillServoOn;
    lastPeerRefillFeed = refillFeederOn;
    peerTx(String("{\"type\":\"event\",\"field\":\"refill\",\"material\":")
           + (refillMaterialAllOn() ? "true" : "false")
           + ",\"dereeler\":" + (refillDereelerOn ? "true" : "false")
           + ",\"servo\":" + (refillServoOn ? "true" : "false")
           + ",\"feeder\":" + (refillFeederOn ? "true" : "false") + "}");
  }
  if (err != lastPeerErr || (uint8_t)systemFault != lastPeerFault)
  {
    lastPeerErr = err;
    lastPeerFault = (uint8_t)systemFault;
    peerTx(String("{\"type\":\"event\",\"side\":\"") + PREFEEDER_SIDE_TAG
           + "\",\"field\":\"error\",\"value\":" + (err ? "true" : "false")
           + ",\"errorCode\":" + String(systemFaultCode())
           + ",\"errorTag\":\"" + pfErrorTagFromFault(systemFault) + "\""
           + ",\"errorReason\":\"" + peerEsc(pfErrorSlugFromFault(systemFault)) + "\"}");
  }
  if (autoEnabled != lastPeerAutoEn || (uint8_t)autoState != lastPeerAutoState)
  {
    lastPeerAutoEn = autoEnabled;
    lastPeerAutoState = (uint8_t)autoState;
    peerTx(String("{\"type\":\"event\",\"field\":\"auto\",\"autoEnabled\":") + (autoEnabled ? "true" : "false")
           + ",\"autoState\":\"" + autoStateName() + "\"}");
  }
  if (trig != lastPeerTrig || (uint8_t)motor2Phase != lastPeerPhase)
  {
    lastPeerTrig = trig;
    lastPeerPhase = (uint8_t)motor2Phase;
    peerTx(String("{\"type\":\"event\",\"field\":\"triggerActive\",\"value\":") + (trig ? "true" : "false")
           + ",\"triggerPhase\":\"" + motor2PhaseName(motor2Phase) + "\"}");
  }
  if (trig && msLeft != lastPeerTrigMsLeft)
  {
    lastPeerTrigMsLeft = msLeft;
    peerTx(String("{\"type\":\"event\",\"field\":\"triggerMsLeft\",\"value\":") + String(msLeft) + "}");
  }
  else if (!trig && lastPeerTrigMsLeft)
    lastPeerTrigMsLeft = 0;
  if (idleMode != lastPeerIdleMode)
  {
    lastPeerIdleMode = idleMode;
    peerTx(String("{\"type\":\"event\",\"field\":\"idleMode\",\"value\":") + (idleMode ? "true" : "false") + "}");
  }
  if (tcmInProcess != lastPeerInProcess)
  {
    lastPeerInProcess = tcmInProcess;
    peerTx(String("{\"type\":\"event\",\"field\":\"inProcess\",\"value\":") + (tcmInProcess ? "true" : "false") + "}");
  }
  if (motor2TfeedIncompleteLatch != lastPeerTfeedIncomplete)
  {
    lastPeerTfeedIncomplete = motor2TfeedIncompleteLatch;
    peerTx(String("{\"type\":\"event\",\"field\":\"tfeedIncomplete\",\"value\":")
           + (motor2TfeedIncompleteLatch ? "true" : "false")
           + ",\"tfeedResumeSec\":"
           + String(motor2TfeedResumeSec, 2) + "}");
  }
}

static void peerPushNow(bool fullStatus = true)
{
  peerTxEvents();
  if (fullStatus) peerTx(peerStatusJson("status"));
  // Sin flush periódico: satura el enlace y tumba el TCP con el TCM.
  peerLastStatusMs = millis();
}

static bool peerDoCmd(const String& cmd, const String& val, int id)
{
  bool ok = false;
  bool saveSettingsNow = false;

  if (cmd == "start")
  {
    if (systemFault == FAULT_NONE)
    {
      // Siempre rearmar (aunque autoEnabled ya fuera true tras una falla).
      autoEnable();
      ok = (systemFault == FAULT_NONE && autoEnabled);
    }
  }
  else if (cmd == "stop")
  {
    autoDisable();
    ok = true;
  }
  else if (cmd == "reset")
  {
    autoReset();
    ok = (systemFault == FAULT_NONE);
  }
  else if (cmd == "trigger")
  {
    const uint16_t trigId = (id > 0 && id <= 65535) ? (uint16_t)id : 0;

    // Mismo id ya ejecutado (reintento tras arranque real): no re-alimentar.
    if (trigId && triggerIdAlreadyDone(trigId))
    {
      ok = true;
      DBG_PRINTF("M2: trigger id=%u dedup — sin re-ejecutar\n", (unsigned)trigId);
    }
    else
    {
      if (bufferMaxActive() && systemFault != FAULT_ENDSTOP)
        enterSystemFault(FAULT_ENDSTOP, false);

      // Encolar aunque In process OFF; NACK solo con falla/sin enlace/Auto OFF/Max.
      if (systemFault == FAULT_NONE && peerLinkOk && autoEnabled
          && !bufferMaxActive())
      {
        if (val.length())
        {
          float v = val.toFloat();
          if (v < 0.05f) v = 0.05f;
          if (v > M2_TRIGGER_FEED_MAX) v = M2_TRIGGER_FEED_MAX;
          motor2TcpTriggerSecRequest = v;
        }
        else
          motor2TcpTriggerSecRequest = 0.0f;  // default triggerFeedS
        motor2TcpTriggerPendingId = trigId;
        motor2TcpTriggerRequest = true;
        ok = true;
        if (!sensorsMotionArmed())
        {
          Serial.printf(
            "[PEER] trigger QUEUED id=%u (sin armado — arranca al In process) "
            "inProc=%d idle=%d\n",
            (unsigned)trigId, (int)tcmInProcess, (int)idleMode);
        }
      }
      else
      {
        Serial.printf("[PEER] trigger NACK id=%u fault=%u link=%d armed=%d auto=%d max=%d inProc=%d\n",
                      (unsigned)trigId, (unsigned)systemFault, (int)peerLinkOk,
                      (int)sensorsMotionArmed(), (int)autoEnabled,
                      (int)bufferMaxActive(), (int)tcmInProcess);
      }
    }
  }
  else if (cmd == "refillMaterial" || cmd == "refillDereeler" || cmd == "refillServo"
           || cmd == "refillFeeder"
           || cmd == "refillHoldMaterial" || cmd == "refillHoldDereeler"
           || cmd == "refillHoldServo" || cmd == "refillHoldFeeder")
  {
    const bool hold = cmd.startsWith("refillHold");
    const bool on = (val == "1" || val == "true" || val == "on");
    String which = "material";
    if (cmd.endsWith("Dereeler")) which = "dereeler";
    else if (cmd.endsWith("Servo")) which = "servo";
    else if (cmd.endsWith("Feeder")) which = "feeder";
    ok = applyRefillCommand(which, on, hold);
  }
  else if (cmd == "setIdleMode" || cmd == "idleMode"
           || cmd == "setTestMode" || cmd == "testMode"
           || cmd == "materialistaCall" || cmd == "setMaterialistaCall")  // aliases → Materialista
  {
    const bool on = (val == "1" || val == "true" || val == "on");
    applyIdleMode(on);
    ok = true;
  }
  else if (cmd == "setRefillPulseS" || cmd == "refillPulseS")
  {
    applyRefillPulseSec(val.toFloat());
    saveSettingsNow = true;
    ok = true;
  }
  else if (cmd == "setInProcess" || cmd == "inProcess")
  {
    const bool on = (val == "1" || val == "true" || val == "on");
    const bool changed = (tcmInProcess != on);
    tcmInProcess = on;
    if (changed)
      DBG_PRINTF("In process: %s\n", on ? "ON" : "OFF");
    if (!on)
    {
      // OFF (Pause HMI): cortar fill/DeReeler/servo/M2; latch Tfeed incompleto.
      clearFillUntilReady("inProcess-off");
      if (!idleMode && !refillOverrideActive())
      {
        motor2LatchTfeedIncompleteOnHoldOff();
        forceStopDereelerAndServo();
        motor2AbortRequested = true;
        if (autoEnabled && systemFault == FAULT_NONE)
          autoState = AUTO_HOME_HOLD;
        syncServoToAutoState();
      }
    }
    else
    {
      const bool alreadyFilling =
          autoState == AUTO_SERVO_LEAD || autoState == AUTO_CW || autoState == AUTO_CCW;
      clearFillUntilReady("inProcess-on");
      if (!idleMode && autoEnabled && systemFault == FAULT_NONE
          && !refillOverrideActive())
      {
        if (alreadyFilling && !bufferFullActive())
          syncServoToAutoState();
        else
          resumeAutoFromSensors();
      }
    }
    ok = true;
  }
  else if (cmd == "halt" || cmd == "motionStop" || cmd == "hardStop")
  {
    motor2ClearTfeedIncompleteLatch();
    clearFillUntilReady("halt");
    tcmInProcess = false;
    refillClearFlags();
    stopAllMotors();
    if (autoEnabled && systemFault == FAULT_NONE)
      autoState = AUTO_HOME_HOLD;
    syncServoToAutoState();
    ok = true;
  }
  else if (cmd == "ping")
  {
    ok = true;
  }
  else if (cmd == "setTriggerFeed" || cmd == "setTriggerCfg")
  {
    applyTriggerFeedSec(val.toFloat());
    saveSettingsNow = true;
    ok = true;
  }
  else if (cmd == "setAllCfg" || cmd == "applyAllCfg")
  {
    // CSV: autoRpm,reverse,servo,m2Rpm,tensCd,tensFault,unused,triggerFeed[,unused]
    float parts[9];
    int n = 0;
    int start = 0;
    while (n < 9)
    {
      int comma = val.indexOf(',', start);
      String tok = (comma < 0) ? val.substring(start) : val.substring(start, comma);
      parts[n++] = tok.toFloat();
      if (comma < 0) break;
      start = comma + 1;
    }
    if (n >= 8)
    {
      autoRpm = constrain(parts[0], MOTOR_RPM_MIN, MOTOR_RPM_MAX);
      autoReverseSec = constrain(parts[1], AUTO_REVERSE_MIN, AUTO_REVERSE_MAX);
      applyServoActivePwmUs((uint16_t)parts[2]);
      motor2RpmSetting = constrain(parts[3], MOTOR_RPM_MIN, MOTOR_RPM_MAX);
      tensionCooldownSec = constrain(parts[4], TENSION_COOLDOWN_MIN, TENSION_COOLDOWN_MAX);
      tensionFaultSec = constrain(parts[5], TENSION_FAULT_SEC_MIN, TENSION_FAULT_SEC_MAX);
      applyTriggerFeedSec(parts[7]);
      if (autoEnabled) syncServoToAutoState();
      if (cmd == "setAllCfg")
        saveSettingsNow = true;
      ok = true;
    }
  }

  peerLastCmd = cmd;
  peerLastCmdMs = millis();
  peerLastCmdOk = ok;
  if (!ok)
    Serial.printf("[PEER] cmd \"%s\" RECHAZADO\n", cmd.c_str());
  else
    DBG_PRINTF("[PEER] cmd \"%s\" OK\n", cmd.c_str());

  if (!ok)
  {
    if (peerLinkOk && systemFault != FAULT_NONE)
      peerPushNow(true);
    return false;
  }

  if (cmd == "trigger" || cmd == "ping")
    peerTxEvents();
  else
    peerPushNow(true);

  if (saveSettingsNow) saveSettings();
  return true;
}

static void peerOnLine(const char* line)
{
  peerSetLastMessage(String(line), 'R');
  if (!strstr(line, "\"type\":\"command\"")) return;
  peerDoCmd(peerJStr(line, "command"), peerJStr(line, "value"), peerJInt(line, "id", 0));
}

static void peerRx()
{
  while (peerClient.available())
  {
    char c = peerClient.read();
    if (c == '\n' || c == '\r')
    {
      if (peerRxLen)
      {
        peerRxLine[peerRxLen] = 0;
        peerOnLine(peerRxLine);
        peerRxLen = 0;
      }
    }
    else if (peerRxLen < sizeof(peerRxLine) - 1)
      peerRxLine[peerRxLen++] = c;
  }
}

static bool peerWifiNeedsRetry()
{
  wl_status_t s = WiFi.status();
  if (s == WL_CONNECTED) return false;
  if (s == WL_NO_SSID_AVAIL || s == WL_CONNECT_FAILED || s == WL_CONNECTION_LOST) return true;
  return wifiConnectStartedMs && (millis() - wifiConnectStartedMs > 15000);
}

static void peerStopServices()
{
  if (peerClient.connected()) peerClient.stop();
  if (peerWasConnected || peerLinkOk)
  {
    peerLinkOk = false;
    stopAllMotors();
  }
  peerWasConnected = false;
  if (!peerServicesUp) return;
  peerServer.end();
  server.close();
  peerServicesUp = false;
  Serial.println("[PEER] Servicios Off");
}

static void peerEnsureServices()
{
  if (peerServicesUp || WiFi.status() != WL_CONNECTED) return;
  IPAddress ip = WiFi.localIP();
  if (ip == IPAddress(0, 0, 0, 0)) return;

  // Tras un disconnect, reabrir sockets limpios (begin() doble deja HTTP/TCP muertos).
  peerServer.end();
  server.close();
  delay(20);
  peerServer.begin();
  peerServer.setNoDelay(true);
  server.begin();
  peerServicesUp = true;
  Serial.printf("[PEER] Servicios On http://%s TCP:%u\n", ip.toString().c_str(), PEER_PORT);
}

static void peerRetryWifi()
{
  peerStopServices();
  wifiConnectStartedMs = millis();
  WiFi.disconnect(true);
  delay(500);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.config(STA_IP, STA_GW, STA_MASK, STA_GW);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.println("[PEER] Reintentando STA...");
}

static void peerLogWifi()
{
  wl_status_t s = WiFi.status();
  bool on = (s == WL_CONNECTED);
  if (on == staWasConnected) return;
  staWasConnected = on;
  if (on)
  {
    wifiConnectStartedMs = 0;
    Serial.printf("[PEER] WiFi On %s\n", WiFi.localIP().toString().c_str());
    peerEnsureServices();
  }
  else
  {
    peerStopServices();
    if (s == WL_NO_SSID_AVAIL)
      Serial.println("[PEER] WiFi Off (SSID no encontrado)");
    else if (s == WL_CONNECT_FAILED)
      Serial.println("[PEER] WiFi Off (fallo auth)");
    else
      Serial.println("[PEER] WiFi Off");
  }
}

// Asigna el cliente ya aceptado (no WiFiClient temporal: el destructor cerraría el socket).
static void peerOnClientAccepted()
{
  peerClient.setNoDelay(true);
  peerRxLen = 0;
  triggerIdClear();
  lastPeerHome = bufferFullActive();
  lastPeerEndstop = bufferMaxActive();
  lastPeerTension = tensionSensorActive();
  lastPeerCyl = cylinderOpenActive();
  lastPeerHose = hoseBeltAbsentActive();
  lastPeerErr = (systemFault != FAULT_NONE);
  lastPeerTrig = (motor2Phase == M2_PHASE_TIMED_FEED);
  lastPeerAutoEn = autoEnabled;
  lastPeerRefillDer = refillDereelerOn;
  lastPeerRefillSrv = refillServoOn;
  lastPeerRefillFeed = refillFeederOn;
  lastPeerIdleMode = idleMode;
  lastPeerInProcess = tcmInProcess;
  lastPeerTfeedIncomplete = motor2TfeedIncompleteLatch;
  lastPeerFault = (uint8_t)systemFault;
  lastPeerAutoState = (uint8_t)autoState;
  lastPeerPhase = (uint8_t)motor2Phase;
  lastPeerTrigMsLeft = peerTriggerMsLeft();
  peerLinkOk = true;
  peerTx(String("{\"ver\":1,\"type\":\"hello\",\"role\":\"") + PREFEEDER_SIDE_ROLE
         + "\",\"side\":\"" + PREFEEDER_SIDE_TAG + "\"}");
  peerPushNow(true);
  Serial.printf("[PEER] TCP On desde %s\n", peerClient.remoteIP().toString().c_str());
  peerWasConnected = true;
  // No autoEnable() aquí: cada reconnect reabría fill + motores.
}

// Cliente nuevo en el server: sustituye el socket viejo (half-open) sin pasar por tcp-off.
static bool peerAcceptIncoming()
{
  if (!peerServicesUp || !peerServer.hasClient()) return false;
  if (peerClient.connected())
    peerClient.stop();
  peerClient = peerServer.available();
  if (!peerClient) return false;
  peerOnClientAccepted();
  return true;
}

static void peerService()
{
  if (peerAcceptIncoming())
    return;

  if (!peerClient.connected())
  {
    static uint32_t peerWaitLogMs = 0;
    if (peerServicesUp && millis() - peerWaitLogMs >= 15000)
    {
      peerWaitLogMs = millis();
      Serial.printf("[PEER] Esperando Master en TCP :%u (http://%s)\n",
                    PEER_PORT, WiFi.localIP().toString().c_str());
    }
    if (peerWasConnected)
    {
      Serial.println("[PEER] TCP Off");
      peerWasConnected = false;
      peerLinkOk = false;
      if (tcmInProcess)
      {
        tcmInProcess = false;
        // Sin TCP: parar DeReeler/fill/M2 (sin latch — no es Pause HMI).
        clearFillUntilReady("tcp-off");
        if (!refillOverrideActive())
        {
          if (fabsf(commandedRpm) > 0.01f)
            motorRun(0.0f);
          motor2AbortRequested = true;
          if (autoEnabled && systemFault == FAULT_NONE)
            autoState = AUTO_HOME_HOLD;
          syncServoToAutoState();
        }
      }
    }
    return;
  }

  peerWasConnected = true;
  peerLinkOk = true;
  peerRx();
  const bool fast = (motor2Phase == M2_PHASE_TIMED_FEED);
  const uint32_t iv = fast ? PEER_STATUS_FAST_MS : PEER_STATUS_MS;
  if (millis() - peerLastStatusMs >= iv)
  {
    peerTxEvents();
    peerTx(peerStatusJson("status"));
    // setNoDelay ya está; flush() en cada status satura el enlace y lo corta.
    peerLastStatusMs = millis();
  }
}

// ====================== SETUP / LOOP ======================
void setupWiFi()
{
  wifiConnectStartedMs = millis();
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  delay(100);

  if (!WiFi.config(STA_IP, STA_GW, STA_MASK, STA_GW))
    Serial.println("WiFi.config() fallo.");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Buscando router \"%s\"...\n", WIFI_SSID);
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - wifiStart < WIFI_CONNECT_TIMEOUT_MS))
  {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  peerLogWifi();
  peerEnsureServices();
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("STA: sin router. Reintentara WiFi/TCP.");
    int n = WiFi.scanNetworks();
    Serial.printf("scan %d redes:", n);
    for (int i = 0; i < n; i++) Serial.printf(" \"%s\"", WiFi.SSID(i).c_str());
    Serial.println();
  }
  else
  {
    Serial.printf("ESCLAVO web http://%s  TCP:%u\n",
                  WiFi.localIP().toString().c_str(), PEER_PORT);
  }
}

void setup()
{
  // Servo lo antes posible: con el pin flotando (antes de LEDC) un servo RC
  // "se vuelve loco" al energizar la máquina. Neutral 1500 µs = parado.
  pinMode(PIN_SERVO_PWM, OUTPUT);
  digitalWrite(PIN_SERVO_PWM, LOW);
  setupRotationServo();

  pinMode(PIN_SENSOR_BUFFER_FULL, INPUT_PULLUP);
  pinMode(PIN_SENSOR_BUFFER_MAX, INPUT_PULLUP);
  pinMode(PIN_SENSOR_TENSION, INPUT_PULLUP);
  pinMode(PIN_SENSOR_CILINDRO, INPUT_PULLDOWN);
  pinMode(PIN_SENSOR_HOSE_BELT, INPUT_PULLUP);
  bufferFullFilterReset();
  bufferFullRawPrev = bufferFullRaw();

  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("========================================");
  Serial.printf("PreFeeder lado %s  IP fija %s  TCP :%u\n",
                PREFEEDER_SIDE_TAG, STA_IP.toString().c_str(), PEER_PORT);
  Serial.printf("Rol %s — L=.101 R=.102 (IPs distintas o el router falla)\n",
                PREFEEDER_SIDE_ROLE);
  Serial.println("========================================");

  loadSettings();
  Serial.printf("NVS: auto %s, %.0f RPM, inversion tension %.0f RPM x %.1fs, espera %.1fs, timeout tension %.1fs, buffer refill %.1fs, servo %u us, M2 feed %.0f RPM\n",
                autoEnabled ? "ON" : "OFF", autoRpm, tensionReverseRpm, autoReverseSec, tensionCooldownSec,
                tensionFaultSec, BUFFER_REFILL_FAULT_SEC, servoActivePwmUs, (float)motor2RpmSetting);

  server.on("/", handleRoot);
  server.on("/api/status", handleStatus);
  server.on("/api/motor", handleMotor1);
  server.on("/api/motor1", handleMotor1);
  server.on("/api/motor2", handleMotor2);
  server.on("/api/auto", handleAuto);
  server.on("/api/refill", handleRefill);
  server.on("/ping", []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/plain", "pong");
  });
  setupWiFi();
  Serial.printf("Operador: %s -> http://%s\n", WIFI_SSID, TCM_IP.toString().c_str());
  Serial.printf("Tecnico:  misma WiFi -> http://%s (UI nativa PreFeeder %s)\n",
                STA_IP.toString().c_str(), PREFEEDER_SIDE_TAG);

  // LEDC ya inicializado al inicio de setup(); reafirma neutral tras WiFi.
  servoStop();

  // DIR DeReeler lo maneja firmware (CW=LOW / inversión=HIGH), no StepperRMT.
  StepperRMT::Pins pins = { PIN_DEREELER_PUL, -1, -1, -1, -1, -1, -1 };
  pinMode(PIN_DEREELER_MOSFET, OUTPUT);
  dereelerDirCw();
  motor = new StepperRMT(DRIVER_DM556T, pins);
  if (!motor->begin())
  {
    Serial.println("ERROR: DeReeler StepperRMT begin() falló");
  }
  motor->setDefault(MOTOR_MICROSTEP, MOTOR_ACCEL, MOTOR_DECEL);
  motor->Brake();

  StepperRMT::Pins pins2 = { PIN_FEEDER_PUL, -1, -1, -1, -1, -1, -1 };
  motor2 = new StepperRMT(DRIVER_DM556T, pins2);
  if (!motor2->begin())
  {
    Serial.println("ERROR: Feeder StepperRMT begin() falló");
  }
  motor2->setDefault(MOTOR_MICROSTEP, MOTOR2_ACCEL, MOTOR2_DECEL);
  motor2->Brake();

  setupRotationServo();

  xTaskCreatePinnedToCore(
    motor2FeederTask,
    "m2_feeder",
    4096,
    nullptr,
    1,
    &motor2TaskHandle,
    M2_TRIGGER_CORE);

  autoEnable(false);
  if (systemFault != FAULT_NONE)
    Serial.println("Boot: falla enclavada — Reset + Iniciar");
  else
    Serial.printf("Boot: Auto ON (%s) · sensores congelados hasta Iniciar/In process\n",
                  idleMode ? "Materialista" : "Idle");
}

void loop()
{
  serviceHttp(12);

  servoTimingOnBufferFullRawEdge();
  updateBufferFullFilter();
  updateTensionReverseFilter();
  if (!idleMode)
  {
    if (bufferMaxActive())
    {
      forceStopDereelerAndServo();
      if (systemFault != FAULT_ENDSTOP)
        enterSystemFault(FAULT_ENDSTOP, false);
    }
    else if (bufferFullStopNow())
    {
      servoTimingOnForceStopFromBufferFull();
      servoServiceBufferFullCut();
      forceStopDereelerAndServo();
      if (autoEnabled && systemFault == FAULT_NONE
          && (autoState == AUTO_SERVO_LEAD || autoState == AUTO_CW || autoState == AUTO_CCW))
        autoState = AUTO_HOME_HOLD;
    }
  }

  peerLogWifi();
  peerEnsureServices();
  if (peerWifiNeedsRetry() && millis() - lastWifiRetryMs > 8000)
  {
    lastWifiRetryMs = millis();
    peerRetryWifi();
  }
  peerService();
  serviceHttp(8);
  serviceServoPwm();

  updateBufferMaxFaultMonitor();
  updateCylinderFaultMonitor();
  updateHoseBeltFaultMonitor();
  updateTensionFaultMonitor();
  updateBufferRefillFaultMonitor();
  serviceRefillPulses();
  serviceAuto();
  serviceHttp(4);
}