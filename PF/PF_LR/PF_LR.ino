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
static volatile float motor2TriggerActiveSec = 0.0f;      // duración del trigger en curso

// Dedup: retries TCM reutilizan el mismo cmd id; ACK sin re-alimentar.
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
static volatile bool idleMode = false;      // Materialista ON (ex-Idle): bloquea buffer/holgura, omite GPIO27, solo refill manual; torre naranja
static volatile bool tcmInProcess = false;  // ciclo/settle TCM: relleno continuo mientras ON
static volatile bool fillUntilReady = false; // one-shot: rellenar hasta buffer+holgura OK, luego congelar
static bool lastPeerHome = false, lastPeerEndstop = false, lastPeerTension = false;
static bool lastPeerCyl = false, lastPeerHose = false, lastPeerErr = false, lastPeerHolgura = false;
static bool lastPeerTrig = false, lastPeerAutoEn = false;
static bool lastPeerRefillDer = false, lastPeerRefillSrv = false, lastPeerRefillFeed = false;
static bool lastPeerIdleMode = false, lastPeerInProcess = false;
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
volatile bool motor2BufferActiveHigh  = false;  // Holgura: LED ON/LOW = OK; LED OFF/HIGH = helper
// Helper holgura (alivio rápido; prioridad > trigger TCP) — params UI propios
volatile float    holguraHelperRpm       = M2_HOLGURA_HELPER_RPM_DEFAULT;
volatile float    holguraHelperSec       = M2_HOLGURA_HELPER_SEC_DEFAULT;
volatile uint32_t holguraHelperAbsentMs  = M2_HOLGURA_HELPER_ABSENT_MS;
volatile float    holguraFaultSec        = M2_HOLGURA_FAULT_SEC;
static volatile bool holguraHelperFiredThisAbsence = false;
TaskHandle_t   motor2TaskHandle       = nullptr;

static void applyTriggerFeedSec(float v)
{
  motor2TriggerFeedSec = constrain(v, M2_TRIGGER_FEED_MIN, M2_TRIGGER_FEED_MAX);
}

static volatile bool holguraStablePresent   = false;
static volatile bool holguraFilterPending   = false;
static volatile uint32_t holguraFilterChangeMs = 0;
// Reloj propio del helper (tarea m2) — no comparte con el monitor de falla del loop.
static volatile uint32_t holguraHelperAbsentStartMs = 0;

static volatile bool bufferFullStable       = false;
static uint32_t      bufferFullHighAccumMs  = 0;
static uint32_t      bufferFullLowAccumMs   = 0;
static uint32_t      bufferFullFilterLastMs = 0;

// ====================== AUTO / FAULTS ======================
bool      autoEnabled    = true;
AutoState autoState      = AUTO_OFF;
float     autoRpm        = AUTO_RPM_DEFAULT;
float     autoReverseSec = AUTO_REVERSE_DEFAULT;
float     tensionCooldownSec = TENSION_COOLDOWN_DEFAULT;
uint32_t  tensionBoostUntilMs = 0;  // boost +RPM en AUTO_CW (sin cambiar sentido)
uint32_t  servoLeadStartMs = 0;
uint32_t  tensionLastRoutineMs = 0;
uint32_t  tensionActiveSinceMs = 0;
uint32_t  tensionReverseHighSinceMs = 0;  // debounce para inversión AUTO
static bool tensionReverseStable = false;
uint32_t  bufferEmptySinceMs = 0;
uint32_t  bufferFullRecoverSinceMs = 0;  // Buffer Full estable antes de cancelar timeout
volatile uint32_t holguraAbsentSinceMs = 0;
SystemFault systemFault = FAULT_NONE;
volatile bool motor2AbortRequested = false;

uint16_t  servoActivePwmUs = SERVO_PWM_ACTIVE_US;  // editable UI/NVS (default por lado)
bool      servoRunning      = false;
uint16_t  servoLastOutputUs = 0;

// Refill / override manual: DeReeler + servo + Feeder (ignora Buffer Full / holgura).
// Clic = un pulso de refillPulseMs; otro clic apaga de inmediato (no relanza).
// volatile: loop + motor2HolguraTask (core 1) leen/escriben.
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
  prefs.putBool("m2_buf_hi", motor2BufferActiveHigh);
  prefs.putFloat("h_help_rpm", (float)holguraHelperRpm);
  prefs.putFloat("h_help_s", (float)holguraHelperSec);
  prefs.putUInt("h_help_ms", (uint32_t)holguraHelperAbsentMs);
  prefs.putFloat("h_fault_s", (float)holguraFaultSec);
  prefs.putFloat("tens_cd", tensionCooldownSec);
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
  const bool bufMigratedV3 = prefs.getBool("m2_buf_v3", false);
  motor2BufferActiveHigh = prefs.getBool("m2_buf_hi", false);
  holguraHelperRpm = prefs.getFloat("h_help_rpm", M2_HOLGURA_HELPER_RPM_DEFAULT);
  holguraHelperSec = prefs.getFloat("h_help_s", M2_HOLGURA_HELPER_SEC_DEFAULT);
  holguraHelperAbsentMs = prefs.getUInt("h_help_ms", M2_HOLGURA_HELPER_ABSENT_MS);
  holguraFaultSec = prefs.getFloat("h_fault_s", M2_HOLGURA_FAULT_SEC);
  tensionCooldownSec = prefs.getFloat("tens_cd", TENSION_COOLDOWN_DEFAULT);
  servoActivePwmUs = (uint16_t)prefs.getUInt("servo_pwm", (uint32_t)SERVO_PWM_ACTIVE_US);
  refillPulseMs = prefs.getUInt("refill_ms", REFILL_PULSE_MS_DEFAULT);
  if (refillPulseMs < REFILL_PULSE_MS_MIN) refillPulseMs = REFILL_PULSE_MS_MIN;
  if (refillPulseMs > REFILL_PULSE_MS_MAX) refillPulseMs = REFILL_PULSE_MS_MAX;
  prefs.end();

  if (!bufMigratedV3)
  {
    // v3: LED ON = holgura OK (active LOW). Pisa NVS v2 (HIGH=OK).
    motor2BufferActiveHigh = false;
    prefs.begin(PREFS_NS, false);
    prefs.putBool("m2_buf_hi", false);
    prefs.putBool("m2_buf_v3", true);
    prefs.end();
  }

  autoRpm = constrain(autoRpm, MOTOR_RPM_MIN, MOTOR_RPM_MAX);
  autoReverseSec = constrain(autoReverseSec, AUTO_REVERSE_MIN, AUTO_REVERSE_MAX);
  motor2RpmSetting = constrain(motor2RpmSetting, MOTOR_RPM_MIN, MOTOR_RPM_MAX);
  applyTriggerFeedSec((float)motor2TriggerFeedSec);
  holguraHelperRpm = constrain(holguraHelperRpm, MOTOR_RPM_MIN, MOTOR_RPM_MAX);
  holguraHelperSec = constrain(holguraHelperSec, M2_HOLGURA_HELPER_SEC_MIN, M2_HOLGURA_HELPER_SEC_MAX);
  if (holguraHelperAbsentMs < M2_HOLGURA_HELPER_ABSENT_MS_MIN)
    holguraHelperAbsentMs = M2_HOLGURA_HELPER_ABSENT_MS_MIN;
  if (holguraHelperAbsentMs > M2_HOLGURA_HELPER_ABSENT_MS_MAX)
    holguraHelperAbsentMs = M2_HOLGURA_HELPER_ABSENT_MS_MAX;
  holguraFaultSec = constrain(holguraFaultSec, M2_HOLGURA_FAULT_SEC_MIN, M2_HOLGURA_FAULT_SEC_MAX);
  tensionCooldownSec = constrain(tensionCooldownSec, TENSION_COOLDOWN_MIN, TENSION_COOLDOWN_MAX);
  servoActivePwmUs = constrain(servoActivePwmUs, SERVO_PWM_MIN_US, SERVO_PWM_MAX_US);
}

static uint32_t servoUsToDuty(uint16_t us)
{
  us = constrain(us, SERVO_PWM_MIN_US, SERVO_PWM_MAX_US);
  return (uint32_t)us * ((1UL << SERVO_LEDC_BITS) - 1) / 20000UL;
}

// PWM efectivo en refill/auto: 1500 µs = neutro (sin giro en RC continuo).
static uint16_t servoMotionPwmUs()
{
  const int delta = (int)servoActivePwmUs - (int)SERVO_PWM_NEUTRAL_US;
  if (delta > -40 && delta < 40)
    return SERVO_PWM_ACTIVE_US;
  return servoActivePwmUs;
}

static void servoWriteUsForced(uint16_t us)
{
  us = constrain(us, SERVO_PWM_MIN_US, SERVO_PWM_MAX_US);
  servoLastOutputUs = us;
  ledcWrite(PIN_SERVO_PWM, servoUsToDuty(us));
}

// No reescribir LEDC si ya está: ledcWrite repetido glitchea el RC (vibra).
static void servoWriteUsIfChanged(uint16_t us)
{
  us = constrain(us, SERVO_PWM_MIN_US, SERVO_PWM_MAX_US);
  if (servoLastOutputUs == us) return;
  servoWriteUsForced(us);
}

static void servoStop()
{
  if (!servoRunning && servoLastOutputUs == SERVO_PWM_NEUTRAL_US)
    return;
  servoWriteUsForced(SERVO_PWM_NEUTRAL_US);
  servoRunning = false;
}

static void applyServoActivePwmUs(uint16_t us)
{
  us = constrain(us, SERVO_PWM_MIN_US, SERVO_PWM_MAX_US);
  servoActivePwmUs = us;
  if (servoRunning)
    servoWriteUsForced(servoActivePwmUs);
}

// Ventana de relleno: In process (TCM ciclo/settle) o fillUntilReady (one-shot Iniciar).
static bool sensorsMotionArmed()
{
  return !idleMode && (tcmInProcess || fillUntilReady);
}

static void resumeAutoFromSensors();
static void requestFillUntilReady();
static void clearFillUntilReady(const char* reason);
static void serviceFillUntilReady();
static bool bufferFullStopNow();
static bool bufferFullAllowsMotion();
static void forceStopDereelerAndServo();

static bool refillMaterialAllOn()
{
  return refillDereelerOn && refillServoOn && refillFeederOn;
}

// Servo ON solo mientras rellena (lead/CW/inversión); HOME_HOLD = Buffer Full estable.
static bool servoShouldRunAuto()
{
  return systemFault == FAULT_NONE
      && autoEnabled
      && sensorsMotionArmed()
      && !bufferFullStopNow()
      && (autoState == AUTO_SERVO_LEAD || autoState == AUTO_CW);
}

static void syncServoToAutoState()
{
  // Materialista: el servo solo vive con refillServoOn (no auto lead/CW).
  if (idleMode && systemFault == FAULT_NONE)
  {
    if (refillServoOn)
    {
      servoWriteUsIfChanged(servoMotionPwmUs());
      servoRunning = true;
    }
    else if (servoRunning || servoLastOutputUs != SERVO_PWM_NEUTRAL_US)
      servoStop();
    return;
  }

  if (servoShouldRunAuto())
  {
    servoWriteUsIfChanged(servoMotionPwmUs());
    servoRunning = true;
  }
  else if (servoRunning || servoLastOutputUs != SERVO_PWM_NEUTRAL_US)
    servoStop();
}

static void setupRotationServo()
{
  ledcAttach(PIN_SERVO_PWM, 50, SERVO_LEDC_BITS);
  servoLastOutputUs = 0;
  servoStop();
}

static bool bufferFullRaw()
{
  return digitalRead(PIN_SENSOR_BUFFER_FULL) == HIGH;
}

static bool bufferFullActive()
{
  return bufferFullStable;
}

// Parar relleno solo con Full confirmado (~80 ms HIGH). Blips cortos en medio
// del buffer no deben cortar DeReeler/servo.
static bool bufferFullStopNow()
{
  return bufferFullActive();
}

// Rearrancar solo tras Full OFF sostenido (~200 ms). No rearrancar en rebote al soltar.
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
    // Glitch: LOW corto no borra el acumulado de HIGH (evita “nunca Full” con ruido).
    if (bufferFullLowAccumMs >= BUFFER_FULL_GLITCH_MS)
      bufferFullHighAccumMs = 0;
    if (bufferFullLowAccumMs >= BUFFER_FULL_OFF_FILTER_MS)
    {
      bufferFullStable = false;
      bufferFullLowAccumMs = BUFFER_FULL_OFF_FILTER_MS;
    }
  }
}

// Corta DeReeler+servo. Un Stop() por consigna; si el RMT ignora, reintenta cada 250 ms.
// Stop() cada loop con currentRPM()>1 → vibración / “trabado”.
static void forceStopDereelerAndServo()
{
  static uint32_t lastStopMs = 0;
  const bool commanded = motor && fabsf(commandedRpm) > 0.01f;
  const bool stillSpinning = motor && fabsf(motor->currentRPM()) > 1.0f;
  if (commanded || (stillSpinning && (uint32_t)(millis() - lastStopMs) >= 250u))
  {
    motor->Stop();
    lastStopMs = millis();
  }
  commandedRpm = 0.0f;
  tensionBoostUntilMs = 0;

  if (servoRunning || servoLastOutputUs != SERVO_PWM_NEUTRAL_US)
  {
    servoWriteUsForced(SERVO_PWM_NEUTRAL_US);
    servoRunning = false;
  }
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

static bool tensionCanTriggerRoutine()
{
  if (!tensionReverseStable || !tensionCooldownReady())
    return false;
  // Con cooldown UI = 0 y tensión pegada/HIGH: al salir de REVERSING volvía a
  // invertir en el mismo tick → DeReeler solo “tiembla” (manual no invierte).
  if (tensionLastRoutineMs != 0)
  {
    const uint32_t minGapMs =
        (uint32_t)(autoReverseSec * 1000.0f) + 500u;  // duración inversión + 0.5 s CW
    if ((uint32_t)(millis() - tensionLastRoutineMs) < minGapMs)
      return false;
  }
  return true;
}

static bool tensionRoutineBlocked()
{
  return tensionSensorActive() && !tensionCooldownReady();
}

static void tensionMarkRoutineTriggered()
{
  tensionLastRoutineMs = millis();
}

static void stopAllMotors();
static void enterSystemFault(SystemFault fault, bool pushPeer = true);
static void peerPushNow(bool fullStatus);
static void updateTensionFaultMonitor();
static void updateBufferRefillFaultMonitor();

static bool gpioInputActive(uint8_t pin, bool activeHigh)
{
  const bool high = digitalRead(pin) == HIGH;
  return activeHigh ? high : !high;
}

static bool holguraActive()
{
  return gpioInputActive(PIN_SENSOR_HOLGURA, motor2BufferActiveHigh);
}

static bool holguraStableActive()
{
  return holguraStablePresent;
}

static void holguraFilterReset()
{
  const bool raw = holguraActive();
  holguraFilterPending = raw;
  holguraStablePresent = raw;
  holguraFilterChangeMs = millis();
}

static void updateHolguraFilter()
{
  const bool raw = holguraActive();
  const uint32_t now = millis();

  if (raw != holguraFilterPending)
  {
    holguraFilterPending = raw;
    holguraFilterChangeMs = now;
    return;
  }

  if ((uint32_t)(now - holguraFilterChangeMs) >= M2_HOLGURA_FILTER_MS)
    holguraStablePresent = raw;
}

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

// StepperRMT DM556: convención fija UI→setSpeed (no es “invertir giro”; sentido físico = bobinas).
static float uiSignedToDriveRpm(float uiSigned, uint8_t idx = 1)
{
  (void)idx;
  const float mag = fabsf(uiSigned);
  return (uiSigned > 0.0f) ? -mag : mag;
}

// DeReeler: firmware solo RPM+ (nunca manda sentido contrario). Tensión = más RPM, no otro giro.
static float dereelerUiRpmOnlyCw(float signedRpm)
{
  if (signedRpm < -0.01f)
    return fabsf(signedRpm);
  return signedRpm;
}

static void motorHardStop(uint8_t idx)
{
  StepperRMT* m = motorByIndex(idx);
  m->Stop();
  motorWaitStopped(idx);
  *commandedRpmByIndex(idx) = 0.0f;
}

static bool motorStartSigned(uint8_t idx, float uiSignedRpm)
{
  if (idx == 0)
    uiSignedRpm = dereelerUiRpmOnlyCw(uiSignedRpm);

  StepperRMT* m = motorByIndex(idx);
  const float driveRpm = uiSignedToDriveRpm(uiSignedRpm, idx);

  if (fabsf(*commandedRpmByIndex(idx)) > 0.01f || fabsf(m->currentRPM()) > 1.0f)
  {
    motorHardStop(idx);
    delay(MOTOR_DIR_SETUP_MS);
  }

  if (!m->setSpeed(driveRpm, MOTOR_ACCEL))
  {
    motorHardStop(idx);
    delay(MOTOR_DIR_SETUP_MS);
    if (!m->setSpeed(driveRpm, MOTOR_ACCEL))
      return false;
  }

  *commandedRpmByIndex(idx) = uiSignedRpm;
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
  if (idx == 0)
    signedRpm = dereelerUiRpmOnlyCw(signedRpm);

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
      return true;
    }
  }

  return motorStartSigned(idx, target);
}

static bool motorRun(float signedRpm)
{
  return motorRun(0, signedRpm);
}

// TEMP: RPM nominal UI + boost; no cambia sentido (solo velocidad).
static float autoRpmTensionBoost()
{
  float rpm = autoRpm + TENSION_BOOST_RPM_OFFSET;
  if (rpm > MOTOR_RPM_MAX) rpm = MOTOR_RPM_MAX;
  if (rpm < MOTOR_RPM_MIN) rpm = MOTOR_RPM_MIN;
  return rpm;
}

// Arranque desde Buffer Full inactivo: servo ya; DeReeler tras DEREELER_START_DELAY_MS.
static void beginAutoCwWithServoLead()
{
  tensionBoostUntilMs = 0;
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
    motorRun(0.0f);
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
  }
  else if (bufferFullAllowsMotion())
    beginAutoCwWithServoLead();
  else
  {
    forceStopDereelerAndServo();
    autoState = AUTO_HOME_HOLD;
  }
  syncServoToAutoState();
}

static void requestFillUntilReady()
{
  if (idleMode || systemFault != FAULT_NONE || !autoEnabled)
    return;
  if (bufferFullActive() && holguraStableActive() && motor2Phase == M2_PHASE_IDLE)
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
  if (bufferFullActive() && holguraStableActive() && motor2Phase == M2_PHASE_IDLE)
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
    if (!onFlag)
      return;
    if (pulseUntilMs == 0 || (int32_t)(now - pulseUntilMs) >= 0)
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

static void applyRefillOutputs()
{
  refillExpireChannels(millis());

  if (refillDereelerOn)
  {
    if (fabsf(commandedRpm - autoRpm) > 0.5f)
      motorRun(autoRpm);
  }
  else if (fabsf(commandedRpm) > 0.01f)
    motorRun(0.0f);

  if (refillServoOn)
  {
    servoWriteUsIfChanged(servoMotionPwmUs());
    servoRunning = true;
  }
  else if (servoRunning || servoLastOutputUs != SERVO_PWM_NEUTRAL_US)
    servoStop();

  // Feeder se aplica en motor2HolguraTask vía refillFeederOn.
  if (!refillFeederOn)
    motor2AbortRequested = true;
}

static void refillStopChannel(volatile bool& onFlag, volatile uint32_t& pulseUntilMs)
{
  onFlag = false;
  pulseUntilMs = 0;
}

// Si ya está ON no reinicia el temporizador (evita “apagar y relanzar 1 s”).
static bool refillStartChannel(volatile bool& onFlag, volatile uint32_t& pulseUntilMs)
{
  if (onFlag)
    return false;
  onFlag = true;
  pulseUntilMs = millis() + refillPulseNowMs();
  return true;
}

static void serviceRefillPulses()
{
  if (!idleMode)
  {
    if (refillOverrideActive())
    {
      refillClearFlags();
      motorRun(0.0f);
      servoStop();
      motor2AbortRequested = true;
    }
    return;
  }

  const bool wasActive = refillOverrideActive();
  refillExpireChannels(millis());

  if (refillOverrideActive() || wasActive)
    applyRefillOutputs();
}

// which: "material" | "dereeler" | "servo" | "feeder".
// on=true → un pulso de refillPulseMs (no relanza si ya está ON).
// on=false → apaga de inmediato.
static bool applyRefillCommand(const String& which, bool on)
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
      const bool der = refillStartChannel(refillDereelerOn, refillDereelerPulseUntilMs);
      const bool srv = refillStartChannel(refillServoOn, refillServoPulseUntilMs);
      const bool fed = refillStartChannel(refillFeederOn, refillFeederPulseUntilMs);
      if (srv)
        setupRotationServo();
      (void)der;
      (void)fed;
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
      refillStartChannel(refillDereelerOn, refillDereelerPulseUntilMs);
    else
      refillStopChannel(refillDereelerOn, refillDereelerPulseUntilMs);
  }
  else if (which == "servo")
  {
    if (on)
    {
      if (refillStartChannel(refillServoOn, refillServoPulseUntilMs))
      {
        setupRotationServo();
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
      refillStartChannel(refillFeederOn, refillFeederPulseUntilMs);
    else
      refillStopChannel(refillFeederOn, refillFeederPulseUntilMs);
  }
  else
    return false;

  if (refillOverrideActive())
    applyRefillOutputs();
  else
  {
    motorRun(0.0f);
    servoStop();
    motor2AbortRequested = true;
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
    // Manual: sensores (Max/Full/holgura/…) no deben dejar el refill trabado.
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
      motorRun(0.0f);
      servoStop();
      motor2AbortRequested = true;
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
  else if (prev == FAULT_HOLGURA_TIMEOUT)
  {
    // Sin holgura enclavado: Reset libre; Iniciar / trigger deben recuperar holgura.
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
  holguraAbsentSinceMs = 0;

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
      applyRefillOutputs();
    else
    {
      if (fabsf(commandedRpm) > 0.01f)
        motorRun(0.0f);
      syncServoToAutoState();
    }
    return;
  }

  if (systemFault != FAULT_NONE)
  {
    // Falla enclavada: quieto hasta Reset + Iniciar.
    // Reintentar stop: RMT a veces ignora un Stop() y commandedRpm ya es 0.
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

  // Idle / sin ventana: quieto.
  if (!sensorsMotionArmed())
  {
    if (fabsf(commandedRpm) > 0.01f)
      motorRun(0.0f);
    syncServoToAutoState();
    serviceFillUntilReady();
    return;
  }

  serviceFillUntilReady();
  if (!sensorsMotionArmed())
  {
    if (fabsf(commandedRpm) > 0.01f)
      motorRun(0.0f);
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

  // Buffer Full (estable): cortar SIEMPRE, aunque commandedRpm ya diga 0
  if (bufferFullStopNow())
  {
    forceStopDereelerAndServo();
    if (autoState == AUTO_SERVO_LEAD || autoState == AUTO_CW
        || autoState == AUTO_HOME_HOLD)
    {
      if (autoState != AUTO_HOME_HOLD)
        DBG_PRINTLN("AUTO: Buffer Full -> HOME_HOLD (force stop)");
      autoState = AUTO_HOME_HOLD;
    }
    syncServoToAutoState();
    return;
  }

  switch (autoState)
  {
    case AUTO_ENDSTOP_FAULT:
    case AUTO_TENSION_FAULT:
    case AUTO_CYLINDER_FAULT:
    case AUTO_HOSE_FAULT:
    case AUTO_BUFFER_FAULT:
    case AUTO_HOLGURA_FAULT:
    case AUTO_OPERATOR_STOP:
      forceStopDereelerAndServo();
      syncServoToAutoState();
      return;

    case AUTO_HOME_HOLD:
      // GPIO19 vacío estable (~200 ms OFF) → rellenar hasta Full ON.
      if (bufferFullAllowsMotion())
        beginAutoCwWithServoLead();
      break;

    case AUTO_SERVO_LEAD:
      if ((uint32_t)(millis() - servoLeadStartMs) >= DEREELER_START_DELAY_MS)
      {
        motorRun(autoRpm);
        autoState = AUTO_CW;
        DBG_PRINTF("AUTO: servo lead %lums -> DeReeler CW\n", (unsigned long)DEREELER_START_DELAY_MS);
      }
      break;

    case AUTO_CW:
    {
      const uint32_t now = millis();
      if (tensionBoostUntilMs != 0)
      {
        if ((int32_t)(now - tensionBoostUntilMs) >= 0)
        {
          tensionBoostUntilMs = 0;
          motorRun(autoRpm);
          DBG_PRINTLN("AUTO: fin boost tension -> RPM nominal");
        }
        else
          motorRun(autoRpmTensionBoost());
      }
      if (tensionBoostUntilMs == 0 && tensionCanTriggerRoutine())
      {
        tensionBoostUntilMs = now + (uint32_t)(autoReverseSec * 1000.0f);
        motorRun(autoRpmTensionBoost());
        tensionMarkRoutineTriggered();
        DBG_PRINTF("AUTO: TENSION -> +%.0f RPM (total %.0f) %.1fs\n",
                      TENSION_BOOST_RPM_OFFSET, autoRpmTensionBoost(), autoReverseSec);
      }
      break;
    }

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
    case M2_FEED_HOLGURA: return "holgura_helper";
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
  const bool bufferRaw  = digitalRead(PIN_SENSOR_HOLGURA) == HIGH;
  const bool holguraInstant = holguraActive();
  const bool holguraPresent = holguraStableActive();
  const bool triggerActive = (motor2Phase == M2_PHASE_TIMED_FEED);
  json += "\"trigger2\":{\"buffer_gpio_raw\":";
  json += bufferRaw ? "true" : "false";
  json += ",\"trigger_active\":";
  json += triggerActive ? "true" : "false";
  json += ",\"trigger_via\":\"";
  json += motor2FeedSourceName(motor2FeedSource);
  json += "\"";
  json += ",\"buffer_tension_instant\":";
  json += holguraInstant ? "true" : "false";
  json += ",\"buffer_tension\":";
  json += holguraPresent ? "true" : "false";
  json += ",\"holgura_present\":";
  json += holguraPresent ? "true" : "false";
  json += ",\"holgura_instant\":";
  json += holguraInstant ? "true" : "false";
  json += ",\"feed_cycle_armed\":";
  json += (motor2Phase != M2_PHASE_IDLE) ? "true" : "false";
  json += ",\"boost_allowed\":";
  json += !holguraInstant ? "true" : "false";
  json += ",\"buffer_active_high\":";
  json += motor2BufferActiveHigh ? "true" : "false";
  json += ",\"phase\":\"";
  json += motor2PhaseName(motor2Phase);
  json += "\",\"feed_source\":\"";
  json += motor2FeedSourceName(motor2FeedSource);
  json += "\",\"boost_active\":";
  json += triggerActive ? "true" : "false";
  json += ",\"rpm_boost\":";
  jsonAppendFloat(json, motor2RpmSetting, 1);
  json += ",\"holgura_fault_s\":";
  jsonAppendFloat(json, (float)holguraFaultSec, 1);
  json += ",\"holgura_helper_rpm\":";
  jsonAppendFloat(json, (float)holguraHelperRpm, 1);
  json += ",\"holgura_helper_s\":";
  jsonAppendFloat(json, (float)holguraHelperSec, 2);
  json += ",\"holgura_helper_absent_ms\":";
  jsonAppendUInt(json, (uint32_t)holguraHelperAbsentMs);
  json += ",\"holgura_extra_feed_s\":0.0";  // legacy slot
  json += ",\"trigger_feed_s\":";
  jsonAppendFloat(json, motor2TriggerFeedSec, 2);
  json += ",\"holgura_filter_ms\":";
  jsonAppendUInt(json, M2_HOLGURA_FILTER_MS);
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

static void motor2DisarmFeedCycle()
{
  motor2HardStopTask();
}

static void stopAllMotors()
{
  forceStopDereelerAndServo();
  // Dueño del RMT feeder = tarea m2_holgura. Brake() desde loop pelea el driver.
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
  holguraAbsentSinceMs = 0;

  Serial.printf("FALTA [%s]: ", pfErrorTagFromFault(fault));
  if (fault == FAULT_BUFFER_TIMEOUT)
    Serial.printf("Buffer Full no rellenó en %.1fs\n", BUFFER_REFILL_FAULT_SEC);
  else if (fault == FAULT_HOLGURA_TIMEOUT)
    Serial.printf("Sin holgura > %.1fs\n", (float)holguraFaultSec);
  else if (fault == FAULT_TENSION_TIMEOUT)
    Serial.printf("Tension > %.1fs\n", TENSION_FAULT_SEC);
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

static void updateTensionFaultMonitor()
{
  if (systemFault != FAULT_NONE)
    return;

  if (!tensionSensorActive())
  {
    tensionActiveSinceMs = 0;
    return;
  }

  if (tensionActiveSinceMs == 0)
    tensionActiveSinceMs = millis();
  else if ((uint32_t)(millis() - tensionActiveSinceMs) >= (uint32_t)(TENSION_FAULT_SEC * 1000.0f))
    enterSystemFault(FAULT_TENSION_TIMEOUT);
}

// Buffer Full consumido: ya no enclava por timeout de relleno (E052/E058).
// DeReeler/servo siguen parando en Full ON y reanudando en Full OFF (filtro GPIO).
static void updateBufferRefillFaultMonitor()
{
  bufferEmptySinceMs = 0;
  bufferFullRecoverSinceMs = 0;
}

// Sin holgura estable ≥ fault_s: falla solo con Buffer Full ya ON.
// Durante relleno (Full OFF) el helper puede correr; no enclavar E057/E063
// — el slack aparece al formar el lazo, y Start aún no está produciendo.
static void updateHolguraFaultMonitor()
{
  if (systemFault != FAULT_NONE || idleMode || refillOverrideActive()
      || !sensorsMotionArmed() || !bufferFullActive())
  {
    holguraAbsentSinceMs = 0;
    return;
  }

  if (holguraStableActive())
  {
    holguraAbsentSinceMs = 0;
    return;
  }

  if (holguraAbsentSinceMs == 0)
  {
    holguraAbsentSinceMs = millis();
    Serial.printf("M2: sin holgura (estable) — falla en %.1fs si no recupera\n",
                  (float)holguraFaultSec);
  }
  else if ((uint32_t)(millis() - holguraAbsentSinceMs)
           >= (uint32_t)(holguraFaultSec * 1000.0f))
  {
    enterSystemFault(FAULT_HOLGURA_TIMEOUT);
  }
}

static void motor2SoftStopToIdle()
{
  motor2BrakeAtRest();
  motor2Phase = M2_PHASE_IDLE;
  motor2FeedSource = M2_FEED_NONE;
  motor2ActiveFeedRpm = 0.0f;
  motor2TriggerFeedEndMs = 0;
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
  float sec = (motor2TcpTriggerSecRequest > 0.05f)
                ? (float)motor2TcpTriggerSecRequest
                : (float)motor2TriggerFeedSec;
  motor2TcpTriggerSecRequest = 0.0f;
  motor2StartTimedFeed(now, M2_FEED_TCP, (float)motor2RpmSetting, sec);
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
    motor2SoftStopToIdle();
    motor2TriggerActiveSec = 0.0f;
    Serial.printf("M2: fin timed_feed src=%s %.2fs\n",
                  motor2FeedSourceName(doneSrc), doneSec);
    return;
  }

  motor2EnsureFeeding();
}

// Helper: SIN HOLGURA (pin ausente) ≥ holguraHelperAbsentMs → feeder (una vez por ausencia).
// Usa lectura cruda en la tarea m2 (sin pelear el filtro del loop).
// Timed feed en curso (Tfeed TCP o helper) no se corta ni se reinicia — como Buffer Full.
static void updateHolguraHelper()
{
  // HOLGURA presente (slack OK) → reset one-shot.
  if (holguraActive())
  {
    holguraHelperAbsentStartMs = 0;
    holguraHelperFiredThisAbsence = false;
    return;
  }

  // A partir de aquí: SIN HOLGURA (crudo).
  if (systemFault != FAULT_NONE || idleMode || refillOverrideActive()
      || !sensorsMotionArmed() || bufferMaxActive())
  {
    static uint32_t lastBlockLogMs = 0;
    const uint32_t now = millis();
    if ((uint32_t)(now - lastBlockLogMs) >= 2000)
    {
      lastBlockLogMs = now;
      Serial.printf(
        "M2: helper bloq sin-holgura armed=%d idle=%d fault=%u bufMax=%d refill=%d\n",
        (int)sensorsMotionArmed(), (int)idleMode, (unsigned)systemFault,
        (int)bufferMaxActive(), (int)refillOverrideActive());
    }
    holguraHelperAbsentStartMs = 0;
    return;
  }

  // Ya en timed feed: dejar terminar (no reemplazar Tfeed TCP ni reiniciar helper).
  if (motor2Phase == M2_PHASE_TIMED_FEED)
    return;

  if (holguraHelperAbsentStartMs == 0)
  {
    holguraHelperAbsentStartMs = millis();
    Serial.printf("M2: SIN HOLGURA — helper en %u ms @ %.0f RPM / %.2fs\n",
                  (unsigned)holguraHelperAbsentMs,
                  (float)holguraHelperRpm, (float)holguraHelperSec);
  }

  if (holguraHelperFiredThisAbsence)
    return;

  if ((uint32_t)(millis() - holguraHelperAbsentStartMs) < (uint32_t)holguraHelperAbsentMs)
    return;

  holguraHelperFiredThisAbsence = true;
  float sec = (float)holguraHelperSec;
  if (sec < M2_HOLGURA_HELPER_SEC_MIN) sec = M2_HOLGURA_HELPER_SEC_MIN;
  if (sec > M2_HOLGURA_HELPER_SEC_MAX) sec = M2_HOLGURA_HELPER_SEC_MAX;
  motor2StartTimedFeed(millis(), M2_FEED_HOLGURA, (float)holguraHelperRpm, sec);
}

static void serviceHolguraFeeder()
{
  // Feeder solo por timed feed (TCP/holgura) o refill.
  if (motor2Phase == M2_PHASE_TIMED_FEED)
    return;
  if (motor2Phase != M2_PHASE_IDLE)
    motor2SoftStopToIdle();
}

static void motor2HolguraTask(void* /*param*/)
{
  pinMode(PIN_SENSOR_HOLGURA, INPUT_PULLUP);
  vTaskDelay(pdMS_TO_TICKS(50));
  holguraFilterReset();

  motor2HardStopTask();

  for (;;)
  {
    updateHolguraFilter();

    if (motor2AbortRequested)
    {
      motor2DisarmFeedCycle();
      motor2AbortRequested = false;
    }

    // Helper pronto: loguea bloqueos (falla/sin armado) aunque luego se haga continue.
    updateHolguraHelper();
    // Timed feed (Tfeed TCP o helper): servir siempre el timer — no cortar por In process OFF.
    if (motor2Phase == M2_PHASE_TIMED_FEED)
      serviceTimedFeeder();

    // Feeder manual Materialista: no cortar por Buffer Max (eso era arranca/para cada 5 ms).
    // DeReeler/servo los mueve solo loop() — RMT no es thread-safe (dos cores = vibra).
    if (refillManualActive())
    {
      motor2TcpTriggerRequest = false;
      if (refillFeederOn)
        motor2EnsureFeeding();
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }

    if (systemFault != FAULT_NONE)
    {
      motor2TcpTriggerRequest = false;
      if (motor2Phase != M2_PHASE_IDLE || fabsf(commandedRpm2) > 0.01f
          || (motor2 && fabsf(motor2->currentRPM()) > 1.0f))
        motor2DisarmFeedCycle();
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }

    // Buffer Max: cortar feeder. DeReeler/servo los para loop() en el mismo criterio.
    if (bufferMaxActive())
    {
      motor2TcpTriggerRequest = false;
      if (motor2Phase != M2_PHASE_IDLE || fabsf(commandedRpm2) > 0.01f
          || (motor2 && fabsf(motor2->currentRPM()) > 1.0f))
        motor2DisarmFeedCycle();
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }

    // Trigger TCP: requiere ventana armada + Auto ON.
    // Timed feed ya iniciado: seguir hasta fin aunque In process OFF / sin armado.
    if (!sensorsMotionArmed() || !autoEnabled)
    {
      motor2TcpTriggerRequest = false;
      if (motor2Phase == M2_PHASE_TIMED_FEED)
      {
        serviceTimedFeeder();
      }
      else if (!sensorsMotionArmed())
      {
        if (motor2Phase != M2_PHASE_IDLE || fabsf(commandedRpm2) > 0.01f
            || (motor2 && fabsf(motor2->currentRPM()) > 1.0f))
          motor2DisarmFeedCycle();
      }
      else
      {
        serviceHolguraFeeder();
      }
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }

    if (motor2TcpTriggerRequest)
    {
      if (!peerLinkOk || bufferMaxActive())
        motor2TcpTriggerRequest = false;
      else if (motor2Phase == M2_PHASE_TIMED_FEED && motor2FeedSource == M2_FEED_HOLGURA)
      {
        // Helper holgura en curso: no cortar ni reiniciar; descartar trigger TCP.
        motor2TcpTriggerRequest = false;
        Serial.println("M2: trigger TCP ignorado — helper holgura activo");
      }
      else if (motor2Phase == M2_PHASE_TIMED_FEED && motor2FeedSource == M2_FEED_TCP)
      {
        // Ya en Tfeed TCM: no reiniciar fin (reintentos TCP).
        motor2TcpTriggerRequest = false;
      }
      else
      {
        motor2TcpTriggerRequest = false;
        motor2StartTriggerFeed(millis());
      }
    }

    serviceTimedFeeder();
    serviceHolguraFeeder();

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
  json += ",\"tension_cooldown_s\":";
  jsonAppendFloat(json, tensionCooldownSec, 1);
  json += ",\"tension_fault_s\":";
  jsonAppendFloat(json, TENSION_FAULT_SEC, 1);
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
  json += ",\"fault_s\":";
  jsonAppendFloat(json, TENSION_FAULT_SEC, 1);
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
  json += ",\"link_ok\":";
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
  if (server.hasArg("buffer_active_high"))
    motor2BufferActiveHigh = server.arg("buffer_active_high").toInt() != 0;
  if (server.hasArg("trigger_feed_s"))
  {
    float v = server.arg("trigger_feed_s").toFloat();
    applyTriggerFeedSec(v);
  }
  if (server.hasArg("holgura_helper_rpm"))
  {
    float v = server.arg("holgura_helper_rpm").toFloat();
    if (v < MOTOR_RPM_MIN) v = MOTOR_RPM_MIN;
    if (v > MOTOR_RPM_MAX) v = MOTOR_RPM_MAX;
    holguraHelperRpm = v;
  }
  if (server.hasArg("holgura_helper_s"))
  {
    float v = server.arg("holgura_helper_s").toFloat();
    if (v < M2_HOLGURA_HELPER_SEC_MIN) v = M2_HOLGURA_HELPER_SEC_MIN;
    if (v > M2_HOLGURA_HELPER_SEC_MAX) v = M2_HOLGURA_HELPER_SEC_MAX;
    holguraHelperSec = v;
  }
  if (server.hasArg("holgura_helper_absent_ms"))
  {
    uint32_t v = (uint32_t)server.arg("holgura_helper_absent_ms").toInt();
    if (v < M2_HOLGURA_HELPER_ABSENT_MS_MIN) v = M2_HOLGURA_HELPER_ABSENT_MS_MIN;
    if (v > M2_HOLGURA_HELPER_ABSENT_MS_MAX) v = M2_HOLGURA_HELPER_ABSENT_MS_MAX;
    holguraHelperAbsentMs = v;
  }
  if (server.hasArg("holgura_fault_s"))
  {
    float v = server.arg("holgura_fault_s").toFloat();
    if (v < M2_HOLGURA_FAULT_SEC_MIN) v = M2_HOLGURA_FAULT_SEC_MIN;
    if (v > M2_HOLGURA_FAULT_SEC_MAX) v = M2_HOLGURA_FAULT_SEC_MAX;
    holguraFaultSec = v;
  }

  holguraFilterReset();

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
  // dereeler_lead_ms / tension_fault / buffer_refill_fault: fijos en firmware
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

  if (server.hasArg("idle_mode"))
    applyIdleMode(server.arg("idle_mode") == "1");
  else if (server.hasArg("test_mode"))  // alias legacy
    applyIdleMode(server.arg("test_mode") == "1");

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
  j += bufferFullStopNow() ? "true" : "false";
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
  j += "\",\"holgura\":";
  j += holguraStableActive() ? "true" : "false";
  j += ",\"triggerActive\":";
  j += (motor2Phase == M2_PHASE_TIMED_FEED) ? "true" : "false";
  j += ",\"triggerPhase\":\"";
  j += motor2PhaseName(motor2Phase);
  j += "\",\"feedSource\":\"";
  j += motor2FeedSourceName(motor2FeedSource);
  j += "\",\"triggerMsLeft\":";
  j += peerTriggerMsLeft();
  j += ",\"holguraExtraFeedS\":0.0";
  j += ",\"holguraFaultS\":";
  jsonAppendFloat(j, (float)holguraFaultSec, 1);
  j += ",\"holguraHelperRpm\":";
  jsonAppendFloat(j, (float)holguraHelperRpm, 1);
  j += ",\"holguraHelperS\":";
  jsonAppendFloat(j, (float)holguraHelperSec, 2);
  j += ",\"holguraHelperAbsentMs\":";
  j += (uint32_t)holguraHelperAbsentMs;
  j += ",\"triggerFeedS\":";
  jsonAppendFloat(j, motor2TriggerFeedSec, 2);
  j += ",\"autoRpm\":";
  jsonAppendFloat(j, autoRpm, 1);
  j += ",\"autoReverseS\":";
  jsonAppendFloat(j, autoReverseSec, 1);
  j += ",\"motor2Rpm\":";
  jsonAppendFloat(j, motor2RpmSetting, 1);
  j += ",\"tensionCooldownS\":";
  jsonAppendFloat(j, tensionCooldownSec, 1);
  j += ",\"tensionFaultS\":";
  jsonAppendFloat(j, TENSION_FAULT_SEC, 1);
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
  const bool home = bufferFullStopNow();
  const bool endstop = bufferMaxActive();
  const bool tension = tensionSensorActive();
  const bool cyl = cylinderOpenActive();
  const bool hose = hoseBeltAbsentActive();
  const bool err = (systemFault != FAULT_NONE);
  const bool holgura = holguraStableActive();
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
  if (holgura != lastPeerHolgura)
  {
    lastPeerHolgura = holgura;
    peerTx(String("{\"type\":\"event\",\"field\":\"holgura\",\"value\":") + (holgura ? "true" : "false") + "}");
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

    // Mismo id ya ejecutado (reintento): no re-alimentar.
    if (trigId && triggerIdAlreadyDone(trigId))
    {
      ok = true;
      DBG_PRINTF("M2: trigger id=%u dedup — sin re-ejecutar\n", (unsigned)trigId);
    }
    else
    {
      // MAX crudo: enclavar ya; falla/estado salen por event/status (trigger sin ACK).
      if (bufferMaxActive() && systemFault != FAULT_ENDSTOP)
        enterSystemFault(FAULT_ENDSTOP, false);

      if (systemFault == FAULT_NONE && peerLinkOk && sensorsMotionArmed() && autoEnabled
          && !bufferMaxActive())
      {
        // Ya en Tfeed: no reiniciar timer; marcar id como hecho.
        if (motor2Phase == M2_PHASE_TIMED_FEED && motor2FeedSource == M2_FEED_TCP)
        {
          ok = true;
          triggerIdRemember(trigId);
        }
        else
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
          motor2TcpTriggerRequest = true;
          ok = true;
          triggerIdRemember(trigId);
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
           || cmd == "refillFeeder")
  {
    const bool on = (val == "1" || val == "true" || val == "on");
    String which = "material";
    if (cmd == "refillDereeler") which = "dereeler";
    else if (cmd == "refillServo") which = "servo";
    else if (cmd == "refillFeeder") which = "feeder";
    ok = applyRefillCommand(which, on);
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
      // OFF: cortar fill/servo/DeReeler. Timed feed (Tfeed/helper) sigue hasta su tiempo
      // — mismo criterio que Buffer Full: no cortar por fin de ciclo / In process OFF.
      clearFillUntilReady("inProcess-off");
      if (!idleMode && !refillOverrideActive())
      {
        if (fabsf(commandedRpm) > 0.01f)
          motorRun(0.0f);
        if (motor2Phase != M2_PHASE_TIMED_FEED)
          motor2AbortRequested = true;
        if (autoEnabled && systemFault == FAULT_NONE)
          autoState = AUTO_HOME_HOLD;
        syncServoToAutoState();
      }
    }
    else if (changed)
    {
      clearFillUntilReady("inProcess-on");
      if (!idleMode && autoEnabled && systemFault == FAULT_NONE
          && !refillOverrideActive())
        resumeAutoFromSensors();
    }
    ok = true;
  }
  else if (cmd == "halt" || cmd == "motionStop" || cmd == "hardStop")
  {
    // Parada inmediata pedida por TCM (Stop/Reset/fault): sin relleno residual.
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
    // ACK = prueba de enlace bidireccional (la falla enclavada se reporta aparte en status).
    ok = true;
  }
  else if (cmd == "setHolguraExtra" || cmd == "setHolguraFault" || cmd == "setHolguraFaultS"
           || cmd == "setDereelerLead" || cmd == "setDereelerLeadMs"
           || cmd == "setTriggerFeed" || cmd == "setTriggerCfg"
           || cmd == "setHolguraHelper" || cmd == "setHolguraHelperRpm"
           || cmd == "setHolguraHelperS" || cmd == "setHolguraHelperAbsentMs")
  {
    if (cmd == "setTriggerFeed" || cmd == "setTriggerCfg")
    {
      applyTriggerFeedSec(val.toFloat());
      saveSettingsNow = true;
    }
    if (cmd == "setHolguraFault" || cmd == "setHolguraFaultS")
    {
      float v = val.toFloat();
      if (v < M2_HOLGURA_FAULT_SEC_MIN) v = M2_HOLGURA_FAULT_SEC_MIN;
      if (v > M2_HOLGURA_FAULT_SEC_MAX) v = M2_HOLGURA_FAULT_SEC_MAX;
      holguraFaultSec = v;
      saveSettingsNow = true;
    }
    else if (cmd == "setHolguraHelperRpm")
    {
      float v = val.toFloat();
      if (v < MOTOR_RPM_MIN) v = MOTOR_RPM_MIN;
      if (v > MOTOR_RPM_MAX) v = MOTOR_RPM_MAX;
      holguraHelperRpm = v;
      saveSettingsNow = true;
    }
    else if (cmd == "setHolguraHelperS")
    {
      float v = val.toFloat();
      if (v < M2_HOLGURA_HELPER_SEC_MIN) v = M2_HOLGURA_HELPER_SEC_MIN;
      if (v > M2_HOLGURA_HELPER_SEC_MAX) v = M2_HOLGURA_HELPER_SEC_MAX;
      holguraHelperSec = v;
      saveSettingsNow = true;
    }
    else if (cmd == "setHolguraHelperAbsentMs")
    {
      uint32_t v = (uint32_t)val.toInt();
      if (v < M2_HOLGURA_HELPER_ABSENT_MS_MIN) v = M2_HOLGURA_HELPER_ABSENT_MS_MIN;
      if (v > M2_HOLGURA_HELPER_ABSENT_MS_MAX) v = M2_HOLGURA_HELPER_ABSENT_MS_MAX;
      holguraHelperAbsentMs = v;
      saveSettingsNow = true;
    }
    else if (cmd == "setHolguraHelper")
    {
      // CSV: rpm,sec,absentMs[,faultS]
      float parts[4] = {0};
      int n = 0;
      int start = 0;
      for (int i = 0; i <= (int)val.length() && n < 4; i++)
      {
        if (i == (int)val.length() || val.charAt(i) == ',')
        {
          parts[n++] = val.substring(start, i).toFloat();
          start = i + 1;
        }
      }
      if (n >= 1)
      {
        float v = parts[0];
        if (v < MOTOR_RPM_MIN) v = MOTOR_RPM_MIN;
        if (v > MOTOR_RPM_MAX) v = MOTOR_RPM_MAX;
        holguraHelperRpm = v;
      }
      if (n >= 2)
      {
        float v = parts[1];
        if (v < M2_HOLGURA_HELPER_SEC_MIN) v = M2_HOLGURA_HELPER_SEC_MIN;
        if (v > M2_HOLGURA_HELPER_SEC_MAX) v = M2_HOLGURA_HELPER_SEC_MAX;
        holguraHelperSec = v;
      }
      if (n >= 3)
      {
        uint32_t v = (uint32_t)parts[2];
        if (v < M2_HOLGURA_HELPER_ABSENT_MS_MIN) v = M2_HOLGURA_HELPER_ABSENT_MS_MIN;
        if (v > M2_HOLGURA_HELPER_ABSENT_MS_MAX) v = M2_HOLGURA_HELPER_ABSENT_MS_MAX;
        holguraHelperAbsentMs = v;
      }
      if (n >= 4)
      {
        float v = parts[3];
        if (v < M2_HOLGURA_FAULT_SEC_MIN) v = M2_HOLGURA_FAULT_SEC_MIN;
        if (v > M2_HOLGURA_FAULT_SEC_MAX) v = M2_HOLGURA_FAULT_SEC_MAX;
        holguraFaultSec = v;
      }
      saveSettingsNow = true;
    }
    ok = true;
  }
  else if (cmd == "setAllCfg" || cmd == "applyAllCfg")
  {
    // CSV: autoRpm,reverse,servo,m2Rpm,tensCd,tensFault,holguraExtra,triggerFeed[,dereelerLeadMs]
    // tensFault / holguraExtra / dereelerLead: fijos o ignorados.
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

  // Sin ACK TCP: TCM fire-and-forget. Errores/timeouts van por event/status.
  if (!ok)
  {
    if (peerLinkOk && systemFault != FAULT_NONE)
      peerPushNow(true);
    return false;
  }

  // trigger/ping: solo eventos delta (barato). El status periódico del loop basta.
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
  lastPeerHome = bufferFullActive();
  lastPeerEndstop = bufferMaxActive();
  lastPeerTension = tensionSensorActive();
  lastPeerCyl = cylinderOpenActive();
  lastPeerHose = hoseBeltAbsentActive();
  lastPeerErr = (systemFault != FAULT_NONE);
  lastPeerHolgura = holguraStableActive();
  lastPeerTrig = (motor2Phase == M2_PHASE_TIMED_FEED);
  lastPeerAutoEn = autoEnabled;
  lastPeerRefillDer = refillDereelerOn;
  lastPeerRefillSrv = refillServoOn;
  lastPeerRefillFeed = refillFeederOn;
  lastPeerIdleMode = idleMode;
  lastPeerInProcess = tcmInProcess;
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
        // Sin TCP: parar DeReeler/fill; timed feed en curso termina su tiempo.
        clearFillUntilReady("tcp-off");
        if (!refillOverrideActive())
        {
          if (fabsf(commandedRpm) > 0.01f)
            motorRun(0.0f);
          if (motor2Phase != M2_PHASE_TIMED_FEED)
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
  if (motor2BufferActiveHigh)
    Serial.println("AVISO M2: holgura en HIGH; contrato actual: LED ON=OK / LED OFF=helper (active LOW).");
  Serial.printf("NVS: auto %s, %.0f RPM, rev %.1fs, tensión espera %.1fs, error tensión %.1fs, buffer refill %.1fs, servo %u us, M2 feed %.0f RPM\n",
                autoEnabled ? "ON" : "OFF", autoRpm, autoReverseSec, tensionCooldownSec,
                TENSION_FAULT_SEC, BUFFER_REFILL_FAULT_SEC, servoActivePwmUs, (float)motor2RpmSetting);

  // Red ANTES de motores/RMT: HTTP/TCP deben quedar listos aunque el driver falle.
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

  StepperRMT::Pins pins = { PIN_DEREELER_PUL, PIN_DEREELER_MOSFET, -1, -1, -1, -1, -1 };
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

  // StepperRMT puede desenganchar LEDC del servo; re-atachar tras motores (como heredado + fix).
  setupRotationServo();

  xTaskCreatePinnedToCore(
    motor2HolguraTask,
    "m2_holgura",
    4096,
    nullptr,
    1,
    &motor2TaskHandle,
    M2_TRIGGER_CORE);

  // Boot: Auto ON desde NVS; sensores congelados hasta Iniciar o In process del TCM.
  autoEnable(false);
  if (systemFault != FAULT_NONE)
    Serial.println("Boot: falla enclavada — Reset + Iniciar");
  else
    Serial.printf("Boot: Auto ON (%s) · sensores congelados hasta Iniciar/In process\n",
                  idleMode ? "Materialista" : "Idle");
  Serial.printf("Helper holgura: ausente≥%u ms → %.0f RPM × %.2fs · falla≥%.1fs\n",
                (unsigned)holguraHelperAbsentMs, (float)holguraHelperRpm,
                (float)holguraHelperSec, (float)holguraFaultSec);
}

void loop()
{
  serviceHttp(12);

  // Seguridad primero: GPIO19/21 antes de TCP/HTTP.
  // Materialista: no enclavar Max ni cortar Full (refill manual).
  updateBufferFullFilter();
  // Holgura filter: solo en motor2HolguraTask (evita carrera dual-core con el helper).
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
      forceStopDereelerAndServo();
      if (autoEnabled && systemFault == FAULT_NONE
          && (autoState == AUTO_SERVO_LEAD || autoState == AUTO_CW))
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

  updateBufferFullFilter();
  updateBufferMaxFaultMonitor();
  updateCylinderFaultMonitor();
  updateHoseBeltFaultMonitor();
  updateTensionFaultMonitor();
  updateBufferRefillFaultMonitor();
  updateHolguraFaultMonitor();
  serviceRefillPulses();
  serviceAuto();
  serviceHttp(8);
}