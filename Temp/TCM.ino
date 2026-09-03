// Tube Cutter Machine v1.0
// ÍNDICE DE SECCIONES (buscar "SECCION NN"):
//   01  CAN / objetos globales de hardware
//   02  WiFi / Web / Peer TCP (STA router + espejo PreFeeder)
//   03  Estado global (variables de ciclo, feed, calibración)
//   04  I/O (PLC, sensores, seguridad física)
//   05  Peer TCP (cliente → PreFeeder)
//   06  NVS y configuración (clamps, math lineal, persistencia)
//   07  Log
//   08  CAN — RX / TX / INIT
//   09  CAN — Movimiento CiA402
//   10  ASDA RS-485 — Actuador lineal (esclavo HTTP)
//   11  Ciclo lineal (no bloqueante)
//   12  Servo feeder (cooperativo)
//   13  Rutina de corte
//   14  Web handlers
//   15  Setup / Loop
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <SPI.h>
#include <mcp_can.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <esp_system.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "config.h"
#include "index_html.h"

// ============================================================
// SECCION 01 — CAN (MCP2515) / objetos globales de hardware
// ============================================================
#define CAN_XTAL   MCP_8MHZ
#define CAN_SPEED  CAN_500KBPS
MCP_CAN CAN0(SPI_CS_PIN);

// Prototipos
bool sendCANMessage(unsigned long id, byte len, byte* data, String desc, bool logSuccess = false);
void canSetMotionProfile(uint32_t velA, uint32_t accA, uint32_t decA, uint32_t velB, uint32_t accB, uint32_t decB);
void canHalt();
void canHaltSide(bool sideR);
void canClearHaltSide(bool sideR);
static void feedSsHaltFromSensorNow();
static void feedHaltTaskStart();
static bool asdaOmReadMm(float* actualMmOut);
void canMoveRelativePP(int32_t stepsA, int32_t stepsB);
bool canReadStatusWord(uint8_t nodeId, uint16_t& statusOut, uint32_t timeoutMs = 500);
bool initCANBus(uint8_t maxRetries = 3);
bool reconnectCANBus(bool fullServoSetup = true);
bool setupServoFeeder();
bool verifyServoEnergized();
void serviceCANRx();
void serviceExternalStopInput();
void serviceTowerCommandTx();
bool externalStopInputActive();
bool safetyAllowsRun();
static void serviceSafetyDebounce(bool force);
static bool pollSensorModule();
static bool pollSensorModuleEx(uint8_t retries, uint32_t timeoutMs);
static bool requestSensorSnapshotOnce(uint32_t timeoutMs);
static bool sensorHasRecentRx(uint32_t windowMs = SENSOR_ONLINE_RECENT_MS);
static void clearSensorOfflineLatch(const char* why);
static void serviceSensorCanDiscovery();
static void latchSensorOffline(const char* reason);
static void noteSensorPollFailure(const char* reason);
static uint8_t sensorCodeFromBitmask(uint8_t mask);
static bool safetyValidateAtStart();
static bool safetyHonorStopAfterStep(const char* stepName);
static void safetyPollAfterPiece();
static void serviceCyclePauseGate(const char* stepName);
static void honorPausePendingIfIdle(const char* why);
static bool cycleGateAfterStep(const char* stepName);
static bool stepByStepShouldPause(const char* stepName);
static const char* stepByStepPauseLabel(const char* stepName);
static void enterCyclePaused(const char* reason);
static void leaveCyclePaused();
static uint32_t cycleElapsedMs();
static void faultStopCycle(const char* reason);
static void resetFeedAbortState();
static void peerSyncInProcessFlag();  // arma/desarma buffer+holgura en PreFeeder
static bool waitPrefeederReadyAtStart();
static bool prefeederRequireHolguraAtHome();  // tras gap paso18→HOME: error si sin holgura
static void prefeederSettleThenDisarm(const char* tag);
static void prefeederHardStopDisarm(const char* tag);  // Stop/fault/Reset: para PF ya
static void abortPendingPrefeederSettle(const char* reason);
static void releasePrefeederSettleHold();
static bool settleShouldYieldToStart();
static bool prefeederTriggerPulse();
static bool prefeederTriggerPulseSecs(float secs);
static void prefeederTriggerAsync(const char* tag);
static bool peerBothAllOkForPrefetch();
static void syncPrefeederPieceLength();
static void persistPrefeederMirrorToBase();
static void commitPrefeederMirrorSideToBase(uint8_t side);
static void persistMachineBaseNow();
static bool saveMachineBase();
static void saveLinearCalToNvs();
static void beginSlaveUiHold(const char* kind);
static void setCycleFaultReason(const char* reason);
static void pushLog(const String& s);
static uint8_t pfErrorCodeFromReason(uint8_t side, const String& reason);
void serviceCycle();
void serviceServoFeed();
void startServoFeedAsync(bool prefetch, bool chunksOnly = false);
void waitServoFeedStep2();
void runServoFeedStep2(bool chunksOnly = false);
void cycleStartBackToHome();
void cycleStartBackwardPulses(uint32_t pulses);
void cycleStartForwardPulses(uint32_t pulses, bool skipDwellAtDest = false);
void waitCycleFinish();
void setupLinearActuator();
void prepareServoFeeder();
bool feedThisCycleSucceeded();
void pausableDelay(uint32_t ms);
static void immediatePhysicalStop();
void abortCycleRoutine(bool clearSafetyError);
void abortCycleRoutine();
void cycle(int cycles, uint32_t stepsPerCut);
void procesarCorte(int longitud, int cantidad);
void prepareBeforeCut();

static bool linearIsMoving();
static void linearService();
static void linearAbort();
static void linearPause();
static void linearResume();
static bool linearRunTorqueHome();
static void asdaEnsureHomeAtBoot();
static void asdaSyncConfig();
static bool feedPhaseIsActive();
static bool feedStep2NeedsWait();
static bool peerConnectAllowed();
static void peerTryReconnect(int8_t forceSide = -1);
static void serviceWebClients(uint8_t maxPasses);
static void serviceBackgroundTick(uint32_t& lastCanMs, uint32_t& lastWebMs);
static bool towerInBootGrace();

// ============================================================
// SECCION 02 — WiFi / Web / Peer TCP (STA → router + espejo PreFeeder)
// ============================================================
static const IPAddress STA_IP(10, 10, 32, 10);  // TCM; .72 es el AP
static const IPAddress STA_GW(10, 10, 32, 72);
static const IPAddress STA_MASK(255, 255, 255, 0);
WebServer server(80);

static uint32_t wifiConnectStartedMs = 0;
static uint32_t lastWifiRetryMs = 0;
static bool wifiWasConnected = false;

static bool wifiIsUp()
{
  return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

static bool wifiNeedsRetry()
{
  wl_status_t s = WiFi.status();
  if (s == WL_CONNECTED) return false;
  if (s == WL_NO_SSID_AVAIL || s == WL_CONNECT_FAILED || s == WL_CONNECTION_LOST) return true;
  return wifiConnectStartedMs && (millis() - wifiConnectStartedMs > 15000);
}

static void wifiBeginSta()
{
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  delay(50);
  if (!WiFi.config(STA_IP, STA_GW, STA_MASK, STA_GW))
    Serial.println("WiFi.config FAIL");
  wifiConnectStartedMs = millis();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

static bool startStationWifi()
{
  Serial.printf("STA → \"%s\"  IP fija %s  gw %s  peers→%s / %s :%u\n",
                WIFI_SSID, STA_IP.toString().c_str(), STA_GW.toString().c_str(),
                PEER_HOST_L, PEER_HOST_R, PEER_PORT);
  wifiBeginSta();
  const unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0 < WIFI_CONNECT_TIMEOUT_MS))
  {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  if (wifiIsUp())
  {
    wifiWasConnected = true;
    wifiConnectStartedMs = 0;
    Serial.printf("STA OK  http://%s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.println("STA: sin router. Reintentará en loop.");
  return false;
}

struct PrefeederMirror {
  bool peerOk = false;
  bool home = false;
  bool endstop = false;
  bool tension = false;
  bool cylinderOpen = false;
  bool hoseAbsent = false;
  bool error = false;
  bool autoEnabled = false;
  bool holgura = false;
  bool triggerActive = false;
  bool refillMaterial = false;
  bool refillDereeler = false;
  bool refillServo = false;
  bool refillFeeder = false;
  bool idleMode = false;
  bool buzzerMuted = false;
  float refillPulseS = 1.0f;
  bool inProcess = false;
  uint8_t errorCode = 0;
  String errorReason = "none";
  String autoState = "off";
  String triggerPhase = "idle";
  uint32_t triggerMsLeft = 0;
  float holguraExtraFeedS = 1.0f;
  float triggerFeedS = 2.0f;
  float autoRpm = 60.0f;
  float autoReverseS = 2.0f;
  float motor2Rpm = 60.0f;
  float tensionCooldownS = 0.0f;
  float tensionFaultS = 30.0f;
  float bufferRefillFaultS = 4.0f;
  uint16_t servoPwmUs = PF_SERVO_PWM_DEFAULT_US;
  uint32_t dereelerLeadMs = 400;
};

struct PeerSlot {
  WiFiClient client;
  PrefeederMirror mirror;
  uint32_t lastRxMs = 0;
  uint32_t lastReconnectMs = 0;
  uint32_t lastMessageMs = 0;
  String lastMessage = "";
  char lastMessageDirection = '-';
  char rxLine[768];
  uint16_t rxLen = 0;
  bool linkWas = false;
  bool fullStatusReceived = false;
  bool everConnected = false;
  uint8_t failStreak = 0;  // connect fallidos → backoff TCP
  uint32_t lastProbeMs = 0;  // keepalive "\n"
};

static PeerSlot peers[PEER_COUNT];
static const char* const PEER_HOSTS[PEER_COUNT] = { PEER_HOST_L, PEER_HOST_R };
static const char* const PEER_TAGS[PEER_COUNT] = { "L", "R" };
static uint16_t peerCmdId = 1;
static bool peerCommSuspect = false;  // enlace dudoso mid-pieza; se confirma al terminar la pieza
static bool peerSettleHoldInProcess = false;  // Stop/fin: mantener In process hasta buffer+holgura OK

// ============================================================
// SECCION 03 — Estado global (ciclo, feed, calibración)
// ============================================================
static SemaphoreHandle_t canMutex = NULL;

bool canInitialized = false;
bool servoCanReady = false;

volatile uint8_t safetyErrorCode = 0;
volatile uint8_t sensorBitmask = 0;         // estado físico del último snapshot
volatile uint8_t sensorLatchedBitmask = 0;  // falla retenida hasta RESET/VDD
volatile uint8_t sensorSequence = 0;
volatile uint32_t sensorLastRxMs = 0;
volatile bool sensorEverOnline = false;
volatile bool sensorStaleLatched = false;  // offline enclavado hasta RESET (no se limpia solo)
volatile bool safetyStopPending = false;   // falla confirmada: terminar paso actual y detener lote
static uint8_t sensorPendingBits = 0;      // candidatos a enclavar (debounce TCM)
static uint32_t sensorPendingSinceMs = 0;
volatile bool cyclePaused = false;
volatile bool cyclePausePending = false;  // Pause: terminar paso actual, luego pausar
volatile uint32_t cyclePausedAccumMs = 0; // tiempo en pausa (no cuenta en elapsed)
volatile uint32_t cyclePauseBeganMs = 0;
volatile bool cycleActive = false;
volatile bool cycleAborted = false;
volatile bool uiOfferLinearRecovery = false;  // tras abort por seguridad: UI pregunta safe zone → HOME
volatile uint32_t uiEventSeq = 1;              // long-poll: sube cuando hay algo que mostrar
static char cyclePauseReason[CYCLE_PAUSE_REASON_MAX] = "";
static char cycleFaultReason[CYCLE_PAUSE_REASON_MAX] = "";
static uint16_t cycleErrorCode = E000;  // ver CycleErrorCode en config.h (UI: solo Exxx)
static uint8_t sensorPollFailStreak = 0;
static volatile bool towerForceResync = false;  // Reset/PLC: reenviar modo aunque no cambie
static volatile bool tcmBuzzerMuted = false;    // mute global (UI principal)
static volatile bool stepByStepMode = false;    // ciclo TCM paso a paso (PreFeeder sigue automático)
// Enclavamiento PreFeeder/TCP en TCM (como sensores): no se auto-limpia al recuperar enlace.
static bool peerEverOk[PEER_COUNT] = { false, false };
static bool peerOfflineLatched[PEER_COUNT] = { false, false };
static uint8_t peerErrorLatchedCode[PEER_COUNT] = { ERR_NONE, ERR_NONE };
static bool pfSettlePending = false;
static char pfSettleTag[24] = "";
static String lastSystemMsg = "";
static uint32_t lastSystemMsgMs = 0;

static inline void uiNotify() { uiEventSeq++; }

static const char* cycleErrorCodeLabel(uint16_t code)
{
  // Solo para Serial/log; la UI muestra únicamente "E0xx".
  switch ((CycleErrorCode)code)
  {
    case E000: return "OK";
    case E001: return "Abort (omitido)";
    case E008: return "Servo CAN no listo";
    case E010: return "Timeout lineal";
    case E011: return "Timeout alimentacion";
    case E012: return "Alimentacion fallida";
    case E016: return "Sensor manguera alimentacion";
    case E020: return "Safety stop";
    case E021: return "Safety pinzas";
    case E022: return "Safety sujetador";
    case E023: return "Safety cortador";
    case E024: return "Safety manguera";
    case E025: return "Safety bandeja";
    case E026: return "Parada externa PreFeeder";
    case E027: return "Sensores CAN offline";
    case E030: return "PreFeeder no inicializado";
    case E031: return "Peer TCP perdido";
    case E099: return "Falla no clasificada";
    default:   return "Desconocido";
  }
}

static String cycleErrorCodeString(uint16_t code)
{
  char buf[8];
  snprintf(buf, sizeof(buf), "E%03u", (unsigned)code);
  return String(buf);
}

static void setCycleError(uint16_t code, const char* detail = nullptr)
{
  const uint16_t prev = cycleErrorCode;
  cycleErrorCode = code;
  if (detail && detail[0])
  {
    strncpy(cycleFaultReason, detail, CYCLE_PAUSE_REASON_MAX - 1);
    cycleFaultReason[CYCLE_PAUSE_REASON_MAX - 1] = '\0';
  }
  else
    cycleFaultReason[0] = '\0';
  // Cualquier Exxx (o su limpieza) debe re-sincronizar torreta / UI.
  if (prev != code)
    towerForceResync = true;
}

bool cycleRunning = false;

uint32_t posPulses = 0;
uint32_t pulsesRemaining = 0;
uint32_t dwellUntilMs = 0;
uint32_t lastFwdCommandPulses = 0;
uint32_t lastBackCommandPulses = 0;
static bool skipDwellAtDestNextFwd = false;

uint16_t D_HOLDER_ON_MS       = D_HOLDER_ON_MS_DEFAULT;
uint16_t D_GRIPPERS_ON_MS     = D_GRIPPERS_ON_MS_DEFAULT;
uint16_t D_LINEAR_DONE_MS     = D_LINEAR_DONE_MS_DEFAULT;
uint16_t D_CUTTER_PULSE_MS    = D_CUTTER_PULSE_MS_DEFAULT;
uint16_t D_CUTTER_POST_MS     = D_CUTTER_POST_MS_DEFAULT;
uint16_t D_GRIPPER_RELEASE_MS = D_GRIPPER_RELEASE_MS_DEFAULT;
uint16_t D_HOLDER_OPEN_MS     = D_HOLDER_OPEN_MS_DEFAULT;

volatile uint16_t linearRpm = LINEAR_RPM_DEFAULT;
volatile uint16_t dwellAtDestMs = DWELL_AT_DEST_MS_DEFAULT;
volatile uint32_t feedServoBasePpA = FEED_SERVO_BASE_PP_DEFAULT;
volatile uint32_t feedServoBasePpB = FEED_SERVO_BASE_PP_DEFAULT;

volatile uint16_t feedParallelStartDelayMs = FEED_PARALLEL_DELAY_MS_DEFAULT;
volatile uint16_t asentarDelayMs = ASENTAR_DELAY_MS_DEFAULT;

volatile uint16_t prefeederTriggerEveryN = 1;  // fijo: siempre 1 (UI eliminada)

volatile uint16_t depositBatchSize = DEPOSIT_BATCH_SIZE_DEFAULT;
volatile float depositExtraMm = DEPOSIT_EXTRA_MM_DEFAULT;
volatile float depositInterlaceMm = DEPOSIT_INTERLACE_MM_DEFAULT;
volatile float depositBetweenBatchMm = DEPOSIT_BETWEEN_BATCH_DEFAULT;

volatile float feedOffsetMm = 0.0f;
volatile float feedOffsetMmB = 0.0f;
volatile float cutOffsetMm = CUT_OFFSET_MM_DEFAULT;  // compensación longitud lineal (+ alarga)
volatile bool laserValidationEnabled = FEED_LASER_VALIDATION_DEFAULT;
volatile bool cutUseLengthOffset = true;  // false en Clean/1st (100 mm fijo)
volatile float feedStepsPerMm = FEED_STEPS_PER_MM_DEFAULT;
volatile float feedStepsPerMmB = FEED_STEPS_PER_MM_DEFAULT;
volatile float feedCalMeasuredMm = FEED_NOMINAL_MM;
volatile float feedCalMeasuredMmB = FEED_NOMINAL_MM;

volatile float linearGripperAreaMm = LINEAR_GRIPPER_AREA_DEFAULT;
volatile float linearStepsPerMm = LINEAR_STEPS_PER_MM;   // pendiente pasos/mm (esclavo ASDA; fallback NVS)
volatile float linearOffsetSteps = 0.0f;                 // intercepto (pasos) — holgura/backlash de arranque

volatile bool cutRequested = false;
volatile int cutLongitud = 0;
volatile int cutCantidad = 0;

volatile int progressStep = 0;
volatile int progressTotalSteps = 0;
volatile int progressCurrentRep = 0;
volatile int progressTotalReps = 0;
volatile uint32_t cycleStartTime = 0;
volatile uint32_t repStartTime = 0;
volatile uint32_t totalCompletedTime = 0;
volatile int completedReps = 0;
static uint32_t lastCycleTotalMs = 0;
static bool lastCycleSuccess = false;

volatile uint32_t gripToHomeStartMs = 0;
volatile uint32_t gripToHomeLastMs = 0;
volatile uint32_t gripToHomeTotalMs = 0;
volatile uint32_t gripToHomeCount = 0;
volatile bool gripToHomeActive = false;
volatile bool gripToHomeEnabled = true;

volatile FeedMode feedMode = FEED_MODE_STEPS_SENSOR;
static FeedMode feedModeThisCycle = FEED_MODE_STEPS_SENSOR;
volatile int32_t feedBypassSteps = FEED_REF_STEPS_DEFAULT;
volatile int32_t feedBypassStepsB = FEED_REF_STEPS_DEFAULT;
volatile int32_t feedTestSolidSteps = FEED_TEST_SOLID_DEFAULT;   // L
volatile int32_t feedTestChunkSteps = FEED_TEST_CHUNK_DEFAULT;   // L
volatile int32_t feedTestSolidStepsR = FEED_TEST_SOLID_DEFAULT;  // R
volatile int32_t feedTestChunkStepsR = FEED_TEST_CHUNK_DEFAULT;  // R
volatile uint32_t feedSsFastPpL = FEED_VELOCITY_PP_DEFAULT;
volatile uint32_t feedSsFastPpR = FEED_VELOCITY_PP_DEFAULT;
volatile int32_t feedStepsTolerance = FEED_STEPS_TOLERANCE_DEFAULT;
volatile uint8_t feedStepsTolerancePercent = FEED_TOLERANCE_PCT_DEFAULT;
volatile bool feedSkipEncoderConfirm = false;

enum FeedPhase : uint8_t {
  FEED_IDLE = 0,
  FEED_RUNNING,
  FEED_CHUNK_DELAY,
  FEED_HOSE_DELAY,
  FEED_PARALLEL_START_DELAY,
  FEED_BYPASS,
  FEED_SS_SOLID,
  FEED_SS_TRANSITION,
  FEED_SS_SLOW,
  FEED_DONE,
  FEED_ERROR
};

enum CycleState : uint8_t { C_IDLE, C_FWD, C_DWELL, C_BACK };

static volatile FeedPhase feedPhase = FEED_IDLE;
static CycleState cycleState = C_IDLE;

static Preferences prefs;
static const char* PREFS_NS = "cortador";
static const char* PREFS_KEY_BYPASS_STEPS = "bypassSteps";
static const char* PREFS_KEY_BYPASS_STEPS_B = "bypassStepsB";
static const char* PREFS_KEY_FEED_MODE = "feedMode";
static const char* PREFS_KEY_STEPS_TOLERANCE = "feedStepsTol";
static const char* PREFS_KEY_TOLERANCE_PCT = "feedTolPct";
static const char* PREFS_KEY_FEED_TEST_SOLID = "feedTestSolid";
static const char* PREFS_KEY_FEED_TEST_CHUNK = "feedTestChunk";
static const char* PREFS_KEY_FEED_TEST_SOLID_R = "feedTestSolidR";
static const char* PREFS_KEY_FEED_TEST_CHUNK_R = "feedTestChunkR";
static const char* PREFS_KEY_FEED_SS_SLOWDOWN_L = "feedSlowdownL";
static const char* PREFS_KEY_FEED_SS_SLOWDOWN_R = "feedSlowdownR";
static const char* PREFS_KEY_FEED_SS_FAST_L = "feedFastPpL";
static const char* PREFS_KEY_FEED_SS_FAST_R = "feedFastPpR";
static const char* PREFS_KEY_FEED_SS_SLOW_L = "feedSlowPpL";
static const char* PREFS_KEY_FEED_SS_SLOW_R = "feedSlowPpR";
static const char* PREFS_KEY_LASER_VALID = "laserValid";
static const char* PREFS_KEY_SKIP_ENCODER = "skipEncConfirm";
static const char* PREFS_KEY_BUZZER_MUTE = "buzzMute";
static const char* PREFS_KEY_STEP_BY_STEP = "stepByStep";
static const char* PREFS_KEY_PREF_TRIG_N = "prefTrigN";
static const char* PREFS_KEY_DEPOSIT_BATCH = "depBatchSz";
static const char* PREFS_KEY_DEPOSIT_EXTRA = "depExtraMm";
static const char* PREFS_KEY_DEPOSIT_ILACE = "depIlaceMm";
static const char* PREFS_KEY_DEPOSIT_BETWEEN = "depBetwMm";
static const char* PREFS_KEY_FEED_OFFSET_MM = "feedOffMm";
static const char* PREFS_KEY_FEED_OFFSET_MM_B = "feedOffMmB";
static const char* PREFS_KEY_FEED_STEPS_PER_MM = "feedStpPerMm";
static const char* PREFS_KEY_FEED_STEPS_PER_MM_B = "feedStpPerMmB";
static const char* PREFS_KEY_FEED_CAL_MM = "feedCalMm";
static const char* PREFS_KEY_FEED_CAL_MM_B = "feedCalMmB";
static const char* PREFS_KEY_LINEAR_GRIPPER = "linGripMm";
static const char* PREFS_KEY_LINEAR_STEPS_PM = "linStpPerMm";
static const char* PREFS_KEY_LINEAR_OFFSET = "linOffStp";
static const char* PREFS_KEY_SP_STEP_HZ = "spStepHz";
static const char* PREFS_KEY_SP_DWELL_DEST = "spDwellDest";
static const char* PREFS_KEY_SP_LIN_DONE = "spLinDone";
static const char* PREFS_KEY_SP_SERVO_PP = "spServoPp";
static const char* PREFS_KEY_SP_SERVO_PP_A = "spServoPpA";
static const char* PREFS_KEY_SP_SERVO_PP_B = "spServoPpB";
static const char* PREFS_KEY_SP_HOLD_ON = "spHoldOn";
static const char* PREFS_KEY_SP_HOLD_OFF = "spHoldOff";
static const char* PREFS_KEY_SP_GRIP_ON = "spGripOn";
static const char* PREFS_KEY_SP_GRIP_REL = "spGripRel";
static const char* PREFS_KEY_SP_CUT_PULSE = "spCutPulse";
static const char* PREFS_KEY_SP_CUT_POST = "spCutPost";
static const char* PREFS_KEY_SP_ASENTAR = "spAsentar";
static const char* PREFS_KEY_SP_PAR_FEED = "spParFeed";

static const char* PART_PREFS_NS = "partRecipes";  // clave NVS histórica; no cambiar o se pierde la BASE
static const char* PART_PREFS_KEY_BASE = "baseRec";
static const char* PART_PREFS_KEY_BASE_OK = "baseOk";

static MachineBase machineBase = {};
static bool machineBaseFromNvs = false;
static bool machineBasePrefeederCaptured = false;
static String activePartNumber = "";
static uint16_t activePartLengthMm = 0;
static bool basePfApplyPending = false;
static uint32_t basePfApplyLastMs = 0;
static uint32_t pfSlaveUiHoldUntilMs = 0;  // >0: UI nativa PF abierta; no pisar esclavos con BASE
static String adminSessionToken = "";
static uint32_t adminSessionLastMs = 0;

static bool feedPrefetch = false;
static bool feedPrefetchSensorLatched = false;
static bool feedChunksOnly = false;
static bool feedSensorArmed = false;
static bool feedSawHoseCleared = false;
// Prefetch ya listo en HOME → siguiente pieza salta re-espera de feed / asentar.
static bool feedHandoffReady = false;
static uint32_t feedPhaseStartMs = 0;
static uint32_t feedDelayUntilMs = 0;
static int feedChunksFed = 0;
static int32_t feedBypassRemaining = 0;
static uint32_t feedStableSinceMs = 0;
static bool feedHaltSent = false;
static int32_t feedStepsFed = 0;
static int32_t feedStepsTargetThisFeed = 0;
static int32_t feedStepsTargetB = 0;
static int32_t feedStartPos601 = 0;
static bool feedEncoderTracking = false;
static volatile bool feedSensorConfirmed = false;
static volatile bool feedSensorConfirmedL = false;
static volatile bool feedSensorConfirmedR = false;
static bool feedNeedSensorL = false;
static bool feedNeedSensorR = false;
static volatile uint8_t feedHoseIrqL = 0;
static volatile uint8_t feedHoseIrqR = 0;
static volatile uint8_t feedHaltArmL = 0;
static volatile uint8_t feedHaltArmR = 0;
static volatile uint32_t feedSsHaltMsL = 0;
static volatile uint32_t feedSsHaltMsR = 0;
static volatile uint8_t feedSsNeedClearL = 0;  // prefetch: no halt hasta ver láser libre
static volatile uint8_t feedSsNeedClearR = 0;
static TaskHandle_t feedHaltTaskHandle = nullptr;
static portMUX_TYPE feedHaltMux = portMUX_INITIALIZER_UNLOCKED;
static bool feedProfileApplied = false;
static volatile bool feedCalibrationTest = false;
static bool feedSolidPhaseActive = false;
static int32_t feedChunkTargetThisFeed = 0;
static int32_t feedChunkTargetB = 0;
static bool feedSolidTargetMet = false;
static int32_t feedSsEncStartPosL = 0;
static int32_t feedSsEncStartPosR = 0;
static bool feedSsEncTrack = false;
static bool feedSsEncTrackL = false;
static bool feedSsEncTrackR = false;
static int32_t feedSsAbsTargetL = 0;
static int32_t feedSsAbsTargetR = 0;
static bool feedSsSensorPrevL = false;
static bool feedSsSensorPrevR = false;
static uint32_t feedSsAbsDueMs = 0;       // 0 = 607A aún no comandado; si no, instante estimado de Target Reached
static uint32_t feedSsMoveStartMs = 0;    // 0 = PP no enviado; timeout 900 ms corre desde aquí
static uint32_t feedSsLastTrPollMs = 0;   // último poll 0x6041 bit 10 (no bloquear el loop)

static bool asdaBusy = false;
static bool asdaHomed = false;
static bool asdaPulseDone = false;
static bool asdaServoOn = false;
static uint32_t asdaLastPollMs = 0;
static int32_t asdaLastPuu = 0;
enum AsdaJob : uint8_t { ASDA_JOB_NONE, ASDA_JOB_HOME, ASDA_JOB_MOVE };
static AsdaJob asdaJob = ASDA_JOB_NONE;

// Alimentación: OM (esclavo) decide 55 mm; láser opcional (solo presencia).
static bool feedOmLengthMet = false;
static bool feedLaserValidated = false;
static bool feedLaserSeen = false;
static float omAtLaserMm = 0.0f;
static float feedOmLastMmAbs = -1.0f;
static uint32_t feedOmLastPollMs = 0;

// Supervisión OM por fase (Opción B: OM cierra feed; ±1 mm interno decide corte).
static float omPhase1Mm = 0.0f;
static float omPhase2Mm = 0.0f;
static float omErrPhase1Mm = 0.0f;
static float omErrPhase2Mm = 0.0f;
static float omTotalMm = 0.0f;
static float omErrTotalMm = 0.0f;
static bool omPhase1Ok = false;
static bool omPhase2Ok = false;
static bool omTotalInternalOk = false;
static bool omTotalProdOk = false;  // solo diagnóstico; no decide corte

bool stCutters  = false;
bool stGrippers = false;
bool stHolder   = true;   // Default ON; OFF solo por rutina o manual
bool stFgtray   = false;
bool stReset    = false;

// ============================================================
// SECCION 04 — I/O (PLC, sensores, seguridad física)
// ============================================================
void applyPlcOutputs()
{
  digitalWrite(PLC_IN_R000_CUTTERS,  stCutters  ? HIGH : LOW);
  digitalWrite(PLC_IN_R002_GRIPPERS, stGrippers ? HIGH : LOW);
  digitalWrite(PLC_IN_R003_HOLDER,   stHolder   ? HIGH : LOW);
  digitalWrite(PLC_IN_R004_FGTRAY,   stFgtray   ? HIGH : LOW);
  digitalWrite(PLC_IN_R103_RESET,    stReset    ? HIGH : LOW);
}

void printPlcStates()
{
  DBG_PRINT("CUTTERS=");   DBG_PRINT(stCutters  ? "ON" : "OFF");
  DBG_PRINT(" | GRIPPERS="); DBG_PRINT(stGrippers ? "ON" : "OFF");
  DBG_PRINT(" | HOLDER=");   DBG_PRINT(stHolder   ? "ON" : "OFF");
  DBG_PRINT(" | FGTRAY=");   DBG_PRINT(stFgtray   ? "ON" : "OFF");
  DBG_PRINT(" | RESET=");    DBG_PRINTLN(stReset    ? "ON" : "OFF");
}

inline int hoseSensorGpioReadSide(bool sideR)
{
  const int pin = sideR ? HOSE_SENSOR_PIN_R : HOSE_SENSOR_PIN_L;
#if HOSE_SENSOR_PIN_L >= 0 || HOSE_SENSOR_PIN_R >= 0
  if (pin < 0) return -1;
  return digitalRead(pin) ? 1 : 0;
#else
  (void)pin;
  return -1;
#endif
}

inline bool isHoseSensorActiveSide(bool sideR)
{
  const int pin = sideR ? HOSE_SENSOR_PIN_R : HOSE_SENSOR_PIN_L;
  if (pin < 0) return false;
  bool level = (hoseSensorGpioReadSide(sideR) == 1);
  return HOSE_SENSOR_ACTIVE_HIGH ? level : !level;
}

#if HOSE_SENSOR_PIN_L >= 0
void IRAM_ATTR feedHoseIsrL()
{
  feedHoseIrqL = 1;
  if (!feedHaltArmL || feedHaltTaskHandle == nullptr) return;
  BaseType_t hp = pdFALSE;
  vTaskNotifyGiveFromISR(feedHaltTaskHandle, &hp);
  if (hp == pdTRUE) portYIELD_FROM_ISR();
}
#endif
#if HOSE_SENSOR_PIN_R >= 0
void IRAM_ATTR feedHoseIsrR()
{
  feedHoseIrqR = 1;
  if (!feedHaltArmR || feedHaltTaskHandle == nullptr) return;
  BaseType_t hp = pdFALSE;
  vTaskNotifyGiveFromISR(feedHaltTaskHandle, &hp);
  if (hp == pdTRUE) portYIELD_FROM_ISR();
}
#endif

static bool isHoseSensorStableActiveSide(bool sideR, uint16_t stableMs)
{
  if (!isHoseSensorActiveSide(sideR)) return false;
  uint32_t t0 = millis();
  while (millis() - t0 < stableMs)
  {
    if (!isHoseSensorActiveSide(sideR)) return false;
    delay(1);
  }
  return true;
}

static void feedSyncSensorConfirmedAll()
{
  feedSensorConfirmed = (!feedNeedSensorL || feedSensorConfirmedL)
                     && (!feedNeedSensorR || feedSensorConfirmedR);
}

void setupIoPins()
{
  pinMode(PLC_IN_R000_CUTTERS,  OUTPUT);
  pinMode(PLC_IN_R002_GRIPPERS, OUTPUT);
  pinMode(PLC_IN_R003_HOLDER,   OUTPUT);
  pinMode(PLC_IN_R004_FGTRAY,   OUTPUT);
  pinMode(PLC_IN_R103_RESET,    OUTPUT);

  stCutters = stGrippers = stFgtray = stReset = false;
  stHolder = true;  // Holder ON por defecto (solo OFF por rutina o manual)
  applyPlcOutputs();

#if HOSE_SENSOR_PIN_L >= 0
  pinMode(HOSE_SENSOR_PIN_L, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(HOSE_SENSOR_PIN_L), feedHoseIsrL,
                  HOSE_SENSOR_ACTIVE_HIGH ? RISING : FALLING);
  DBG_PRINT("Sensor manguera L GPIO ");
  DBG_PRINT(HOSE_SENSOR_PIN_L);
  DBG_PRINT(" raw=");
  DBG_PRINT(hoseSensorGpioReadSide(false));
  DBG_PRINT(" -> ");
  DBG_PRINTLN(isHoseSensorActiveSide(false) ? "DETECTADO" : "libre");
#endif
#if HOSE_SENSOR_PIN_R >= 0
  pinMode(HOSE_SENSOR_PIN_R, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(HOSE_SENSOR_PIN_R), feedHoseIsrR,
                  HOSE_SENSOR_ACTIVE_HIGH ? RISING : FALLING);
  DBG_PRINT("Sensor manguera R GPIO ");
  DBG_PRINT(HOSE_SENSOR_PIN_R);
  DBG_PRINT(" raw=");
  DBG_PRINT(hoseSensorGpioReadSide(true));
  DBG_PRINT(" -> ");
  DBG_PRINTLN(isHoseSensorActiveSide(true) ? "DETECTADO" : "libre");
#endif

  DBG_PRINTLN("Disparo prefeeder: solo TCP (sin GPIO)");
  DBG_PRINTLN("Parada externa: sin enlace TCP o PreFeeder en falla");
}

// ============================================================
// SECCION 05 — Peer TCP (cliente → PreFeeder L/R)
// ============================================================
static String peerEsc(const String& s)
{
  // setAllCfg usa ~40+ chars; no truncar agresivo o el esclavo rechaza el comando.
  String o;
  for (unsigned i = 0; i < s.length() && o.length() < 160; i++)
  {
    char c = s.charAt(i);
    if (c == '"' || c == '\\') o += '\\';
    if (c != '\n' && c != '\r') o += c;
  }
  return o;
}

static bool peerJBool(const String& j, const char* k, bool d = false)
{
  String n = String("\"") + k + "\":";
  int i = j.indexOf(n);
  return i < 0 ? d : j.substring(i + n.length()).startsWith("true");
}

static int peerJInt(const String& j, const char* k, int d = 0)
{
  String n = String("\"") + k + "\":";
  int i = j.indexOf(n);
  return i < 0 ? d : j.substring(i + n.length()).toInt();
}

static float peerJFloat(const String& j, const char* k, float d = 0)
{
  String n = String("\"") + k + "\":";
  int i = j.indexOf(n);
  return i < 0 ? d : j.substring(i + n.length()).toFloat();
}

static String peerJStr(const String& j, const char* k)
{
  String n = String("\"") + k + "\":\"";
  int i = j.indexOf(n);
  if (i < 0) return "";
  int a = i + n.length(), b = j.indexOf('"', a);
  return b < 0 ? "" : j.substring(a, b);
}

static int8_t peerParseSideArg(const String& raw)
{
  if (!raw.length()) return -1;
  char c = raw.charAt(0);
  if (c == 'L' || c == 'l' || c == '0') return PEER_L;
  if (c == 'R' || c == 'r' || c == '1') return PEER_R;
  return -1;
}

static void peerApplyStatus(uint8_t side, const String& msg)
{
  if (side >= PEER_COUNT) return;
  PrefeederMirror& m = peers[side].mirror;
  const bool home = peerJBool(msg, "home", m.home);
  const bool endstop = peerJBool(msg, "endstop", m.endstop);
  const bool tension = peerJBool(msg, "tension", m.tension);
  const bool cylinderOpen = peerJBool(msg, "cylinderOpen", m.cylinderOpen);
  const bool hoseAbsent = peerJBool(msg, "hoseAbsent", m.hoseAbsent);
  const bool error = peerJBool(msg, "error", m.error);
  const bool autoEnabled = peerJBool(msg, "autoEnabled", m.autoEnabled);
  const bool holgura = peerJBool(msg, "holgura", m.holgura);
  const bool triggerActive = peerJBool(msg, "triggerActive", m.triggerActive);
  const bool refillMaterial = peerJBool(msg, "refillMaterial", m.refillMaterial);
  const bool refillDereeler = peerJBool(msg, "refillDereeler", m.refillDereeler);
  const bool refillServo = peerJBool(msg, "refillServo", m.refillServo);
  const bool refillFeeder = peerJBool(msg, "refillFeeder", m.refillFeeder);
  const bool idleMode = msg.indexOf("\"idleMode\":") >= 0
      ? peerJBool(msg, "idleMode", m.idleMode)
      : peerJBool(msg, "testMode", m.idleMode);  // alias antiguo
  const bool buzzerMuted = peerJBool(msg, "buzzerMuted", m.buzzerMuted);
  const bool muteChanged = (buzzerMuted != m.buzzerMuted);
  const bool inProcess = peerJBool(msg, "inProcess", m.inProcess);
  const uint32_t triggerMsLeft = (uint32_t)peerJInt(msg, "triggerMsLeft", (int)m.triggerMsLeft);
  bool changed = (home != m.home) || (endstop != m.endstop)
    || (tension != m.tension) || (cylinderOpen != m.cylinderOpen)
    || (hoseAbsent != m.hoseAbsent)
    || (error != m.error) || (autoEnabled != m.autoEnabled)
    || (holgura != m.holgura) || (triggerActive != m.triggerActive)
    || (refillMaterial != m.refillMaterial) || (refillDereeler != m.refillDereeler)
    || (refillServo != m.refillServo) || (refillFeeder != m.refillFeeder)
    || (idleMode != m.idleMode) || (buzzerMuted != m.buzzerMuted)
    || (inProcess != m.inProcess)
    || (triggerMsLeft != m.triggerMsLeft);

  m.home = home;
  m.endstop = endstop;
  m.tension = tension;
  m.cylinderOpen = cylinderOpen;
  m.hoseAbsent = hoseAbsent;
  m.error = error;
  m.autoEnabled = autoEnabled;
  m.holgura = holgura;
  m.triggerActive = triggerActive;
  m.refillMaterial = refillMaterial;
  m.refillDereeler = refillDereeler;
  m.refillServo = refillServo;
  m.refillFeeder = refillFeeder;
  m.idleMode = idleMode;
  m.buzzerMuted = buzzerMuted;
  m.inProcess = inProcess;
  m.triggerMsLeft = triggerMsLeft;
  if (msg.indexOf("\"errorReason\":\"") >= 0)
  {
    String er = peerJStr(msg, "errorReason");
    if (er != m.errorReason) { m.errorReason = er; changed = true; }
  }
  if (msg.indexOf("\"errorCode\":") >= 0)
  {
    const uint8_t ec = (uint8_t)constrain(peerJInt(msg, "errorCode", (int)m.errorCode), 0, 255);
    if (ec != m.errorCode) { m.errorCode = ec; changed = true; }
  }
  else if (!error)
  {
    if (m.errorCode != 0) { m.errorCode = 0; changed = true; }
  }
  else
  {
    const uint8_t mapped = pfErrorCodeFromReason(side, m.errorReason);
    if (mapped != m.errorCode) { m.errorCode = mapped; changed = true; }
  }
  if (msg.indexOf("\"autoState\":\"") >= 0)
  {
    String st = peerJStr(msg, "autoState");
    if (st != m.autoState) { m.autoState = st; changed = true; }
  }
  if (msg.indexOf("\"triggerPhase\":\"") >= 0)
  {
    String ph = peerJStr(msg, "triggerPhase");
    if (ph != m.triggerPhase) { m.triggerPhase = ph; changed = true; }
  }
  // Settings finos (.50/.51): detectar cambio real para no pisarlos con reintentos de BASE.
  bool settingsChanged = false;
  if (msg.indexOf("\"holguraExtraFeedS\":") >= 0)
  {
    const float v = peerJFloat(msg, "holguraExtraFeedS", m.holguraExtraFeedS);
    if (fabsf(v - m.holguraExtraFeedS) > 0.0005f) { m.holguraExtraFeedS = v; settingsChanged = true; }
  }
  if (msg.indexOf("\"triggerFeedS\":") >= 0)
  {
    const float v = peerJFloat(msg, "triggerFeedS", m.triggerFeedS);
    if (fabsf(v - m.triggerFeedS) > 0.0005f) { m.triggerFeedS = v; settingsChanged = true; }
  }
  if (msg.indexOf("\"autoRpm\":") >= 0)
  {
    const float v = peerJFloat(msg, "autoRpm", m.autoRpm);
    if (fabsf(v - m.autoRpm) > 0.0005f) { m.autoRpm = v; settingsChanged = true; }
  }
  if (msg.indexOf("\"autoReverseS\":") >= 0)
  {
    const float v = peerJFloat(msg, "autoReverseS", m.autoReverseS);
    if (fabsf(v - m.autoReverseS) > 0.0005f) { m.autoReverseS = v; settingsChanged = true; }
  }
  if (msg.indexOf("\"motor2Rpm\":") >= 0)
  {
    const float v = peerJFloat(msg, "motor2Rpm", m.motor2Rpm);
    if (fabsf(v - m.motor2Rpm) > 0.0005f) { m.motor2Rpm = v; settingsChanged = true; }
  }
  if (msg.indexOf("\"tensionCooldownS\":") >= 0)
  {
    const float v = peerJFloat(msg, "tensionCooldownS", m.tensionCooldownS);
    if (fabsf(v - m.tensionCooldownS) > 0.0005f) { m.tensionCooldownS = v; settingsChanged = true; }
  }
  if (msg.indexOf("\"tensionFaultS\":") >= 0)
  {
    const float v = peerJFloat(msg, "tensionFaultS", m.tensionFaultS);
    if (fabsf(v - m.tensionFaultS) > 0.0005f) { m.tensionFaultS = v; settingsChanged = true; }
  }
  if (msg.indexOf("\"bufferRefillFaultS\":") >= 0)
  {
    const float v = peerJFloat(msg, "bufferRefillFaultS", m.bufferRefillFaultS);
    if (fabsf(v - m.bufferRefillFaultS) > 0.0005f) { m.bufferRefillFaultS = v; settingsChanged = true; }
  }
  if (msg.indexOf("\"servoPwmUs\":") >= 0)
  {
    const uint16_t v = (uint16_t)peerJInt(msg, "servoPwmUs", (int)m.servoPwmUs);
    if (v != m.servoPwmUs) { m.servoPwmUs = v; settingsChanged = true; }
  }
  if (msg.indexOf("\"dereelerLeadMs\":") >= 0)
  {
    const uint32_t lead = (uint32_t)constrain(peerJInt(msg, "dereelerLeadMs", (int)m.dereelerLeadMs), 0, 5000);
    if (lead != m.dereelerLeadMs) { m.dereelerLeadMs = lead; settingsChanged = true; }
  }
  if (msg.indexOf("\"refillPulseS\":") >= 0)
  {
    const float v = peerJFloat(msg, "refillPulseS", m.refillPulseS);
    if (fabsf(v - m.refillPulseS) > 0.05f) { m.refillPulseS = v; changed = true; }
  }
  if (settingsChanged)
  {
    changed = true;
    // Ajuste en esclavo/UI nativa: absorber espejo y no reaplicar BASE pendiente.
    basePfApplyPending = false;
    persistPrefeederMirrorToBase();
  }
  if (muteChanged)
    towerForceResync = true;
  if (changed) uiNotify();
}

static void peerOnMsg(uint8_t side, const String& msg)
{
  if (side >= PEER_COUNT) return;
  PeerSlot& p = peers[side];
  PrefeederMirror& m = p.mirror;
  p.lastRxMs = millis();
  p.lastMessageMs = p.lastRxMs;
  p.lastMessage = msg;
  p.lastMessageDirection = 'R';
  // ACK TCP eliminado: solo status/event/hello. Ignorar ack residual de firmware viejo.
  if (msg.indexOf("\"type\":\"ack\"") >= 0)
    return;
  if (msg.indexOf("\"type\":\"status\"") >= 0)
  {
    peerApplyStatus(side, msg);
    p.fullStatusReceived = true;
  }
  else if (msg.indexOf("\"type\":\"hello\"") >= 0)
  {
    peerApplyStatus(side, msg);
  }
  else if (msg.indexOf("\"type\":\"event\"") >= 0)
  {
    String f = peerJStr(msg, "field");
    if (f == "home") m.home = peerJBool(msg, "value", m.home);
    else if (f == "endstop") m.endstop = peerJBool(msg, "value", m.endstop);
    else if (f == "tension") m.tension = peerJBool(msg, "value", m.tension);
    else if (f == "cylinderOpen") m.cylinderOpen = peerJBool(msg, "value", m.cylinderOpen);
    else if (f == "hoseAbsent") m.hoseAbsent = peerJBool(msg, "value", m.hoseAbsent);
    else if (f == "refill")
    {
      m.refillMaterial = peerJBool(msg, "material", m.refillMaterial);
      m.refillDereeler = peerJBool(msg, "dereeler", m.refillDereeler);
      m.refillServo = peerJBool(msg, "servo", m.refillServo);
      m.refillFeeder = peerJBool(msg, "feeder", m.refillFeeder);
    }
    else if (f == "materialistaCall")
    {
      // Alias antiguo: Materialista = idleMode.
      if (peerJBool(msg, "value", false))
        m.idleMode = true;
    }
    else if (f == "error")
    {
      m.error = peerJBool(msg, "value", m.error);
      if (msg.indexOf("\"errorReason\":\"") >= 0) m.errorReason = peerJStr(msg, "errorReason");
      if (msg.indexOf("\"errorCode\":") >= 0)
        m.errorCode = (uint8_t)constrain(peerJInt(msg, "errorCode", (int)m.errorCode), 0, 255);
      else if (!m.error)
        m.errorCode = 0;
    }
    else if (f == "auto")
    {
      m.autoEnabled = peerJBool(msg, "autoEnabled", m.autoEnabled);
      if (msg.indexOf("\"autoState\":\"") >= 0) m.autoState = peerJStr(msg, "autoState");
    }
    else if (f == "holgura") m.holgura = peerJBool(msg, "value", m.holgura);
    else if (f == "triggerActive")
    {
      m.triggerActive = peerJBool(msg, "value", m.triggerActive);
      if (msg.indexOf("\"triggerPhase\":\"") >= 0) m.triggerPhase = peerJStr(msg, "triggerPhase");
    }
    else if (f == "triggerMsLeft")
      m.triggerMsLeft = (uint32_t)peerJInt(msg, "value", (int)m.triggerMsLeft);
    else if (f == "idleMode" || f == "testMode")
      m.idleMode = peerJBool(msg, "value", m.idleMode);
    else if (f == "buzzerMuted")
    {
      const bool muted = peerJBool(msg, "value", m.buzzerMuted);
      if (muted != m.buzzerMuted)
      {
        m.buzzerMuted = muted;
        towerForceResync = true;
      }
    }
    else if (f == "inProcess")
      m.inProcess = peerJBool(msg, "value", m.inProcess);
    uiNotify();
  }
}

static void peerMarkDead(uint8_t side, const char* why);
static bool peerSendCmdEx(uint8_t side, const char* cmd, const String& val, uint32_t ackTimeoutMs);

static void peerRx(uint8_t side)
{
  if (side >= PEER_COUNT) return;
  PeerSlot& p = peers[side];
  const bool wasUp = p.client.connected() || p.mirror.peerOk;
  if (!wasUp) return;
  while (p.client.available())
  {
    int c = p.client.read();
    if (c < 0)
    {
      peerMarkDead(side, "rx");
      return;
    }
    if (c == '\n' || c == '\r')
    {
      if (p.rxLen)
      {
        p.rxLine[p.rxLen] = 0;
        peerOnMsg(side, String(p.rxLine));
        p.rxLen = 0;
      }
    }
    else if (p.rxLen < sizeof(p.rxLine) - 1)
      p.rxLine[p.rxLen++] = (char)c;
  }
  if (!p.client.connected())
    peerMarkDead(side, "rx-disconnect");
}

static void peerRxDrain(uint8_t side)
{
  for (uint8_t i = 0; i < 24; i++)
  {
    peerRx(side);
    if (!peers[side].client.available()) break;
  }
}

static void peerRxDrainAll()
{
  for (uint8_t s = 0; s < PEER_COUNT; s++)
    peerRxDrain(s);
}

static void wifiStopPeers()
{
  for (uint8_t s = 0; s < PEER_COUNT; s++)
  {
    peers[s].client.stop();
    peers[s].mirror.peerOk = false;
    peers[s].fullStatusReceived = false;
  }
}

static void wifiRetry()
{
  wifiStopPeers();
  server.close();
  WiFi.disconnect(true);
  delay(200);
  wifiBeginSta();
  Serial.println("[WIFI] Reintento STA...");
}

// Conservador: cualquier duda de movimiento → false. Mejor retrasar reconnect que bloquear RMT/feed/dwell.
static bool peerConnectAllowed()
{
  if (asdaBusy) return false;
  if (cycleRunning) return false;
  if (feedPhaseIsActive()) return false;
  if (feedStep2NeedsWait()) return false;
  if (cycleActive && !cyclePaused) return false;
  if (cyclePausePending) return false;
  return true;
}

static void peerMarkDead(uint8_t side, const char* why)
{
  if (side >= PEER_COUNT) return;
  PeerSlot& p = peers[side];
  const bool wasUp = p.client.connected() || p.mirror.peerOk;
  p.client.stop();
  p.mirror.peerOk = false;
  p.fullStatusReceived = false;
  if (!wasUp) return;
  if (p.failStreak < 10) p.failStreak++;
  Serial.printf("[PEER-%s] TCP dead (%s)\n", PEER_TAGS[side], why ? why : "?");
}

static void wifiService()
{
  const bool on = wifiIsUp();
  if (on != wifiWasConnected)
  {
    wifiWasConnected = on;
    if (on)
    {
      wifiConnectStartedMs = 0;
      server.close();
      delay(20);
      server.begin();
      for (uint8_t s = 0; s < PEER_COUNT; s++)
      {
        peers[s].failStreak = 0;
        peers[s].lastReconnectMs = 0;
      }
      Serial.printf("[WIFI] On http://%s\n", WiFi.localIP().toString().c_str());
    }
    else
    {
      wifiStopPeers();
      server.close();
      Serial.println("[WIFI] Off");
    }
  }

  if (on || !wifiNeedsRetry()) return;
  const bool busyMotion = cycleRunning || (cycleActive && !cyclePaused);
  if (busyMotion) return;
  if (millis() - lastWifiRetryMs < 8000) return;
  lastWifiRetryMs = millis();
  wifiRetry();
}

// Único sitio que llama WiFiClient.connect(). peerTryReconnect() es el único caller.
static void peerConnect(uint8_t side, bool force = false)
{
  if (side >= PEER_COUNT) return;
  if (!peerConnectAllowed()) return;
  PeerSlot& p = peers[side];
  if (p.client.connected()) return;
  p.mirror.peerOk = false;
  if (!wifiIsUp()) return;

  const uint32_t now = millis();
  uint32_t retryMs;
  if (!p.everConnected)
  {
    if (p.failStreak == 0) retryMs = 400u;
    else if (p.failStreak == 1) retryMs = 800u;
    else if (p.failStreak == 2) retryMs = 1500u;
    else if (p.failStreak == 3) retryMs = 2000u;
    else retryMs = 3000u;
  }
  else
  {
    retryMs = 2000u;
    if (p.failStreak >= 2) retryMs = 5000u;
    if (p.failStreak >= 4) retryMs = 10000u;
  }
  if (!force && (now - p.lastReconnectMs < retryMs)) return;
  p.lastReconnectMs = now;

  p.client.stop();
  p.fullStatusReceived = false;

  if (p.client.connect(PEER_HOSTS[side], PEER_PORT, PEER_CONNECT_TIMEOUT_MS))
  {
    p.client.setNoDelay(true);
    p.lastRxMs = 0;
    p.rxLen = 0;
    p.lastProbeMs = 0;
    p.failStreak = 0;
    p.everConnected = true;
    p.mirror.peerOk = true;
    Serial.printf("[PEER-%s] TCP connect OK (%s)%s\n",
                  PEER_TAGS[side], PEER_HOSTS[side], force ? " [manual]" : "");
  }
  else
  {
    if (p.failStreak < 10) p.failStreak++;
    p.client.stop();
    Serial.printf("[PEER-%s] TCP fail streak=%u%s\n",
                  PEER_TAGS[side], p.failStreak,
                  force ? " [manual]" : "");
  }
}

static void peerTryReconnect(int8_t forceSide)
{
  if (!peerConnectAllowed() || !wifiIsUp()) return;

  if (forceSide >= 0 && forceSide < (int8_t)PEER_COUNT)
  {
    peerConnect((uint8_t)forceSide, true);
    return;
  }

  static uint8_t peerReconnectCursor = 0;
  for (uint8_t i = 0; i < PEER_COUNT; i++)
  {
    const uint8_t s = (uint8_t)((peerReconnectCursor + i) % PEER_COUNT);
    PeerSlot& p = peers[s];
    if (p.client.connected()) continue;
    const uint32_t before = p.lastReconnectMs;
    peerConnect(s, false);
    peerReconnectCursor = (uint8_t)((s + 1) % PEER_COUNT);
    if (p.client.connected() || p.lastReconnectMs != before)
      break;
  }
}

// Manual: si el camino no es crítico, connect ya. Si no, solo programa (lastReconnectMs=0).
static bool peerForceReconnect(uint8_t side)
{
  if (side >= PEER_COUNT) return false;
  PeerSlot& p = peers[side];
  peerMarkDead(side, "manual");
  p.failStreak = 0;
  p.lastReconnectMs = 0;
  Serial.printf("[PEER-%s] Reconexión manual pedida%s\n",
                PEER_TAGS[side],
                peerConnectAllowed() ? "" : " (programada)");
  peerTryReconnect((int8_t)side);
  return p.client.connected();
}

static void peerProbeKeepalive(uint8_t side)
{
  if (side >= PEER_COUNT) return;
  PeerSlot& p = peers[side];
  if (!p.client.connected() || !p.lastRxMs) return;
  const uint32_t now = millis();
  if ((now - p.lastRxMs) < PEER_KEEPALIVE_MS) return;
  if (p.lastProbeMs && (now - p.lastProbeMs) < PEER_KEEPALIVE_MS) return;
  p.lastProbeMs = now;
  if (!p.client.print("\n"))
    peerMarkDead(side, "keepalive");
}

static void peerService()
{
  peerRxDrainAll();

  const uint32_t now = millis();
  for (uint8_t s = 0; s < PEER_COUNT; s++)
  {
    PeerSlot& p = peers[s];
    if (p.client.connected())
    {
      if (p.lastRxMs && (now - p.lastRxMs > PEER_STALE_MS))
        peerMarkDead(s, "stale");
      else
        peerProbeKeepalive(s);
    }
    else if (p.mirror.peerOk)
      peerMarkDead(s, "disconnect");

    p.mirror.peerOk = p.client.connected();
    if (p.mirror.peerOk != p.linkWas)
    {
      Serial.printf("[PEER-%s] TCP %s\n",
                    PEER_TAGS[s],
                    p.mirror.peerOk ? "On" : "Off");
      p.linkWas = p.mirror.peerOk;
      if (p.mirror.peerOk)
      {
        p.mirror.buzzerMuted = tcmBuzzerMuted;
        (void)peerSendCmdEx(s, "setBuzzerMute", tcmBuzzerMuted ? "1" : "0", 0);
      }
      uiNotify();
    }
  }

  peerSyncInProcessFlag();
}

// Sin ACK TCP: envío fire-and-forget. ok = bytes escritos (enlace vivo).
// ackTimeoutMs se ignora (compat call sites); errores llegan por status/event.
static bool peerSendCmdEx(uint8_t side, const char* cmd, const String& val, uint32_t /*ackTimeoutMs*/)
{
  if (side >= PEER_COUNT) return false;
  PeerSlot& p = peers[side];
  if (!p.client.connected()) return false;

  const uint16_t id = peerCmdId++;
  if (!peerCmdId) peerCmdId = 1;

  String j = "{\"type\":\"command\",\"command\":\"";
  j += cmd;
  j += "\"";
  if (val.length())
  {
    j += ",\"value\":\"";
    j += peerEsc(val);
    j += "\"";
  }
  j += ",\"id\":";
  j += id;
  j += "}\n";

  p.lastMessageMs = millis();
  p.lastMessage = j;
  p.lastMessage.trim();
  p.lastMessageDirection = 'E';

  // setNoDelay ya está; flush() bloquea el hilo de ciclo innecesariamente.
  const size_t wrote = p.client.print(j);
  if (!wrote)
  {
    peerMarkDead(side, "tx");
    return false;
  }

  // Drenar RX por si llegó status/event; no esperar ACK.
  peerRxDrain(side);
  return true;
}

static bool peerSendCmd(uint8_t side, const char* cmd, const String& val = "")
{
  return peerSendCmdEx(side, cmd, val, UINT32_MAX);
}

// Avisa a PreFeeder L/R: In process ON con ciclo activo o settle post-lote.
// Armado → buffer/holgura en Production; OFF → sin movimiento automático.
static void peerSyncInProcessFlag()
{
  static int8_t lastSent[PEER_COUNT] = { -1, -1 };
  const int8_t want = (cycleActive || peerSettleHoldInProcess) ? 1 : 0;
  for (uint8_t s = 0; s < PEER_COUNT; s++)
  {
    if (!peers[s].client.connected() || !peers[s].mirror.peerOk)
    {
      lastSent[s] = -1;
      continue;
    }
    if (lastSent[s] == want) continue;
    if (peerSendCmd(s, "setInProcess", want ? "1" : "0"))
      lastSent[s] = want;
  }
}

static bool prefeederIdleModeActive()
{
  for (uint8_t s = 0; s < PEER_COUNT; s++)
  {
    if (peers[s].mirror.peerOk && peers[s].mirror.idleMode) return true;
  }
  return false;
}

// home = Buffer Full; holgura = slack presente. Ambos OK = material listo en ese lado.
static bool peerSideBufferOk(uint8_t side)
{
  if (side >= PEER_COUNT) return false;
  const PrefeederMirror& m = peers[side].mirror;
  return m.peerOk && !m.error && m.home;
}

static bool peerSideHolguraOk(uint8_t side)
{
  if (side >= PEER_COUNT) return false;
  const PrefeederMirror& m = peers[side].mirror;
  return m.peerOk && !m.error && m.holgura;
}

static bool peerSideQuiet(uint8_t side)
{
  if (side >= PEER_COUNT) return false;
  const PrefeederMirror& m = peers[side].mirror;
  if (!m.peerOk) return false;
  if (m.triggerActive) return false;
  if (m.triggerPhase.length() && m.triggerPhase != "idle") return false;
  if (m.autoState == "cw" || m.autoState == "servo_lead")
    return false;
  return true;
}

static bool peerBothHolguraOk()
{
  return peerSideHolguraOk(PEER_L) && peerSideHolguraOk(PEER_R);
}

static bool peerBothMaterialReady()
{
  return peerSideBufferOk(PEER_L) && peerSideBufferOk(PEER_R)
      && peerSideHolguraOk(PEER_L) && peerSideHolguraOk(PEER_R);
}

static bool peerBothReadyQuiet()
{
  if (!peerBothMaterialReady()) return false;
  return peerSideQuiet(PEER_L) && peerSideQuiet(PEER_R);
}

static bool peerBothAutoArmedForRefill()
{
  for (uint8_t s = 0; s < PEER_COUNT; s++)
  {
    const PrefeederMirror& m = peers[s].mirror;
    if (!m.peerOk || m.idleMode || !m.autoEnabled) return false;
  }
  return true;
}

// Espera cooperativa: drena TCP, web y respeta abort/pause.
// requireQuiet: además de buffer+holgura, PF sin movimiento.
// holguraOnly: solo holgura (entre piezas).
// abortable: si cycleAborted/!cycleActive (y no settle), sale.
static bool waitPrefeederCondition(uint32_t timeoutMs, bool requireQuiet, bool holguraOnly,
                                   bool abortable, const char* tag)
{
  const uint32_t t0 = millis();
  uint32_t stableSince = 0;
  uint32_t lastCanMs = millis();
  uint32_t lastWebMs = millis();
  bool loggedWait = false;

  while ((uint32_t)(millis() - t0) < timeoutMs)
  {
    peerService();
    serviceExternalStopInput();
    serviceBackgroundTick(lastCanMs, lastWebMs);
    if (cycleRunning) serviceCycle();
    if (feedStep2NeedsWait() || !linearIsMoving())
      serviceServoFeed();

    if (abortable && (!cycleActive || cycleAborted))
      return false;

    // Pause pendiente durante espera de Start / material: respetar ya.
    // Paso a paso no congela buffer/holgura del PreFeeder (sigue automático).
    if (abortable && cyclePausePending && cycleActive)
    {
      const bool stepByStepAuto = stepByStepMode
        && !(cyclePauseReason[0] && !strncmp(cyclePauseReason, "Pause manual", 12));
      if (!stepByStepAuto)
      {
        cyclePausePending = false;
        enterCyclePaused(tag && tag[0] ? tag : "espera PF");
      }
    }
    while (abortable && cyclePaused && cycleActive)
    {
      peerService();
      serviceExternalStopInput();
      serviceBackgroundTick(lastCanMs, lastWebMs);
      delay(10);
    }
    if (abortable && (!cycleActive || cycleAborted))
      return false;

    // Settle post-lote: ceder si el operador ya pidió Start.
    if (settleShouldYieldToStart())
      return false;

    if (prefeederIdleModeActive())
    {
      pushLog(String("PREFEEDER wait abort Materialista (") + (tag ? tag : "?") + ")");
      return false;
    }

    bool ok = holguraOnly ? peerBothHolguraOk()
                          : (requireQuiet ? peerBothReadyQuiet() : peerBothMaterialReady());

    if (ok)
    {
      if (!stableSince) stableSince = millis();
      else if ((uint32_t)(millis() - stableSince) >= PREFEEDER_READY_STABLE_MS)
      {
        DBG_PRINTF("PREFEEDER wait(%s): OK en %lu ms\n",
                   tag ? tag : "?", (unsigned long)(millis() - t0));
        return true;
      }
    }
    else
    {
      stableSince = 0;
      if (!loggedWait)
      {
        loggedWait = true;
        pushLog(String("PREFEEDER: esperando ") + (tag ? tag : "listo"));
      }
    }

    while (cyclePaused && cycleActive)
    {
      peerService();
      peerTryReconnect();
      serviceExternalStopInput();
      serviceBackgroundTick(lastCanMs, lastWebMs);
      delay(5);
    }

    delay(5);
  }

  Serial.printf("PREFEEDER wait(%s): timeout %lu ms\n",
                tag ? tag : "?", (unsigned long)timeoutMs);
  pushLog(String("PREFEEDER: timeout ") + (tag ? tag : "espera"));
  return false;
}

// Primer Start: armar y esperar buffer Full + holgura.
// No exigir "quiet": tras In process ON el PF puede seguir ajustando y
// retrasaba la 1ª pieza muchos segundos aunque el material ya estuviera listo.
static bool waitPrefeederReadyAtStart()
{
  peerSyncInProcessFlag();
  peerRxDrainAll();  // el PF emite status al setInProcess; no esperar el tick de 2 s
  if (!peerBothAutoArmedForRefill())
  {
    Serial.println("PREFEEDER Start: falta Production+Iniciar en L/R");
    pushLog("PREFEEDER: falta Iniciar (Production)");
    return false;
  }
  if (peerBothMaterialReady())
    return true;
  return waitPrefeederCondition(PREFEEDER_READY_TIMEOUT_MS, false, false, true, "Start");
}

// Tras HOME: el gap paso18→HOME ya debió rellenar holgura. Si aún falta → E030.
// BYPASS temporal: holgura no se exige entre piezas.
static bool prefeederRequireHolguraAtHome()
{
  return true;
}

static bool peerBothCanTrigger()
{
  for (uint8_t s = 0; s < PEER_COUNT; s++)
  {
    const PrefeederMirror& m = peers[s].mirror;
    if (!m.peerOk || m.error || m.idleMode || !m.autoEnabled) return false;
  }
  return true;
}

// Espera PF listo para trigger (Production+Iniciar, sin falla). Si hay falla: esperar Reset.
static bool waitPrefeederCanTrigger(uint32_t timeoutMs, const char* tag)
{
  if (peerBothCanTrigger()) return true;
  Serial.printf("PREFEEDER: esperando PF OK (%s)\n", tag ? tag : "?");
  pushLog(String("PREFEEDER: esperando OK (") + (tag ? tag : "trigger") + ")");

  const uint32_t t0 = millis();
  uint32_t lastCanMs = millis();
  uint32_t lastWebMs = millis();
  while ((uint32_t)(millis() - t0) < timeoutMs)
  {
    peerService();
    serviceExternalStopInput();
    serviceBackgroundTick(lastCanMs, lastWebMs);
    if (peerBothCanTrigger()) return true;
    if (prefeederIdleModeActive()) return false;
    if (settleShouldYieldToStart()) return false;
    delay(10);
  }
  Serial.printf("PREFEEDER: timeout esperando PF OK (%s)\n", tag ? tag : "?");
  return false;
}

// Stop / fin: esperar buffer+holgura OK (sin disparar feeder — evita acumular material).
// false = no quedó quieto (log only; no E014 — ese código es solo “no inicializado”).
static bool prefeederWaitReadyNoTrigger(const char* tag)
{
  if (peerBothReadyQuiet())
    return true;
  if (settleShouldYieldToStart())
    return true;

  if (!waitPrefeederCanTrigger(PREFEEDER_STOP_SETTLE_TIMEOUT_MS, tag)
      && !peerBothAutoArmedForRefill())
  {
    if (settleShouldYieldToStart()) return true;
    Serial.printf("PREFEEDER settle(%s): PF no listo para relleno\n", tag ? tag : "?");
    pushLog(String("PREFEEDER: no listo (") + (tag ? tag : "?") + ")");
    return false;
  }

  Serial.printf("PREFEEDER settle(%s): wait buffer/holgura (sin trigger feeder)\n",
                tag ? tag : "?");

  if (peerBothReadyQuiet())
    return true;
  if (settleShouldYieldToStart())
    return true;

  if (!waitPrefeederCondition(PREFEEDER_STOP_SETTLE_TIMEOUT_MS, true, false, false, tag))
  {
    if (settleShouldYieldToStart()) return true;
    Serial.printf("PREFEEDER settle(%s): buffer/holgura no OK tras timeout (sin E014)\n",
                  tag ? tag : "?");
    pushLog(String("PREFEEDER: buffer no OK (") + (tag ? tag : "?") + ")");
    return false;
  }
  return true;
}

// Fin normal de ciclo/lote: rellenar buffer+holgura y desarmar (diferido al loop).
// Stop/fault/Reset → prefeederHardStopDisarm.
static void prefeederSettleThenDisarm(const char* tag)
{
  if (tag && tag[0])
  {
    strncpy(pfSettleTag, tag, sizeof(pfSettleTag) - 1);
    pfSettleTag[sizeof(pfSettleTag) - 1] = '\0';
  }
  else
  {
    strcpy(pfSettleTag, "settle");
  }
  pfSettlePending = true;
  peerSettleHoldInProcess = true;
  peerSyncInProcessFlag();
}

// Parada inmediata PreFeeder L/R: corta In process + fill/servo/DeReeler.
static void prefeederHardStopDisarm(const char* tag)
{
  pfSettlePending = false;
  peerSettleHoldInProcess = false;
  Serial.printf("PREFEEDER hard-stop (%s)\n", tag ? tag : "?");
  pushLog(String("PREFEEDER hard-stop (") + (tag ? tag : "?") + ")");

  // Forzar OFF aunque el espejo ya diga 0 (limpia fillUntilReady residual).
  for (uint8_t s = 0; s < PEER_COUNT; s++)
  {
    if (!peers[s].client.connected()) continue;
    (void)peerSendCmdEx(s, "halt", "", 0);
    (void)peerSendCmdEx(s, "setInProcess", "0", 0);
  }
  peerSyncInProcessFlag();
}

static void servicePendingPrefeederSettle()
{
  if (!pfSettlePending) return;
  if (settleShouldYieldToStart())
  {
    abortPendingPrefeederSettle("start");
    return;
  }
  pfSettlePending = false;
  const char* tag = pfSettleTag[0] ? pfSettleTag : "settle";

  peerSettleHoldInProcess = true;
  peerSyncInProcessFlag();
  const bool settleOk = prefeederWaitReadyNoTrigger(tag);
  if (settleShouldYieldToStart())
  {
    abortPendingPrefeederSettle("start");
    return;
  }
  peerSettleHoldInProcess = false;
  peerSyncInProcessFlag();
  // Tras timeout de settle: forzar halt (setInProcess OFF ya no reabre fill).
  if (!settleOk)
  {
    for (uint8_t s = 0; s < PEER_COUNT; s++)
    {
      if (!peers[s].client.connected()) continue;
      (void)peerSendCmdEx(s, "halt", "", 0);
    }
  }
}

// Start/Clean/dispatch: el ciclo toma el relevo. Halt+In process OFF
// cortaba el relleno y la 1ª pieza esperaba de nuevo buffer/holgura.
static bool settleAbortIsStartHandoff(const char* reason)
{
  if (!reason || !reason[0]) return false;
  return strcmp(reason, "start") == 0
      || strcmp(reason, "startCut") == 0
      || strcmp(reason, "clean") == 0
      || strcmp(reason, "dispatch") == 0;
}

static void releasePrefeederSettleHold()
{
  if (!peerSettleHoldInProcess && !pfSettlePending) return;
  pfSettlePending = false;
  peerSettleHoldInProcess = false;
  peerSyncInProcessFlag();
}

// Start nuevo: no quedar atrapado detrás del settle post-lote (hasta 60–120 s).
static void abortPendingPrefeederSettle(const char* reason)
{
  if (!pfSettlePending && !peerSettleHoldInProcess) return;
  pfSettlePending = false;
  const bool keepArmed = settleAbortIsStartHandoff(reason);
  Serial.printf("PREFEEDER settle abortado (%s)%s\n",
                reason ? reason : "?",
                keepArmed ? " — In process se mantiene" : "");
  pushLog(String("PREFEEDER settle abortado (") + (reason ? reason : "?") + ")");
  if (keepArmed)
  {
    peerSettleHoldInProcess = true;
    peerSyncInProcessFlag();
    return;
  }
  peerSettleHoldInProcess = false;
  peerSyncInProcessFlag();
  for (uint8_t s = 0; s < PEER_COUNT; s++)
  {
    if (!peers[s].client.connected()) continue;
    (void)peerSendCmdEx(s, "halt", "", 0);
    (void)peerSendCmdEx(s, "setInProcess", "0", 0);
  }
}

static bool settleShouldYieldToStart()
{
  return cutRequested || cycleActive;
}

static String peerWebJsonSide(uint8_t side, bool ok)
{
  if (side >= PEER_COUNT) side = PEER_L;
  const PeerSlot& p = peers[side];
  const PrefeederMirror& m = p.mirror;
  String j = "{\"ok\":";
  j += ok ? "true" : "false";
  j += ",\"side\":\"";
  j += PEER_TAGS[side];
  j += "\",\"peerOk\":";
  j += m.peerOk ? "true" : "false";
  j += ",\"rxMs\":";
  j += p.lastRxMs ? (millis() - p.lastRxMs) : 9999;
  j += ",\"lastMessageDirection\":\"";
  j += p.lastMessageDirection;
  j += "\",\"lastMessageAgeMs\":";
  j += p.lastMessageMs ? (millis() - p.lastMessageMs) : 0;
  j += ",\"lastMessage\":\"";
  j += peerEsc(p.lastMessage);
  j += "\"";
  j += ",\"home\":";
  j += m.home ? "true" : "false";
  j += ",\"endstop\":";
  j += m.endstop ? "true" : "false";
  j += ",\"tension\":";
  j += m.tension ? "true" : "false";
  j += ",\"cylinderOpen\":";
  j += m.cylinderOpen ? "true" : "false";
  j += ",\"hoseAbsent\":";
  j += m.hoseAbsent ? "true" : "false";
  j += ",\"error\":";
  j += m.error ? "true" : "false";
  j += ",\"errorCode\":";
  j += m.errorCode;
  j += ",\"errorReason\":\"";
  j += peerEsc(m.errorReason);
  j += "\",\"autoEnabled\":";
  j += m.autoEnabled ? "true" : "false";
  j += ",\"autoState\":\"";
  j += peerEsc(m.autoState);
  j += "\",\"holgura\":";
  j += m.holgura ? "true" : "false";
  j += ",\"triggerActive\":";
  j += m.triggerActive ? "true" : "false";
  j += ",\"triggerPhase\":\"";
  j += peerEsc(m.triggerPhase);
  j += "\",\"triggerMsLeft\":";
  j += m.triggerMsLeft;
  j += ",\"holguraExtraFeedS\":";
  j += String(m.holguraExtraFeedS, 1);
  j += ",\"triggerFeedS\":";
  j += String(m.triggerFeedS, 1);
  j += ",\"autoRpm\":";
  j += String(m.autoRpm, 1);
  j += ",\"autoReverseS\":";
  j += String(m.autoReverseS, 1);
  j += ",\"motor2Rpm\":";
  j += String(m.motor2Rpm, 1);
  j += ",\"tensionCooldownS\":";
  j += String(m.tensionCooldownS, 1);
  j += ",\"tensionFaultS\":";
  j += String(m.tensionFaultS, 1);
  j += ",\"bufferRefillFaultS\":";
  j += String(m.bufferRefillFaultS, 1);
  j += ",\"servoPwmUs\":";
  j += m.servoPwmUs;
  j += ",\"dereelerLeadMs\":";
  j += m.dereelerLeadMs;
  j += ",\"refillMaterial\":";
  j += m.refillMaterial ? "true" : "false";
  j += ",\"refillDereeler\":";
  j += m.refillDereeler ? "true" : "false";
  j += ",\"refillServo\":";
  j += m.refillServo ? "true" : "false";
  j += ",\"refillFeeder\":";
  j += m.refillFeeder ? "true" : "false";
  j += ",\"idleMode\":";
  j += m.idleMode ? "true" : "false";
  j += ",\"buzzerMuted\":";
  j += m.buzzerMuted ? "true" : "false";
  j += ",\"refillPulseS\":";
  j += String(m.refillPulseS, 1);
  j += ",\"inProcess\":";
  j += m.inProcess ? "true" : "false";
  j += ",\"sensorsArmed\":";
  j += (!m.idleMode && m.inProcess) ? "true" : "false";
  j += "}";
  return j;
}

static bool peerBothOk()
{
  for (uint8_t s = 0; s < PEER_COUNT; s++)
  {
    // WiFiClient::connected() no es const en el core ESP32.
    PeerSlot& p = peers[s];
    if (!p.mirror.peerOk || !p.client.connected()) return false;
  }
  return true;
}

static String peerWebJson(bool ok)
{
  String j = "{\"ok\":";
  j += ok ? "true" : "false";
  j += ",\"everyN\":";
  j += prefeederTriggerEveryN;
  j += ",\"settleHold\":";
  j += peerSettleHoldInProcess ? "true" : "false";
  j += ",\"peerOk\":";
  j += peerBothOk() ? "true" : "false";
  j += ",\"L\":";
  j += peerWebJsonSide(PEER_L, peers[PEER_L].mirror.peerOk);
  j += ",\"R\":";
  j += peerWebJsonSide(PEER_R, peers[PEER_R].mirror.peerOk);
  j += "}";
  return j;
}

// Prefeeder: solo TCP (sin GPIO de respaldo). Disparo fire-and-forget a L+R.
// No espera ACK: evita pausas del ciclo por timeout/reintento TCP.
// El PreFeeder deduplica por id y rechaza trigger inválido internamente.
// secs <= 0 → duración default del PreFeeder (triggerFeedS).
static bool prefeederTriggerEnsureSides(const String& val, bool& okL, bool& okR)
{
  okL = peerSendCmdEx(PEER_L, "trigger", val, 0);
  okR = peerSendCmdEx(PEER_R, "trigger", val, 0);
  return okL && okR;
}

static bool prefeederTriggerPulseSecs(float secs)
{
  String val;
  if (secs > 0.05f)
    val = String(secs, 2);

  bool okL = false;
  bool okR = false;
  if (prefeederTriggerEnsureSides(val, okL, okR))
  {
    if (val.length())
      DBG_PRINTF("PREFEEDER: disparo TCP trigger L+R (%.2fs)\n", secs);
    else
      DBG_PRINTLN("PREFEEDER: disparo TCP trigger L+R");
    return true;
  }
  DBG_PRINTF("PREFEEDER: disparo TCP parcial/falló (L=%d R=%d)\n", (int)okL, (int)okR);
  return false;
}

static bool prefeederTriggerPulse()
{
  return prefeederTriggerPulseSecs(0.0f);
}

static bool peerSideHealthy(uint8_t side)
{
  if (side >= PEER_COUNT) return false;
  PeerSlot& p = peers[side];
  if (!p.client.connected()) return false;
  if (!p.fullStatusReceived || !p.lastRxMs) return false;
  if (millis() - p.lastRxMs > PEER_STALE_MS) return false;
  if (p.mirror.error && !p.mirror.idleMode) return false;
  return true;
}

static bool peerBothHealthy()
{
  return peerSideHealthy(PEER_L) && peerSideHealthy(PEER_R);
}

// Enlace OK = TCP + status fresco + sin falla PF (sin ACK/ping).
static bool peerVerifyBothWays()
{
  peerRxDrainAll();
  return peerBothHealthy();
}

// Happy path: status TCP fresco = OK. Si sospechoso, revalida health (sin ACK).
static bool peerVerifyLinkFastOrPing()
{
  peerRxDrainAll();
  if (!peerCommSuspect && peerBothHealthy())
    return true;
  return peerVerifyBothWays();
}

// Tras HOME: solo status TCP (conectado + RX fresco + sin falla PF).
// Sin ping/ACK: igual que sensores — si el espejo ya dice OK, no preguntar.
static void peerCheckAfterPieceOrPause()
{
  peerRxDrainAll();
  const bool ok = peerBothHealthy();
  peerCommSuspect = false;
  if (ok) return;

  safetyErrorCode = SAFETY_ERROR_EXTERNAL;
  enterCyclePaused("PEER post-pieza");
  Serial.println("PEER: comunicación no OK tras pieza — ciclo pausado (reanudar cuando haya TCP L+R)");
  pushLog("PEER: sin comunicación tras pieza — ciclo pausado");

  uint32_t lastCanMs = millis();
  uint32_t lastWebMs = millis();
  while (cyclePaused && cycleActive)
  {
    peerService();
    peerTryReconnect();
    serviceExternalStopInput();
    serviceBackgroundTick(lastCanMs, lastWebMs);
    if (cycleRunning) serviceCycle();
    if (feedStep2NeedsWait() || !linearIsMoving())
      serviceServoFeed();
    delay(10);
  }
}

// Tras terminar el paso en curso: si hay falla enclavada / pending → detiene el lote (reset).
static bool safetyHonorStopAfterStep(const char* stepName)
{
  serviceSafetyDebounce(false);
  if (!safetyStopPending && !sensorStaleLatched && sensorLatchedBitmask == 0)
    return false;

  Serial.printf("SAFETY STOP tras paso %s — lote detenido, pulse Reset (PLC)\n",
                stepName ? stepName : "?");
  pushLog(String("SAFETY STOP tras paso ") + (stepName ? stepName : "?") + " — Reset PLC requerido");
  {
    uint16_t ec = E020;  // Safety stop genérico tras un paso del ciclo
    if (sensorStaleLatched)
      ec = E027;  // Sensores CAN offline / sin comunicación
    else if (externalStopInputActive())
      ec = E026;  // Parada externa PreFeeder (TCP / falla PF)
    else
    {
      switch (sensorCodeFromBitmask(sensorLatchedBitmask))
      {
        case 1: ec = E021; break;  // pinzas
        case 2: ec = E022; break;  // sujetador
        case 3: ec = E023; break;  // cortador
        case 4: ec = E024; break;  // manguera
        case 5: ec = E025; break;  // bandeja
        default: break;
      }
    }
    setCycleError(ec, stepName && stepName[0] ? stepName : "safety_stop");
  }
  lastCycleSuccess = false;
  lastCycleTotalMs = 0;
  uiOfferLinearRecovery = true;
  safetyStopPending = false;
  cyclePausePending = false;
  leaveCyclePaused();
  cycleAborted = true;
  cycleActive = false;
  cyclePaused = false;
  cutRequested = false;
  prefeederHardStopDisarm("safety-stop");
  refreshSafetyErrorCode();
  uiNotify();
  return true;
}

static void setCyclePauseReason(const char* reason)
{
  if (!reason) reason = "";
  strncpy(cyclePauseReason, reason, CYCLE_PAUSE_REASON_MAX - 1);
  cyclePauseReason[CYCLE_PAUSE_REASON_MAX - 1] = '\0';
}

static void setCycleFaultReason(const char* reason)
{
  if (!reason) reason = "";
  strncpy(cycleFaultReason, reason, CYCLE_PAUSE_REASON_MAX - 1);
  cycleFaultReason[CYCLE_PAUSE_REASON_MAX - 1] = '\0';
}

static void enterCyclePaused(const char* reason)
{
  const bool reasonChanged = reason && reason[0] && strcmp(cyclePauseReason, reason) != 0;
  if (reason && reason[0])
    setCyclePauseReason(reason);

  if (!cyclePaused)
  {
    cyclePaused = true;
    cyclePauseBeganMs = millis();
    Serial.printf("CYCLE PAUSADO%s%s\n",
                  reason && reason[0] ? " — " : "",
                  reason && reason[0] ? reason : "");
    pushLog(String("CYCLE PAUSADO") + (reason && reason[0] ? String(" — ") + reason : ""));
    uiNotify();
  }
  else
  {
    if (!cyclePauseBeganMs)
      cyclePauseBeganMs = millis();
    if (reasonChanged)
      uiNotify();
  }
}

static void leaveCyclePaused()
{
  if (cyclePauseBeganMs)
  {
    uint32_t dt = millis() - cyclePauseBeganMs;
    cyclePausedAccumMs += dt;
    repStartTime += dt;  // el tiempo de la pieza actual no avanza en pausa
    cyclePauseBeganMs = 0;
  }
  if (cyclePaused)
  {
    cyclePaused = false;
    setCyclePauseReason("");
    Serial.println("CYCLE REANUDADO");
    pushLog("CYCLE REANUDADO");
    // Tras falla PF + Resume: forzar reenvío torre (quitar rojo/buzzer).
    towerForceResync = true;
    uiNotify();
  }
}

// Abort por falla de rutina (timeout lineal/feed): conserva progreso UI + recovery.
static void faultStopCycle(const char* reason)
{
  const char* r = (reason && reason[0]) ? reason : "fault";
  uint16_t ec = E099;  // Falla de rutina no clasificada
  if (!strcmp(r, "lineal_timeout"))
    ec = E010;  // Timeout lineal
  else if (!strcmp(r, "feed_timeout"))
    ec = E011;  // Timeout alimentación
  setCycleError(ec, r);
  Serial.printf("CYCLE FAULT %s: %s (%s)\n",
                cycleErrorCodeString(ec).c_str(), r, cycleErrorCodeLabel(ec));
  pushLog(String("CYCLE FAULT ") + cycleErrorCodeString(ec) + ": " + r);
  lastCycleSuccess = false;
  lastCycleTotalMs = cycleElapsedMs();
  uiOfferLinearRecovery = true;
  cyclePausePending = false;
  leaveCyclePaused();
  setCyclePauseReason("");
  cycleAborted = true;
  cycleActive = false;
  cyclePaused = false;
  cyclePauseBeganMs = 0;
  cutRequested = false;
  immediatePhysicalStop();
  resetFeedAbortState();
  prefeederHardStopDisarm("fault-stop");
  uiNotify();
}

static uint32_t cycleElapsedMs()
{
  if (!cycleActive && lastCycleSuccess && lastCycleTotalMs > 0)
    return lastCycleTotalMs;
  if (!cycleStartTime) return 0;
  uint32_t raw = millis() - cycleStartTime;
  uint32_t paused = cyclePausedAccumMs;
  if (cyclePaused && cyclePauseBeganMs)
    paused += millis() - cyclePauseBeganMs;
  if (raw <= paused) return 0;
  return raw - paused;
}

// Paso a paso: no detener esperas/checks del PreFeeder ni transiciones internas.
static bool stepByStepShouldPause(const char* stepName)
{
  if (!stepByStepMode) return false;
  if (!stepName || !stepName[0]) return false;
  if (!strcmp(stepName, "rep-start")) return false;
  if (!strcmp(stepName, "post-pieza")) return false;
  if (!strcmp(stepName, "post-pieza-peer")) return false;
  return true;
}

static const char* stepByStepPauseLabel(const char* stepName)
{
  if (!stepName) return "paso";
  if (!strcmp(stepName, "listo")) return "listo para iniciar";
  if (!strcmp(stepName, "holder-on")) return "Holder ON";
  if (!strcmp(stepName, "feed")) return "Alimentacion";
  if (!strcmp(stepName, "feed-prefetch")) return "Alimentacion (prefetch)";
  if (!strcmp(stepName, "feed-consume")) return "Alimentacion lista";
  if (!strcmp(stepName, "offset")) return "Offset";
  if (!strcmp(stepName, "grippers-on")) return "Pinzas ON";
  if (!strcmp(stepName, "holder-off")) return "Holder OFF";
  if (!strcmp(stepName, "lineal-fwd")) return "Lineal avance";
  if (!strcmp(stepName, "lineal-dwell")) return "Lineal dwell";
  if (!strcmp(stepName, "holder-precut")) return "Holder pre-corte";
  if (!strcmp(stepName, "cutter-on")) return "Cortador ON";
  if (!strcmp(stepName, "cutter-off")) return "Cortador OFF";
  if (!strcmp(stepName, "deposit")) return "Deposito";
  if (!strcmp(stepName, "grippers-off")) return "Pinzas OFF";
  if (!strcmp(stepName, "home")) return "HOME";
  if (!strcmp(stepName, "asentar")) return "Asentar";
  return stepName;
}

// Pause cooperativo: termina el paso en curso y espera Resume (sin parada física inmediata).
static void serviceCyclePauseGate(const char* stepName)
{
  if (cyclePausePending && cycleActive)
  {
    cyclePausePending = false;
    if (stepByStepMode && stepByStepShouldPause(stepName))
    {
      char buf[CYCLE_PAUSE_REASON_MAX];
      snprintf(buf, sizeof(buf), "Paso a paso — %s", stepByStepPauseLabel(stepName));
      enterCyclePaused(buf);
    }
    else
      enterCyclePaused(stepName ? stepName : "fin de paso");
  }
  if (!cyclePaused || !cycleActive) return;

  uint32_t lastCanMs = millis();
  uint32_t lastWebMs = millis();
  while (cyclePaused && cycleActive)
  {
    peerService();
    peerTryReconnect();
    serviceExternalStopInput();
    serviceSafetyDebounce(false);
    if (safetyHonorStopAfterStep(stepName ? stepName : "pause"))
      return;
    serviceBackgroundTick(lastCanMs, lastWebMs);
    if (cycleRunning) serviceCycle();
    if (feedStep2NeedsWait() || !linearIsMoving())
      serviceServoFeed();
    delay(10);
  }
}

// En delays / esperas sin movimiento lineal: aplicar Pause pendiente ya.
static void honorPausePendingIfIdle(const char* why)
{
  if (!cycleActive || cyclePaused || !cyclePausePending) return;
  if (cycleRunning || linearIsMoving()) return;  // dejar terminar el lineal
  cyclePausePending = false;
  enterCyclePaused(why ? why : "Pause");
  serviceCyclePauseGate(why ? why : "Pause");
}

// true = salir del ciclo (abort/safety/stop).
static bool cycleGateAfterStep(const char* stepName)
{
  if (!cycleActive || cycleAborted) return true;
  if (safetyHonorStopAfterStep(stepName)) return true;
  if (stepByStepShouldPause(stepName) && !cyclePaused && !cyclePausePending)
    cyclePausePending = true;
  serviceCyclePauseGate(stepName);
  return !cycleActive || cycleAborted;
}

// Fin de pieza: sin poll bloqueante. Alertas van event-driven (0xC0); aquí solo
// drenar RX, aplicar debounce pendiente y parar si ya hay falla enclavada.
static void safetyPollAfterPiece()
{
  serviceCANRx();
  serviceSafetyDebounce(true);
  refreshSafetyErrorCode();
  safetyHonorStopAfterStep("post-pieza");
}

// Validación en Start: poll + debounce. Si no responde → enclava offline.
static bool safetyValidateAtStart()
{
  serviceSafetyDebounce(true);
  const bool recent = sensorHasRecentRx();
  const uint8_t retries = recent ? (uint8_t)SENSOR_POLL_START_RETRIES : SENSOR_POLL_RETRIES;
  const uint32_t toMs = recent ? (uint32_t)SENSOR_POLL_START_TIMEOUT_MS
                               : (uint32_t)SENSOR_POLL_TIMEOUT_MS;
  if (!pollSensorModuleEx(retries, toMs))
  {
    noteSensorPollFailure("SENSORS CAN: sin respuesta poll en Start — offline enclavado");
    if (sensorStaleLatched)
      return false;
    // Poll fallido con RX reciente → no bloquear arranque.
    if (!sensorHasRecentRx())
      return false;
  }
  serviceSafetyDebounce(true);
  refreshSafetyErrorCode();
  return safetyAllowsRun();
}

// --- 06.0 Clamps por parámetro (clampVal está en config.h) ---
static uint16_t clampPrefeederTriggerEveryN(uint32_t v)
{
  (void)v;
  return 1;  // fijo: disparo cada 1 pieza (ya no ajustable)
}

// Disparo PreFeeder (Tfeed). Fire-and-forget: no espera ACK ni fin de alimentación.
static void prefeederTriggerAsync(const char* tag)
{
  Serial.printf("PREFEEDER: trigger (%s)\n", tag ? tag : "?");

  bool okL = false;
  bool okR = false;
  const String val;  // default triggerFeedS en cada PF
  if (prefeederTriggerEnsureSides(val, okL, okR))
    return;

  Serial.printf("PREFEEDER: trigger TCP no enviado (%s) L=%d R=%d\n",
                tag ? tag : "?", (int)okL, (int)okR);
  pushLog(String("PREFEEDER: trigger sin TCP (") + (tag ? tag : "?") + ")");
}

// All OK L+R (TCP + sin falla + Production + Auto) para prefetch.
static bool peerBothAllOkForPrefetch()
{
  if (!peerBothOk()) return false;
  for (uint8_t s = 0; s < PEER_COUNT; s++)
  {
    const PrefeederMirror& m = peers[s].mirror;
    if (!m.peerOk || m.error || m.idleMode || !m.autoEnabled) return false;
  }
  return true;
}

static float clampLinearStepsPerMm(float v)
{
  return clampVal(v, LINEAR_STEPS_PER_MM_MIN, LINEAR_STEPS_PER_MM_MAX);
}

static float clampLinearOffsetSteps(float v)
{
  return clampVal(v, LINEAR_OFFSET_STEPS_MIN, LINEAR_OFFSET_STEPS_MAX);
}

static float clampCutOffsetMm(float v)
{
  return clampVal(v, CUT_OFFSET_MM_MIN, CUT_OFFSET_MM_MAX);
}

// Modelo afin para un movimiento ABSOLUTO desde HOME: pasos = offset + mm * pasos/mm.
// El offset (backlash de arranque) se aplica una sola vez por movimiento.
static inline uint32_t mmToLinearStepsAbs(float mm)
{
  if (mm <= 0.0f) return 0;
  float s = linearOffsetSteps + mm * linearStepsPerMm;
  if (s <= 0.0f) return 0;
  return (uint32_t)(s + 0.5f);
}

// Conversion con la referencia FIJA de fabrica (offset 0, pasos/mm de fabrica).
// Se usa en el movimiento de prueba de calibracion para que NO dependa de la
// calibracion guardada (que puede estar corrupta) y sea siempre predecible.
static inline uint32_t mmToLinearStepsFactory(float mm)
{
  if (mm <= 0.0f) return 0;
  float s = mm * LINEAR_STEPS_PER_MM;
  if (s <= 0.0f) return 0;
  return (uint32_t)(s + 0.5f);
}

inline uint32_t lengthToSteps(int lengthMm)
{
  if (lengthMm <= 0) return 0;
  float linearActuatorMm = (float)lengthMm - linearGripperAreaMm;  // recorrido linearActuator; pieza = linearActuatorMm + G
  if (linearActuatorMm <= 0.0f) return 0;
  return mmToLinearStepsAbs(linearActuatorMm);
}

// Longitud nominal a pasos (precision sub-milimetrica via float intermedio).
// Efectiva = nominal + cutOffsetMm (+ alarga / − acorta).
static uint32_t cutLengthToSteps(int nominalMm, float* effMmOut)
{
  float effMm = (float)nominalMm + (cutUseLengthOffset ? cutOffsetMm : 0.0f);
  if (effMmOut) *effMmOut = effMm;
  if (effMm <= 0.0f) return 0;
  float linearActuatorMm = effMm - linearGripperAreaMm;
  if (linearActuatorMm <= 0.0f) return 0;
  return mmToLinearStepsAbs(linearActuatorMm);
}

static inline bool omAbsErrWithinTol(float actual, float nominal, float tolMm)
{
  return fabsf(actual - nominal) <= tolMm + 1e-4f;
}

static float omNominalPieceLengthMm()
{
  float L = (float)cutLongitud;
  if (cutUseLengthOffset) L += cutOffsetMm;
  return L;
}

static float omNominalPhase2Mm()
{
  const float linearActuator = omNominalPieceLengthMm() - linearGripperAreaMm;
  return (linearActuator > 0.0f) ? linearActuator : 0.0f;
}

static float feedStepsToMm(int32_t steps, bool sideR);

static void omClearPieceValidation()
{
  omPhase1Mm = 0.0f;
  omPhase2Mm = 0.0f;
  omErrPhase1Mm = 0.0f;
  omErrPhase2Mm = 0.0f;
  omTotalMm = 0.0f;
  omErrTotalMm = 0.0f;
  omPhase1Ok = false;
  omPhase2Ok = false;
  omTotalInternalOk = false;
  omTotalProdOk = false;
}

static void omCapturePhase1(float mm)
{
  const float nom = feedOmNominalTargetMm();
  omPhase1Mm = mm;
  omErrPhase1Mm = mm - nom;
  omPhase1Ok = omAbsErrWithinTol(mm, nom, INTERNAL_LENGTH_TOL_MM);
}

static float feedOmNominalTargetMm()
{
  if (feedStepsTargetThisFeed > 0)
    return feedStepsToMm(feedStepsTargetThisFeed, false);
  if (feedStepsTargetB > 0)
    return feedStepsToMm(feedStepsTargetB, true);
  if (feedTestSolidSteps > 0)
    return feedStepsToMm(feedTestSolidSteps, false);
  return FEED_NOMINAL_MM;
}

static bool omValidatePhase1AfterFeed()
{
  float mm = omPhase1Mm;
  if (mm <= 0.0f && feedOmLastMmAbs >= 0.0f)
    mm = feedOmLastMmAbs;
  if (mm <= 0.0f && !asdaOmReadMm(&mm))
  {
    DBG_PRINTLN("OM: sin lectura Fase 1");
    return false;
  }
  omCapturePhase1(mm);
  DBG_PRINTF("OM Fase1: %.2f mm err=%.2f (nom %.1f ±%.1f) %s\n",
             omPhase1Mm, omErrPhase1Mm, feedOmNominalTargetMm(), INTERNAL_LENGTH_TOL_MM,
             omPhase1Ok ? "OK" : "NG");
  return omPhase1Ok;
}

static bool omValidatePhase2AndTotal()
{
  float mm = 0.0f;
  if (!asdaOmReadMm(&mm))
  {
    DBG_PRINTLN("OM: sin lectura Fase 2");
    return false;
  }
  omPhase2Mm = mm;
  const float nom2 = omNominalPhase2Mm();
  omErrPhase2Mm = mm - nom2;
  omPhase2Ok = omAbsErrWithinTol(mm, nom2, INTERNAL_LENGTH_TOL_MM);

  omTotalMm = omPhase1Mm + omPhase2Mm;
  const float nomTotal = omNominalPieceLengthMm();
  omErrTotalMm = omTotalMm - nomTotal;
  omTotalInternalOk = omAbsErrWithinTol(omTotalMm, nomTotal, INTERNAL_LENGTH_TOL_MM);
  omTotalProdOk = omAbsErrWithinTol(omTotalMm, nomTotal, PRODUCTION_LENGTH_TOL_MM);

  DBG_PRINTF("OM Fase2: %.2f mm err=%.2f (nom %.1f ±%.1f) %s\n",
             omPhase2Mm, omErrPhase2Mm, nom2, INTERNAL_LENGTH_TOL_MM,
             omPhase2Ok ? "OK" : "NG");
  DBG_PRINTF("OM Total: %.2f mm err=%.2f (nom %.1f int±%.1f prod±%.1f) %s / prod %s\n",
             omTotalMm, omErrTotalMm, nomTotal,
             INTERNAL_LENGTH_TOL_MM, PRODUCTION_LENGTH_TOL_MM,
             omTotalInternalOk ? "OK" : "NG",
             omTotalProdOk ? "OK" : "NG");
  return omPhase1Ok && omPhase2Ok && omTotalInternalOk;
}

static bool omCommitPhase1OrFault()
{
  if (omValidatePhase1AfterFeed())
    return true;
  if (cycleErrorCode == E000)
    setCycleError(E012, "om_phase1");
  return false;
}

static bool omPieceLengthOkForCut()
{
  return omPhase1Ok && omPhase2Ok && omTotalInternalOk;
}

// Conversion PROPORCIONAL pura (sin offset): para deltas/incrementos (deposito), no posicion absoluta.
static inline uint32_t mmToLinearSteps(float mm)
{
  if (mm <= 0.0f) return 0;
  return (uint32_t)(mm * linearStepsPerMm + 0.5f);
}

static uint16_t clampDepositBatchSize(uint32_t v)
{
  return (uint16_t)clampVal(v, (uint32_t)DEPOSIT_BATCH_SIZE_MIN, (uint32_t)DEPOSIT_BATCH_SIZE_MAX);
}

static float clampDepositExtraMm(float mm)
{
  (void)mm;
  return DEPOSIT_EXTRA_MM_DEFAULT;
}

static float clampDepositInterlaceMm(float mm)
{
  (void)mm;
  return 0.0f;
}

static float clampDepositBetweenBatchMm(float mm)
{
  (void)mm;
  return DEPOSIT_BETWEEN_GAP_MM;
}

static float depositPartLengthMm()
{
  float L = (float)cutLongitud;
  if (cutUseLengthOffset) L += cutOffsetMm;
  if (L < 0.0f) L = 0.0f;
  return L;
}

static float depositLinearActuatorMm()
{
  float linearActuator = depositPartLengthMm() - linearGripperAreaMm;
  return (linearActuator > 0.0f) ? linearActuator : 0.0f;
}

static float depositBaseMm()
{
  const float G = linearGripperAreaMm;
  const float linearActuator = depositLinearActuatorMm();
  const float Extra = DEPOSIT_EXTRA_MM_DEFAULT;
  return G + linearActuator + Extra;
}

static float depositBetweenBatchAutoMm()
{
  const float G = linearGripperAreaMm;
  const float linearActuator = depositLinearActuatorMm();
  const float Extra = DEPOSIT_EXTRA_MM_DEFAULT;
  const float Gap = DEPOSIT_BETWEEN_GAP_MM;
  return G + linearActuator + Extra + Gap;
}

static float depositClearanceMm()
{
  const float G = linearGripperAreaMm;
  const float linearActuator = depositLinearActuatorMm();
  return G + linearActuator;
}

static uint16_t depositMaxBatchIndex()
{
  float zoneSpan = depositBetweenBatchAutoMm();
  if (zoneSpan < 1.0f) zoneSpan = 1.0f;
  float peak = depositBaseMm();
  float available = TRAY_LENGTH_MM - peak;
  if (available < 0.0f) available = 0.0f;
  uint16_t n = (uint16_t)(available / zoneSpan) + 1;
  if (n < 1) n = 1;
  return n;
}

static float depositExtraDeltaMm(uint16_t batchIndex, uint16_t /*pieceInBatch*/)
{
  if (batchIndex < 1) batchIndex = 1;
  const float Pos_base = depositBaseMm();
  const float Pos_entreBatch = depositBetweenBatchAutoMm();
  return Pos_base + (float)(batchIndex - 1) * Pos_entreBatch;
}

static int32_t mmToLinearStepsSigned(float mm)
{
  if (mm > 0.0f) return (int32_t)(mm * linearStepsPerMm + 0.5f);
  if (mm < 0.0f) return -(int32_t)((-mm) * linearStepsPerMm + 0.5f);
  return 0;
}

static int32_t depositExtraSignedSteps(uint16_t batchIndex, uint16_t pieceInBatch)
{
  return mmToLinearStepsSigned(depositExtraDeltaMm(batchIndex, pieceInBatch));
}

// ============================================================
// SECCION 06 — NVS y configuración (clamps, math lineal, persistencia)
// ============================================================

static const uint16_t PART_CATALOG_JSON_VERSION = 2;
static const char* PART_CATALOG_DIR = "/recipes";
static const char* PART_CATALOG_ACTIVE_FILE = "/recipes/_active.json";
static bool partCatalogFsReady = false;

static inline bool isProtectedPart(const String& pn)
{
  return pn.equalsIgnoreCase("Prueba")
      || pn.equalsIgnoreCase("Dev.Receipt")
      || pn.equalsIgnoreCase("Dev. Receipt");
}

static void partModelSetIdentity(PartModel& r, const char* partNumber, uint16_t lengthMm)
{
  memset(&r, 0, sizeof(r));
  if (partNumber && partNumber[0])
    strlcpy(r.partNumber, partNumber, sizeof(r.partNumber));
  r.lengthMm = lengthMm;
}

static bool partCatalogValidPartNumber(const char* partNumber)
{
  if (!partNumber || !partNumber[0]) return false;
  if (partNumber[0] == '_') return false;
  size_t n = strlen(partNumber);
  if (n > PART_NUMBER_MAX_LEN) return false;
  for (size_t i = 0; i < n; i++)
  {
    char c = partNumber[i];
    if (!isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.')
      return false;
  }
  return true;
}

static String partCatalogPathForPart(const char* partNumber)
{
  String path = PART_CATALOG_DIR;
  path += "/";
  path += partNumber;
  path += ".json";
  return path;
}

static bool partCatalogEnsureDir()
{
  if (LittleFS.exists(PART_CATALOG_DIR))
  {
    File d = LittleFS.open(PART_CATALOG_DIR);
    if (d && d.isDirectory())
    {
      d.close();
      return true;
    }
    if (d) d.close();
  }
  return LittleFS.mkdir(PART_CATALOG_DIR);
}

static String partCatalogJsonEscape(const char* s)
{
  String o;
  if (!s) return o;
  for (; *s; ++s)
  {
    if (*s == '"' || *s == '\\') { o += '\\'; o += *s; }
    else o += *s;
  }
  return o;
}

static bool partCatalogJsonFindString(const String& doc, const char* key, String& out)
{
  String pat = String("\"") + key + "\"";
  int k = doc.indexOf(pat);
  if (k < 0) return false;
  int c = doc.indexOf(':', k + pat.length());
  if (c < 0) return false;
  int q1 = doc.indexOf('"', c + 1);
  if (q1 < 0) return false;
  int q2 = q1 + 1;
  while (q2 < (int)doc.length())
  {
    if (doc[q2] == '"' && doc[q2 - 1] != '\\') break;
    q2++;
  }
  if (q2 >= (int)doc.length()) return false;
  out = doc.substring(q1 + 1, q2);
  return true;
}

static bool partCatalogJsonFindNumber(const String& doc, const char* key, double& out)
{
  String pat = String("\"") + key + "\"";
  int pos = 0;
  while (true)
  {
    int k = doc.indexOf(pat, pos);
    if (k < 0) return false;
    int c = doc.indexOf(':', k + pat.length());
    if (c < 0) return false;
    int i = c + 1;
    while (i < (int)doc.length() && isspace((unsigned char)doc[i])) i++;
    if (i >= (int)doc.length()) return false;
    char* endp = nullptr;
    const char* start = doc.c_str() + i;
    double v = strtod(start, &endp);
    if (endp == start)
    {
      pos = k + 1;
      continue;
    }
    out = v;
    return true;
  }
}

static String partCatalogSerialize(const PartModel& r)
{
  String j;
  j.reserve(128);
  j += "{\n";
  j += "  \"version\":" + String(PART_CATALOG_JSON_VERSION) + ",\n";
  j += "  \"partNumber\":\"" + partCatalogJsonEscape(r.partNumber) + "\",\n";
  j += "  \"lengthMm\":" + String(r.lengthMm) + "\n";
  j += "}\n";
  return j;
}

static bool partCatalogParse(const String& doc, PartModel& out)
{
  memset(&out, 0, sizeof(out));
  String pn;
  if (partCatalogJsonFindString(doc, "partNumber", pn) && pn.length())
    strlcpy(out.partNumber, pn.c_str(), sizeof(out.partNumber));
  double v = 0;
  if (partCatalogJsonFindNumber(doc, "lengthMm", v) && v > 0)
    out.lengthMm = (uint16_t)constrain((int)v, 1, 60000);
  return out.partNumber[0] != '\0' && out.lengthMm > 0;
}

static bool partCatalogWriteFile(const char* path, const String& body)
{
  File f = LittleFS.open(path, "w");
  if (!f) return false;
  size_t n = f.print(body);
  f.close();
  return n == body.length();
}

static bool partCatalogReadFile(const char* path, String& out)
{
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  out = f.readString();
  f.close();
  return out.length() > 0;
}

static bool partCatalogReady()
{
  return partCatalogFsReady;
}

static bool partCatalogSave(const PartModel& r)
{
  if (!partCatalogFsReady || !partCatalogValidPartNumber(r.partNumber) || r.lengthMm == 0) return false;
  String path = partCatalogPathForPart(r.partNumber);
  return partCatalogWriteFile(path.c_str(), partCatalogSerialize(r));
}

static bool partCatalogLoad(const char* partNumber, PartModel& out)
{
  if (!partCatalogFsReady || !partCatalogValidPartNumber(partNumber)) return false;
  String path = partCatalogPathForPart(partNumber);
  String doc;
  if (!partCatalogReadFile(path.c_str(), doc)) return false;
  if (!partCatalogParse(doc, out)) return false;
  strlcpy(out.partNumber, partNumber, sizeof(out.partNumber));
  return true;
}

static bool partCatalogExists(const char* partNumber)
{
  if (!partCatalogFsReady || !partCatalogValidPartNumber(partNumber)) return false;
  return LittleFS.exists(partCatalogPathForPart(partNumber));
}

static bool partCatalogDelete(const char* partNumber)
{
  if (!partCatalogFsReady || !partCatalogValidPartNumber(partNumber)) return false;
  String path = partCatalogPathForPart(partNumber);
  if (!LittleFS.exists(path)) return false;
  return LittleFS.remove(path);
}

static uint8_t partCatalogList(PartCatalogEntry* out, uint8_t maxOut)
{
  if (!partCatalogFsReady || !out || !maxOut) return 0;
  uint8_t n = 0;
  File root = LittleFS.open(PART_CATALOG_DIR);
  if (!root || !root.isDirectory())
  {
    if (root) root.close();
    return 0;
  }
  File f = root.openNextFile();
  while (f && n < maxOut)
  {
    const char* name = f.name();
    String base = name ? String(name) : String();
    int slash = base.lastIndexOf('/');
    if (slash >= 0) base = base.substring(slash + 1);
    if (!f.isDirectory() && base.endsWith(".json") && !base.startsWith("_"))
    {
      String pn = base.substring(0, base.length() - 5);
      if (partCatalogValidPartNumber(pn.c_str()))
      {
        PartModel tmp;
        if (partCatalogLoad(pn.c_str(), tmp))
        {
          strlcpy(out[n].partNumber, tmp.partNumber, sizeof(out[n].partNumber));
          out[n].lengthMm = tmp.lengthMm;
          n++;
        }
      }
    }
    f.close();
    f = root.openNextFile();
  }
  root.close();
  return n;
}

static bool partCatalogSaveActive(const char* partNumber)
{
  if (!partCatalogFsReady) return false;
  String body = "{\n  \"active\":\"";
  body += partNumber ? partCatalogJsonEscape(partNumber) : "";
  body += "\"\n}\n";
  return partCatalogWriteFile(PART_CATALOG_ACTIVE_FILE, body);
}

static bool partCatalogLoadActive(char* outPn, size_t outLen)
{
  if (!partCatalogFsReady || !outPn || outLen == 0) return false;
  String doc;
  if (!partCatalogReadFile(PART_CATALOG_ACTIVE_FILE, doc)) return false;
  String pn;
  if (!partCatalogJsonFindString(doc, "active", pn)) return false;
  if (pn.length() && !partCatalogValidPartNumber(pn.c_str())) return false;
  strlcpy(outPn, pn.c_str(), outLen);
  return outPn[0] != '\0';
}

static bool partCatalogBegin()
{
  partCatalogFsReady = false;
  if (!LittleFS.begin(false))
  {
    Serial.println("modelos: LittleFS mount falló — intentando format...");
    if (!LittleFS.begin(true))
    {
      Serial.println("modelos: ERROR LittleFS no disponible");
      return false;
    }
    Serial.println("modelos: LittleFS formateado");
  }
  if (!partCatalogEnsureDir())
  {
    Serial.println("modelos: ERROR no se pudo crear el directorio");
    return false;
  }
  partCatalogFsReady = true;
  if (!partCatalogExists("Prueba"))
  {
    PartModel pr;
    partModelSetIdentity(pr, "Prueba", 100);
    if (!partCatalogSave(pr))
      Serial.println("modelos: WARN no se pudo asegurar modelo Prueba");
  }
  Serial.println("modelos: OK catálogo");
  return true;
}

static bool feedModeUsesSensor(FeedMode mode)
{
  return mode == FEED_MODE_STEPS_SENSOR;
}

static const char* feedModeLabel(FeedMode mode)
{
  (void)mode;
  return "om-encoder";
}

// Único modo de alimentación (OM decide mm; láser valida presencia).
static FeedMode clampFeedMode(int mode)
{
  (void)mode;
  return FEED_MODE_STEPS_SENSOR;
}

static int32_t clampFeedBypassSteps(int32_t steps)
{
  return clampVal(steps, (int32_t)FEED_REF_STEPS_MIN, (int32_t)FEED_REF_STEPS_MAX);
}

static int32_t clampFeedStepsTolerance(int32_t tol)
{
  return clampVal(tol, (int32_t)FEED_STEPS_TOLERANCE_MIN, (int32_t)FEED_STEPS_TOLERANCE_MAX);
}

static uint8_t clampFeedTolerancePercent(int percent)
{
  return (uint8_t)clampVal(percent, (int)FEED_TOLERANCE_PCT_MIN, (int)FEED_TOLERANCE_PCT_MAX);
}

static uint16_t clampLinearRpm(uint32_t rpm)
{
  return (uint16_t)clampVal(rpm, (uint32_t)LINEAR_RPM_MIN, (uint32_t)LINEAR_RPM_MAX);
}

static uint16_t migrateLinearRpm(uint32_t stored)
{
  if (stored == 0) return LINEAR_RPM_DEFAULT;
  if (stored <= LINEAR_RPM_MAX) return clampLinearRpm(stored);
  return clampLinearRpm(linearHzToRpm(stored));
}

static uint16_t clampDwellAtDestMs(uint32_t ms)
{
  return (uint16_t)clampVal(ms, (uint32_t)0, (uint32_t)DWELL_AT_DEST_MS_MAX);
}

static uint16_t clampLinearDoneMs(uint32_t ms)
{
  return (uint16_t)clampVal(ms, (uint32_t)0, (uint32_t)D_LINEAR_DONE_MS_MAX);
}

static uint32_t clampFeedServoBasePp(uint32_t pp)
{
  return clampVal(pp, (uint32_t)FEED_SERVO_BASE_PP_MIN, (uint32_t)FEED_SERVO_BASE_PP_MAX);
}

static uint16_t clampHolderOnMs(uint32_t ms)
{
  return (uint16_t)clampVal(ms, (uint32_t)0, (uint32_t)D_HOLDER_ON_MS_MAX);
}

static uint16_t clampHolderOpenMs(uint32_t ms)
{
  return (uint16_t)clampVal(ms, (uint32_t)0, (uint32_t)D_HOLDER_OPEN_MS_MAX);
}

static uint16_t clampGrippersOnMs(uint32_t ms)
{
  return (uint16_t)clampVal(ms, (uint32_t)0, (uint32_t)D_GRIPPERS_ON_MS_MAX);
}

static uint16_t clampGripperReleaseMs(uint32_t ms)
{
  return (uint16_t)clampVal(ms, (uint32_t)0, (uint32_t)D_GRIPPER_RELEASE_MS_MAX);
}

static uint16_t clampCutterPulseMs(uint32_t ms)
{
  return (uint16_t)clampVal(ms, (uint32_t)D_CUTTER_PULSE_MS_MIN, (uint32_t)D_CUTTER_PULSE_MS_MAX);
}

static uint16_t clampCutterPostMs(uint32_t ms)
{
  return (uint16_t)clampVal(ms, (uint32_t)0, (uint32_t)D_CUTTER_POST_MS_MAX);
}

static uint16_t clampAsentarDelayMs(uint32_t ms)
{
  return (uint16_t)clampVal(ms, (uint32_t)0, (uint32_t)ASENTAR_DELAY_MS_MAX);
}

static uint16_t clampFeedParallelDelayMs(uint32_t ms)
{
  return (uint16_t)clampVal(ms, (uint32_t)0, (uint32_t)FEED_PARALLEL_DELAY_MS_MAX);
}

static void applySpeedDefaults()
{
  linearRpm = LINEAR_RPM_DEFAULT;
  dwellAtDestMs = DWELL_AT_DEST_MS_DEFAULT;
  D_LINEAR_DONE_MS = D_LINEAR_DONE_MS_DEFAULT;
  feedServoBasePpA = FEED_SERVO_BASE_PP_DEFAULT;
  feedServoBasePpB = FEED_SERVO_BASE_PP_DEFAULT;
  D_HOLDER_ON_MS = D_HOLDER_ON_MS_DEFAULT;
  D_HOLDER_OPEN_MS = D_HOLDER_OPEN_MS_DEFAULT;
  D_GRIPPERS_ON_MS = D_GRIPPERS_ON_MS_DEFAULT;
  D_GRIPPER_RELEASE_MS = D_GRIPPER_RELEASE_MS_DEFAULT;
  D_CUTTER_PULSE_MS = D_CUTTER_PULSE_MS_DEFAULT;
  D_CUTTER_POST_MS = D_CUTTER_POST_MS_DEFAULT;
  asentarDelayMs = ASENTAR_DELAY_MS_DEFAULT;
  feedParallelStartDelayMs = FEED_PARALLEL_DELAY_MS_DEFAULT;
}

static void loadSpeedConfigFromNvs()
{
  prefs.begin(PREFS_NS, true);
  linearRpm = migrateLinearRpm(prefs.getUInt(PREFS_KEY_SP_STEP_HZ, LINEAR_RPM_DEFAULT));
  if (prefs.isKey(PREFS_KEY_SP_DWELL_DEST))
    dwellAtDestMs = clampDwellAtDestMs(prefs.getUInt(PREFS_KEY_SP_DWELL_DEST, DWELL_AT_DEST_MS_DEFAULT));
  else
  {
    uint32_t legacyMax = prefs.getUInt("spDwellMax", 100);
    uint32_t legacyRev = prefs.getUInt("spDwellRev", 50);
    dwellAtDestMs = clampDwellAtDestMs(legacyMax + legacyRev);
  }
  D_LINEAR_DONE_MS = clampLinearDoneMs(prefs.getUInt(PREFS_KEY_SP_LIN_DONE, D_LINEAR_DONE_MS_DEFAULT));
  {
    uint32_t legacyPp = clampFeedServoBasePp(prefs.getUInt(PREFS_KEY_SP_SERVO_PP, FEED_SERVO_BASE_PP_DEFAULT));
    feedServoBasePpA = clampFeedServoBasePp(prefs.getUInt(PREFS_KEY_SP_SERVO_PP_A, legacyPp));
    feedServoBasePpB = clampFeedServoBasePp(prefs.getUInt(PREFS_KEY_SP_SERVO_PP_B, legacyPp));
  }
  D_HOLDER_ON_MS = clampHolderOnMs(prefs.getUInt(PREFS_KEY_SP_HOLD_ON, D_HOLDER_ON_MS_DEFAULT));
  D_HOLDER_OPEN_MS = clampHolderOpenMs(prefs.getUInt(PREFS_KEY_SP_HOLD_OFF, D_HOLDER_OPEN_MS_DEFAULT));
  D_GRIPPERS_ON_MS = clampGrippersOnMs(prefs.getUInt(PREFS_KEY_SP_GRIP_ON, D_GRIPPERS_ON_MS_DEFAULT));
  D_GRIPPER_RELEASE_MS = clampGripperReleaseMs(prefs.getUInt(PREFS_KEY_SP_GRIP_REL, D_GRIPPER_RELEASE_MS_DEFAULT));
  D_CUTTER_PULSE_MS = clampCutterPulseMs(prefs.getUInt(PREFS_KEY_SP_CUT_PULSE, D_CUTTER_PULSE_MS_DEFAULT));
  D_CUTTER_POST_MS = clampCutterPostMs(prefs.getUInt(PREFS_KEY_SP_CUT_POST, D_CUTTER_POST_MS_DEFAULT));
  asentarDelayMs = clampAsentarDelayMs(prefs.getUInt(PREFS_KEY_SP_ASENTAR, ASENTAR_DELAY_MS_DEFAULT));
  feedParallelStartDelayMs = 0;  // dwell post-pulso eliminado; siempre 0
  prefs.end();
}

static void saveSpeedConfigToNvs()
{
  prefs.begin(PREFS_NS, false);
  prefs.putUInt(PREFS_KEY_SP_STEP_HZ, linearRpm);
  prefs.putUInt(PREFS_KEY_SP_DWELL_DEST, dwellAtDestMs);
  prefs.putUInt(PREFS_KEY_SP_LIN_DONE, D_LINEAR_DONE_MS);
  prefs.putUInt(PREFS_KEY_SP_SERVO_PP_A, feedServoBasePpA);
  prefs.putUInt(PREFS_KEY_SP_SERVO_PP_B, feedServoBasePpB);
  prefs.putUInt(PREFS_KEY_SP_HOLD_ON, D_HOLDER_ON_MS);
  prefs.putUInt(PREFS_KEY_SP_HOLD_OFF, D_HOLDER_OPEN_MS);
  prefs.putUInt(PREFS_KEY_SP_GRIP_ON, D_GRIPPERS_ON_MS);
  prefs.putUInt(PREFS_KEY_SP_GRIP_REL, D_GRIPPER_RELEASE_MS);
  prefs.putUInt(PREFS_KEY_SP_CUT_PULSE, D_CUTTER_PULSE_MS);
  prefs.putUInt(PREFS_KEY_SP_CUT_POST, D_CUTTER_POST_MS);
  prefs.putUInt(PREFS_KEY_SP_ASENTAR, asentarDelayMs);
  prefs.end();
  persistMachineBaseNow();
}

static void saveFeedModeToNvs()
{
  prefs.begin(PREFS_NS, false);
  prefs.putInt(PREFS_KEY_FEED_MODE, (int)feedMode);
  prefs.end();
  persistMachineBaseNow();
}

static void saveFeedTestConfigToNvs()
{
  prefs.begin(PREFS_NS, false);
  prefs.putInt(PREFS_KEY_FEED_TEST_SOLID, (int)feedTestSolidSteps);
  prefs.putInt(PREFS_KEY_FEED_TEST_CHUNK, (int)feedTestChunkSteps);
  prefs.putInt(PREFS_KEY_FEED_TEST_SOLID_R, (int)feedTestSolidStepsR);
  prefs.putInt(PREFS_KEY_FEED_TEST_CHUNK_R, (int)feedTestChunkStepsR);
  prefs.putUInt(PREFS_KEY_FEED_SS_FAST_L, feedSsFastPpL);
  prefs.putUInt(PREFS_KEY_FEED_SS_FAST_R, feedSsFastPpR);
  prefs.putBool(PREFS_KEY_LASER_VALID, laserValidationEnabled);
  prefs.end();
  persistMachineBaseNow();
}

static float feedSpm(bool sideR);
static float feedMaxMmS(bool sideR);
static int32_t feedMmToSteps(float mm, bool sideR);
static float feedStepsToMm(int32_t steps, bool sideR);
static uint32_t feedMmSToPp(float mmS, bool sideR);
static float feedPpToMmS(uint32_t pp, bool sideR);

static String speedConfigJson(bool includeLocked)
{
  String r = "{";
  r += "\"dwellAtDestMs\":" + String(dwellAtDestMs);
  r += ",\"linearDoneMs\":" + String(D_LINEAR_DONE_MS);
  r += ",\"holderOnMs\":" + String(D_HOLDER_ON_MS);
  r += ",\"holderOpenMs\":" + String(D_HOLDER_OPEN_MS);
  r += ",\"grippersOnMs\":" + String(D_GRIPPERS_ON_MS);
  r += ",\"gripperReleaseMs\":" + String(D_GRIPPER_RELEASE_MS);
  r += ",\"cutterPulseMs\":" + String(D_CUTTER_PULSE_MS);
  r += ",\"cutterPostMs\":" + String(D_CUTTER_POST_MS);
  r += ",\"asentarMs\":" + String(asentarDelayMs);
  r += ",\"parallelFeedDelayMs\":" + String(feedParallelStartDelayMs);
  if (includeLocked)
    r += ",\"locked\":" + String((cycleActive || linearIsMoving()) ? "true" : "false");
  r += "}";
  return r;
}

static bool parseAndApplySpeedArg(const String& key, const String& val)
{
  if (key == "linearRpm") {
    int32_t rpm = val.toInt();
    if (rpm < 0) rpm = 0;
    linearRpm = clampLinearRpm((uint32_t)rpm);
    return true;
  }
  if (key == "stepFreqHz") { linearRpm = migrateLinearRpm((uint32_t)val.toInt()); return true; }
  if (key == "dwellAtDestMs") { dwellAtDestMs = clampDwellAtDestMs(val.toInt()); return true; }
  if (key == "linearDoneMs") { D_LINEAR_DONE_MS = clampLinearDoneMs(val.toInt()); return true; }
  if (key == "feedServoMmSA") { feedServoBasePpA = feedMmSToPp(val.toFloat(), false); return true; }
  if (key == "feedServoMmSB") { feedServoBasePpB = feedMmSToPp(val.toFloat(), true); return true; }
  if (key == "feedServoBasePpA") { feedServoBasePpA = clampFeedServoBasePp(val.toInt()); return true; }
  if (key == "feedServoBasePpB") { feedServoBasePpB = clampFeedServoBasePp(val.toInt()); return true; }
  if (key == "holderOnMs") { D_HOLDER_ON_MS = clampHolderOnMs(val.toInt()); return true; }
  if (key == "holderOpenMs") { D_HOLDER_OPEN_MS = clampHolderOpenMs(val.toInt()); return true; }
  if (key == "grippersOnMs") { D_GRIPPERS_ON_MS = clampGrippersOnMs(val.toInt()); return true; }
  if (key == "gripperReleaseMs") { D_GRIPPER_RELEASE_MS = clampGripperReleaseMs(val.toInt()); return true; }
  if (key == "cutterPulseMs") { D_CUTTER_PULSE_MS = clampCutterPulseMs(val.toInt()); return true; }
  if (key == "cutterPostMs") { D_CUTTER_POST_MS = clampCutterPostMs(val.toInt()); return true; }
  if (key == "asentarMs") { asentarDelayMs = clampAsentarDelayMs(val.toInt()); return true; }
  if (key == "parallelFeedDelayMs") { feedParallelStartDelayMs = 0; return true; }
  return false;
}

static int32_t clampFeedTestSolidSteps(int32_t steps)
{
  return clampVal(steps, (int32_t)FEED_TEST_SOLID_MIN, (int32_t)FEED_TEST_SOLID_MAX);
}

static int32_t clampFeedTestChunkSteps(int32_t steps)
{
  return clampVal(steps, (int32_t)FEED_TEST_CHUNK_MIN, (int32_t)FEED_TEST_CHUNK_MAX);
}

static uint32_t clampFeedSsSpeedPp(uint32_t pp)
{
  return clampVal(pp, (uint32_t)FEED_SERVO_BASE_PP_MIN, (uint32_t)FEED_SERVO_BASE_PP_MAX);
}

static float clampFeedOffsetMm(float mm)
{
  return clampVal(mm, FEED_OFFSET_MM_MIN, FEED_OFFSET_MM_MAX);
}

static float clampFeedStepsPerMm(float v)
{
  return clampVal(v, FEED_STEPS_PER_MM_MIN, FEED_STEPS_PER_MM_MAX);
}

static float feedSpm(bool sideR)
{
  float s = sideR ? (float)feedStepsPerMmB : (float)feedStepsPerMm;
  if (s < 0.01f) s = FEED_STEPS_PER_MM_DEFAULT;
  return s;
}

static float feedMaxMmS(bool sideR)
{
  return (float)SERVO_MAX_PPS / feedSpm(sideR);
}

static int32_t feedMmToSteps(float mm, bool sideR)
{
  float s = mm * feedSpm(sideR);
  return (int32_t)(s >= 0.0f ? s + 0.5f : s - 0.5f);
}

static float feedStepsToMm(int32_t steps, bool sideR)
{
  return (float)steps / feedSpm(sideR);
}

static uint32_t feedMmSToPp(float mmS, bool sideR)
{
  if (mmS < FEED_MM_S_MIN) mmS = FEED_MM_S_MIN;
  float maxS = feedMaxMmS(sideR);
  if (mmS > maxS) mmS = maxS;
  float pp = mmS * feedSpm(sideR);
  if (pp < (float)FEED_SERVO_BASE_PP_MIN) pp = (float)FEED_SERVO_BASE_PP_MIN;
  if (pp > (float)FEED_SERVO_BASE_PP_MAX) pp = (float)FEED_SERVO_BASE_PP_MAX;
  return (uint32_t)(pp + 0.5f);
}

static float feedPpToMmS(uint32_t pp, bool sideR)
{
  return (float)pp / feedSpm(sideR);
}

// 0x6083/6084: a = V / t_rampa (DSY 5.9). No copiar V (eso deja t=1 s y no llega a Fast).
static uint32_t feedProfileAccForVel(uint32_t vel)
{
  if (vel < 1) vel = 1;
  uint64_t a = ((uint64_t)vel * 1000ULL) / (uint64_t)FEED_SERVO_RAMP_MS;
  if (a < (uint64_t)FEED_SERVO_ACC_MIN) a = FEED_SERVO_ACC_MIN;
  if (a > (uint64_t)FEED_SERVO_ACC_MAX) a = FEED_SERVO_ACC_MAX;
  return (uint32_t)a;
}

static void feedCanSetVelAcc(uint32_t velL, uint32_t velR)
{
  const uint32_t aL = feedProfileAccForVel(velL);
  const uint32_t aR = feedProfileAccForVel(velR);
  canSetMotionProfile(velL, aL, aL, velR, aR, aR);
}

// Modelo S — calculador único de perfil PP (0 → V → 0).
static FeedProfilePlan feedProfilePlanCore(float targetMm, float vReqMmS, bool sideR)
{
  FeedProfilePlan p = {};
  p.targetMm = targetMm;
  p.vReqMmS = vReqMmS;
  if (targetMm <= 0.0f || vReqMmS < FEED_MM_S_MIN)
    return p;
  if (vReqMmS > feedMaxMmS(sideR))
    return p;

  const uint32_t velPp = feedMmSToPp(vReqMmS, sideR);
  const uint32_t accPp = feedProfileAccForVel(velPp);
  const float spm = feedSpm(sideR);
  if (spm < 0.01f)
    return p;
  const float aMm = (float)accPp / spm;
  if (aMm < 0.01f)
    return p;

  p.accMmS2 = aMm;
  p.decMmS2 = aMm;
  p.velPp = velPp;
  p.accPp = accPp;
  p.decPp = accPp;
  p.dAccMm = (vReqMmS * vReqMmS) / (2.0f * aMm);
  p.dDecMm = p.dAccMm;
  p.dMinMm = p.dAccMm + p.dDecMm;
  p.marginMm = targetMm - p.dMinMm;
  p.valid = (p.marginMm >= -0.001f);
  if (p.valid)
    p.dCruiseMm = (p.marginMm > 0.0f) ? p.marginMm : 0.0f;
  return p;
}

static float feedProfileMaxVelocityMmS(float targetMm, bool sideR)
{
  if (targetMm <= 0.0f)
    return 0.0f;
  float lo = FEED_MM_S_MIN;
  float hi = feedMaxMmS(sideR);
  const float tRamp = (float)FEED_SERVO_RAMP_MS / 1000.0f;
  if (tRamp > 0.0001f)
  {
    const float triMax = targetMm / tRamp;
    if (triMax < hi)
      hi = triMax;
  }
  if (hi < lo)
    return 0.0f;
  for (int i = 0; i < 32; i++)
  {
    if (hi - lo < 0.05f)
      break;
    const float mid = (lo + hi) * 0.5f;
    if (feedProfilePlanCore(targetMm, mid, sideR).valid)
      lo = mid;
    else
      hi = mid;
  }
  return feedProfilePlanCore(targetMm, lo, sideR).valid ? lo : 0.0f;
}

static FeedProfilePlan feedProfilePlan(float targetMm, float vReqMmS, bool sideR)
{
  FeedProfilePlan p = feedProfilePlanCore(targetMm, vReqMmS, sideR);
  p.vMaxMmS = feedProfileMaxVelocityMmS(targetMm, sideR);
  return p;
}

static bool feedProfileSideValid(float targetMm, float vReqMmS, bool sideR)
{
  return feedProfilePlanCore(targetMm, vReqMmS, sideR).valid;
}

static void feedProfileAppendJson(const FeedProfilePlan& p, String& out, const char* prefix)
{
  out += ",\""; out += prefix; out += "Valid\":"; out += p.valid ? "true" : "false";
  out += ",\""; out += prefix; out += "VmaxMmS\":"; out += String(p.vMaxMmS, 1);
  out += ",\""; out += prefix; out += "AccMmS2\":"; out += String(p.accMmS2, 1);
  out += ",\""; out += prefix; out += "DecMmS2\":"; out += String(p.decMmS2, 1);
  out += ",\""; out += prefix; out += "DAccMm\":"; out += String(p.dAccMm, 2);
  out += ",\""; out += prefix; out += "DDecMm\":"; out += String(p.dDecMm, 2);
  out += ",\""; out += prefix; out += "DCruiseMm\":"; out += String(p.dCruiseMm, 2);
  out += ",\""; out += prefix; out += "DMinMm\":"; out += String(p.dMinMm, 2);
  out += ",\""; out += prefix; out += "MarginMm\":"; out += String(p.marginMm, 2);
}

// 0 en NVS → default del lado (evita constrain(0)→500 = sentido ambiguo).
static uint16_t sanitizePfServoPwmUs(uint16_t us)
{
  if (us < PF_SERVO_PWM_MIN_US)
    return (uint16_t)PF_SERVO_PWM_DEFAULT_US;
  if (us > PF_SERVO_PWM_MAX_US)
    return (uint16_t)PF_SERVO_PWM_MAX_US;
  return us;
}

static int32_t feedOffsetSteps()
{
  float s = feedOffsetMm * feedStepsPerMm;
  return (int32_t)(s >= 0 ? s + 0.5f : s - 0.5f);
}

static int32_t feedOffsetStepsB()
{
  float s = feedOffsetMmB * feedStepsPerMmB;
  return (int32_t)(s >= 0 ? s + 0.5f : s - 0.5f);
}

static int32_t feedNominalStepsForCal()
{
  return feedBypassSteps;
}

static int32_t feedNominalStepsForCalB()
{
  return feedBypassStepsB;
}

static int32_t feedStepsForB(int32_t stepsA)
{
  if (feedStepsTargetThisFeed > 0 && feedStepsTargetB > 0)
    return (int32_t)(((int64_t)stepsA * feedStepsTargetB + (feedStepsTargetThisFeed / 2)) / feedStepsTargetThisFeed);
  if (feedStepsPerMm > 0.01f)
  {
    float s = (float)stepsA * (feedStepsPerMmB / feedStepsPerMm);
    return (int32_t)(s >= 0 ? s + 0.5f : s - 0.5f);
  }
  return stepsA;
}

// Recalcula pasos/mm desde mm medidos del nominal.
static void feedRecomputeStepsPerMm()
{
  if (feedCalMeasuredMm <= 0.0f) return;
  int32_t ref = feedNominalStepsForCal();
  if (ref <= 0) return;
  feedStepsPerMm = clampFeedStepsPerMm((float)ref / feedCalMeasuredMm);
}

static void feedRecomputeStepsPerMmB()
{
  if (feedCalMeasuredMmB <= 0.0f) return;
  int32_t ref = feedNominalStepsForCalB();
  if (ref <= 0) return;
  feedStepsPerMmB = clampFeedStepsPerMm((float)ref / feedCalMeasuredMmB);
}

static void loadPersistedSettings()
{
  prefs.begin(PREFS_NS, true);
  feedBypassSteps = clampFeedBypassSteps(prefs.getInt(PREFS_KEY_BYPASS_STEPS, FEED_REF_STEPS_DEFAULT));
  feedBypassStepsB = clampFeedBypassSteps(prefs.getInt(PREFS_KEY_BYPASS_STEPS_B, feedBypassSteps));
  feedMode = clampFeedMode(prefs.getInt(PREFS_KEY_FEED_MODE, FEED_MODE_STEPS_SENSOR));
  feedStepsTolerance = 0;
  feedStepsTolerancePercent = 0;
  feedTestSolidSteps = clampFeedTestSolidSteps(prefs.getInt(PREFS_KEY_FEED_TEST_SOLID, FEED_TEST_SOLID_DEFAULT));
  feedTestChunkSteps = clampFeedTestChunkSteps(prefs.getInt(PREFS_KEY_FEED_TEST_CHUNK, FEED_TEST_CHUNK_DEFAULT));
  feedTestSolidStepsR = clampFeedTestSolidSteps(prefs.getInt(PREFS_KEY_FEED_TEST_SOLID_R, feedTestSolidSteps));
  feedTestChunkStepsR = clampFeedTestChunkSteps(prefs.getInt(PREFS_KEY_FEED_TEST_CHUNK_R, feedTestChunkSteps));
  feedSsFastPpL = clampFeedSsSpeedPp(prefs.getUInt(PREFS_KEY_FEED_SS_FAST_L, FEED_SS_FAST_PP_DEFAULT));
  feedSsFastPpR = clampFeedSsSpeedPp(prefs.getUInt(PREFS_KEY_FEED_SS_FAST_R, feedSsFastPpL));
  laserValidationEnabled = prefs.getBool(PREFS_KEY_LASER_VALID, FEED_LASER_VALIDATION_DEFAULT);
  feedSkipEncoderConfirm = false;
  prefeederTriggerEveryN = 1;
  depositBatchSize = clampDepositBatchSize(
    prefs.getUInt(PREFS_KEY_DEPOSIT_BATCH, DEPOSIT_BATCH_SIZE_DEFAULT));
  depositExtraMm = DEPOSIT_EXTRA_MM_DEFAULT;       // fijo 30 mm
  depositInterlaceMm = 0.0f;                       // Interlace eliminado
  depositBetweenBatchMm = DEPOSIT_BETWEEN_GAP_MM;  // gap fijo; paso total auto con L
  feedOffsetMm = clampFeedOffsetMm(prefs.getFloat(PREFS_KEY_FEED_OFFSET_MM, 0.0f));
  feedOffsetMmB = clampFeedOffsetMm(prefs.getFloat(PREFS_KEY_FEED_OFFSET_MM_B, feedOffsetMm));
  feedStepsPerMm = clampFeedStepsPerMm(prefs.getFloat(PREFS_KEY_FEED_STEPS_PER_MM, FEED_STEPS_PER_MM_DEFAULT));
  feedStepsPerMmB = clampFeedStepsPerMm(prefs.getFloat(PREFS_KEY_FEED_STEPS_PER_MM_B, feedStepsPerMm));
  feedCalMeasuredMm = prefs.getFloat(PREFS_KEY_FEED_CAL_MM, FEED_NOMINAL_MM);
  feedCalMeasuredMmB = prefs.getFloat(PREFS_KEY_FEED_CAL_MM_B, feedCalMeasuredMm);
  linearGripperAreaMm = LINEAR_GRIPPER_AREA_DEFAULT;  // fijo; no editable / no NVS
  linearStepsPerMm = clampLinearStepsPerMm(prefs.getFloat(PREFS_KEY_LINEAR_STEPS_PM, LINEAR_STEPS_PER_MM));
  linearOffsetSteps = clampLinearOffsetSteps(prefs.getFloat(PREFS_KEY_LINEAR_OFFSET, 0.0f));
  tcmBuzzerMuted = prefs.getBool(PREFS_KEY_BUZZER_MUTE, false);
  stepByStepMode = prefs.getBool(PREFS_KEY_STEP_BY_STEP, false);
  prefs.end();
  // Limpia valor viejo de NVS si quedó distinto de fábrica.
  prefs.begin(PREFS_NS, false);
  if (prefs.isKey(PREFS_KEY_LINEAR_GRIPPER)
      && fabsf(prefs.getFloat(PREFS_KEY_LINEAR_GRIPPER, LINEAR_GRIPPER_AREA_DEFAULT)
               - LINEAR_GRIPPER_AREA_DEFAULT) > 0.05f)
    prefs.putFloat(PREFS_KEY_LINEAR_GRIPPER, LINEAR_GRIPPER_AREA_DEFAULT);
  prefs.putFloat(PREFS_KEY_DEPOSIT_EXTRA, depositExtraMm);
  prefs.putFloat(PREFS_KEY_DEPOSIT_ILACE, depositInterlaceMm);
  prefs.putFloat(PREFS_KEY_DEPOSIT_BETWEEN, depositBetweenBatchMm);
  prefs.end();
  loadSpeedConfigFromNvs();
  Serial.printf("NVS: feed=%s steps=%ld | lineal G=%.1f(fijo) spm=%.2f | prefeeder/%u batch=%u\n",
                feedModeLabel(feedMode), (long)feedBypassSteps,
                linearGripperAreaMm, linearStepsPerMm,
                (unsigned)prefeederTriggerEveryN, (unsigned)depositBatchSize);
  DBG_PRINTF("NVS detail: tolSteps=%d tolEnc=%u%% solid=%ld chunk=%ld skipEnc=%d feedOff=%.1f extra=%.1f ilace=%.1f betw=%.1f\n",
             (int)feedStepsTolerance, (unsigned)feedStepsTolerancePercent,
             (long)feedTestSolidSteps, (long)feedTestChunkSteps,
             feedSkipEncoderConfirm ? 1 : 0, feedOffsetMm, depositExtraMm,
             depositInterlaceMm, depositBetweenBatchMm);
}

static void saveFeedBypassStepsToNvs()
{
  prefs.begin(PREFS_NS, false);
  prefs.putInt(PREFS_KEY_BYPASS_STEPS, (int)feedBypassSteps);
  prefs.putInt(PREFS_KEY_BYPASS_STEPS_B, (int)feedBypassStepsB);
  prefs.end();
}

static void saveFeedSkipEncoderToNvs()
{
  prefs.begin(PREFS_NS, false);
  prefs.putBool(PREFS_KEY_SKIP_ENCODER, feedSkipEncoderConfirm);
  prefs.end();
}

static void savePrefeederTriggerConfigToNvs()
{
  prefeederTriggerEveryN = 1;
  persistMachineBaseNow();
}

static void copyPrefeederCfgSideLtoR(MachineBase& r)
{
  r.pfAutoRpmR = r.pfAutoRpm;
  r.pfAutoReverseSR = r.pfAutoReverseS;
  r.pfServoPwmUsR = (uint16_t)PF_SERVO_PWM_R_US;
  r.pfMotor2RpmR = r.pfMotor2Rpm;
  r.pfTensionCooldownSR = r.pfTensionCooldownS;
  r.pfTensionFaultSR = r.pfTensionFaultS;
  r.pfHolguraExtraSR = r.pfHolguraExtraS;
  r.pfTriggerFeedSR = r.pfTriggerFeedS;
}

static void fillPrefeederCfgFromMirror(MachineBase& r, uint8_t side)
{
  if (side >= PEER_COUNT) side = PEER_L;
  const PrefeederMirror& m = peers[side].mirror;
  if (side == PEER_R)
  {
    r.pfAutoRpmR = m.autoRpm;
    r.pfAutoReverseSR = m.autoReverseS;
    r.pfServoPwmUsR = m.servoPwmUs;
    r.pfMotor2RpmR = m.motor2Rpm;
    r.pfTensionCooldownSR = m.tensionCooldownS;
    r.pfTensionFaultSR = m.tensionFaultS;
    r.pfHolguraExtraSR = m.holguraExtraFeedS;
    r.pfTriggerFeedSR = m.triggerFeedS;
  }
  else
  {
    r.pfAutoRpm = m.autoRpm;
    r.pfAutoReverseS = m.autoReverseS;
    r.pfServoPwmUs = m.servoPwmUs;
    r.pfMotor2Rpm = m.motor2Rpm;
    r.pfTensionCooldownS = m.tensionCooldownS;
    r.pfTensionFaultS = m.tensionFaultS;
    r.pfHolguraExtraS = m.holguraExtraFeedS;
    r.pfTriggerFeedS = m.triggerFeedS;
  }
}

static MachineBase upgradeMachineBaseV2(const MachineBaseV2& old)
{
  MachineBase r = {};
  r.cutOffsetMm = clampCutOffsetMm(old.cutOffsetMm);
  r.stepFreqHz = old.stepFreqHz;
  r.dwellAtDestMs = old.dwellAtDestMs;
  r.linearDoneMs = old.linearDoneMs;
  r.feedServoBasePpA = old.feedServoBasePpA;
  r.feedServoBasePpB = old.feedServoBasePpB;
  r.holderOnMs = old.holderOnMs;
  r.holderOpenMs = old.holderOpenMs;
  r.grippersOnMs = old.grippersOnMs;
  r.gripperReleaseMs = old.gripperReleaseMs;
  r.cutterPulseMs = old.cutterPulseMs;
  r.cutterPostMs = old.cutterPostMs;
  r.asentarMs = old.asentarMs;
  r.parallelFeedDelayMs = 0;  // dwell post-pulso eliminado
  r.feedMode = (uint8_t)clampFeedMode(old.feedMode);
  r.feedSolidSteps = old.feedSolidSteps;
  r.feedChunkSteps = old.feedChunkSteps;
  r.feedSolidStepsR = old.feedSolidSteps;
  r.feedChunkStepsR = old.feedChunkSteps;
  r.feedSlowdownStepsL = FEED_SS_SLOWDOWN_DEFAULT;
  r.feedSlowdownStepsR = FEED_SS_SLOWDOWN_DEFAULT;
  r.feedSsFastPpL = FEED_SS_FAST_PP_DEFAULT;
  r.feedSsFastPpR = FEED_SS_FAST_PP_DEFAULT;
  r.feedSsSlowPpL = FEED_SS_SLOW_PP_DEFAULT;
  r.feedSsSlowPpR = FEED_SS_SLOW_PP_DEFAULT;
  r.depositBatchSize = old.depositBatchSize;
  r.depositExtraMm = DEPOSIT_EXTRA_MM_DEFAULT;
  r.depositInterlaceMm = 0.0f;
  r.depositBetweenBatchMm = DEPOSIT_BETWEEN_GAP_MM;
  r.prefeederEveryN = old.prefeederEveryN;
  r.pfAutoRpm = old.pfAutoRpm;
  r.pfAutoReverseS = old.pfAutoReverseS;
  r.pfServoPwmUs = old.pfServoPwmUs;
  r.pfMotor2Rpm = old.pfMotor2Rpm;
  r.pfTensionCooldownS = old.pfTensionCooldownS;
  r.pfTensionFaultS = old.pfTensionFaultS;
  r.pfHolguraExtraS = old.pfHolguraExtraS;
  r.pfTriggerFeedS = old.pfTriggerFeedS;
  copyPrefeederCfgSideLtoR(r);
  return r;
}

static MachineBase upgradeMachineBaseV3(const MachineBaseV3& old)
{
  MachineBase r = {};
  r.cutOffsetMm = clampCutOffsetMm(old.cutOffsetMm);
  r.stepFreqHz = old.stepFreqHz;
  r.dwellAtDestMs = old.dwellAtDestMs;
  r.linearDoneMs = old.linearDoneMs;
  r.feedServoBasePpA = old.feedServoBasePpA;
  r.feedServoBasePpB = old.feedServoBasePpB;
  r.holderOnMs = old.holderOnMs;
  r.holderOpenMs = old.holderOpenMs;
  r.grippersOnMs = old.grippersOnMs;
  r.gripperReleaseMs = old.gripperReleaseMs;
  r.cutterPulseMs = old.cutterPulseMs;
  r.cutterPostMs = old.cutterPostMs;
  r.asentarMs = old.asentarMs;
  r.parallelFeedDelayMs = 0;  // dwell post-pulso eliminado
  r.feedMode = (uint8_t)clampFeedMode(old.feedMode);
  r.feedSolidSteps = old.feedSolidSteps;
  r.feedChunkSteps = old.feedChunkSteps;
  r.feedSolidStepsR = old.feedSolidSteps;
  r.feedChunkStepsR = old.feedChunkSteps;
  r.feedSlowdownStepsL = FEED_SS_SLOWDOWN_DEFAULT;
  r.feedSlowdownStepsR = FEED_SS_SLOWDOWN_DEFAULT;
  r.feedSsFastPpL = FEED_SS_FAST_PP_DEFAULT;
  r.feedSsFastPpR = FEED_SS_FAST_PP_DEFAULT;
  r.feedSsSlowPpL = FEED_SS_SLOW_PP_DEFAULT;
  r.feedSsSlowPpR = FEED_SS_SLOW_PP_DEFAULT;
  r.depositBatchSize = old.depositBatchSize;
  r.depositExtraMm = DEPOSIT_EXTRA_MM_DEFAULT;
  r.depositInterlaceMm = 0.0f;
  r.depositBetweenBatchMm = DEPOSIT_BETWEEN_GAP_MM;
  r.prefeederEveryN = old.prefeederEveryN;
  r.pfAutoRpm = old.pfAutoRpm;
  r.pfAutoReverseS = old.pfAutoReverseS;
  r.pfServoPwmUs = old.pfServoPwmUs;
  r.pfMotor2Rpm = old.pfMotor2Rpm;
  r.pfTensionCooldownS = old.pfTensionCooldownS;
  r.pfTensionFaultS = old.pfTensionFaultS;
  r.pfHolguraExtraS = old.pfHolguraExtraS;
  r.pfTriggerFeedS = old.pfTriggerFeedS;
  r.pfAutoRpmR = old.pfAutoRpmR;
  r.pfAutoReverseSR = old.pfAutoReverseSR;
  r.pfServoPwmUsR = old.pfServoPwmUsR;
  r.pfMotor2RpmR = old.pfMotor2RpmR;
  r.pfTensionCooldownSR = old.pfTensionCooldownSR;
  r.pfTensionFaultSR = old.pfTensionFaultSR;
  r.pfHolguraExtraSR = old.pfHolguraExtraSR;
  r.pfTriggerFeedSR = old.pfTriggerFeedSR;
  return r;
}

static MachineBase upgradeMachineBaseV4(const MachineBaseV4& old)
{
  MachineBase r = {};
  r.cutOffsetMm = clampCutOffsetMm(old.cutOffsetMm);
  r.stepFreqHz = old.stepFreqHz;
  r.dwellAtDestMs = old.dwellAtDestMs;
  r.linearDoneMs = old.linearDoneMs;
  r.feedServoBasePpA = old.feedServoBasePpA;
  r.feedServoBasePpB = old.feedServoBasePpB;
  r.holderOnMs = old.holderOnMs;
  r.holderOpenMs = old.holderOpenMs;
  r.grippersOnMs = old.grippersOnMs;
  r.gripperReleaseMs = old.gripperReleaseMs;
  r.cutterPulseMs = old.cutterPulseMs;
  r.cutterPostMs = old.cutterPostMs;
  r.asentarMs = old.asentarMs;
  r.parallelFeedDelayMs = 0;
  r.feedMode = (uint8_t)clampFeedMode(old.feedMode);
  r.feedSolidSteps = old.feedSolidSteps;
  r.feedChunkSteps = old.feedChunkSteps;
  r.feedSolidStepsR = old.feedSolidStepsR;
  r.feedChunkStepsR = old.feedChunkStepsR;
  r.feedSlowdownStepsL = FEED_SS_SLOWDOWN_DEFAULT;
  r.feedSlowdownStepsR = FEED_SS_SLOWDOWN_DEFAULT;
  r.feedSsFastPpL = FEED_SS_FAST_PP_DEFAULT;
  r.feedSsFastPpR = FEED_SS_FAST_PP_DEFAULT;
  r.feedSsSlowPpL = FEED_SS_SLOW_PP_DEFAULT;
  r.feedSsSlowPpR = FEED_SS_SLOW_PP_DEFAULT;
  r.depositBatchSize = old.depositBatchSize;
  r.depositExtraMm = DEPOSIT_EXTRA_MM_DEFAULT;
  r.depositInterlaceMm = 0.0f;
  r.depositBetweenBatchMm = DEPOSIT_BETWEEN_GAP_MM;
  r.prefeederEveryN = old.prefeederEveryN;
  r.pfAutoRpm = old.pfAutoRpm;
  r.pfAutoReverseS = old.pfAutoReverseS;
  r.pfServoPwmUs = old.pfServoPwmUs;
  r.pfMotor2Rpm = old.pfMotor2Rpm;
  r.pfTensionCooldownS = old.pfTensionCooldownS;
  r.pfTensionFaultS = old.pfTensionFaultS;
  r.pfHolguraExtraS = old.pfHolguraExtraS;
  r.pfTriggerFeedS = old.pfTriggerFeedS;
  r.pfAutoRpmR = old.pfAutoRpmR;
  r.pfAutoReverseSR = old.pfAutoReverseSR;
  r.pfServoPwmUsR = old.pfServoPwmUsR;
  r.pfMotor2RpmR = old.pfMotor2RpmR;
  r.pfTensionCooldownSR = old.pfTensionCooldownSR;
  r.pfTensionFaultSR = old.pfTensionFaultSR;
  r.pfHolguraExtraSR = old.pfHolguraExtraSR;
  r.pfTriggerFeedSR = old.pfTriggerFeedSR;
  return r;
}

static void saveDepositConfigToNvs()
{
  depositExtraMm = DEPOSIT_EXTRA_MM_DEFAULT;
  depositInterlaceMm = 0.0f;
  depositBetweenBatchMm = DEPOSIT_BETWEEN_GAP_MM;
  prefs.begin(PREFS_NS, false);
  prefs.putUInt(PREFS_KEY_DEPOSIT_BATCH, depositBatchSize);
  prefs.putFloat(PREFS_KEY_DEPOSIT_EXTRA, depositExtraMm);
  prefs.putFloat(PREFS_KEY_DEPOSIT_ILACE, depositInterlaceMm);
  prefs.putFloat(PREFS_KEY_DEPOSIT_BETWEEN, depositBetweenBatchMm);
  prefs.end();
  persistMachineBaseNow();
}

static void saveFeedOffsetToNvs()
{
  prefs.begin(PREFS_NS, false);
  prefs.putFloat(PREFS_KEY_FEED_OFFSET_MM, feedOffsetMm);
  prefs.putFloat(PREFS_KEY_FEED_OFFSET_MM_B, feedOffsetMmB);
  prefs.end();
}

static void saveFeedCalToNvs()
{
  prefs.begin(PREFS_NS, false);
  prefs.putFloat(PREFS_KEY_FEED_CAL_MM, feedCalMeasuredMm);
  prefs.putFloat(PREFS_KEY_FEED_STEPS_PER_MM, feedStepsPerMm);
  prefs.putFloat(PREFS_KEY_FEED_CAL_MM_B, feedCalMeasuredMmB);
  prefs.putFloat(PREFS_KEY_FEED_STEPS_PER_MM_B, feedStepsPerMmB);
  prefs.end();
}

static MachineBase captureCurrentMachineBase()
{
  MachineBase r = {};
  r.cutOffsetMm = clampCutOffsetMm(cutOffsetMm);
  r.stepFreqHz = linearRpm;
  r.dwellAtDestMs = dwellAtDestMs;
  r.linearDoneMs = D_LINEAR_DONE_MS;
  r.feedServoBasePpA = feedServoBasePpA;
  r.feedServoBasePpB = feedServoBasePpB;
  r.holderOnMs = D_HOLDER_ON_MS;
  r.holderOpenMs = D_HOLDER_OPEN_MS;
  r.grippersOnMs = D_GRIPPERS_ON_MS;
  r.gripperReleaseMs = D_GRIPPER_RELEASE_MS;
  r.cutterPulseMs = D_CUTTER_PULSE_MS;
  r.cutterPostMs = D_CUTTER_POST_MS;
  r.asentarMs = asentarDelayMs;
  r.parallelFeedDelayMs = 0;  // legacy NVS; dwell post-pulso fijo 0
  r.feedMode = (uint8_t)clampFeedMode((int)feedMode);
  r.feedSolidSteps = feedTestSolidSteps;
  r.feedChunkSteps = feedTestChunkSteps;
  r.feedSolidStepsR = feedTestSolidStepsR;
  r.feedChunkStepsR = feedTestChunkStepsR;
  r.feedSlowdownStepsL = 0;
  r.feedSlowdownStepsR = 0;
  r.feedSsFastPpL = feedSsFastPpL;
  r.feedSsFastPpR = feedSsFastPpR;
  r.feedSsSlowPpL = feedSsFastPpL;
  r.feedSsSlowPpR = feedSsFastPpR;
  r.depositBatchSize = depositBatchSize;
  r.depositExtraMm = DEPOSIT_EXTRA_MM_DEFAULT;
  r.depositInterlaceMm = 0.0f;
  r.depositBetweenBatchMm = DEPOSIT_BETWEEN_GAP_MM;
  r.prefeederEveryN = 1;
  fillPrefeederCfgFromMirror(r, PEER_L);
  fillPrefeederCfgFromMirror(r, PEER_R);
  // Si R aún no reportó, espejar L para no guardar ceros.
  if (!peers[PEER_R].mirror.peerOk || !peers[PEER_R].fullStatusReceived)
    copyPrefeederCfgSideLtoR(r);
  r.pfServoPwmUs = (uint16_t)PF_SERVO_PWM_L_US;
  r.pfServoPwmUsR = (uint16_t)PF_SERVO_PWM_R_US;
  return r;
}

static bool peerPrefeederMirrorFresh(uint8_t side, uint32_t maxAgeMs = PEER_STALE_MS)
{
  if (side >= PEER_COUNT) return false;
  const PeerSlot& p = peers[side];
  return p.mirror.peerOk && p.fullStatusReceived && p.lastRxMs != 0
    && (millis() - p.lastRxMs <= maxAgeMs);
}

static void persistMachineBaseNow()
{
  MachineBase snap = captureCurrentMachineBase();
  if (!peerPrefeederMirrorFresh(PEER_L))
  {
    snap.pfAutoRpm = machineBase.pfAutoRpm;
    snap.pfAutoReverseS = machineBase.pfAutoReverseS;
    snap.pfServoPwmUs = machineBase.pfServoPwmUs;
    snap.pfMotor2Rpm = machineBase.pfMotor2Rpm;
    snap.pfTensionCooldownS = machineBase.pfTensionCooldownS;
    snap.pfTensionFaultS = machineBase.pfTensionFaultS;
    snap.pfHolguraExtraS = machineBase.pfHolguraExtraS;
    snap.pfTriggerFeedS = machineBase.pfTriggerFeedS;
  }
  if (!peerPrefeederMirrorFresh(PEER_R))
  {
    snap.pfAutoRpmR = machineBase.pfAutoRpmR;
    snap.pfAutoReverseSR = machineBase.pfAutoReverseSR;
    snap.pfServoPwmUsR = machineBase.pfServoPwmUsR;
    snap.pfMotor2RpmR = machineBase.pfMotor2RpmR;
    snap.pfTensionCooldownSR = machineBase.pfTensionCooldownSR;
    snap.pfTensionFaultSR = machineBase.pfTensionFaultSR;
    snap.pfHolguraExtraSR = machineBase.pfHolguraExtraSR;
    snap.pfTriggerFeedSR = machineBase.pfTriggerFeedSR;
  }
  snap.pfServoPwmUs = (uint16_t)PF_SERVO_PWM_L_US;
  snap.pfServoPwmUsR = (uint16_t)PF_SERVO_PWM_R_US;
  machineBase = snap;
  saveMachineBase();
}

static bool findBasePartLength(const String& partNumber, uint16_t& lengthMm)
{
  for (size_t i = 0; i < BASE_PART_COUNT; i++)
  {
    if (partNumber == BASE_PARTS[i].partNumber)
    {
      lengthMm = BASE_PARTS[i].lengthMm;
      return true;
    }
  }
  return false;
}

static bool findPartLength(const String& partNumber, uint16_t& lengthMm)
{
  if (partCatalogReady() && partCatalogExists(partNumber.c_str()))
  {
    PartModel tmp;
    if (partCatalogLoad(partNumber.c_str(), tmp) && tmp.lengthMm > 0)
    {
      lengthMm = tmp.lengthMm;
      return true;
    }
  }
  return findBasePartLength(partNumber, lengthMm);
}

static bool saveMachineBase()
{
  Preferences p;
  if (!p.begin(PART_PREFS_NS, false))
  {
    Serial.println("BASE: NVS begin FAIL");
    return false;
  }
  size_t n = p.putBytes(PART_PREFS_KEY_BASE, &machineBase, sizeof(machineBase));
  const bool ok = (n == sizeof(machineBase));
  p.putBool(PART_PREFS_KEY_BASE_OK, ok);
  p.end();
  if (!ok)
    Serial.printf("BASE: save FAIL wrote=%u need=%u\n",
                  (unsigned)n, (unsigned)sizeof(machineBase));
  return ok;
}

static bool loadMachineBase()
{
  Preferences p;
  if (!p.begin(PART_PREFS_NS, true)) return false;
  bool okFlag = p.getBool(PART_PREFS_KEY_BASE_OK, false);
  size_t len = p.getBytesLength(PART_PREFS_KEY_BASE);
  if (!okFlag || len == 0)
  {
    p.end();
    return false;
  }
  if (len == sizeof(MachineBase))
  {
    MachineBase tmp;
    memset(&tmp, 0, sizeof(tmp));
    if (p.getBytes(PART_PREFS_KEY_BASE, &tmp, sizeof(tmp)) != sizeof(tmp))
    {
      p.end();
      return false;
    }
    p.end();
    machineBase = tmp;
    return true;
  }
  if (len == sizeof(MachineBaseV4))
  {
    MachineBaseV4 old;
    memset(&old, 0, sizeof(old));
    if (p.getBytes(PART_PREFS_KEY_BASE, &old, sizeof(old)) != sizeof(old))
    {
      p.end();
      return false;
    }
    p.end();
    machineBase = upgradeMachineBaseV4(old);
    Serial.println("BASE: migrada v4→v5");
    saveMachineBase();
    return true;
  }
  // Migrar BASE v3/v2 → layout actual.
  if (len == sizeof(MachineBaseV3))
  {
    MachineBaseV3 old;
    memset(&old, 0, sizeof(old));
    if (p.getBytes(PART_PREFS_KEY_BASE, &old, sizeof(old)) != sizeof(old))
    {
      p.end();
      return false;
    }
    p.end();
    machineBase = upgradeMachineBaseV3(old);
    Serial.println("BASE: migrada v3→v5");
    saveMachineBase();
    return true;
  }
  if (len == sizeof(MachineBaseV2))
  {
    MachineBaseV2 old;
    memset(&old, 0, sizeof(old));
    if (p.getBytes(PART_PREFS_KEY_BASE, &old, sizeof(old)) != sizeof(old))
    {
      p.end();
      return false;
    }
    p.end();
    machineBase = upgradeMachineBaseV2(old);
    Serial.println("BASE: migrada v2→v5");
    saveMachineBase();
    return true;
  }
  p.end();
  return false;
}

static String prefeederCfgCsvForSide(const MachineBase& r, uint8_t side)
{
  // Retardo servo→DeReeler es local del PreFeeder (UI/NVS propia); no se envía aquí.
  if (side == PEER_R)
  {
    return String(r.pfAutoRpmR, 1) + "," + String(r.pfAutoReverseSR, 1)
      + "," + String((unsigned)sanitizePfServoPwmUs(r.pfServoPwmUsR)) + "," + String(r.pfMotor2RpmR, 1)
      + "," + String(r.pfTensionCooldownSR, 1) + "," + String(r.pfTensionFaultSR, 1)
      + "," + String(r.pfHolguraExtraSR, 1) + "," + String(r.pfTriggerFeedSR, 1);
  }
  return String(r.pfAutoRpm, 1) + "," + String(r.pfAutoReverseS, 1)
    + "," + String((unsigned)sanitizePfServoPwmUs(r.pfServoPwmUs)) + "," + String(r.pfMotor2Rpm, 1)
    + "," + String(r.pfTensionCooldownS, 1) + "," + String(r.pfTensionFaultS, 1)
    + "," + String(r.pfHolguraExtraS, 1) + "," + String(r.pfTriggerFeedS, 1);
}

// CSV: autoRpm,reverse,servo,m2Rpm,tensCd,tensFault,holguraExtra,triggerFeed
static void applyPrefeederCfgCsvToMirror(uint8_t side, const String& csv)
{
  if (side >= PEER_COUNT || !csv.length()) return;
  float vals[8];
  int n = 0;
  int start = 0;
  while (n < 8 && start <= (int)csv.length())
  {
    int comma = csv.indexOf(',', start);
    String part = (comma < 0) ? csv.substring(start) : csv.substring(start, comma);
    vals[n++] = part.toFloat();
    if (comma < 0) break;
    start = comma + 1;
  }
  if (n < 8) return;
  PrefeederMirror& m = peers[side].mirror;
  m.autoRpm = vals[0];
  m.autoReverseS = vals[1];
  m.servoPwmUs = sanitizePfServoPwmUs((uint16_t)vals[2]);
  m.motor2Rpm = vals[3];
  m.tensionCooldownS = vals[4];
  m.tensionFaultS = vals[5];
  m.holguraExtraFeedS = vals[6];
  m.triggerFeedS = vals[7];
  uiNotify();
}

static bool sendPrefeederRuntimeCfg(const MachineBase& r)
{
  // setAllCfg: también persiste en NVS de cada PreFeeder (sobrevive reinicio).
  const String csvL = prefeederCfgCsvForSide(r, PEER_L);
  const String csvR = prefeederCfgCsvForSide(r, PEER_R);
  const bool okL = peerSendCmd(PEER_L, "setAllCfg", csvL);
  const bool okR = peerSendCmd(PEER_R, "setAllCfg", csvR);
  if (okL) applyPrefeederCfgCsvToMirror(PEER_L, csvL);
  if (okR) applyPrefeederCfgCsvToMirror(PEER_R, csvR);
  syncPrefeederPieceLength();
  return okL && okR;
}

static void syncPrefeederPieceLength()
{
  if (activePartLengthMm == 0) return;
  const String v = String(activePartLengthMm);
  (void)peerSendCmd(PEER_L, "setPieceLength", v);
  (void)peerSendCmd(PEER_R, "setPieceLength", v);
}

static void applyMachineBase(const MachineBase& rIn)
{
  MachineBase r = rIn;
  r.pfServoPwmUs = (uint16_t)PF_SERVO_PWM_L_US;
  r.pfServoPwmUsR = (uint16_t)PF_SERVO_PWM_R_US;
  cutOffsetMm = clampCutOffsetMm(r.cutOffsetMm);
  linearRpm = migrateLinearRpm(r.stepFreqHz);
  dwellAtDestMs = r.dwellAtDestMs;
  D_LINEAR_DONE_MS = r.linearDoneMs;
  feedServoBasePpA = r.feedServoBasePpA;
  feedServoBasePpB = r.feedServoBasePpB;
  D_HOLDER_ON_MS = r.holderOnMs;
  D_HOLDER_OPEN_MS = r.holderOpenMs;
  D_GRIPPERS_ON_MS = r.grippersOnMs;
  D_GRIPPER_RELEASE_MS = r.gripperReleaseMs;
  D_CUTTER_PULSE_MS = r.cutterPulseMs;
  D_CUTTER_POST_MS = r.cutterPostMs;
  asentarDelayMs = r.asentarMs;
  feedParallelStartDelayMs = 0;
  feedMode = clampFeedMode(r.feedMode);
  feedModeThisCycle = feedMode;
  feedTestSolidSteps = r.feedSolidSteps;
  feedTestChunkSteps = r.feedChunkSteps;
  feedTestSolidStepsR = (r.feedSolidStepsR > 0) ? r.feedSolidStepsR : r.feedSolidSteps;
  feedTestChunkStepsR = (r.feedChunkStepsR > 0) ? r.feedChunkStepsR : r.feedChunkSteps;
  feedSsFastPpL = clampFeedSsSpeedPp(r.feedSsFastPpL ? r.feedSsFastPpL : FEED_SS_FAST_PP_DEFAULT);
  feedSsFastPpR = clampFeedSsSpeedPp(r.feedSsFastPpR ? r.feedSsFastPpR : feedSsFastPpL);
  depositBatchSize = r.depositBatchSize;
  depositExtraMm = DEPOSIT_EXTRA_MM_DEFAULT;
  depositInterlaceMm = 0.0f;
  depositBetweenBatchMm = DEPOSIT_BETWEEN_GAP_MM;
  prefeederTriggerEveryN = 1;
  const bool pfHasValues = (r.pfAutoRpm > 0.01f || r.pfMotor2Rpm > 0.01f);
  basePfApplyPending = pfHasValues && !sendPrefeederRuntimeCfg(r);
}

static bool selectActivePart(const String& partNumber)
{
  if (!partCatalogReady()) return false;
  PartModel pr;
  if (!partCatalogLoad(partNumber.c_str(), pr)) return false;
  activePartNumber = partNumber;
  activePartLengthMm = pr.lengthMm;
  syncPrefeederPieceLength();
  (void)partCatalogSaveActive(partNumber.c_str());
  return true;
}

static void initializeMachineBaseAndParts()
{
  machineBaseFromNvs = loadMachineBase();
  if (machineBaseFromNvs)
  {
    applyMachineBase(machineBase);
    Serial.println("BASE: restaurada desde NVS");
  }
  else
  {
    machineBase = captureCurrentMachineBase();
    saveMachineBase();
    Serial.println("BASE: sembrada desde NVS/runtime actual");
  }

  char activePn[PART_NUMBER_MAX_LEN + 1] = {0};
  if (partCatalogReady() && partCatalogLoadActive(activePn, sizeof(activePn)) && activePn[0]
      && selectActivePart(String(activePn)))
    return;
  if (selectActivePart("Prueba"))
    return;
  activePartNumber = "";
  activePartLengthMm = 0;
}

static void persistPrefeederMirrorToBase()
{
  bool any = false;
  if (peers[PEER_L].mirror.peerOk && peers[PEER_L].fullStatusReceived)
  {
    fillPrefeederCfgFromMirror(machineBase, PEER_L);
    any = true;
  }
  if (peers[PEER_R].mirror.peerOk && peers[PEER_R].fullStatusReceived)
  {
    fillPrefeederCfgFromMirror(machineBase, PEER_R);
    any = true;
  }
  if (!any) return;
  machineBase.pfServoPwmUs = (uint16_t)PF_SERVO_PWM_L_US;
  machineBase.pfServoPwmUsR = (uint16_t)PF_SERVO_PWM_R_US;
  machineBasePrefeederCaptured = true;
  saveMachineBase();
}

// Tras setAllCfg local: el espejo ya tiene los valores; forzar BASE aunque lastRx sea viejo.
static void commitPrefeederMirrorSideToBase(uint8_t side)
{
  if (side >= PEER_COUNT) return;
  fillPrefeederCfgFromMirror(machineBase, side);
  machineBase.pfServoPwmUs = (uint16_t)PF_SERVO_PWM_L_US;
  machineBase.pfServoPwmUsR = (uint16_t)PF_SERVO_PWM_R_US;
  machineBasePrefeederCaptured = true;
  saveMachineBase();
}

static void beginSlaveUiHold(const char* kind)
{
  if (!kind) kind = "";
  if (strcmp(kind, "asda") == 0 || strcmp(kind, "linear") == 0)
  {
    // Calibración/rpm viven en el esclavo; refrescar espejo TCM y no tocar RPM de receta.
    asdaSyncConfig();
    Serial.println("SLAVE-UI: hold ASDA (sync cal)");
    return;
  }
  // PreFeeder L/R: pausar apply BASE→esclavo y absorber cambios del operador.
  pfSlaveUiHoldUntilMs = millis() + PF_SLAVE_UI_HOLD_MS;
  basePfApplyPending = false;
  Serial.printf("SLAVE-UI: hold PreFeeder %lu ms\n", (unsigned long)PF_SLAVE_UI_HOLD_MS);
}

static void serviceMachineBasePrefeeder()
{
  if (pfSlaveUiHoldUntilMs)
  {
    if ((int32_t)(millis() - pfSlaveUiHoldUntilMs) >= 0)
    {
      pfSlaveUiHoldUntilMs = 0;
      persistPrefeederMirrorToBase();
      Serial.println("SLAVE-UI: hold PF expirado → BASE actualizada desde espejo");
    }
    else
    {
      // Mientras el operador edita en UI nativa, nunca empujar BASE vieja.
      basePfApplyPending = false;
      return;
    }
  }

  const PeerSlot& pL = peers[PEER_L];
  const PeerSlot& pR = peers[PEER_R];
  if (!pL.mirror.peerOk || !pL.fullStatusReceived || !pL.lastRxMs || cycleActive || cycleRunning) return;
  if (!machineBasePrefeederCaptured)
  {
    const bool basePfEmpty = (machineBase.pfAutoRpm == 0.0f
                              && machineBase.pfMotor2Rpm == 0.0f);
    if (!machineBaseFromNvs || basePfEmpty)
    {
      fillPrefeederCfgFromMirror(machineBase, PEER_L);
      if (pR.mirror.peerOk && pR.fullStatusReceived)
        fillPrefeederCfgFromMirror(machineBase, PEER_R);
      else
        copyPrefeederCfgSideLtoR(machineBase);
      machineBase.pfServoPwmUs = (uint16_t)PF_SERVO_PWM_L_US;
      machineBase.pfServoPwmUsR = (uint16_t)PF_SERVO_PWM_R_US;
      saveMachineBase();
      machineBasePrefeederCaptured = true;
    }
    else
    {
      if (pR.mirror.peerOk && pR.fullStatusReceived
          && machineBase.pfAutoRpmR == 0.0f
          && machineBase.pfMotor2RpmR == 0.0f)
      {
        fillPrefeederCfgFromMirror(machineBase, PEER_R);
        saveMachineBase();
      }
      machineBasePrefeederCaptured = true;
      basePfApplyPending = true;
    }
  }
  else if (pR.mirror.peerOk && pR.fullStatusReceived
           && machineBase.pfAutoRpmR == 0.0f
           && machineBase.pfMotor2RpmR == 0.0f)
  {
    fillPrefeederCfgFromMirror(machineBase, PEER_R);
    saveMachineBase();
  }
  if (basePfApplyPending)
  {
    const uint32_t now = millis();
    if (now - basePfApplyLastMs < BASE_PF_APPLY_RETRY_MS) return;
    basePfApplyLastMs = now;
    basePfApplyPending = !sendPrefeederRuntimeCfg(machineBase);
  }
}

static void saveFeedTolerancePctToNvs()
{
  prefs.begin(PREFS_NS, false);
  prefs.putInt(PREFS_KEY_TOLERANCE_PCT, (int)feedStepsTolerancePercent);
  prefs.end();
}

void prepareServoFeeder()
{
  feedModeThisCycle = feedMode;
  feedPhase = FEED_IDLE;
  feedCalibrationTest = false;
  DBG_PRINT("Servo feeder: ");
  DBG_PRINTLN(String("Feed L target ") + feedTestSolidSteps + " vel "
      + feedPpToMmS(feedSsFastPpL, false) + " mm/s | R target " + feedTestSolidStepsR
      + " vel " + feedPpToMmS(feedSsFastPpR, true) + " mm/s");
}

// ============================================================
// SECCION 07 — Log
// ============================================================
static void pushLog(const String& s)
{
  if (!s.length()) return;
  Serial.println(s);
  lastSystemMsg = s;
  if (lastSystemMsg.length() > 160)
    lastSystemMsg = lastSystemMsg.substring(0, 160);
  lastSystemMsgMs = millis();
  uiNotify();
}

// ============================================================
// SECCION 08 — CAN — RX / TX / INIT
// ============================================================
static bool isServoOperationEnabled(uint16_t statusWord)
{
  if (statusWord & 0x0008) return false;
  return (statusWord & 0x006F) == 0x0027;
}

// Event-driven: "online" = ya hubo RX y no hay offline enclavado (sin heartbeat).
static bool sensorCanOnline()
{
  return sensorEverOnline && !sensorStaleLatched;
}

static uint8_t sensorCodeFromBitmask(uint8_t mask)
{
  if (mask & SENSOR_BIT_GRIPPER) return ERR_SENSOR_GRIPPER;
  if (mask & SENSOR_BIT_HOLDER)  return ERR_SENSOR_HOLDER;
  if (mask & SENSOR_BIT_CUTTER)  return ERR_SENSOR_CUTTER;
  if (mask & SENSOR_BITS_HOSE)   return ERR_SENSOR_HOSE;
  if (mask & SENSOR_BIT_TRAY)    return ERR_SENSOR_TRAY;
  return ERR_NONE;
}

static uint8_t sensorBitFromAlertCode(uint8_t code)
{
  switch (code)
  {
    case ERR_SENSOR_GRIPPER: return SENSOR_BIT_GRIPPER;
    case ERR_SENSOR_HOLDER:  return SENSOR_BIT_HOLDER;
    case ERR_SENSOR_CUTTER:  return SENSOR_BIT_CUTTER;
    case ERR_SENSOR_HOSE:    return SENSOR_BITS_HOSE;
    case ERR_SENSOR_TRAY:    return SENSOR_BIT_TRAY;
    default: return 0;
  }
}

static uint8_t pfErrorCodeFromReason(uint8_t side, const String& reason)
{
  const uint8_t base = (side == PEER_R) ? ERR_PF_R_OFFLINE : ERR_PF_L_OFFLINE;
  if (reason == "endstop") return (uint8_t)(base + 1);
  if (reason == "tension_timeout") return (uint8_t)(base + 2);
  if (reason == "cylinder_open") return (uint8_t)(base + 3);
  if (reason == "hose_absent") return (uint8_t)(base + 4);
  if (reason == "buffer_timeout") return (uint8_t)(base + 5);
  if (reason == "holgura_timeout") return (uint8_t)(base + 6);
  if (reason == "operator_stop") return (uint8_t)(base + 7);
  return base;
}

static uint8_t cycleFaultErrorCode(const char* reason)
{
  if (!reason || !reason[0]) return ERR_NONE;
  if (strcmp(reason, "lineal_timeout") == 0) return ERR_CYCLE_LINEAL_TIMEOUT;
  if (strcmp(reason, "feed_timeout") == 0) return ERR_CYCLE_FEED_TIMEOUT;
  if (strcmp(reason, "feed_incomplete") == 0) return ERR_CYCLE_ABORT;  // catálogo 12
  if (strcmp(reason, "abort") == 0) return ERR_CYCLE_ABORT;
  if (strcmp(reason, "safety_stop") == 0) return ERR_CYCLE_SAFETY_STOP;
  if (strcmp(reason, "prefeeder_not_ready") == 0) return ERR_CYCLE_PF_NOT_READY;
  if (strcmp(reason, "servo_can") == 0) return ERR_SERVO_CAN;
  if (strcmp(reason, "feed_hose") == 0) return ERR_FEED_HOSE;
  // Holgura / settle: no son E014 (solo “PreFeeder no inicializado”).
  if (strcmp(reason, "prefeeder_holgura") == 0) return ERR_NONE;
  if (strcmp(reason, "prefeeder_all_ok") == 0) return ERR_CYCLE_SAFETY_STOP;
  if (strcmp(reason, "prefeeder_settle") == 0) return ERR_NONE;
  // GPIO feed: timeout sin detección de manguera (antes caía en feed_timeout genérico).
  if (strstr(reason, "sensor no detectado") != nullptr) return ERR_FEED_HOSE;
  if (strstr(reason, "timeout") != nullptr) return ERR_CYCLE_FEED_TIMEOUT;
  return ERR_NONE;
}

// CycleErrorCode (E010…) → código unificado del catálogo UI (ERR_*).
static uint8_t catalogCodeFromCycleError(uint16_t code)
{
  switch ((CycleErrorCode)code)
  {
    case E008: return ERR_SERVO_CAN;
    case E010: return ERR_CYCLE_LINEAL_TIMEOUT;
    case E011: return ERR_CYCLE_FEED_TIMEOUT;
    case E012: return ERR_CYCLE_ABORT;
    case E016: return ERR_FEED_HOSE;
    case E020: return ERR_CYCLE_SAFETY_STOP;       // genérico tras paso
    case E021: return ERR_SENSOR_GRIPPER;
    case E022: return ERR_SENSOR_HOLDER;
    case E023: return ERR_SENSOR_CUTTER;
    case E024: return ERR_SENSOR_HOSE;
    case E025: return ERR_SENSOR_TRAY;
    case E026: return ERR_CYCLE_SAFETY_STOP;       // parada externa / PF
    case E027: return ERR_SENSOR_CAN_OFFLINE;
    case E030: return ERR_CYCLE_PF_NOT_READY;
    case E031: return ERR_CYCLE_PEER_LOST;
    case E099: return ERR_UNCLASSIFIED;
    default:   return ERR_NONE;
  }
}

static void errPushUnique(uint8_t* out, uint8_t& n, uint8_t maxOut, uint8_t code)
{
  if (code == ERR_NONE || n >= maxOut) return;
  for (uint8_t i = 0; i < n; i++)
  {
    if (out[i] == code) return;
  }
  out[n++] = code;
}

static bool peerSideLiveOk(uint8_t s)
{
  if (s >= PEER_COUNT) return false;
  PeerSlot& p = peers[s];
  return p.client.connected()
         && p.fullStatusReceived
         && p.lastRxMs
         && (millis() - p.lastRxMs <= PEER_STALE_MS);
}

// Enlace malo para torre/UI/interlock: prioriza enclavado.
// Tras grace, un peer nunca OK también cuenta.
static bool peerSideFaulted(uint8_t s)
{
  if (s >= PEER_COUNT) return true;
  if (peerOfflineLatched[s]) return true;
  if (peerSideLiveOk(s)) return false;
  if (peerEverOk[s]) return true;  // mismo tick antes de enclavar offline
  return !towerInBootGrace();
}

static uint8_t peerLiveErrorCode(uint8_t s)
{
  if (s >= PEER_COUNT) return ERR_NONE;
  PeerSlot& p = peers[s];
  if (!p.mirror.error || p.mirror.idleMode) return ERR_NONE;
  uint8_t ec = p.mirror.errorCode;
  const uint8_t offlineCode = (s == PEER_R) ? ERR_PF_R_OFFLINE : ERR_PF_L_OFFLINE;
  if (ec == ERR_NONE || ec == ERR_EXTERNAL_PREFEEDER || ec == offlineCode)
    ec = pfErrorCodeFromReason(s, p.mirror.errorReason);
  return ec;
}

static void clearPeerFaultLatches()
{
  for (uint8_t s = 0; s < PEER_COUNT; s++)
  {
    peerOfflineLatched[s] = false;
    peerErrorLatchedCode[s] = ERR_NONE;
  }
}

// Enclava offline (tras haber estado OK) y fallas PF reportadas. Solo Reset libera.
static void servicePeerFaultLatches()
{
  if (towerInBootGrace()) return;
  bool changed = false;
  for (uint8_t s = 0; s < PEER_COUNT; s++)
  {
    const bool sideOk = peerSideLiveOk(s);
    if (sideOk)
    {
      if (!peerEverOk[s])
      {
        peerEverOk[s] = true;
        changed = true;
      }
      const uint8_t liveEc = peerLiveErrorCode(s);
      if (liveEc != ERR_NONE && peerErrorLatchedCode[s] == ERR_NONE)
      {
        peerErrorLatchedCode[s] = liveEc;
        changed = true;
        Serial.printf("PEER %s: falla enclavada E%03u\n",
                      PEER_TAGS[s], (unsigned)liveEc);
        pushLog(String("PEER ") + PEER_TAGS[s] + " falla enclavada E"
                + String((unsigned)liveEc));
      }
    }
    else if (peerEverOk[s] && !peerOfflineLatched[s])
    {
      peerOfflineLatched[s] = true;
      changed = true;
      Serial.printf("PEER %s: enlace perdido — offline enclavado\n", PEER_TAGS[s]);
      pushLog(String("PEER ") + PEER_TAGS[s] + " offline enclavado");
    }
  }
  if (changed)
  {
    towerForceResync = true;
    uiNotify();
  }
}

// Recoge todos los códigos activos (sensores, ciclo, PreFeeder L/R).
static uint8_t collectActiveErrorCodes(uint8_t* out, uint8_t maxOut)
{
  uint8_t n = 0;
  if (!out || maxOut == 0) return 0;

  if (sensorStaleLatched || !sensorEverOnline)
    errPushUnique(out, n, maxOut, ERR_SENSOR_CAN_OFFLINE);

  // Servo feeder CAN (ASDA): distinto del módulo sensores (código 7).
  if (!towerInBootGrace() && !servoCanReady)
    errPushUnique(out, n, maxOut, ERR_SERVO_CAN);

  if (sensorLatchedBitmask & SENSOR_BIT_GRIPPER)
    errPushUnique(out, n, maxOut, ERR_SENSOR_GRIPPER);
  if (sensorLatchedBitmask & SENSOR_BIT_HOLDER)
    errPushUnique(out, n, maxOut, ERR_SENSOR_HOLDER);
  if (sensorLatchedBitmask & SENSOR_BIT_CUTTER)
    errPushUnique(out, n, maxOut, ERR_SENSOR_CUTTER);
  if (sensorLatchedBitmask & SENSOR_BITS_HOSE)
    errPushUnique(out, n, maxOut, ERR_SENSOR_HOSE);
  if (sensorLatchedBitmask & SENSOR_BIT_TRAY)
    errPushUnique(out, n, maxOut, ERR_SENSOR_TRAY);

  {
    uint8_t faultCode = catalogCodeFromCycleError(cycleErrorCode);
    // E012 genérico: si el detalle es sensor manguera GPIO, preferir código 16.
    if (faultCode == ERR_CYCLE_ABORT && cycleFaultReason[0]
        && cycleFaultErrorCode(cycleFaultReason) == ERR_FEED_HOSE)
      faultCode = ERR_FEED_HOSE;
    if (faultCode == ERR_NONE && cycleFaultReason[0])
      faultCode = cycleFaultErrorCode(cycleFaultReason);
    if (faultCode != ERR_NONE)
      errPushUnique(out, n, maxOut, faultCode);
  }

  for (uint8_t s = 0; s < PEER_COUNT; s++)
  {
    const uint8_t offlineCode = (s == PEER_R) ? ERR_PF_R_OFFLINE : ERR_PF_L_OFFLINE;
    if (peerSideFaulted(s))
    {
      errPushUnique(out, n, maxOut, offlineCode);
      // Offline no oculta falla PF ya enclavada.
      if (peerErrorLatchedCode[s] != ERR_NONE)
        errPushUnique(out, n, maxOut, peerErrorLatchedCode[s]);
      continue;
    }
    uint8_t ec = peerErrorLatchedCode[s];
    if (ec == ERR_NONE)
      ec = peerLiveErrorCode(s);
    errPushUnique(out, n, maxOut, ec);
  }

  return n;
}

static void refreshSafetyErrorCode()
{
  if (externalStopInputActive())
  {
    safetyErrorCode = SAFETY_ERROR_EXTERNAL;
    return;
  }
  if (sensorStaleLatched || !sensorEverOnline)
  {
    safetyErrorCode = SAFETY_ERROR_SENSOR_OFFLINE;
    return;
  }
  uint8_t fromMask = sensorCodeFromBitmask(sensorLatchedBitmask);
  safetyErrorCode = fromMask;
}

static void resetSensorLatches()
{
  // Solo libera fallas cuya entrada física ya regresó a estado seguro.
  // Si una entrada continúa activa, permanece enclavada en rojo.
  uint8_t before = sensorLatchedBitmask;
  sensorLatchedBitmask &= sensorBitmask;
  sensorPendingBits = 0;
  sensorPendingSinceMs = 0;
  // Offline enclavado solo se limpia aquí tras poll OK (resetSensorLatchesAfterFreshSnapshot).
  refreshSafetyErrorCode();

  if (before != sensorLatchedBitmask)
  {
    Serial.print("RESET/VDD: sensores liberados mask 0x");
    Serial.print(before, HEX);
    Serial.print(" -> 0x");
    Serial.println(sensorLatchedBitmask, HEX);
    pushLog("RESET/VDD: enclavamiento sensores actualizado");
    uiNotify();
  }
}

static bool requestSensorSnapshotOnce(uint32_t timeoutMs)
{
  if (!canInitialized) return false;
  serviceCANRx();
  const uint32_t startRx = sensorLastRxMs;
  uint8_t data[1] = { SENSOR_POLL_MAGIC };

  bool txOk = false;
  for (uint8_t t = 0; t < SENSOR_POLL_TX_RETRIES; t++)
  {
    if (sendCANMessage(CAN_ID_TOWER_CMD, 1, data, "SensorPoll", false))
    {
      txOk = true;
      break;
    }
    serviceCANRx();
    delay(2);
  }
  if (!txOk) return false;

  const uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs)
  {
    serviceCANRx();
    // Cualquier frame 0xC0 (snapshot o alerta) = módulo en línea.
    if (sensorLastRxMs != startRx)
      return true;
    delay(2);
  }
  return false;
}

static bool pollSensorModuleEx(uint8_t retries, uint32_t timeoutMs)
{
  if (retries == 0) retries = 1;
  for (uint8_t i = 0; i < retries; i++)
  {
    if (requestSensorSnapshotOnce(timeoutMs))
    {
      sensorPollFailStreak = 0;
      return true;
    }
  }
  return false;
}

static bool pollSensorModule()
{
  return pollSensorModuleEx(SENSOR_POLL_RETRIES, SENSOR_POLL_TIMEOUT_MS);
}

static bool sensorHasRecentRx(uint32_t windowMs)
{
  if (!sensorEverOnline) return false;
  return (millis() - sensorLastRxMs) < windowMs;
}

static void clearSensorOfflineLatch(const char* why)
{
  sensorPollFailStreak = 0;
  if (!sensorStaleLatched) return;
  sensorStaleLatched = false;
  if (why && why[0])
  {
    Serial.println(why);
    pushLog(why);
  }
}

// Discovery periódico: boot sin primer 0xC0, o bus recuperado tras E007 (el enclavamiento
// offline sigue exigiendo Reset; aquí solo probamos el enlace y reseteamos el streak).
static void serviceSensorCanDiscovery()
{
  if (!canInitialized) return;
  // Online y sin enclavamiento: nada que descubrir.
  if (sensorCanOnline()) return;

  static uint32_t lastDiscoveryMs = 0;
  const uint32_t now = millis();
  if (lastDiscoveryMs != 0 && (now - lastDiscoveryMs) < (uint32_t)SENSOR_DISCOVERY_PERIOD_MS)
    return;
  lastDiscoveryMs = now;

  if (requestSensorSnapshotOnce(250))
  {
    sensorPollFailStreak = 0;
    // Si solo faltaba el primer snapshot de arranque, E007 de UI se limpia solo.
    refreshSafetyErrorCode();
    uiNotify();
  }
}

static void latchSensorOffline(const char* reason)
{
  if (sensorStaleLatched) return;
  sensorStaleLatched = true;
  safetyStopPending = true;
  refreshSafetyErrorCode();
  if (cycleActive && cycleErrorCode == E000)
    setCycleError(E027, reason);  // Sensores CAN offline
  Serial.println(reason);
  pushLog(reason);
  uiNotify();
}

static void noteSensorPollFailure(const char* reason)
{
  // Tráfico 0xC0 reciente: el módulo está vivo; no acumular hacia E007.
  if (sensorHasRecentRx())
  {
    Serial.printf("SENSORS CAN: poll falló con RX reciente (%lu ms) — streak sin cambio\n",
                  (unsigned long)(millis() - sensorLastRxMs));
    return;
  }

  sensorPollFailStreak++;
  if (sensorPollFailStreak < SENSOR_POLL_FAIL_LATCH_STREAK)
  {
    Serial.printf("SENSORS CAN: poll falló (%u/%u) — sin enclavar aún\n",
                  (unsigned)sensorPollFailStreak,
                  (unsigned)SENSOR_POLL_FAIL_LATCH_STREAK);
    return;
  }
  latchSensorOffline(reason);
}

// Confirma candidatos tras debounce. force=true en Start / fin de pieza (sin poll).
static void serviceSafetyDebounce(bool force)
{
  if (!sensorPendingBits) return;
  const uint32_t now = millis();
  if (!force && (now - sensorPendingSinceMs) < SAFETY_FAULT_DEBOUNCE_MS)
    return;

  uint8_t confirm = (uint8_t)(sensorPendingBits & sensorBitmask);
  sensorPendingBits = 0;
  sensorPendingSinceMs = 0;
  if (!confirm) return;

  uint8_t before = sensorLatchedBitmask;
  sensorLatchedBitmask = (uint8_t)(sensorLatchedBitmask | confirm);
  if (sensorLatchedBitmask == before) return;

  safetyStopPending = true;
  refreshSafetyErrorCode();
  uint8_t code = sensorCodeFromBitmask(confirm);
  const char* errName[] = { "", "GRIPPER", "HOLDER", "CUTTER", "MANGUERA", "TRAY" };
  String msg = String("ALERTA SEGURIDAD (debounce): ") +
               ((code >= 1 && code <= 5) ? errName[code] : "SENSOR");
  if (cycleActive)
    msg += " — safety stop al terminar el paso";
  Serial.println(msg);
  pushLog(msg);
  uiNotify();
}

static void noteSensorFaultCandidates(uint8_t bits)
{
  bits = (uint8_t)(bits & ~sensorLatchedBitmask);
  if (!bits) return;
  if (!(sensorPendingBits & bits) || !sensorPendingBits)
    sensorPendingSinceMs = millis();
  sensorPendingBits = (uint8_t)(sensorPendingBits | bits);
}

// Reset de PreFeeder L/R (TCP cmd "reset"): misma acción que el botón Reset del PF.
// Se intenta en ambos lados aunque el enlace esté flojo (un reintento inmediato).
static void resetPrefeederFaults()
{
  uint8_t okN = 0;
  for (uint8_t s = 0; s < PEER_COUNT; s++)
  {
    bool ok = peerSendCmdEx(s, "reset", "", 0);
    if (!ok)
    {
      delay(20);
      ok = peerSendCmdEx(s, "reset", "", 0);
    }
    if (ok) okN++;
    Serial.printf("RESET: PreFeeder %s %s\n", PEER_TAGS[s], ok ? "OK" : "sin enlace");
  }
  if (okN)
    pushLog(String("RESET: PreFeeder clear fault (") + String(okN) + "/2)");
  else
    pushLog("RESET: PreFeeder sin enlace L/R");
}

// El pulso de RESET del PLC limpia físicamente los sensores (Hose A/B, etc.).
// Poll fresco tras reset: libera bits seguros; offline se libera si poll OK o RX reciente.
// Si el poll falla sin RX reciente, NO re-enclavar de más: reinit MCP y reintentar.
// También limpia Exxx de ciclo y pide reset de fallas a PreFeeder L/R (sin HOME).
static void resetSensorLatchesAfterFreshSnapshot()
{
  sensorPendingBits = 0;
  sensorPendingSinceMs = 0;

  bool ok = pollSensorModule();
  if (!ok)
  {
    // Bus/MCP a veces queda sordo tras SDO: reinit ligero (sin re-setup de servos).
    Serial.println("RESET: poll falló — reinit MCP2515 y reintento");
    pushLog("RESET: reinit MCP2515 (poll falló)");
    if (reconnectCANBus(false))
    {
      delay(30);
      serviceCANRx();
      ok = pollSensorModule();
    }
  }

  if (ok || sensorHasRecentRx())
  {
    clearSensorOfflineLatch(ok
      ? "RESET: sensores CAN online — offline liberado"
      : "RESET: RX reciente — offline liberado (poll perdido)");
    resetSensorLatches();
    safetyStopPending = (sensorLatchedBitmask != 0);
  }
  else
  {
    // Antes: latchSensorOffline aquí hacía que Reset "funcionara" y E007 volviera al instante.
    if (!sensorStaleLatched)
      latchSensorOffline("RESET: sin respuesta poll sensores CAN — offline enclavado");
    else
    {
      Serial.println("RESET: sin respuesta CAN — offline sigue enclavado (revisar módulo/bus)");
      pushLog("RESET: sin respuesta CAN — offline sigue enclavado");
    }
    resetSensorLatches();  // libera bits físicos OK si había snapshot previo
  }
  // Reset PLC: quitar código Exxx de la UI (además de liberar sensores).
  setCycleError(E000);
  setCycleFaultReason("");
  clearPeerFaultLatches();
  resetPrefeederFaults();
  // Cortar settle/relleno residual (servo/DeReeler no deben seguir tras Reset).
  prefeederHardStopDisarm("Reset");
  refreshSafetyErrorCode();
  // Reenviar modo de torre (si un TX OK falló, el módulo podía quedar en rojo con UI OK).
  towerForceResync = true;
  uiNotify();
}

static void processSafetySnapshotRx(unsigned char len, unsigned char* rxBuf)
{
  if (len < 3) return;
  uint8_t mask = rxBuf[1];
  uint8_t seq = rxBuf[2];
  uint8_t prevMask = sensorBitmask;
  bool wasOnline = sensorCanOnline();

  sensorBitmask = mask;
  sensorSequence = seq;
  sensorLastRxMs = millis();
  sensorEverOnline = true;
  // No limpiar sensorStaleLatched aquí: solo RESET (enclavamiento).

  // Módulo (re)apareció: reafirmar torre (pudo reiniciarse en OFF mientras TCM creía OK).
  if (!wasOnline)
    towerForceResync = true;

  // Boot: actualizar link CAN pero no enclavar/reaccionar (ruido al energizar).
  if (towerInBootGrace())
  {
    // Cancela candidatos que ya no están activos.
    sensorPendingBits = (uint8_t)(sensorPendingBits & mask);
    refreshSafetyErrorCode();
    if (mask != prevMask || !wasOnline) uiNotify();
    return;
  }

  sensorPendingBits = (uint8_t)(sensorPendingBits & mask);  // glitch corto → no confirmar
  uint8_t risen = (uint8_t)(mask & ~prevMask);
  if (risen)
    noteSensorFaultCandidates(risen);
  // Bits ya altos en snapshot (p. ej. tras poll) también deben poder enclavar.
  noteSensorFaultCandidates(mask);

  refreshSafetyErrorCode();
  if (mask != prevMask || !wasOnline) uiNotify();
}

static void processSafetyAlertRx(unsigned long rxId, unsigned char len, unsigned char* rxBuf)
{
  if (rxId != CAN_ID_SAFETY_ALERT || len < 2) return;

  if (rxBuf[0] == SENSOR_SNAP_MAGIC)
  {
    processSafetySnapshotRx(len, rxBuf);
    return;
  }
  if (rxBuf[0] != SENSOR_ALERT_MAGIC) return;

  uint8_t code = rxBuf[1];
  if (code < 1 || code > 5) return;

  // Boot: ignorar alertas puntuales (el snapshot post-grace / poll enclava si sigue activo).
  if (towerInBootGrace())
  {
    sensorLastRxMs = millis();
    sensorEverOnline = true;
    return;
  }

  uint8_t alertBits = sensorBitFromAlertCode(code);
  sensorBitmask = (uint8_t)(sensorBitmask | alertBits);
  if (code == 4 && (sensorBitmask & SENSOR_BITS_HOSE))
    alertBits = (uint8_t)(sensorBitmask & SENSOR_BITS_HOSE);
  sensorLastRxMs = millis();
  sensorEverOnline = true;
  noteSensorFaultCandidates(alertBits);
  refreshSafetyErrorCode();
  uiNotify();
}

bool externalStopInputActive()
{
  // Interlock TCP L+R: enlace enclavado/faltante estable, y falla PF viva O enclavada.
  for (uint8_t s = 0; s < PEER_COUNT; s++)
  {
    if (peerSideFaulted(s)) return true;
    if (peerErrorLatchedCode[s] != ERR_NONE) return true;
    if (peerLiveErrorCode(s) != ERR_NONE) return true;
  }
  return false;
}

bool safetyAllowsRun()
{
  if (externalStopInputActive()) return false;
  if (!sensorCanOnline()) return false;
  if (sensorLatchedBitmask != 0) return false;
  if (sensorPendingBits != 0) return false;
  return true;
}

static bool canBusMotionBlocked()
{
  // Test de alimentación (engineering): no silenciar 607A/6040 por interlock PF.
  if (feedCalibrationTest) return false;
  // El inicio de ciclo valida todos los sensores en safetyAllowsRun().
  // Aquí solo se bloquea la parada externa para permitir HOME/Safe de recuperación.
  return externalStopInputActive();
}

void serviceExternalStopInput()
{
  static bool prevActive = false;
  static bool prevPeerOk = true;
  static bool prevPfError = false;
  static bool prevPfIdle = false;

  servicePeerFaultLatches();

  bool peerOk = true;
  bool pfError = false;
  bool pfIdle = false;
  for (uint8_t s = 0; s < PEER_COUNT; s++)
  {
    if (peerSideFaulted(s)) peerOk = false;
    // Idle: no tratar error vivo de ese lado como parada; latched sí cuenta.
    if (peerErrorLatchedCode[s] != ERR_NONE || peerLiveErrorCode(s) != ERR_NONE)
      pfError = true;
    if (peers[s].mirror.idleMode) pfIdle = true;
  }
  const bool active = !peerOk || pfError;

  // Idle mode PreFeeder (refill): pausa ciclo activo; start/resume ya validan aparte.
  if (pfIdle && cycleActive)
  {
    if (!prevPfIdle || !cyclePaused)
    {
      immediatePhysicalStop();
      enterCyclePaused("PreFeeder Materialista");
      safetyErrorCode = SAFETY_ERROR_EXTERNAL;
    }
    else
      enterCyclePaused("PreFeeder Materialista");
  }

  // Falla enclavada del PreFeeder: pausa inmediata (seguridad).
  if (pfError)
  {
    if (!prevPfError || safetyErrorCode != SAFETY_ERROR_EXTERNAL)
    {
      safetyErrorCode = SAFETY_ERROR_EXTERNAL;
      if (cycleActive)
        immediatePhysicalStop();
      enterCyclePaused("PreFeeder en falla");
      String sides;
      for (uint8_t s = 0; s < PEER_COUNT; s++)
      {
        if (peerErrorLatchedCode[s] == ERR_NONE && peerLiveErrorCode(s) == ERR_NONE)
          continue;
        if (sides.length()) sides += '+';
        sides += PEER_TAGS[s];
        if (peers[s].mirror.errorReason.length()
            && peers[s].mirror.errorReason != "none")
        {
          sides += '(';
          sides += peers[s].mirror.errorReason;
          sides += ')';
        }
      }
      Serial.printf("PARADA EXTERNA: PreFeeder %s en falla — parada inmediata, ciclo pausado\n",
                    sides.length() ? sides.c_str() : "?");
      pushLog(String("PARADA EXTERNA PreFeeder ") + (sides.length() ? sides : "?")
              + " en falla — parada inmediata");
    }
    else
      enterCyclePaused("PreFeeder en falla");
  }
  // Pérdida de enlace: no cortar mid-pieza; en HOME se mira el status (sin ping).
  else if (!peerOk)
  {
    if (cycleActive && !cyclePaused)
      peerCommSuspect = true;
    if (!prevActive || safetyErrorCode != SAFETY_ERROR_EXTERNAL)
      safetyErrorCode = SAFETY_ERROR_EXTERNAL;
    if (prevPeerOk)
    {
      Serial.println("PEER: enlace TCP sospechoso — se revisará status en HOME");
      pushLog("PEER: enlace TCP sospechoso (status HOME)");
    }
  }
  else if (prevActive)
  {
    // Link recuperado y sin falla viva: latches pueden seguir hasta Reset.
    // Si aún hay latch, active sigue true y no entramos aquí.
    Serial.println("Interlock PreFeeder liberado (TCP OK L+R, sin falla) — listo para reanudar");
    pushLog("Interlock PreFeeder liberado — listo para reanudar");
    refreshSafetyErrorCode();
    // Reenviar modo de torre (OK): un TX previo pudo perderse en el bus.
    towerForceResync = true;
  }

  prevActive = active;
  prevPeerOk = peerOk;
  prevPfError = pfError;
  prevPfIdle = pfIdle;

  // Debounce de fallas sensor (event-driven). Offline solo se enclava en poll Start/fin pieza.
  serviceSafetyDebounce(false);
  serviceSensorCanDiscovery();
  if (!sensorEverOnline && !sensorStaleLatched)
    safetyErrorCode = SAFETY_ERROR_SENSOR_OFFLINE;
  else
    refreshSafetyErrorCode();
}

// Torre andon (CAN 0xC1): Error > Materialista > OK_DONE > OK.
// OK: Verde. Materialista (ex-Idle PF): Naranja+buzzer. Error: Rojo+buzzer.
// OK_DONE: Verde+buzzer 6 s (fin lote).
// Boot: HOLD (sin TX) solo hasta que el módulo sensor esté online; peers no bloquean el verde inicial.
// Heartbeat TOWER_HEARTBEAT_MS: TX OK del MCP2515 no implica RX en el módulo.
// Mantener OK_DONE en el bus durante TOWER_OK_DONE_MS: si se TX OK al instante,
// el módulo sensor cancela el buzzer (queda verde sin aviso).
static volatile bool towerLotCompletePulse = false;
static uint32_t towerLotCompleteUntilMs = 0;
static uint32_t towerBootGraceUntilMs = 0;  // se arma al terminar setup()

static void towerNotifyLotComplete()
{
  towerLotCompletePulse = true;
  towerLotCompleteUntilMs = millis() + TOWER_OK_DONE_MS;
}

static bool towerLotCompleteActive()
{
  if (!towerLotCompletePulse) return false;
  if ((int32_t)(millis() - towerLotCompleteUntilMs) >= 0)
  {
    towerLotCompletePulse = false;
    return false;
  }
  return true;
}

static bool towerInBootGrace()
{
  return towerBootGraceUntilMs != 0 && (int32_t)(millis() - towerBootGraceUntilMs) < 0;
}

static void towerPeerFlags(bool& peerOk, bool& pfError, bool& pfMaterialista)
{
  peerOk = true;
  pfError = false;
  pfMaterialista = false;
  for (uint8_t s = 0; s < PEER_COUNT; s++)
  {
    PeerSlot& p = peers[s];
    if (peerSideFaulted(s)) peerOk = false;
    if (p.mirror.idleMode) pfMaterialista = true;
    if (peerErrorLatchedCode[s] != ERR_NONE || peerLiveErrorCode(s) != ERR_NONE)
      pfError = true;
  }
}

static uint8_t computeTowerMode()
{
  const bool bootGrace = towerInBootGrace();

  bool peerOk = true;
  bool pfError = false;
  bool pfMaterialista = false;
  towerPeerFlags(peerOk, pfError, pfMaterialista);

  // Arranque: HOLD solo si el módulo sensor aún no está online.
  if (bootGrace)
  {
    if (!sensorEverOnline || !sensorCanOnline())
      return TOWER_MODE_HOLD;
    if (sensorLatchedBitmask != 0)
      return TOWER_MODE_HOLD;  // no alarmar en grace; el post-grace enclava
    if (pfMaterialista)
      return TOWER_MODE_MATERIALISTA;
    if (towerLotCompleteActive())
      return TOWER_MODE_OK_DONE;
    return TOWER_MODE_OK;
  }

  if (sensorLatchedBitmask != 0)
    return TOWER_MODE_ERROR;
  if (!sensorEverOnline || !sensorCanOnline())
    return TOWER_MODE_ERROR;
  // Servo feeder CAN no energizado / sin SDO → rojo (código 8).
  if (!servoCanReady)
    return TOWER_MODE_ERROR;
  // Fallas de ciclo (timeout lineal/feed, E030, abort, etc.): rojo hasta Reset.
  if (cycleErrorCode != E000 && cycleErrorCode != E001)
    return TOWER_MODE_ERROR;
  // Prioridad: ERROR > Materialista.
  if (pfError || !peerOk)
    return TOWER_MODE_ERROR;
  if (pfMaterialista)
    return TOWER_MODE_MATERIALISTA;
  if (towerLotCompleteActive())
    return TOWER_MODE_OK_DONE;
  return TOWER_MODE_OK;
}

static void saveTcmBuzzerMuteToNvs()
{
  prefs.begin(PREFS_NS, false);
  prefs.putBool(PREFS_KEY_BUZZER_MUTE, tcmBuzzerMuted);
  prefs.end();
}

static void saveStepByStepToNvs()
{
  prefs.begin(PREFS_NS, false);
  prefs.putBool(PREFS_KEY_STEP_BY_STEP, stepByStepMode);
  prefs.end();
}

static void applyStepByStepMode(bool on)
{
  if (stepByStepMode == on)
    return;
  stepByStepMode = on;
  saveStepByStepToNvs();
  Serial.printf("PASO A PASO %s (PreFeeder sigue automático)\n", on ? "ON" : "OFF");
  pushLog(String("Paso a paso ") + (on ? "ON" : "OFF"));
  uiNotify();
}

static void applyTcmBuzzerMute(bool on)
{
  if (tcmBuzzerMuted == on)
  {
    towerForceResync = true;
    return;
  }
  tcmBuzzerMuted = on;
  saveTcmBuzzerMuteToNvs();
  towerForceResync = true;
  for (uint8_t s = 0; s < PEER_COUNT; s++)
  {
    peers[s].mirror.buzzerMuted = on;
    (void)peerSendCmdEx(s, "setBuzzerMute", on ? "1" : "0", 0);
  }
  uiNotify();
}

static bool towerBuzzerMutedNow()
{
  return tcmBuzzerMuted;
}

void serviceTowerCommandTx()
{
  if (!canInitialized) return;

  static uint8_t lastMode = 0xFF;
  static uint8_t lastMute = 0xFF;
  static uint32_t lastTxMs = 0;
  static uint8_t errorPending = 0;
  static uint32_t errorPendingSinceMs = 0;
  static uint8_t clearPending = 0;
  static uint32_t clearPendingSinceMs = 0;
  static bool wasInBootGrace = true;

  // Fin de grace: enclavar fallas físicas vistas durante el arranque + poll discovery.
  const bool inGrace = towerInBootGrace();
  if (wasInBootGrace && !inGrace)
  {
    if (!sensorEverOnline)
    {
      if (!pollSensorModule())
        pollSensorModuleEx(3, 400);
    }
    if (sensorBitmask != 0)
    {
      uint8_t before = sensorLatchedBitmask;
      sensorLatchedBitmask = (uint8_t)(sensorLatchedBitmask | sensorBitmask);
      if (sensorLatchedBitmask != before)
      {
        refreshSafetyErrorCode();
        uiNotify();
      }
    }
    // Primer modo post-grace: reenviar aunque coincida (frame de arranque a menudo se pierde).
    towerForceResync = true;
  }
  wasInBootGrace = inGrace;

  const uint8_t desired = computeTowerMode();
  const uint32_t now = millis();

  // Boot: sin TX (torre OFF en el módulo sensor) mientras no hay sensor online.
  if (desired == TOWER_MODE_HOLD)
  {
    errorPending = 0;
    clearPending = 0;
    return;
  }

  // Debounce al entrar en ERROR.
  uint8_t mode = desired;
  if (desired == TOWER_MODE_ERROR)
  {
    clearPending = 0;
    if (!errorPending)
    {
      errorPending = 1;
      errorPendingSinceMs = now;
    }
    if ((now - errorPendingSinceMs) < TOWER_ALARM_DEBOUNCE_MS)
    {
      if (lastMode == 0xFF || lastMode == TOWER_MODE_HOLD)
        return;
      mode = lastMode;
    }
  }
  else
  {
    errorPending = 0;
    // Debounce al salir de ERROR.
    if (lastMode == TOWER_MODE_ERROR)
    {
      if (!clearPending)
      {
        clearPending = 1;
        clearPendingSinceMs = now;
      }
      if ((now - clearPendingSinceMs) < (uint32_t)TOWER_ALARM_CLEAR_MS)
        mode = TOWER_MODE_ERROR;
      else
        clearPending = 0;
    }
    else
      clearPending = 0;
  }

  const uint8_t mute = towerBuzzerMutedNow() ? 1 : 0;
  const bool muteChanged = (mute != lastMute);
  const bool modeChanged = (mode != lastMode) || towerForceResync || muteChanged;
  // Heartbeat: TX OK en MCP2515 no garantiza RX en el módulo sensor.
  // OK_DONE también se reafirma: el módulo ignora duplicados (no reinicia el buzzer).
  const bool heartbeat = !modeChanged && lastTxMs != 0
                         && (now - lastTxMs) >= (uint32_t)TOWER_HEARTBEAT_MS;
  const bool needTx = modeChanged || heartbeat;
  if (!needTx) return;
  if (lastTxMs != 0 && (now - lastTxMs) < (uint32_t)TOWER_TX_RETRY_MS) return;

  uint8_t data[3] = { TOWER_CMD_MAGIC, mode, mute };
  // Cambio de modo / salir de alarma / resync: varios intentos (bus ocupado por SDO).
  const bool clearingAlarm = (lastMode == TOWER_MODE_ERROR) && (mode != TOWER_MODE_ERROR);
  const uint8_t attempts = (modeChanged || clearingAlarm) ? 3 : 1;
  bool txOk = false;
  for (uint8_t a = 0; a < attempts; a++)
  {
    if (a) delay(2);
    if (sendCANMessage(CAN_ID_TOWER_CMD, 3, data, "Tower", a == 0 && modeChanged))
    {
      txOk = true;
      break;
    }
  }
  // No limpiar el pulso aquí: towerLotCompleteActive() lo mantiene TOWER_OK_DONE_MS
  // para que el sensor no reciba OK inmediato y cancele el buzzer.
  lastTxMs = now;
  if (!txOk)
  {
    // No actualizar lastMode: reintentar hasta que el módulo reciba el comando.
    towerForceResync = true;
    return;
  }
  if (modeChanged)
  {
    const char* name = (mode == TOWER_MODE_ERROR) ? "ERROR"
                       : (mode == TOWER_MODE_MATERIALISTA) ? "MATERIALISTA"
                       : (mode == TOWER_MODE_OK_DONE) ? "OK_DONE" : "OK";
    pushLog(String("TOWER -> ") + name + (mute ? " (buzzer mute)" : ""));
  }
  lastMode = mode;
  lastMute = mute;
  towerForceResync = false;
}

bool sendCANMessage(unsigned long id, byte len, byte* data, String desc, bool logSuccess)
{
  if (!canInitialized)
  {
    if (logSuccess)
    {
      pushLog("TX SKIP: CAN no inicializado (" + desc + ")");
      DBG_PRINTLN("TX SKIP: CAN no inicializado");
    }
    return false;
  }

  if (canMutex) xSemaphoreTake(canMutex, portMAX_DELAY);
  byte sndStat = CAN0.sendMsgBuf(id, 0, len, data);
  if (canMutex) xSemaphoreGive(canMutex);

  if (sndStat != CAN_OK)
  {
    // Solo web log; Serial solo en modo verbose (evita saturar el bus en polls).
    pushLog("TX FAIL [0x" + String(id, HEX) + "] err=" + String(sndStat) + " (" + desc + ")");
    DBG_PRINTF("TX FAIL [0x%lX] err=%u (%s)\n", id, (unsigned)sndStat, desc.c_str());
    return false;
  }
  if (logSuccess)
  {
    pushLog("TX [0x" + String(id, HEX) + "] OK (" + desc + ")");
    DBG_PRINTF("TX [0x%lX] OK (%s)\n", id, desc.c_str());
  }
  return true;
}

// Halt CiA402 bit 8, un solo frame. Sin String/log: ruta de parada del láser.
static bool sendCanHaltImmediate(bool sideR)
{
  if (!canInitialized) return false;
  byte data[8] = {0x2B, 0x40, 0x60, 0x00, 0x0F, 0x01, 0x00, 0x00};
  const unsigned long id = sideR ? SERVO_CAN_TX_R : SERVO_CAN_TX_L;
  if (canMutex) xSemaphoreTake(canMutex, portMAX_DELAY);
  byte sndStat = 0xFF;
  for (uint8_t i = 0; i < 3; i++)
  {
    sndStat = CAN0.sendMsgBuf(id, 0, 8, data);
    if (sndStat == CAN_OK) break;
  }
  if (canMutex) xSemaphoreGive(canMutex);
  return sndStat == CAN_OK;
}

bool canReadStatusWord(uint8_t nodeId, uint16_t& statusOut, uint32_t timeoutMs)
{
  if (!canInitialized) return false;

  unsigned char data[] = {0x40, 0x41, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00};
  unsigned long txId = 0x600 + nodeId;
  sendCANMessage(txId, 8, data, "Poll Status N" + String(nodeId), false);

  unsigned long expectedRx = 0x580 + nodeId;
  uint32_t start = millis();

  while (millis() - start < timeoutMs)
  {
    if (canMutex) xSemaphoreTake(canMutex, portMAX_DELAY);
    if (CAN0.checkReceive() != CAN_MSGAVAIL)
    {
      if (canMutex) xSemaphoreGive(canMutex);
      feedSsHaltFromSensorNow();
      delay(5);
      continue;
    }

    long unsigned int rxId;
    unsigned char len = 0;
    unsigned char rxBuf[8];
    CAN0.readMsgBuf(&rxId, &len, rxBuf);
    if (canMutex) xSemaphoreGive(canMutex);

    processSafetyAlertRx(rxId, len, rxBuf);

    if (rxId == expectedRx && len >= 6 && rxBuf[1] == 0x41 && rxBuf[2] == 0x60)
    {
      statusOut = rxBuf[4] | (rxBuf[5] << 8);
      return true;
    }
  }
  return false;
}

static bool canReadSdoI32(uint8_t nodeId, uint16_t index, uint8_t subIndex, int32_t& valOut, uint32_t timeoutMs = 150)
{
  if (!canInitialized) return false;

  unsigned char data[] = {
    0x40,
    (byte)(index & 0xFF), (byte)((index >> 8) & 0xFF), subIndex,
    0x00, 0x00, 0x00, 0x00
  };
  unsigned long txId = 0x600 + nodeId;
  sendCANMessage(txId, 8, data, "SDO read 0x" + String(index, HEX), false);

  unsigned long expectedRx = 0x580 + nodeId;
  uint32_t start = millis();

  while (millis() - start < timeoutMs)
  {
    if (canMutex) xSemaphoreTake(canMutex, portMAX_DELAY);
    if (CAN0.checkReceive() != CAN_MSGAVAIL)
    {
      if (canMutex) xSemaphoreGive(canMutex);
      feedSsHaltFromSensorNow();
      delay(2);
      continue;
    }

    long unsigned int rxId;
    unsigned char len = 0;
    unsigned char rxBuf[8];
    CAN0.readMsgBuf(&rxId, &len, rxBuf);
    if (canMutex) xSemaphoreGive(canMutex);

    processSafetyAlertRx(rxId, len, rxBuf);

    if (rxId != expectedRx || len < 8) continue;
    if (rxBuf[0] != 0x43) continue;
    if (rxBuf[1] != (byte)(index & 0xFF) || rxBuf[2] != (byte)((index >> 8) & 0xFF) || rxBuf[3] != subIndex)
      continue;

    valOut = (int32_t)((uint32_t)rxBuf[4] | ((uint32_t)rxBuf[5] << 8) | ((uint32_t)rxBuf[6] << 16) | ((uint32_t)rxBuf[7] << 24));
    return true;
  }
  return false;
}

bool verifyServoEnergized()
{
  uint16_t sw1 = 0;
  uint16_t sw2 = 0;
  bool ok1 = canReadStatusWord(1, sw1, 600);
  bool ok2 = canReadStatusWord(2, sw2, 600);

  bool ready = ok1 && ok2 && isServoOperationEnabled(sw1) && isServoOperationEnabled(sw2);
  servoCanReady = ready;

  if (ready)
    pushLog("SERVO: energizado OK (601=0x" + String(sw1, HEX) + " 602=0x" + String(sw2, HEX) + ")");
  else
    pushLog("WARN: servo sin confirmar (601=0x" + String(sw1, HEX) + "/" + String(ok1 ? "rx" : "timeout") +
            " 602=0x" + String(sw2, HEX) + "/" + String(ok2 ? "rx" : "timeout") + ")");
  return ready;
}

bool initCANBus(uint8_t maxRetries)
{
  canInitialized = false;
  servoCanReady = false;

  for (uint8_t attempt = 1; attempt <= maxRetries; attempt++)
  {
    DBG_PRINT("CAN init intento ");
    DBG_PRINT(attempt);
    DBG_PRINT("/");
    DBG_PRINTLN(maxRetries);

    if (canMutex) xSemaphoreTake(canMutex, portMAX_DELAY);
    byte stat = CAN0.begin(MCP_ANY, CAN_SPEED, CAN_XTAL);
    if (stat == CAN_OK)
    {
      CAN0.setMode(MCP_NORMAL);
      canInitialized = true;
      if (canMutex) xSemaphoreGive(canMutex);
      pushLog("CAN: MCP2515 OK intento " + String(attempt));
      return true;
    }
    if (canMutex) xSemaphoreGive(canMutex);
    pushLog("CAN: init FAIL intento " + String(attempt) + " err=" + String(stat));
    delay(CAN_INIT_RETRY_MS);
  }
  return false;
}

bool setupServoFeeder()
{
  servoCanReady = false;

  unsigned char faultReset[] = {0x2B, 0x40, 0x60, 0x00, 0x80, 0x00, 0x00, 0x00};
  sendCANMessage(0x601, 8, faultReset, "Fault Reset 0x80", false);
  sendCANMessage(0x602, 8, faultReset, "Fault Reset 0x80", false);
  delay(300);

  unsigned char data1[] = {0x80, 0x00};
  sendCANMessage(0x000, 2, data1, "Pre-Operational (broadcast)", false);
  delay(500);

  unsigned char data2[] = {0x01, 0x00};
  sendCANMessage(0x000, 2, data2, "Operational (broadcast)", false);
  delay(500);

  unsigned char data3[] = {0x2B, 0x40, 0x60, 0x00, 0x06, 0x00, 0x00, 0x00};
  sendCANMessage(0x601, 8, data3, "Shutdown 0x06", false);
  sendCANMessage(0x602, 8, data3, "Shutdown 0x06", false);
  delay(500);

  unsigned char data4[] = {0x2B, 0x40, 0x60, 0x00, 0x07, 0x00, 0x00, 0x00};
  sendCANMessage(0x601, 8, data4, "Switch On 0x07", false);
  sendCANMessage(0x602, 8, data4, "Switch On 0x07", false);
  delay(500);

  unsigned char data5[] = {0x2B, 0x40, 0x60, 0x00, 0x0F, 0x00, 0x00, 0x00};
  sendCANMessage(0x601, 8, data5, "Enable Op 0x0F", false);
  sendCANMessage(0x602, 8, data5, "Enable Op 0x0F", false);
  delay(500);

  unsigned char data6[] = {0x2F, 0x60, 0x60, 0x00, 0x01, 0x00, 0x00, 0x00};
  sendCANMessage(0x601, 8, data6, "Mode PP", false);
  sendCANMessage(0x602, 8, data6, "Mode PP", false);
  delay(500);

  // 0x607F Maximal profile velocity: default de fábrica ~600000 (~275 RPM).
  // Subir al tope de 3000 RPM para que 0x6081 no quede saturado.
  {
    uint32_t maxPps = SERVO_MAX_PPS;
    byte maxVel[8] = {
      0x23, 0x7F, 0x60, 0x00,
      (byte)(maxPps & 0xFF), (byte)((maxPps >> 8) & 0xFF),
      (byte)((maxPps >> 16) & 0xFF), (byte)((maxPps >> 24) & 0xFF)
    };
    sendCANMessage(0x601, 8, maxVel, "MaxVel 0x607F", false);
    sendCANMessage(0x602, 8, maxVel, "MaxVel 0x607F", false);
  }
  delay(50);

  // Halt bit 8 debe usar rampa Quick Stop (0x6085), no la decel de perfil 0x6084.
  {
    byte haltOpt[8] = {0x2B, 0x5D, 0x60, 0x00, (byte)FEED_HALT_OPTION_CODE, 0x00, 0x00, 0x00};
    sendCANMessage(0x601, 8, haltOpt, "HaltOpt 0x605D", false);
    sendCANMessage(0x602, 8, haltOpt, "HaltOpt 0x605D", false);
    uint32_t hd = FEED_HALT_DECEL_PP;
    byte qdec[8] = {
      0x23, 0x85, 0x60, 0x00,
      (byte)(hd & 0xFF), (byte)((hd >> 8) & 0xFF),
      (byte)((hd >> 16) & 0xFF), (byte)((hd >> 24) & 0xFF)
    };
    sendCANMessage(0x601, 8, qdec, "QSDec 0x6085", false);
    sendCANMessage(0x602, 8, qdec, "QSDec 0x6085", false);
  }
  delay(200);

  uint32_t val = (uint32_t)SERVO_HOME_PP;
  feedCanSetVelAcc(val, val);
  delay(500);

  unsigned char data10[] = {0x23, 0x7A, 0x60, 0x00, (byte)(val & 0xFF), (byte)((val >> 8) & 0xFF), (byte)((val >> 16) & 0xFF), (byte)((val >> 24) & 0xFF)};
  sendCANMessage(0x601, 8, data10, "Pos " + String(val), false);
  sendCANMessage(0x602, 8, data10, "Pos " + String(val), false);
  delay(500);

  unsigned char data11[] = {0x2B, 0x40, 0x60, 0x00, 0x2F, 0x00, 0x00, 0x00};
  sendCANMessage(0x601, 8, data11, "Reset Set-point", false);
  sendCANMessage(0x602, 8, data11, "Reset Set-point", false);
  delay(500);

  return verifyServoEnergized();
}

bool reconnectCANBus(bool fullServoSetup)
{
  DBG_PRINTLN("=== RECONECTAR CAN BUS ===");
  pushLog("INFO: Reconexion CAN iniciada");

  if (!initCANBus(CAN_INIT_MAX_RETRIES))
  {
    pushLog("ERROR: Reconexion CAN fallo");
    servoCanReady = false;
    return false;
  }

  if (fullServoSetup)
    setupServoFeeder();
  return true;
}

void serviceCANRx()
{
  if (!canInitialized) return;

  // Drenar a fondo por pasada para no perder heartbeats 0xC0 bajo tráfico SDO.
  for (uint8_t n = 0; n < CAN_RX_DRAIN_MAX; n++)
  {
    if (canMutex) xSemaphoreTake(canMutex, portMAX_DELAY);
    if (CAN0.checkReceive() != CAN_MSGAVAIL)
    {
      if (canMutex) xSemaphoreGive(canMutex);
      break;
    }

    long unsigned int rxId;
    unsigned char len = 0;
    unsigned char rxBuf[8];
    CAN0.readMsgBuf(&rxId, &len, rxBuf);
    if (canMutex) xSemaphoreGive(canMutex);

    if (rxId == CAN_ID_SAFETY_ALERT)
    {
      processSafetyAlertRx(rxId, len, rxBuf);
      continue;
    }
    // Otros frames (SDO/PDO) se ignoran en log; la lógica de movimiento no depende de ellos aquí.
  }
}

// ============================================================
// SECCION 09 — CAN — Movimiento CiA402
// ============================================================
// valL / valR = lado lógico máquina (PreFeeder L/R), no ID de nodo CAN.
static void canWriteU32BothNodes(uint16_t index, uint32_t valL, uint32_t valR, const String& desc)
{
  byte dataL[8] = {
    0x23, (byte)(index & 0xFF), (byte)((index >> 8) & 0xFF), 0x00,
    (byte)(valL & 0xFF), (byte)((valL >> 8) & 0xFF), (byte)((valL >> 16) & 0xFF), (byte)((valL >> 24) & 0xFF)
  };
  sendCANMessage(SERVO_CAN_TX_L, 8, dataL, desc + " L " + String(valL));
  byte dataR[8] = {
    0x23, (byte)(index & 0xFF), (byte)((index >> 8) & 0xFF), 0x00,
    (byte)(valR & 0xFF), (byte)((valR >> 8) & 0xFF), (byte)((valR >> 16) & 0xFF), (byte)((valR >> 24) & 0xFF)
  };
  sendCANMessage(SERVO_CAN_TX_R, 8, dataR, desc + " R " + String(valR));
}

void canSetProfileVelocity(uint32_t valL, uint32_t valR) { canWriteU32BothNodes(0x6081, valL, valR, "Vel"); }
void canSetProfileAccel(uint32_t valL, uint32_t valR)    { canWriteU32BothNodes(0x6083, valL, valR, "Acc"); }
void canSetProfileDecel(uint32_t valL, uint32_t valR)    { canWriteU32BothNodes(0x6084, valL, valR, "Dec"); }

void canSetMotionProfile(uint32_t velL, uint32_t accL, uint32_t decL, uint32_t velR, uint32_t accR, uint32_t decR)
{
  if (canBusMotionBlocked()) return;
  canSetProfileVelocity(velL, velR);
  canSetProfileAccel(accL, accR);
  canSetProfileDecel(decL, decR);
}

// stepsL / stepsR = avance lógico (+ alimenta). El signo de montaje se aplica por nodo.
// 0 = no enviar a ese nodo (permite mover un solo lado / offsets con signo).
void canSetTargetPos(int32_t stepsL, int32_t stepsR)
{
  if (canBusMotionBlocked()) return;
  if (stepsL != 0)
  {
    int32_t cmdL = (int32_t)SERVO_CMD_SIGN_L * stepsL;
    byte dataL[8] = {
      0x23, 0x7A, 0x60, 0x00,
      (byte)(cmdL & 0xFF),
      (byte)((cmdL >> 8) & 0xFF),
      (byte)((cmdL >> 16) & 0xFF),
      (byte)((cmdL >> 24) & 0xFF)
    };
    sendCANMessage(SERVO_CAN_TX_L, 8, dataL, "Target Pos L " + String(cmdL));
  }
  if (stepsR != 0)
  {
    int32_t cmdR = (int32_t)SERVO_CMD_SIGN_R * stepsR;
    byte dataR[8] = {
      0x23, 0x7A, 0x60, 0x00,
      (byte)(cmdR & 0xFF),
      (byte)((cmdR >> 8) & 0xFF),
      (byte)((cmdR >> 16) & 0xFF),
      (byte)((cmdR >> 24) & 0xFF)
    };
    sendCANMessage(SERVO_CAN_TX_R, 8, dataR, "Target Pos R " + String(cmdR));
  }
}

static void canResetNewSetpointSides(bool doL, bool doR)
{
  if (canBusMotionBlocked()) return;
  byte data[8] = {0x2B, 0x40, 0x60, 0x00, 0x2F, 0x00, 0x00, 0x00};
  if (doL) sendCANMessage(SERVO_CAN_TX_L, 8, data, "Reset New Set-point L");
  if (doR) sendCANMessage(SERVO_CAN_TX_R, 8, data, "Reset New Set-point R");
}

static void canExecuteMoveSides(bool doL, bool doR)
{
  if (canBusMotionBlocked()) return;
  byte data[8] = {0x2B, 0x40, 0x60, 0x00, 0x7F, 0x00, 0x00, 0x00};
  if (doL) sendCANMessage(SERVO_CAN_TX_L, 8, data, "Execute Relative L");
  if (doR) sendCANMessage(SERVO_CAN_TX_R, 8, data, "Execute Relative R");
}

void canHaltSide(bool sideR)
{
  sendCanHaltImmediate(sideR);
}

static void feedCanPrimeHaltDecel()
{
  byte haltOpt[8] = {0x2B, 0x5D, 0x60, 0x00, (byte)FEED_HALT_OPTION_CODE, 0x00, 0x00, 0x00};
  sendCANMessage(SERVO_CAN_TX_L, 8, haltOpt, "HaltOpt L", false);
  sendCANMessage(SERVO_CAN_TX_R, 8, haltOpt, "HaltOpt R", false);
  uint32_t hd = FEED_HALT_DECEL_PP;
  byte qdec[8] = {
    0x23, 0x85, 0x60, 0x00,
    (byte)(hd & 0xFF), (byte)((hd >> 8) & 0xFF),
    (byte)((hd >> 16) & 0xFF), (byte)((hd >> 24) & 0xFF)
  };
  sendCANMessage(SERVO_CAN_TX_L, 8, qdec, "QSDec L", false);
  sendCANMessage(SERVO_CAN_TX_R, 8, qdec, "QSDec R", false);
}

void canHalt()
{
  canHaltSide(false);
  canHaltSide(true);
}

void canClearHaltSide(bool sideR)
{
  byte data[8] = {0x2B, 0x40, 0x60, 0x00, 0x0F, 0x00, 0x00, 0x00};
  sendCANMessage(sideR ? SERVO_CAN_TX_R : SERVO_CAN_TX_L, 8, data, sideR ? "Clear Halt R" : "Clear Halt L");
}

void canMoveRelativePP(int32_t stepsL, int32_t stepsR)
{
  if (stepsL <= 0 && stepsR <= 0)
    return;
  if (stepsL < 0) stepsL = 0;
  if (stepsR < 0) stepsR = 0;
  if (canBusMotionBlocked()) return;
  const bool doL = stepsL > 0;
  const bool doR = stepsR > 0;
  canSetTargetPos(stepsL, stepsR);
  canResetNewSetpointSides(doL, doR);
  if (doL) canClearHaltSide(false);
  if (doR) canClearHaltSide(true);
  canExecuteMoveSides(doL, doR);
}

// 607A crudo (coordenada absoluta del drive, signo de montaje ya aplicado).
static void canSetTargetPosRaw(int32_t cmdL, int32_t cmdR, bool doL, bool doR)
{
  if (canBusMotionBlocked()) return;
  if (doL)
  {
    byte dataL[8] = {
      0x23, 0x7A, 0x60, 0x00,
      (byte)(cmdL & 0xFF),
      (byte)((cmdL >> 8) & 0xFF),
      (byte)((cmdL >> 16) & 0xFF),
      (byte)((cmdL >> 24) & 0xFF)
    };
    sendCANMessage(SERVO_CAN_TX_L, 8, dataL, "Target Abs L " + String(cmdL));
  }
  if (doR)
  {
    byte dataR[8] = {
      0x23, 0x7A, 0x60, 0x00,
      (byte)(cmdR & 0xFF),
      (byte)((cmdR >> 8) & 0xFF),
      (byte)((cmdR >> 16) & 0xFF),
      (byte)((cmdR >> 24) & 0xFF)
    };
    sendCANMessage(SERVO_CAN_TX_R, 8, dataR, "Target Abs R " + String(cmdR));
  }
}

static void canSetProfileVelocitySide(bool sideR, uint32_t vel)
{
  if (canBusMotionBlocked()) return;
  byte data[8] = {
    0x23, 0x81, 0x60, 0x00,
    (byte)(vel & 0xFF), (byte)((vel >> 8) & 0xFF),
    (byte)((vel >> 16) & 0xFF), (byte)((vel >> 24) & 0xFF)
  };
  sendCANMessage(sideR ? SERVO_CAN_TX_R : SERVO_CAN_TX_L, 8, data,
                 sideR ? ("Vel R " + String(vel)) : ("Vel L " + String(vel)));
}

// DSY 5.6.4: absoluto inmediato = 0x2F → 0x3F (New setpoint + Change set immediately).
static void canExecuteAbsImmediateSides(bool doL, bool doR)
{
  if (canBusMotionBlocked()) return;
  byte data[8] = {0x2B, 0x40, 0x60, 0x00, 0x3F, 0x00, 0x00, 0x00};
  if (doL) sendCANMessage(SERVO_CAN_TX_L, 8, data, "Execute Abs Immediate L");
  if (doR) sendCANMessage(SERVO_CAN_TX_R, 8, data, "Execute Abs Immediate R");
}

static void feedSsMaskHaltedSides(bool& doL, bool& doR)
{
  if (doL && feedSensorConfirmedL) doL = false;
  if (doR && feedSensorConfirmedR) doR = false;
}

static void canAbsImmediateUpdate(int32_t cmdL, int32_t cmdR, uint32_t velL, uint32_t velR,
                                 bool doL, bool doR, bool clearHalt)
{
  feedSsMaskHaltedSides(doL, doR);
  if (!doL && !doR) return;
  // DSY 5.6.4: 607A → 6081 → 6040 (0x0F si venía de Halt, luego 0x2F → 0x3F).
  canSetTargetPosRaw(cmdL, cmdR, doL, doR);
  feedSsMaskHaltedSides(doL, doR);
  if (doL) canSetProfileVelocitySide(false, velL);
  if (doR) canSetProfileVelocitySide(true, velR);
  feedSsMaskHaltedSides(doL, doR);
  if (!doL && !doR) return;
  if (clearHalt)
  {
    if (doL) canClearHaltSide(false);
    if (doR) canClearHaltSide(true);
  }
  feedSsMaskHaltedSides(doL, doR);
  if (!doL && !doR) return;
  canResetNewSetpointSides(doL, doR);
  feedSsMaskHaltedSides(doL, doR);
  if (!doL && !doR) return;
  canExecuteAbsImmediateSides(doL, doR);
}

static int32_t feedEncoderAbsDelta(int32_t startPos, int32_t endPos)
{
  int32_t d = endPos - startPos;
  return d < 0 ? -d : d;
}

// Cero virtual UI (0x6064 no se resetea en el drive).
static int32_t feedCanEncZeroL = 0;
static int32_t feedCanEncZeroR = 0;
static bool feedCanEncZeroLSet = false;
static bool feedCanEncZeroRSet = false;

static float feedCanEncCountsToMm(int32_t counts)
{
  return (float)counts / FEED_ENC_COUNTS_PER_MM;
}

static int32_t feedCanEncSignedDelta(int32_t pos, int32_t zero)
{
  return pos - zero;
}

// ============================================================
// SECCION 10 — ASDA RS-485 — Actuador lineal (esclavo HTTP)
// ============================================================
static bool jsonExtractBool(const String& body, const char* key, bool defVal)
{
  String needle = String("\"") + key + "\"";
  int k = body.indexOf(needle);
  if (k < 0) return defVal;
  int colon = body.indexOf(':', k + needle.length());
  if (colon < 0) return defVal;
  String rest = body.substring(colon + 1);
  rest.trim();
  rest.toLowerCase();
  if (rest.startsWith("true")) return true;
  if (rest.startsWith("false")) return false;
  return defVal;
}

static long jsonExtractLong(const String& body, const char* key, long defVal)
{
  String needle = String("\"") + key + "\"";
  int k = body.indexOf(needle);
  if (k < 0) return defVal;
  int colon = body.indexOf(':', k + needle.length());
  if (colon < 0) return defVal;
  return body.substring(colon + 1).toInt();
}

static float jsonExtractFloat(const String& body, const char* key, float defVal)
{
  String needle = String("\"") + key + "\"";
  int k = body.indexOf(needle);
  if (k < 0) return defVal;
  int colon = body.indexOf(':', k + needle.length());
  if (colon < 0) return defVal;
  return body.substring(colon + 1).toFloat();
}

static bool asdaHttp(const char* method, const char* path, const char* body,
                     String& resp, uint32_t timeoutMs)
{
  resp = "";
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient client;
  HTTPClient http;
  http.setTimeout((int)timeoutMs);
  String url = String("http://") + ASDA_HOST + path;
  if (!http.begin(client, url)) return false;
  int code = -1;
  if (body && body[0] && strcmp(method, "GET") != 0)
  {
    http.addHeader("Content-Type", "application/json");
    code = http.POST(body);
  }
  else if (strcmp(method, "POST") == 0)
    code = http.POST("{}");
  else
    code = http.GET();
  if (code > 0) resp = http.getString();
  http.end();
  return code >= 200 && code < 300;
}

static bool asdaOverviewStart(float pieceMm)
{
  char body[128];
  snprintf(body, sizeof(body),
           "{\"pieceMm\":%.2f,\"extraMm\":%.2f,\"tolMm\":%.2f,"
           "\"requireAsda\":true,\"requireOm\":true}",
           pieceMm, (double)DEPOSIT_EXTRA_MM_DEFAULT, (double)ASDA_OVERVIEW_TOL_MM);
  String resp;
  return asdaHttp("POST", "/api/overview/start", body, resp, ASDA_HTTP_TIMEOUT_MS);
}

static bool asdaOverviewMark(const char* event)
{
  if (!event || !event[0]) return false;
  char body[64];
  snprintf(body, sizeof(body), "{\"event\":\"%s\"}", event);
  String resp;
  return asdaHttp("POST", "/api/overview/mark", body, resp, ASDA_HTTP_TIMEOUT_MS);
}

static bool asdaEncoderReset()
{
  String resp;
  return asdaHttp("POST", "/api/encoder/reset", "{}", resp, ASDA_HTTP_TIMEOUT_MS);
}

// Lee mmAbs del encoder OM en el esclavo ASDA.
static bool asdaOmReadMm(float* actualMmOut)
{
  String resp;
  if (!asdaHttp("GET", "/api/encoder", nullptr, resp, ASDA_OM_HTTP_TIMEOUT_MS))
    return false;
  const float mmAbs = jsonExtractFloat(resp, "mmAbs", -1.0f);
  if (actualMmOut) *actualMmOut = mmAbs;
  if (mmAbs < 0.0f) return false;
  feedOmLastMmAbs = mmAbs;
  return true;
}

// true si OM alcanzó el objetivo de alimentación (mmAbs >= target - tol).
static bool asdaOmFeedMet(float targetMm, float* actualMmOut)
{
  float mmAbs = -1.0f;
  if (!asdaOmReadMm(&mmAbs))
    return false;
  if (actualMmOut) *actualMmOut = mmAbs;
  return mmAbs >= (targetMm - ASDA_OVERVIEW_TOL_MM);
}

static bool asdaOverviewHitOk(const char* hitKey)
{
  String resp;
  if (!asdaHttp("GET", "/api/overview", nullptr, resp, ASDA_OM_HTTP_TIMEOUT_MS))
    return false;
  // Busca "hitKey":{ ... "ok":true
  String needle = String("\"") + hitKey + "\"";
  int k = resp.indexOf(needle);
  if (k < 0) return false;
  int okPos = resp.indexOf("\"ok\":", k);
  if (okPos < 0 || okPos > k + 120) return false;
  return resp.substring(okPos + 5).startsWith("true");
}

static bool asdaOn()
{
  String resp;
  if (!asdaHttp("POST", "/api/on", "{}", resp, ASDA_HTTP_TIMEOUT_MS)) return false;
  asdaServoOn = jsonExtractBool(resp, "ok", true);
  return asdaServoOn;
}

static bool asdaStopCmd()
{
  String resp;
  const bool ok = asdaHttp("POST", "/api/stop", "{}", resp, ASDA_HTTP_TIMEOUT_MS);
  asdaBusy = false;
  asdaPulseDone = true;
  asdaJob = ASDA_JOB_NONE;
  if (resp.length())
  {
    asdaLastPuu = (int32_t)jsonExtractLong(resp, "positionPuu", asdaLastPuu);
    posPulses = puuToCommandSteps(asdaLastPuu);
  }
  return ok;
}

static bool asdaPollLocked()
{
  String resp;
  if (!asdaHttp("GET", "/api/status", nullptr, resp, ASDA_HTTP_TIMEOUT_MS))
    return false;
  const bool busy = jsonExtractBool(resp, "busy", false);
  asdaLastPuu = (int32_t)jsonExtractLong(resp, "positionPuu", asdaLastPuu);
  if (asdaBusy && !busy)
  {
    asdaBusy = false;
    asdaPulseDone = true;
    if (asdaJob == ASDA_JOB_HOME)
    {
      asdaHomed = true;
      posPulses = 0;
      asdaLastPuu = 0;
    }
    asdaJob = ASDA_JOB_NONE;
  }
  else
    asdaBusy = busy;
  return true;
}

static bool linearIsMoving()
{
  return asdaBusy;
}

static void linearService()
{
  // Solo preguntar status mientras esperamos cambio busy→idle (no saturar en idle).
  if (!asdaBusy) return;
  uint32_t now = millis();
  if ((uint32_t)(now - asdaLastPollMs) < (uint32_t)ASDA_POLL_INTERVAL_MS) return;
  asdaLastPollMs = now;
  asdaPollLocked();
}

static void linearAbort()
{
  asdaStopCmd();
  String resp;
  if (asdaHttp("GET", "/api/pos", nullptr, resp, ASDA_HTTP_TIMEOUT_MS))
  {
    asdaLastPuu = (int32_t)jsonExtractLong(resp, "positionPuu", asdaLastPuu);
    posPulses = puuToCommandSteps(asdaLastPuu);
  }
  pulsesRemaining = 0;
}

static void linearPause() {}
static void linearResume() {}

static uint32_t linearMoveTimeoutMs(uint32_t pulses)
{
  uint16_t rpm = linearRpm;
  if (rpm < 1) rpm = 1;
  // Timeout con escala encoder legacy: la posición comandada es 1:1 con pasos,
  // pero el eje sigue moviéndose a ritmo de motor/encoder.
  uint64_t puu = (uint64_t)pulses * (uint64_t)LINEAR_EGEAR_LEGACY_N / (uint64_t)LINEAR_EGEAR_M;
  uint64_t cps = (uint64_t)rpm * (uint64_t)LINEAR_ENCODER_PPR / 60ULL;
  if (cps < 1) cps = 1;
  uint32_t moveMs = (uint32_t)((puu * 1000ULL + cps - 1ULL) / cps);
  uint32_t t = moveMs + (uint32_t)dwellAtDestMs + (uint32_t)LINEAR_MOVE_TIMEOUT_BASE_MS;
  if (t < (uint32_t)LINEAR_MOVE_TIMEOUT_BASE_MS)
    t = LINEAR_MOVE_TIMEOUT_BASE_MS;
  if (t > (uint32_t)LINEAR_MOVE_TIMEOUT_MAX_MS)
    t = LINEAR_MOVE_TIMEOUT_MAX_MS;
  return t;
}

static bool asdaStartMovePuu(int32_t puu)
{
  if (!asdaServoOn && !asdaOn()) return false;
  char body[128];
  snprintf(body, sizeof(body),
           "{\"position\":%ld,\"timeoutMs\":%lu,\"wait\":0}",
           (long)puu, (unsigned long)LINEAR_MOVE_TIMEOUT_MAX_MS);
  String resp;
  if (!asdaHttp("POST", "/api/move", body, resp, ASDA_HTTP_TIMEOUT_MS))
    return false;
  asdaBusy = true;
  asdaPulseDone = false;
  asdaJob = ASDA_JOB_MOVE;
  asdaLastPollMs = 0;
  return true;
}

static bool asdaStartHomeTorque()
{
  if (!asdaServoOn && !asdaOn()) return false;
  String resp;
  if (!asdaHttp("POST", "/api/home", "{\"wait\":0}", resp, ASDA_HTTP_TIMEOUT_MS))
    return false;
  asdaBusy = true;
  asdaPulseDone = false;
  asdaJob = ASDA_JOB_HOME;
  asdaLastPollMs = 0;
  asdaHomed = false;
  return true;
}

static bool linearStartAbsSteps(uint32_t targetSteps)
{
  const int32_t puu = commandStepsToPuu(targetSteps);
  if (!asdaStartMovePuu(puu))
  {
    cycleState = C_IDLE;
    cycleRunning = false;
    Serial.println("LINEAR: ASDA move falló");
    return false;
  }
  return true;
}

void setupLinearActuator()
{
  posPulses = 0;
  pulsesRemaining = 0;
  lastFwdCommandPulses = 0;
  lastBackCommandPulses = 0;
  asdaBusy = false;
  asdaPulseDone = false;
  asdaHomed = false;
  asdaServoOn = false;
  asdaJob = ASDA_JOB_NONE;
  asdaLastPuu = 0;
  cycleState = C_IDLE;
  cycleRunning = false;
  DBG_PRINTLN("Actuador lineal: esclavo ASDA RS-485");
}

static void saveLinearCalToNvs()
{
  prefs.begin(PREFS_NS, false);
  prefs.putFloat(PREFS_KEY_LINEAR_STEPS_PM, linearStepsPerMm);
  prefs.putFloat(PREFS_KEY_LINEAR_OFFSET, linearOffsetSteps);
  prefs.end();
}

static void asdaSyncConfig()
{
  String resp;
  if (!asdaHttp("GET", "/api/config", nullptr, resp, ASDA_HTTP_TIMEOUT_MS))
    return;
  const float spm = clampLinearStepsPerMm(
      jsonExtractFloat(resp, "stepsPerMm", linearStepsPerMm));
  const float off = clampLinearOffsetSteps(
      jsonExtractFloat(resp, "offsetSteps", linearOffsetSteps));
  const bool calChanged = (fabsf(spm - linearStepsPerMm) > 0.0005f)
                       || (fabsf(off - linearOffsetSteps) > 0.05f);
  linearStepsPerMm = spm;
  linearOffsetSteps = off;
  if (calChanged)
  {
    saveLinearCalToNvs();
    DBG_PRINTF("ASDA sync cal → NVS spm=%.3f off=%.1f\n", linearStepsPerMm, linearOffsetSteps);
  }
  // moveRpm del esclavo es default de banco; la receta TCM (linearRpm / BASE) manda.
}

static void asdaEnsureHomeAtBoot()
{
  if (WiFi.status() != WL_CONNECTED) return;
  // Sync cal una vez (o hasta OK); no cada 8 s para siempre.
  static bool calSynced = false;
  if (!calSynced)
  {
    static uint32_t lastSyncTry = 0;
    if (lastSyncTry == 0 || (millis() - lastSyncTry) > 8000)
    {
      lastSyncTry = millis();
      asdaSyncConfig();
      calSynced = true;  // aunque falle parcialmente: no martillar /api/config
    }
  }
  if (asdaHomed || asdaBusy) return;
  static uint32_t lastTry = 0;
  if (lastTry != 0 && (millis() - lastTry) < 5000) return;
  lastTry = millis();
  if (!asdaOn())
  {
    Serial.println("LINEAR: ASDA ON falló (reintento)");
    return;
  }
  if (!asdaStartHomeTorque())
    Serial.println("LINEAR: HOME torque no disparado");
  else
    Serial.println("LINEAR: HOME torque (origen +) disparado");
}

// ============================================================
// SECCION 11 — Ciclo lineal (no bloqueante)
// ============================================================
void cycleStartBackToHome()
{
  if (posPulses == 0)
  {
    cycleState = C_IDLE;
    cycleRunning = false;
    DBG_PRINTLN("LINEAR: ya en HOME");
    return;
  }

  const uint32_t backPulses = posPulses;
  pulsesRemaining = backPulses;
  lastBackCommandPulses = backPulses;
  cycleState = C_BACK;
  cycleRunning = true;
  if (!linearStartAbsSteps(0))
    return;
  DBG_PRINT("LINEAR: BACK start pulses=");
  DBG_PRINTLN(backPulses);
}

void cycleStartBackwardPulses(uint32_t pulses)
{
  if (pulses == 0) return;
  if (pulses > posPulses) pulses = posPulses;
  if (pulses == 0)
  {
    cycleState = C_IDLE;
    cycleRunning = false;
    return;
  }

  pulsesRemaining = pulses;
  lastBackCommandPulses = pulses;
  cycleState = C_BACK;
  cycleRunning = true;
  uint32_t target = posPulses - pulses;
  if (!linearStartAbsSteps(target))
    return;
  DBG_PRINT("LINEAR: BACK pulses=");
  DBG_PRINTLN(pulses);
}

void cycleStartForwardPulses(uint32_t pulses, bool skipDwellAtDest)
{
  lastFwdCommandPulses = pulses;
  skipDwellAtDestNextFwd = skipDwellAtDest;
  if (pulses == 0)
  {
    DBG_PRINTLN("WARN: cycleStartForwardPulses(0)");
    return;
  }

  pulsesRemaining = pulses;
  cycleState = C_FWD;
  cycleRunning = true;
  if (!linearStartAbsSteps(posPulses + pulses))
    return;
  DBG_PRINT("LINEAR: FWD pulses=");
  DBG_PRINTLN(pulses);
}

void serviceCycle()
{
  if (!cycleRunning) return;

  if (cycleState == C_DWELL)
  {
    if ((int32_t)(millis() - dwellUntilMs) >= 0)
    {
      cycleState = C_IDLE;
      cycleRunning = false;
      DBG_PRINTLN("LINEAR: FWD done (sin retorno)");
    }
    return;
  }

  if (cycleState == C_FWD || cycleState == C_BACK)
  {
    linearService();
    if (asdaBusy && !asdaPulseDone) return;
    asdaPulseDone = false;
    pulsesRemaining = 0;
  }

  if (pulsesRemaining == 0)
  {
    if (cycleState == C_FWD)
    {
      posPulses += lastFwdCommandPulses;
      if (skipDwellAtDestNextFwd || dwellAtDestMs == 0)
      {
        skipDwellAtDestNextFwd = false;
        cycleState = C_IDLE;
        cycleRunning = false;
        DBG_PRINTLN("LINEAR: FWD done (sin dwell)");
      }
      else
      {
        dwellUntilMs = millis() + dwellAtDestMs;
        cycleState = C_DWELL;
      }
    }
    else if (cycleState == C_BACK)
    {
      if (lastBackCommandPulses >= posPulses)
        posPulses = 0;
      else
        posPulses -= lastBackCommandPulses;
      lastBackCommandPulses = 0;
      cycleState = C_IDLE;
      cycleRunning = false;
      DBG_PRINT("LINEAR: BACK done, posPulses=");
      DBG_PRINTLN(posPulses);
    }
  }
}

static void waitCycleFinishTick(uint32_t& lastCanMs, uint32_t& lastWebMs, bool requireCycleActive)
{
  if (cycleAborted || (requireCycleActive && !cycleActive))
  {
    if (cycleRunning || linearIsMoving())
    {
      linearAbort();
      cycleState = C_IDLE;
      cycleRunning = false;
    }
    return;
  }

  serviceCycle();
  linearService();

  if (!linearIsMoving() || feedPhaseIsActive())
    serviceServoFeed();

  serviceBackgroundTick(lastCanMs, lastWebMs);

  if (!linearIsMoving() && !cycleRunning)
    honorPausePendingIfIdle("lineal-idle");

  while (cyclePaused && cycleActive)
  {
    linearPause();
    peerService();
    peerTryReconnect();
    serviceExternalStopInput();
    serviceCycle();
    serviceCANRx();
    if (!linearIsMoving() || feedPhaseIsActive())
      serviceServoFeed();
    serviceBackgroundTick(lastCanMs, lastWebMs);
    delay(5);
  }
  linearResume();
  yield();
}

void waitCycleFinish()
{
  uint32_t lastCanMs = millis();
  uint32_t lastWebMs = millis();
  uint32_t cmdPulses = pulsesRemaining;
  if (cmdPulses == 0)
  {
    if (cycleState == C_BACK)
      cmdPulses = lastBackCommandPulses;
    else
      cmdPulses = lastFwdCommandPulses;
  }
  const uint32_t timeoutMs = linearMoveTimeoutMs(cmdPulses);
  uint32_t activeElapsed = 0;
  uint32_t lastTick = millis();
  const bool requireCycleActive = cycleActive;

  while (cycleRunning)
  {
    if (requireCycleActive && !cycleActive)
    {
      linearAbort();
      cycleState = C_IDLE;
      cycleRunning = false;
      break;
    }

    const uint32_t now = millis();
    if (!cyclePaused)
      activeElapsed += (now - lastTick);
    lastTick = now;

    if (!cyclePaused && activeElapsed > timeoutMs)
    {
      Serial.printf("LINEAR TIMEOUT: %lu ms (cmd=%lu pulsos)\n",
                    (unsigned long)activeElapsed, (unsigned long)cmdPulses);
      if (requireCycleActive)
        faultStopCycle("lineal_timeout");
      else
      {
        linearAbort();
        cycleState = C_IDLE;
        cycleRunning = false;
        Serial.println("LINEAR TIMEOUT (manual) — movimiento cancelado");
      }
      break;
    }

    waitCycleFinishTick(lastCanMs, lastWebMs, requireCycleActive);
  }
}

static void waitLinearFinish()
{
  waitCycleFinish();
}

static void linearMoveToPulses(uint32_t targetPulses)
{
  if (targetPulses == posPulses) return;

  cyclePaused = false;

  if (targetPulses > posPulses)
  {
    cycleStartForwardPulses(targetPulses - posPulses, false);
    waitLinearFinish();
    return;
  }

  cycleStartBackwardPulses(posPulses - targetPulses);
  waitLinearFinish();
}

static bool linearRunTorqueHome()
{
  cyclePaused = false;
  if (asdaBusy)
  {
    lastBackCommandPulses = 0;
    pulsesRemaining = 1;
    cycleState = C_BACK;
    cycleRunning = true;
    waitLinearFinish();
    if (asdaHomed)
      return true;
  }
  pulsesRemaining = 1;
  lastBackCommandPulses = posPulses;
  cycleState = C_BACK;
  cycleRunning = true;
  asdaPulseDone = false;
  if (!asdaStartHomeTorque())
  {
    cycleState = C_IDLE;
    cycleRunning = false;
    return false;
  }
  waitLinearFinish();
  posPulses = asdaHomed ? 0 : puuToCommandSteps(asdaLastPuu);
  return asdaHomed;
}

// ============================================================
// SECCION 12 — Servo feeder (cooperativo)
// ============================================================
static int32_t feedAllowedDeviation(int32_t target)
{
  if (target <= 0) return 0;

  int32_t dev = 0;
  if (feedStepsTolerancePercent > 0)
  {
    dev = (int32_t)((int64_t)target * feedStepsTolerancePercent / 100);
    if (dev < 1) dev = 1;
  }
  if (dev == 0)
    dev = feedStepsTolerance;
  else if (feedStepsTolerance > dev)
    dev = feedStepsTolerance;
  return dev;
}

static bool feedStepsWithinTolerance(int32_t fed, int32_t target)
{
  if (target <= 0) return true;
  int32_t dev = feedAllowedDeviation(target);
  if (dev <= 0)
    return fed >= target;
  int32_t lo = target - dev;
  if (lo < 0) lo = 0;
  return fed >= lo && fed <= (target + dev);
}

static int32_t feedTargetLo(int32_t target)
{
  if (target <= 0) return 0;
  int32_t lo = target - feedAllowedDeviation(target);
  return lo < 0 ? 0 : lo;
}

static bool feedEncoderAtLeastMin(int32_t fed, int32_t target)
{
  if (target <= 0) return true;
  return fed >= feedTargetLo(target);
}

static bool feedEncoderValidationEnabled()
{
  return !feedSkipEncoderConfirm;
}

static bool feedEncoderTargetReached(int32_t fed, int32_t target)
{
  if (target <= 0) return true;
  if (!feedEncoderValidationEnabled()) return true;
  return feedStepsWithinTolerance(fed, target);
}

// BYPASS legacy: basta alcanzar el mínimo. Overshoot = material de sobra = OK.
// Evita E011 cuando el feed físico ya cumplió pero el encoder pasó del tope alto.
static bool feedEncoderEnough(int32_t fed, int32_t target)
{
  if (target <= 0) return true;
  if (!feedEncoderValidationEnabled()) return true;
  return feedEncoderAtLeastMin(fed, target);
}

// BYPASS legacy: settle corto al cerrar feed (antes del handoff a pinzas).
static uint32_t feedDoneSettleMs()
{
  return FEED_DONE_SETTLE_MS;
}

static void feedSyncStepsFromEncoder()
{
  if (!feedEncoderTracking) return;

  int32_t pos = 0;
  if (!canReadSdoI32(SERVO_NODE_L, 0x6064, 0x00, pos, 150)) return;

  feedStepsFed = feedEncoderAbsDelta(feedStartPos601, pos);
  feedBypassRemaining = feedStepsTargetThisFeed - feedStepsFed;
  if (feedBypassRemaining < 0) feedBypassRemaining = 0;
}

static void feedCaptureStartPosition()
{
  if (feedSkipEncoderConfirm)
  {
    feedEncoderTracking = false;
    return;
  }
  feedEncoderTracking = canReadSdoI32(SERVO_NODE_L, 0x6064, 0x00, feedStartPos601, 150);
}

static void feedAbort(const char* reason)
{
  Serial.println(reason);
  feedPhase = FEED_ERROR;
  if (feedCalibrationTest)
  {
    // Motivo para /feedTestSensor; no enclava falla de producción.
    if (reason && reason[0])
    {
      strncpy(cycleFaultReason, reason, CYCLE_PAUSE_REASON_MAX - 1);
      cycleFaultReason[CYCLE_PAUSE_REASON_MAX - 1] = '\0';
    }
    return;
  }
  if (!feedPrefetch)
  {
    if (cycleErrorCode == E000)
    {
      // Sensor manguera GPIO: E016 solo si Laser Validation está ON.
      if (laserValidationEnabled
          && reason
          && (strstr(reason, "sensor no detectado") != nullptr
              || strstr(reason, "laser ocupado") != nullptr))
        setCycleError(E016, "feed_hose");
      else
        setCycleError(E012, reason);  // Alimentación fallida / incompleta
    }
    cycleActive = false;
  }
}

static void feedMarkDone()
{
  feedSyncStepsFromEncoder();
  if (feedModeThisCycle == FEED_MODE_STEPS_SENSOR && !feedCalibrationTest && !feedSensorConfirmed)
  {
    feedPhase = FEED_RUNNING;
    if (feedSolidTargetMet && !feedSensorArmed)
      feedArmHoseSensor();
    else
      feedPhaseStartMs = millis();
    return;
  }
  feedPhase = FEED_DONE;
}

// Láser en vivo. Con Laser Validation OFF se ignora por completo.
static bool feedHoseSensorReadyNow()
{
  if (!laserValidationEnabled)
    return true;
  if (feedModeThisCycle != FEED_MODE_STEPS_SENSOR)
    return true;
  bool needL = feedNeedSensorL;
  bool needR = feedNeedSensorR;
  if (!needL && !needR)
  {
    needL = feedTestSolidSteps > 0;
    needR = feedTestSolidStepsR > 0;
  }
  if (needL && !isHoseSensorActiveSide(false)) return false;
  if (needR && !isHoseSensorActiveSide(true)) return false;
  return true;
}

bool feedThisCycleSucceeded()
{
  if (feedPhase != FEED_DONE) return false;
  if (feedModeThisCycle == FEED_MODE_STEPS_SENSOR)
  {
    if (!feedOmLengthMet) return false;
    if (laserValidationEnabled)
    {
      if (!feedLaserSeen) return false;
      if (!feedLaserValidated)
      {
        feedLaserValidated = true;
        asdaOverviewMark("laserOk");
      }
    }
    else if (!feedLaserValidated)
    {
      feedLaserValidated = true;
    }
    return true;
  }
  return true;
}

static bool feedPhaseIsActive()
{
  return feedPhase != FEED_IDLE && feedPhase != FEED_DONE && feedPhase != FEED_ERROR;
}

static void feedBeginStepsSensorMode2(bool chunksOnly);
static bool feedDeferForLinearMotion();
static void feedIssueMove(int32_t steps);
static int32_t feedPositiveMoveSteps(int32_t steps);
static void feedApplyMotionProfile();
static bool feedServosSettled(uint32_t now);

static int32_t feedSsEncoderFedSide(bool sideR)
{
  if (sideR)
  {
    if (!feedSsEncTrackR) return 0;
    int32_t pos = 0;
    if (!canReadSdoI32(SERVO_NODE_R, 0x6064, 0x00, pos, 150)) return 0;
    return feedEncoderAbsDelta(feedSsEncStartPosR, pos);
  }
  if (!feedSsEncTrackL) return feedStepsFed;
  int32_t pos = 0;
  if (!canReadSdoI32(SERVO_NODE_L, 0x6064, 0x00, pos, 150)) return feedStepsFed;
  return feedEncoderAbsDelta(feedSsEncStartPosL, pos);
}

// true = prueba por un solo lado: no copiar pasos del otro eje si van en 0.
static bool feedTestExactSides = false;

static void feedIssueMoveLR(int32_t stepsL, int32_t stepsR)
{
  stepsL = feedPositiveMoveSteps(stepsL);
  stepsR = feedPositiveMoveSteps(stepsR);
  if (stepsL <= 0 && stepsR <= 0) return;
  // Alimentación: lados independientes (L/R). No copiar pasos al otro eje.
  if (!feedTestExactSides && feedModeThisCycle != FEED_MODE_STEPS_SENSOR)
  {
    if (stepsL <= 0) stepsL = stepsR;
    if (stepsR <= 0) stepsR = stepsL;
  }
  canMoveRelativePP(stepsL, stepsR);
  feedHaltSent = false;
}

static uint32_t feedSsCmdTimeMs(int32_t steps, uint32_t vel)
{
  if (steps <= 0) return 0;
  if (vel < 1) vel = 1;
  const uint32_t acc = feedProfileAccForVel(vel);
  // s_accel = v² / (2a). Si el tramo es más corto, aún no hay crucero: t = sqrt(2s/a).
  const uint64_t sAcc = ((uint64_t)vel * (uint64_t)vel) / (2ULL * (uint64_t)acc);
  if ((uint64_t)steps <= sAcc)
  {
    const double tMs = 1000.0 * sqrt((2.0 * (double)steps) / (double)acc);
    uint32_t ms = (uint32_t)(tMs + 0.5);
    return ms < 1u ? 1u : ms;
  }
  const uint32_t tAccMs = (uint32_t)(((uint64_t)vel * 1000ULL) / (uint64_t)acc);
  const uint32_t tCruiseMs = (uint32_t)((((uint64_t)steps - sAcc) * 1000ULL) / (uint64_t)vel);
  return tAccMs + tCruiseMs;
}

static void feedSsArmAbsDueMs(uint32_t now, uint32_t cmdMs)
{
  feedSsMoveStartMs = now;
  feedSsAbsDueMs = now + cmdMs + 40u;
  feedSsLastTrPollMs = 0;
}

static uint32_t feedSsAbsTravelSteps(bool sideR)
{
  const int32_t start = sideR ? feedSsEncStartPosR : feedSsEncStartPosL;
  const int32_t tgt = sideR ? feedSsAbsTargetR : feedSsAbsTargetL;
  int32_t d = tgt - start;
  if (d < 0) d = -d;
  return (uint32_t)d;
}

static void feedSsDisarmHalt()
{
  feedHaltArmL = 0;
  feedHaltArmR = 0;
  feedHoseIrqL = 0;
  feedHoseIrqR = 0;
  feedSsHaltMsL = 0;
  feedSsHaltMsR = 0;
  feedSsNeedClearL = 0;
  feedSsNeedClearR = 0;
}

static void feedSsLatchAndHalt(bool sideR)
{
  portENTER_CRITICAL(&feedHaltMux);
  const bool armed = sideR ? (feedHaltArmR && !feedSensorConfirmedR)
                           : (feedHaltArmL && !feedSensorConfirmedL);
  portEXIT_CRITICAL(&feedHaltMux);
  if (!armed) return;
  if (!sendCanHaltImmediate(sideR)) return;

  portENTER_CRITICAL(&feedHaltMux);
  if (sideR)
  {
    feedHaltArmR = 0;
    feedSensorConfirmedR = true;
  }
  else
  {
    feedHaltArmL = 0;
    feedSensorConfirmedL = true;
  }
  portEXIT_CRITICAL(&feedHaltMux);
  if (sideR) feedSsHaltMsR = millis();
  else feedSsHaltMsL = millis();
}

// Halt por decisión OM — solo abort/emergencia legacy; Modelo S no usa en movimiento normal.
static void feedSsHaltFromOmLength()
{
  if (feedOmLengthMet) return;
  feedSsHaltFromSensorNow();
  feedOmLengthMet = true;
  feedSyncSensorConfirmedAll();
  asdaOverviewMark("omFeed");
  if (feedPrefetch)
    feedPrefetchSensorLatched = true;
  feedSolidTargetMet = true;
  feedPhase = FEED_DONE;
}

// Solo monitoreo OM durante el movimiento (no HALT, no control de trayectoria).
static void feedSsPollOmMonitor(uint32_t now)
{
  if (feedPhase == FEED_DONE || feedPhase == FEED_ERROR || feedPhase == FEED_IDLE)
    return;
  if (feedOmLastPollMs != 0 && (now - feedOmLastPollMs) < (uint32_t)ASDA_OM_FEED_POLL_MS)
    return;
  feedOmLastPollMs = now;
  float actual = 0.0f;
  if (asdaOmReadMm(&actual))
    feedOmLastMmAbs = actual;
}

// Latch rising edge del láser + posición OM. Nunca manda HALT.
static void feedSsPollLaserLatch()
{
  if (!laserValidationEnabled)
  {
    feedSsSensorPrevL = isHoseSensorActiveSide(false);
    feedSsSensorPrevR = isHoseSensorActiveSide(true);
    feedHoseIrqL = 0;
    feedHoseIrqR = 0;
    return;
  }
  if (feedPhase == FEED_IDLE || feedPhase == FEED_ERROR)
    return;

  const bool nowL = isHoseSensorActiveSide(false);
  const bool nowR = isHoseSensorActiveSide(true);
  bool edge = false;
  if (!feedLaserSeen)
  {
    if (feedNeedSensorL && !feedSsSensorPrevL && nowL) edge = true;
    if (feedNeedSensorR && !feedSsSensorPrevR && nowR) edge = true;
    if (feedHoseIrqL || feedHoseIrqR) edge = true;
  }
  feedSsSensorPrevL = nowL;
  feedSsSensorPrevR = nowR;
  feedHoseIrqL = 0;
  feedHoseIrqR = 0;
  if (!edge || feedLaserSeen) return;

  float mm = feedOmLastMmAbs;
  if (mm < 0.0f)
  {
    if (!asdaOmReadMm(&mm))
      mm = 0.0f;
  }
  feedLaserSeen = true;
  omAtLaserMm = mm;
  DBG_PRINTF("FEED: laser latch OM=%.2f mm (no HALT)\n", omAtLaserMm);
}

static void feedSsHaltFromSensorNow()
{
  if (feedHaltArmL && !feedSensorConfirmedL)
  {
    feedHoseIrqL = 0;
    const bool active = isHoseSensorActiveSide(false);
    if (feedSsNeedClearL)
    {
      if (!active) feedSsNeedClearL = 0;
    }
    else if (active)
      feedSsLatchAndHalt(false);
  }
  if (feedHaltArmR && !feedSensorConfirmedR)
  {
    feedHoseIrqR = 0;
    const bool active = isHoseSensorActiveSide(true);
    if (feedSsNeedClearR)
    {
      if (!active) feedSsNeedClearR = 0;
    }
    else if (active)
      feedSsLatchAndHalt(true);
  }
  feedSyncSensorConfirmedAll();
  if (feedPrefetch && feedSensorConfirmed)
    feedPrefetchSensorLatched = true;
  if (feedSensorConfirmed
      && (feedPhase == FEED_SS_SOLID || feedPhase == FEED_RUNNING))
  {
    feedSolidTargetMet = true;
    feedPhase = FEED_DONE;
  }
}

static void feedHaltUrgentTask(void* arg)
{
  (void)arg;
  for (;;)
  {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    feedSsHaltFromSensorNow();
  }
}

static void feedHaltTaskStart()
{
  if (feedHaltTaskHandle) return;
  xTaskCreatePinnedToCore(feedHaltUrgentTask, "feedHalt", 3072, nullptr,
                          configMAX_PRIORITIES - 1, &feedHaltTaskHandle, 1);
}

static void feedSsArmRisingEdge()
{
  feedHoseIrqL = 0;
  feedHoseIrqR = 0;
  feedSsHaltMsL = 0;
  feedSsHaltMsR = 0;
  feedOmLengthMet = false;
  feedLaserValidated = false;
  feedLaserSeen = false;
  omAtLaserMm = 0.0f;
  feedOmLastMmAbs = -1.0f;
  feedOmLastPollMs = 0;
  omClearPieceValidation();

  // OM decide el halt. Láser nunca arma cierre CAN.
  feedHaltArmL = 0;
  feedHaltArmR = 0;
  feedSsSensorPrevL = isHoseSensorActiveSide(false);
  feedSsSensorPrevR = isHoseSensorActiveSide(true);
  // Prefetch / cal: esperar láser libre solo si Laser Validation ON.
  if (laserValidationEnabled && (feedPrefetch || feedCalibrationTest))
  {
    feedSsNeedClearL = (feedNeedSensorL && feedSsSensorPrevL) ? 1 : 0;
    feedSsNeedClearR = (feedNeedSensorR && feedSsSensorPrevR) ? 1 : 0;
  }
  else
  {
    feedSsNeedClearL = 0;
    feedSsNeedClearR = 0;
  }
}

static bool feedSsTryFinishIfTriggered()
{
  if (feedOmLengthMet)
  {
    feedSolidTargetMet = true;
    feedPhase = FEED_DONE;
    return true;
  }
  // Solo láser legacy si aún hubiera arms (no debería en OM-mode).
  if (feedHaltArmL || feedHaltArmR)
  {
    feedSsHaltFromSensorNow();
    if (!feedSensorConfirmed)
      return false;
    feedSolidTargetMet = true;
    feedPhase = FEED_DONE;
    return true;
  }
  return false;
}

static void feedSsServicePrefetchLaserClear()
{
  if (!feedSsNeedClearL && !feedSsNeedClearR) return;
  if (feedSsNeedClearL && feedNeedSensorL && !isHoseSensorActiveSide(false))
    feedSsNeedClearL = 0;
  if (feedSsNeedClearR && feedNeedSensorR && !isHoseSensorActiveSide(true))
    feedSsNeedClearR = 0;
}

static void feedSsBeginStepsSensor(bool chunksOnly)
{
  feedStepsTargetThisFeed = feedTestSolidSteps;
  if (feedTestExactSides)
    feedStepsTargetB = feedTestSolidStepsR > 0 ? feedTestSolidStepsR : 0;
  else
    feedStepsTargetB = feedTestSolidStepsR > 0 ? feedTestSolidStepsR : feedTestSolidSteps;
  feedChunkTargetThisFeed = feedTestChunkSteps;
  if (feedTestExactSides)
    feedChunkTargetB = feedTestChunkStepsR > 0 ? feedTestChunkStepsR : 0;
  else
    feedChunkTargetB = feedTestChunkStepsR > 0 ? feedTestChunkStepsR : feedTestChunkSteps;

  feedNeedSensorL = (feedStepsTargetThisFeed > 0);
  feedNeedSensorR = (feedStepsTargetB > 0);

  if (feedNeedSensorL)
  {
    const float tMm = feedStepsToMm(feedStepsTargetThisFeed, false);
    const float vMm = feedPpToMmS(feedSsFastPpL, false);
    if (!feedProfileSideValid(tMm, vMm, false))
    {
      feedAbort("FEED: perfil L invalido (Target/Velocity)");
      return;
    }
  }
  if (feedNeedSensorR)
  {
    const float tMm = feedStepsToMm(feedStepsTargetB, true);
    const float vMm = feedPpToMmS(feedSsFastPpR, true);
    if (!feedProfileSideValid(tMm, vMm, true))
    {
      feedAbort("FEED: perfil R invalido (Target/Velocity)");
      return;
    }
  }

  feedSensorConfirmedL = !feedNeedSensorL;
  feedSensorConfirmedR = !feedNeedSensorR;
  feedSyncSensorConfirmedAll();
  feedCanPrimeHaltDecel();
  feedSsArmRisingEdge();

  feedSsAbsTargetL = 0;
  feedSsAbsTargetR = 0;

  feedSsEncTrackL = false;
  feedSsEncTrackR = false;
  if (feedNeedSensorL)
  {
    feedSsEncTrackL = canReadSdoI32(SERVO_NODE_L, 0x6064, 0x00, feedSsEncStartPosL, 150);
    if (!feedSsEncTrackL)
    {
      feedAbort("FEED: no se pudo leer 6064 L");
      return;
    }
    feedSsAbsTargetL = feedSsEncStartPosL
        + (int32_t)SERVO_CMD_SIGN_L * feedStepsTargetThisFeed;
  }
  if (feedNeedSensorR)
  {
    feedSsEncTrackR = canReadSdoI32(SERVO_NODE_R, 0x6064, 0x00, feedSsEncStartPosR, 150);
    if (!feedSsEncTrackR)
    {
      feedAbort("FEED: no se pudo leer 6064 R");
      return;
    }
    feedSsAbsTargetR = feedSsEncStartPosR
        + (int32_t)SERVO_CMD_SIGN_R * feedStepsTargetB;
  }
  feedSsEncTrack = feedSsEncTrackL || feedSsEncTrackR;
  feedEncoderTracking = feedSsEncTrack;
  if (feedSsEncTrackL)
    feedStartPos601 = feedSsEncStartPosL;

  const uint32_t now = millis();
  feedPhaseStartMs = now;
  feedSsAbsDueMs = 0;
  feedSsMoveStartMs = 0;
  feedSsLastTrPollMs = 0;

  if (chunksOnly || (feedStepsTargetThisFeed <= 0 && feedStepsTargetB <= 0))
  {
    feedSolidTargetMet = true;
    feedPhase = FEED_RUNNING;
    return;
  }

  feedSolidTargetMet = false;
  feedPhase = FEED_SS_SOLID;
}

static void feedInitPrefetchSensorState()
{
  feedSawHoseCleared = false;
}

static bool feedSideEncoderOk(bool sideR)
{
  const bool track = sideR ? feedSsEncTrackR : feedSsEncTrackL;
  if (!track) return true;
  const int32_t solid = sideR ? feedStepsTargetB : feedStepsTargetThisFeed;
  if (solid <= 0) return true;
  return feedEncoderAtLeastMin(feedSsEncoderFedSide(sideR), solid);
}

static uint32_t feedSsSensorTimeoutMs()
{
  uint32_t t = (uint32_t)FEED_TEST_SENSOR_TIMEOUT_MS;
  if (feedSsMoveStartMs != 0 && feedSsAbsDueMs > feedSsMoveStartMs)
  {
    const uint32_t due = (feedSsAbsDueMs - feedSsMoveStartMs) + 100u;
    if (due > t) t = due;
  }
  if (t > (uint32_t)FEED_WAIT_TIMEOUT_MS) t = (uint32_t)FEED_WAIT_TIMEOUT_MS;
  return t;
}

// Target Reached (6041 bit 10) = fin normal Modelo S; luego validar OM ±1 mm.
static bool feedSsCompleteOnTargetReached(uint32_t now)
{
  if (feedSsAbsDueMs == 0 || now < feedSsAbsDueMs) return false;
  if (feedSsLastTrPollMs != 0 && (now - feedSsLastTrPollMs) < 80u) return false;
  feedSsLastTrPollMs = now;
  if (!servoCanReady) return false;

  bool hitL = false;
  bool hitR = false;
  if (feedNeedSensorL && !feedSensorConfirmedL)
  {
    uint16_t sw = 0;
    if (canReadStatusWord(SERVO_NODE_L, sw, 50) && (sw & 0x0400))
      hitL = true;
  }
  if (feedNeedSensorR && !feedSensorConfirmedR)
  {
    uint16_t sw = 0;
    if (canReadStatusWord(SERVO_NODE_R, sw, 50) && (sw & 0x0400))
      hitR = true;
  }
  if (feedNeedSensorL && !feedSensorConfirmedL && !hitL) return false;
  if (feedNeedSensorR && !feedSensorConfirmedR && !hitR) return false;
  if (!hitL && !hitR) return false;

  float omMm = 0.0f;
  if (!asdaOmReadMm(&omMm))
  {
    if (feedOmLastMmAbs >= 0.0f)
      omMm = feedOmLastMmAbs;
  }
  omPhase1Mm = omMm;
  feedOmLastMmAbs = omMm;
  feedOmLengthMet = true;
  if (feedNeedSensorL) feedSensorConfirmedL = true;
  if (feedNeedSensorR) feedSensorConfirmedR = true;
  feedSyncSensorConfirmedAll();
  feedSolidTargetMet = true;
  feedPhase = FEED_DONE;
  asdaOverviewMark("feedTr");
  DBG_PRINTF("FEED: Target Reached — OM %.2f mm (obj %.1f)%s\n",
             omMm, feedOmNominalTargetMm(), feedCalibrationTest ? " (cal)" : "");
  return true;
}

static bool feedWaitingPrefetchClear()
{
  return (feedSsNeedClearL && feedNeedSensorL && !feedSensorConfirmedL)
      || (feedSsNeedClearR && feedNeedSensorR && !feedSensorConfirmedR);
}

static void serviceFeedStepsSensor(uint32_t now)
{
  feedSsServicePrefetchLaserClear();
  feedSsPollLaserLatch();
  feedSsPollOmMonitor(now);
  if (feedPhase == FEED_DONE) return;

  if (feedSsTryFinishIfTriggered())
    return;

  if (feedSsCompleteOnTargetReached(now))
    return;

  const bool waitingClear = feedWaitingPrefetchClear();
  if (feedSsMoveStartMs != 0 && !waitingClear
      && now - feedSsMoveStartMs > feedSsSensorTimeoutMs())
  {
    feedAbort("FEED: TIMEOUT - Target Reached no confirmado");
    return;
  }

  if (feedPhase == FEED_SS_SOLID)
  {
    if (waitingClear)
      return;
    if (feedDeferForLinearMotion()) return;
    const uint32_t velL = clampFeedSsSpeedPp(feedSsFastPpL);
    const uint32_t velR = clampFeedSsSpeedPp(feedSsFastPpR);
    feedCanSetVelAcc(velL, velR);
    canAbsImmediateUpdate(feedSsAbsTargetL, feedSsAbsTargetR, velL, velR,
                          feedNeedSensorL, feedNeedSensorR, true);
    if (feedPhase == FEED_DONE) return;
    feedHaltSent = false;
    feedSolidTargetMet = true;
    uint32_t tL = 0;
    uint32_t tR = 0;
    if (feedNeedSensorL && !feedSensorConfirmedL)
      tL = feedSsCmdTimeMs(feedStepsTargetThisFeed, velL);
    if (feedNeedSensorR && !feedSensorConfirmedR)
      tR = feedSsCmdTimeMs(feedStepsTargetB, velR);
    feedSsArmAbsDueMs(now, tL > tR ? tL : tR);
    if (feedPhase != FEED_DONE)
      feedPhase = FEED_RUNNING;
    return;
  }

  if (feedPhase == FEED_RUNNING)
  {
    (void)feedSsCompleteOnTargetReached(now);
  }
}

static void feedBeginStepsSensorMode2(bool chunksOnly)
{
  feedSsBeginStepsSensor(chunksOnly);
}

// El lineal (RMT) y el feeder (CAN) son independientes; en prefetch deben correr en paralelo.
static bool feedDeferForLinearMotion()
{
  if (feedCalibrationTest || feedPrefetch) return false;
  return linearIsMoving();
}

static void feedResumeIfIncompleteDone()
{
  if (feedPhase == FEED_ERROR && feedPrefetch && cycleActive)
  {
    feedSensorConfirmed = false;
    feedSensorConfirmedL = !feedNeedSensorL;
    feedSensorConfirmedR = !feedNeedSensorR;
    if (feedModeThisCycle == FEED_MODE_BYPASS)
    {
      // BYPASS legacy: reanudar en BYPASS (SS_CHUNKS no se atiende fuera del feed OM → E011).
      feedPhase = FEED_BYPASS;
      feedPhaseStartMs = millis();
      return;
    }
    if (feedModeThisCycle == FEED_MODE_STEPS_SENSOR && feedTestSolidSteps > 0 && !feedSolidTargetMet)
    {
      feedSensorArmed = false;
      feedPhase = FEED_SS_SOLID;
    }
    else
    {
      feedPhase = FEED_RUNNING;
      feedPhaseStartMs = millis();
    }
    return;
  }

  if (feedPhase != FEED_DONE) return;
  if (feedThisCycleSucceeded()) return;

  feedSensorConfirmed = false;
  feedSensorConfirmedL = !feedNeedSensorL;
  feedSensorConfirmedR = !feedNeedSensorR;
  if (feedModeThisCycle == FEED_MODE_BYPASS)
  {
    feedPhase = FEED_BYPASS;
    feedPhaseStartMs = millis();
    return;
  }
  if (feedModeThisCycle == FEED_MODE_STEPS_SENSOR && feedTestSolidSteps > 0 && !feedSolidTargetMet)
  {
    feedSensorArmed = false;
    feedPhase = FEED_SS_SOLID;
  }
  else
  {
    feedPhase = FEED_RUNNING;
    feedPhaseStartMs = millis();
  }
}

static bool feedRetryStep2IfNeeded()
{
  if (feedThisCycleSucceeded()) return true;
  if (feedPhaseIsActive())
  {
    waitServoFeedStep2();
    if (feedThisCycleSucceeded()) return true;
    if (!cycleActive) return false;
  }
  if (feedPhase == FEED_DONE || feedPhase == FEED_ERROR)
    feedPhase = FEED_IDLE;
  cycleActive = true;
  bool chunksOnly = (feedModeThisCycle == FEED_MODE_STEPS_SENSOR && feedSolidTargetMet);
  runServoFeedStep2(chunksOnly);
  return feedThisCycleSucceeded();
}

static void feedConsumeReadyMaterial()
{
  feedPhase = FEED_IDLE;
  feedPrefetch = false;
  feedPrefetchSensorLatched = false;
}

static void feedApplyMotionProfile()
{
  feedCanSetVelAcc((uint32_t)feedServoBasePpA, (uint32_t)feedServoBasePpB);
  feedProfileApplied = true;
}

static void feedEnsureMotionProfile()
{
  if (!feedProfileApplied)
    feedApplyMotionProfile();
}

static bool feedServoSideHalted(bool sideR)
{
  return sideR ? (feedSensorConfirmedR || feedSsHaltMsR != 0)
               : (feedSensorConfirmedL || feedSsHaltMsL != 0);
}

static bool feedServoTargetReachedNode(uint8_t nodeId)
{
  uint16_t sw = 0;
  if (!canReadStatusWord(nodeId, sw, 80)) return false;
  return (sw & 0x0400) != 0;
}

static bool feedServosTargetReached(bool needL = true, bool needR = true)
{
  if (!servoCanReady) return true;
  // Solo esperar lados que aún alimentan (el otro puede estar en halt).
  if (feedModeThisCycle == FEED_MODE_STEPS_SENSOR
      && (feedPhase == FEED_SS_SOLID || feedPhase == FEED_RUNNING || feedPhase == FEED_DONE))
  {
    if (needL && feedNeedSensorL && !feedServoSideHalted(false)
        && !feedServoTargetReachedNode(SERVO_NODE_L))
      return false;
    if (needR && feedNeedSensorR && !feedServoSideHalted(true)
        && !feedServoTargetReachedNode(SERVO_NODE_R))
      return false;
    return true;
  }
  if (needL && !feedServoSideHalted(false) && !feedServoTargetReachedNode(SERVO_NODE_L))
    return false;
  if (needR && !feedServoSideHalted(true) && !feedServoTargetReachedNode(SERVO_NODE_R))
    return false;
  return true;
}

// Tras el settle, no bloquear indefinidamente si el CAN no responde (p. ej. prefetch + lineal).
static bool feedServosSettled(uint32_t now)
{
  if (feedServosTargetReached()) return true;
  return now >= feedDelayUntilMs + FEED_SERVO_SETTLE_MAX_MS;
}

static int32_t feedPositiveMoveSteps(int32_t steps)
{
  if (steps <= 0) return 0;
  return steps;
}

static void feedWaitServosIdle(uint32_t timeoutMs = 2500)
{
  uint32_t start = millis();
  while (millis() - start < timeoutMs)
  {
    if (feedServosTargetReached()) return;
    serviceCANRx();
    delay(5);
  }
}

static void feedIssueMove(int32_t steps)
{
  steps = feedPositiveMoveSteps(steps);
  if (steps <= 0) return;
  int32_t stepsB = feedPositiveMoveSteps(feedStepsForB(steps));
  if (stepsB <= 0) stepsB = steps;
  canMoveRelativePP(steps, stepsB);
  feedHaltSent = false;
}

static void feedArmHoseSensor()
{
  if (feedSensorArmed) return;
  feedSensorArmed = true;
  feedSawHoseCleared = true;
  feedStableSinceMs = 0;
  feedPhaseStartMs = millis();
}

void serviceServoFeed()
{
  if (feedPhase == FEED_IDLE || feedPhase == FEED_DONE || feedPhase == FEED_ERROR)
    return;

  if (!cycleActive && !feedCalibrationTest)
  {
    feedPhase = FEED_ERROR;
    return;
  }

  if (cyclePaused && !feedCalibrationTest)
    return;

  uint32_t now = millis();

  if (feedPhase == FEED_PARALLEL_START_DELAY)
  {
    if (now < feedDelayUntilMs) return;
    feedPhaseStartMs = now;
    if (feedModeThisCycle == FEED_MODE_BYPASS)
      feedPhase = FEED_BYPASS;
    else
      feedBeginStepsSensorMode2(feedChunksOnly);
    return;
  }

  if (feedModeThisCycle == FEED_MODE_STEPS_SENSOR
      && (feedPhase == FEED_SS_SOLID || feedPhase == FEED_RUNNING))
  {
    serviceFeedStepsSensor(now);
    return;
  }

  // Defensa: BYPASS legacy nunca debe quedar en fases SS_* (no se atienden → E011).
  if (feedModeThisCycle == FEED_MODE_BYPASS
      && feedPhase == FEED_SS_SOLID)
  {
    feedPhase = FEED_BYPASS;
    feedPhaseStartMs = now;
  }

  if (feedPhase == FEED_CHUNK_DELAY || feedPhase == FEED_HOSE_DELAY)
  {
    if (now < feedDelayUntilMs) return;
    if (!feedServosSettled(now)) return;
    feedSyncStepsFromEncoder();
    if (feedPhase == FEED_HOSE_DELAY)
    {
      feedMarkDone();
      return;
    }
    // BYPASS: al terminar movimiento, mínimo de encoder OK o seguir alimentando
    if (feedModeThisCycle == FEED_MODE_BYPASS)
    {
      feedSyncStepsFromEncoder();
      if (!feedEncoderValidationEnabled())
      {
        feedDelayUntilMs = now + feedDoneSettleMs();
        feedPhase = FEED_HOSE_DELAY;
        return;
      }
      if (feedEncoderEnough(feedStepsFed, feedStepsTargetThisFeed))
      {
        feedDelayUntilMs = now + feedDoneSettleMs();
        feedPhase = FEED_HOSE_DELAY;
        return;
      }
      feedPhase = FEED_BYPASS;
      return;
    }
    // Feed OM no usa FEED_CHUNK_DELAY; cualquier otro → error.
    feedPhase = FEED_ERROR;
  }

  if (feedPhase == FEED_BYPASS)
  {
    if (feedDeferForLinearMotion()) return;
    feedSyncStepsFromEncoder();
    if (feedEncoderValidationEnabled() && feedStepsTargetThisFeed > 0)
    {
      if (feedEncoderEnough(feedStepsFed, feedStepsTargetThisFeed))
      {
        if (!feedServosSettled(now)) return;
        if (feedCalibrationTest)
        {
          feedMarkDone();
          return;
        }
        feedDelayUntilMs = now + feedDoneSettleMs();
        feedPhase = FEED_HOSE_DELAY;
        return;
      }
    }
    else if (feedBypassRemaining <= 0)
    {
      if (!feedServosSettled(now)) return;
      if (feedCalibrationTest)
      {
        feedMarkDone();
        return;
      }
      feedDelayUntilMs = now + feedDoneSettleMs();
      feedPhase = FEED_HOSE_DELAY;
      return;
    }
    int32_t n = feedBypassRemaining;
    if (n <= 0)
      n = feedTargetLo(feedStepsTargetThisFeed) - feedStepsFed;
    // Nunca spamear chunks tras overshoot: eso impedía cerrar OK → E011.
    if (n <= 0)
    {
      // Si el mínimo no se cumple, no marcar DONE (evita E012 falso).
      if (feedEncoderValidationEnabled() && feedStepsTargetThisFeed > 0
          && !feedEncoderEnough(feedStepsFed, feedStepsTargetThisFeed))
      {
        n = feedTargetLo(feedStepsTargetThisFeed) - feedStepsFed;
        if (n < 1) n = 1;
      }
      else
      {
        if (!feedServosSettled(now)) return;
        if (feedCalibrationTest)
        {
          feedMarkDone();
          return;
        }
        feedDelayUntilMs = now + feedDoneSettleMs();
        feedPhase = FEED_HOSE_DELAY;
        return;
      }
    }
    feedEnsureMotionProfile();
    feedIssueMove(n);
    if (!feedEncoderTracking)
    {
      feedStepsFed += n;
      feedBypassRemaining -= n;
      if (feedBypassRemaining < 0) feedBypassRemaining = 0;
    }
    feedDelayUntilMs = now + FEED_SETTLE_FAST_MS;
    feedPhase = FEED_CHUNK_DELAY;
    return;
  }

  // Fallback feed OM: feedMarkDone sin confirmación → reentrar a SS_*.
  if (feedPhase == FEED_RUNNING)
  {
    if (feedModeThisCycle == FEED_MODE_STEPS_SENSOR)
    {
      feedPhase = feedSolidTargetMet ? FEED_RUNNING : FEED_SS_SOLID;
      return;
    }
    feedPhase = FEED_ERROR;
  }
}

static void beginServoFeed(bool prefetch, int32_t bypassOverride = -1, bool chunksOnly = false,
                           bool skipIdleWait = false)
{
  if (!servoCanReady)
  {
    if (!feedCalibrationTest && !feedPrefetch && cycleErrorCode == E000)
      setCycleError(E008, "servo_can");
    feedPhase = FEED_ERROR;
    return;
  }

  // Tras Halt/Stop el bit Target Reached suele estar en 0: esperar hasta 2.5 s
  // retrasaba la 1ª pieza de forma variable. Solo esperar si aún hay feed activo.
  if (!prefetch && !skipIdleWait && feedPhaseIsActive())
    feedWaitServosIdle();

  feedPrefetch = prefetch;
  feedPrefetchSensorLatched = false;
  feedChunksOnly = chunksOnly;
  feedSensorArmed = false;
  feedInitPrefetchSensorState();
  feedPhaseStartMs = millis();
  feedDelayUntilMs = 0;
  feedChunksFed = 0;
  feedBypassRemaining = 0;
  feedStableSinceMs = 0;
  feedHaltSent = false;
  feedStepsFed = 0;
  feedSensorConfirmed = false;
  feedSensorConfirmedL = false;
  feedSensorConfirmedR = false;
  feedNeedSensorL = false;
  feedNeedSensorR = false;
  feedSsDisarmHalt();
  feedProfileApplied = false;
  feedSolidPhaseActive = false;
  feedChunkTargetThisFeed = 0;
  feedChunkTargetB = 0;
  feedSolidTargetMet = false;
  feedCaptureStartPosition();

  if (feedModeThisCycle == FEED_MODE_BYPASS)
  {
    feedStepsTargetThisFeed = (bypassOverride >= 0) ? bypassOverride : feedBypassSteps;
    feedStepsTargetB = (bypassOverride >= 0) ? feedStepsForB(feedStepsTargetThisFeed) : feedBypassStepsB;
    if (feedStepsTargetB <= 0) feedStepsTargetB = feedStepsTargetThisFeed;
    feedBypassRemaining = feedStepsTargetThisFeed;
    feedPhase = FEED_BYPASS;
    return;
  }

  if (feedModeThisCycle == FEED_MODE_STEPS_SENSOR)
  {
    omClearPieceValidation();
    float effMm = 0.0f;
    (void)cutLengthToSteps(cutLongitud, &effMm);
    if (effMm < 1.0f) effMm = (float)cutLongitud;
    if (effMm < 1.0f) effMm = 100.0f;
    asdaOverviewStart(effMm);
    asdaEncoderReset();  // cero antes de medir alimentación 55 mm
    feedBeginStepsSensorMode2(chunksOnly);
    return;
  }

  // Modo desconocido → error (solo existen BYPASS y STEPS_SENSOR).
  feedPhase = FEED_ERROR;
}

static bool feedStep2NeedsWait()
{
  if (feedPhase == FEED_IDLE) return false;
  if (feedPhase == FEED_ERROR) return true;
  if (feedPhase == FEED_DONE) return !feedThisCycleSucceeded();
  return true;
}

static void waitCycleFeedTick(uint32_t& lastCanMs, uint32_t& lastWebMs)
{
  if ((!cycleActive && !feedCalibrationTest) || (cycleAborted && !feedCalibrationTest))
  {
    if (cycleRunning || linearIsMoving())
    {
      linearAbort();
      cycleState = C_IDLE;
      cycleRunning = false;
    }
    return;
  }

  serviceServoFeed();
  if (!feedCalibrationTest)
    serviceCycle();
  if (linearIsMoving())
    linearService();

  serviceBackgroundTick(lastCanMs, lastWebMs);

  // Entre movimientos de feed (sin lineal): aplicar Pause pendiente.
  if (!feedCalibrationTest && !linearIsMoving() && !cycleRunning)
    honorPausePendingIfIdle("feed-wait");

  while (cyclePaused && cycleActive && !feedCalibrationTest)
  {
    linearPause();
    peerService();
    peerTryReconnect();
    serviceExternalStopInput();
    serviceCANRx();
    if (cycleRunning) serviceCycle();
    serviceServoFeed();
    serviceBackgroundTick(lastCanMs, lastWebMs);
    delay(5);
  }
  if (!feedCalibrationTest)
    linearResume();
  yield();
}

// Lineal y feeder en paralelo: no esperar HOME antes de avanzar el feed.
static void waitCycleAndFeedParallel()
{
  feedResumeIfIncompleteDone();

  uint32_t lastCanMs = millis();
  uint32_t lastWebMs = millis();
  uint32_t cmdPulses = pulsesRemaining;
  if (cmdPulses == 0)
  {
    if (cycleState == C_BACK)
      cmdPulses = lastBackCommandPulses;
    else
      cmdPulses = lastFwdCommandPulses;
  }
  const uint32_t linearTimeoutMs = linearMoveTimeoutMs(cmdPulses);
  const uint32_t feedTimeoutMs = FEED_WAIT_TIMEOUT_MS;
  uint32_t linearActiveMs = 0;
  uint32_t feedActiveMs = 0;
  uint32_t lastTick = millis();

  while (cycleActive)
  {
    if (cycleAborted)
    {
      linearAbort();
      cycleState = C_IDLE;
      cycleRunning = false;
      break;
    }
    if (!cycleRunning && !feedStep2NeedsWait())
      break;
    if (feedPhase == FEED_ERROR)
    {
      feedResumeIfIncompleteDone();
      if (feedPhase == FEED_ERROR && !cycleRunning)
        break;
    }
    if (feedPhase == FEED_DONE)
    {
      if (feedThisCycleSucceeded())
      {
        if (!cycleRunning)
          break;
      }
      else
      {
        feedResumeIfIncompleteDone();
      }
    }

    if (!cycleActive)
    {
      linearAbort();
      cycleState = C_IDLE;
      cycleRunning = false;
      break;
    }

    const uint32_t now = millis();
    if (!cyclePaused)
    {
      const uint32_t dt = now - lastTick;
      if (cycleRunning)
        linearActiveMs += dt;
      if (feedStep2NeedsWait() && !feedWaitingPrefetchClear())
        feedActiveMs += dt;
    }
    lastTick = now;

    if (!cyclePaused && cycleRunning && linearActiveMs > linearTimeoutMs)
    {
      Serial.printf("LINEAR TIMEOUT (coop): %lu ms\n", (unsigned long)linearActiveMs);
      faultStopCycle("lineal_timeout");
      break;
    }
    if (!cyclePaused && feedStep2NeedsWait() && feedActiveMs > feedTimeoutMs)
    {
      feedAbort("FEED: TIMEOUT espera paralelo");
      faultStopCycle("feed_timeout");
      break;
    }

    waitCycleFeedTick(lastCanMs, lastWebMs);
  }
}

// Espera lineal; si hay feed prefetch en curso, avanzarlo en paralelo.
static void waitCycleFinishCoop()
{
  if (feedStep2NeedsWait())
    waitCycleAndFeedParallel();
  else
    waitCycleFinish();
}

void waitServoFeedStep2()
{
  feedResumeIfIncompleteDone();

  uint32_t lastCanMs = millis();
  uint32_t lastWebMs = millis();
  uint32_t activeElapsed = 0;
  uint32_t lastTick = millis();
  // Test: también cuenta tiempo con láser ocupado (si no, se cuelga sin mover).
  const bool countWhileWaitingClear = feedCalibrationTest;

  while (cycleActive || feedCalibrationTest)
  {
    if (cycleAborted && !feedCalibrationTest) break;
    feedSsHaltFromSensorNow();
    if (feedPhase == FEED_IDLE)
      break;
    if (feedPhase == FEED_ERROR)
    {
      feedResumeIfIncompleteDone();
      if (feedPhase == FEED_ERROR)
        break;
    }
    if (feedPhase == FEED_DONE)
    {
      if (feedThisCycleSucceeded())
        break;
      feedResumeIfIncompleteDone();
    }

    const uint32_t now = millis();
    const bool waitingClear = feedWaitingPrefetchClear();
    if (!cyclePaused && (countWhileWaitingClear || !waitingClear))
      activeElapsed += (now - lastTick);
    lastTick = now;

    if (!cyclePaused && waitingClear && feedCalibrationTest && laserValidationEnabled
        && activeElapsed > 2000u)
    {
      feedAbort("FEED: TIMEOUT - laser ocupado (retire manguera del sensor)");
      break;
    }

    if (!cyclePaused && activeElapsed > (uint32_t)FEED_WAIT_TIMEOUT_MS)
    {
      feedAbort("FEED: TIMEOUT espera step2");
      if (!feedCalibrationTest)
        faultStopCycle("feed_timeout");
      break;
    }

    waitCycleFeedTick(lastCanMs, lastWebMs);
  }
}

void startServoFeedAsync(bool prefetch, bool chunksOnly)
{
  beginServoFeed(prefetch, -1, chunksOnly);
}

void runServoFeedStep2(bool chunksOnly)
{
  startServoFeedAsync(false, chunksOnly);
  waitServoFeedStep2();
}

// En HOME / antes de pinzas: esperar a que el feed termine.
static bool feedEnsureReadyAtHome()
{
  if (feedStep2NeedsWait())
    waitServoFeedStep2();
  if (!cycleActive || cycleAborted) return false;

  if (feedThisCycleSucceeded() && feedHoseSensorReadyNow())
    return true;

  if (feedThisCycleSucceeded() && !feedHoseSensorReadyNow())
  {
    DBG_PRINTLN("FEED: latch sin láser en HOME — alimentar");
    feedPhase = FEED_IDLE;
    feedSolidTargetMet = false;
    feedPrefetch = false;
    runServoFeedStep2(false);
    if (!cycleActive || cycleAborted) return false;
    return feedThisCycleSucceeded();
  }

  if (!feedThisCycleSucceeded())
  {
    DBG_PRINTLN("FEED: no listo en HOME — reintento");
    if (!feedRetryStep2IfNeeded())
      return false;
  }
  return feedThisCycleSucceeded();
}

// Tras alimentar: holder queda cerrado. Si withCut, pulso de cortador y fin.
static void feedTestFinishKeepHolder(bool withCut)
{
  if (withCut && cycleActive)
  {
    stCutters = true;
    applyPlcOutputs();
    DBG_PRINTLN("FEED TEST: CUTTER ON");
    pausableDelay(D_CUTTER_PULSE_MS);
    stCutters = false;
    applyPlcOutputs();
    DBG_PRINTLN("FEED TEST: CUTTER OFF");
    if (cycleActive)
      pausableDelay(D_CUTTER_POST_MS);
  }
  // Holder permanece cerrado al terminar la prueba.
}

static bool feedTestWaitServosReached(uint32_t timeoutMs, bool needL, bool needR)
{
  uint32_t start = millis();
  uint32_t lastCanMs = millis();
  uint32_t lastWebMs = millis();
  while (millis() - start < timeoutMs)
  {
    if (!cycleActive) return false;
    if (cycleRunning) serviceCycle();
    serviceBackgroundTick(lastCanMs, lastWebMs);
    if (feedServosTargetReached(needL, needR))
    {
      delay(FEED_SETTLE_FAST_MS);
      return true;
    }
    delay(5);
  }
  return false;
}

// Movimiento relativo con signo (offset de ajuste): + alarga, - acorta (reversa).
// Tras Halt del láser no esperar 60 s de Target Reached: eso dejaba las pinzas colgadas.
static bool feedApplyOffsetMove(int32_t offsetStepsA, int32_t offsetStepsB)
{
  if (offsetStepsA == 0 && offsetStepsB == 0) return true;
  feedCanSetVelAcc((uint32_t)feedServoBasePpA, (uint32_t)feedServoBasePpB);
  const bool doL = offsetStepsA != 0;
  const bool doR = offsetStepsB != 0;
  if (doL) { feedSsHaltMsL = 0; feedHaltArmL = 0; }
  if (doR) { feedSsHaltMsR = 0; feedHaltArmR = 0; }
  canSetTargetPos(offsetStepsA, offsetStepsB);
  canResetNewSetpointSides(doL, doR);
  if (doL) canClearHaltSide(false);
  if (doR) canClearHaltSide(true);
  canExecuteMoveSides(doL, doR);
  feedHaltSent = false;
  feedTestWaitServosReached(800, doL, doR);
  return true;
}

// onlySide: -1 = ambos, 0 = solo L, 1 = solo R
static bool runFeedTestSolidThenSensor(int32_t solidL, int32_t chunkL, int32_t solidR, int32_t chunkR,
                                       bool withCut, int8_t onlySide)
{
  // Test anterior colgado (FEED_SS_* sin cycleActive) o FEED_ERROR residual.
  if ((!cycleActive && feedPhaseIsActive()) || feedPhase == FEED_ERROR)
  {
    resetFeedAbortState();
    feedCalibrationTest = false;
  }
  if (cycleActive || feedPhaseIsActive())
    return false;
  if (!servoCanReady)
    return false;

  if (onlySide == 0)
  {
    solidR = 0;
    chunkR = 0;
  }
  else if (onlySide == 1)
  {
    solidL = 0;
    chunkL = 0;
  }

  solidL = clampFeedTestSolidSteps(solidL);
  solidR = clampFeedTestSolidSteps(solidR);
  if (onlySide != 1)
    chunkL = clampFeedTestChunkSteps(chunkL);
  else
    chunkL = 0;
  if (onlySide != 0)
    chunkR = clampFeedTestChunkSteps(chunkR);
  else
    chunkR = 0;

  int32_t savedSolid = feedTestSolidSteps;
  int32_t savedChunk = feedTestChunkSteps;
  int32_t savedSolidR = feedTestSolidStepsR;
  int32_t savedChunkR = feedTestChunkStepsR;
  feedTestSolidSteps = solidL;
  feedTestChunkSteps = chunkL;
  feedTestSolidStepsR = solidR;
  feedTestChunkStepsR = chunkR;
  feedTestExactSides = (onlySide >= 0);

  // Limpiar Halt / FEED_ERROR residual de un test anterior (si no, 607A no arranca).
  resetFeedAbortState();
  canClearHaltSide(false);
  canClearHaltSide(true);
  delay(30);
  // Fallas E012/E016 de pruebas previas no deben dejar la torre en rojo.
  if (cycleErrorCode == E012 || cycleErrorCode == E016)
  {
    setCycleError(E000);
    setCycleFaultReason("");
  }

  feedModeThisCycle = FEED_MODE_STEPS_SENSOR;
  feedPhase = FEED_IDLE;
  feedCalibrationTest = true;
  cycleActive = true;
  cyclePaused = false;
  cycleAborted = false;

  stHolder = true;
  applyPlcOutputs();
  pausableDelay(D_HOLDER_ON_MS);
  if (!cycleActive && !feedCalibrationTest)
  {
    feedCalibrationTest = false;
    feedTestExactSides = false;
    feedTestSolidSteps = savedSolid;
    feedTestChunkSteps = savedChunk;
    feedTestSolidStepsR = savedSolidR;
    feedTestChunkStepsR = savedChunkR;
    return false;
  }

  beginServoFeed(false, -1, false, true);
  waitServoFeedStep2();
  bool ok = feedThisCycleSucceeded();
  if (ok)
    feedTestFinishKeepHolder(withCut);

  feedCalibrationTest = false;
  feedPhase = FEED_IDLE;
  cycleActive = false;
  feedTestExactSides = false;
  feedTestSolidSteps = savedSolid;
  feedTestChunkSteps = savedChunk;
  feedTestSolidStepsR = savedSolidR;
  feedTestChunkStepsR = savedChunkR;
  return ok;
}

// ============================================================
// SECCION 13 — Rutina de corte
// ============================================================
static void gripToHomeMarkStart()
{
  if (!gripToHomeEnabled) return;
  gripToHomeStartMs = millis();
  gripToHomeActive = true;
}

static void gripToHomeMarkEnd()
{
  if (!gripToHomeActive) return;
  gripToHomeLastMs = millis() - gripToHomeStartMs;
  gripToHomeActive = false;
  gripToHomeTotalMs += gripToHomeLastMs;
  gripToHomeCount++;
  DBG_PRINT("CYCLE TIME grip->HOME: ");
  DBG_PRINT(gripToHomeLastMs);
  DBG_PRINTLN(" ms");
}

static void gripToHomeCancel()
{
  gripToHomeActive = false;
}

static void gripToHomeReset()
{
  gripToHomeActive = false;
  gripToHomeStartMs = 0;
  gripToHomeLastMs = 0;
  gripToHomeTotalMs = 0;
  gripToHomeCount = 0;
}

void pausableDelay(uint32_t ms)
{
  uint32_t start = millis();
  uint32_t lastCanMs = millis();
  uint32_t lastWebMs = millis();
  while (millis() - start < ms)
  {
    if (cycleRunning)
      serviceCycle();
    if (feedStep2NeedsWait() || !linearIsMoving())
      serviceServoFeed();
    serviceBackgroundTick(lastCanMs, lastWebMs);
    // Delays: Pause puede aplicar ya (no hay lineal en curso).
    honorPausePendingIfIdle("delay");
    while (cyclePaused && cycleActive)
    {
      peerService();
      peerTryReconnect();
      serviceExternalStopInput();
      if (cycleRunning) serviceCycle();
      if (feedStep2NeedsWait() || !linearIsMoving())
        serviceServoFeed();
      serviceBackgroundTick(lastCanMs, lastWebMs);
      delay(5);
    }
    if (!cycleActive || cycleAborted) return;
    yield();
  }
}

void cycle(int cycles, uint32_t stepsPerCut)
{
  if (cycleAborted)
  {
    Serial.println("CICLO no arranca: Stop ya solicitado");
    cycleAborted = false;
    cycleActive = false;
    peerSettleHoldInProcess = false;
    peerSyncInProcessFlag();
    return;
  }
  cycleActive = true;
  peerSettleHoldInProcess = false;  // el ciclo sostiene In process
  peerSyncInProcessFlag();  // Start: arma buffer/holgura en PreFeeder (Production)
  cyclePaused = false;
  cyclePausePending = false;
  cyclePausedAccumMs = 0;
  cyclePauseBeganMs = 0;
  setCyclePauseReason("");
  setCycleFaultReason("");
  setCycleError(E000);
  feedPhase = FEED_IDLE;
  feedHandoffReady = false;
  feedModeThisCycle = feedMode;
  int32_t feedOffsetStepsThisCycleA = feedOffsetSteps();
  int32_t feedOffsetStepsThisCycleB = feedOffsetStepsB();

  progressTotalSteps = 11;
  progressTotalReps = cycles;
  progressCurrentRep = 0;
  progressStep = 0;
  cycleStartTime = millis();
  totalCompletedTime = 0;
  completedReps = 0;
  lastCycleTotalMs = 0;
  lastCycleSuccess = false;
  repStartTime = millis();
  gripToHomeCancel();
  prefeederTriggerEveryN = 1;

  Serial.printf("=== CICLO START x%d modo=%s\n", cycles, feedModeLabel(feedModeThisCycle));
  if (feedOffsetStepsThisCycleA != 0 || feedOffsetStepsThisCycleB != 0)
    DBG_PRINTF("Offset alimentacion: A %.1f mm -> %ld | B %.1f mm -> %ld pasos\n",
               feedOffsetMm, (long)feedOffsetStepsThisCycleA,
               feedOffsetMmB, (long)feedOffsetStepsThisCycleB);

  // Esperar Buffer Full + holgura antes de la 1ª pieza.
  if (!waitPrefeederReadyAtStart())
  {
    if (cycleAborted || !cycleActive)
    {
      Serial.println("CICLO cancelado: Stop durante espera PreFeeder");
      pushLog("CICLO cancelado: Stop");
      cycleActive = false;
      cyclePaused = false;
      lastCycleSuccess = false;
      peerSyncInProcessFlag();
      return;
    }
    if (!peerBothAutoArmedForRefill())
    {
      Serial.println("CICLO cancelado: PreFeeder no inicializado (falta Production+Iniciar)");
      pushLog("CICLO cancelado: PreFeeder no inicializado");
      setCycleError(E030, "prefeeder_not_ready");
    }
    else
    {
      Serial.println("CICLO cancelado: PreFeeder buffer/holgura timeout");
      pushLog("CICLO cancelado: PreFeeder timeout");
    }
    cycleActive = false;
    cyclePaused = false;
    lastCycleSuccess = false;
    prefeederHardStopDisarm("start-fail");
    uiNotify();
    return;
  }

  // Pieza:
  //   1 Holder ON (solo 1ª)  2 Feed  3 Offset
  //   4 Pinzas ON  5 Holder OFF + lineal FWD  6 Holder ON
  //   7 Corte  8 Prefetch  9 Extra  10 Pinzas OFF + trigger PF
  //   11 HOME + handoff  12 Asentar + post-pieza
  for (int rep = 1; rep <= cycles; rep++)
  {
    if (!cycleActive || cycleAborted) break;
    if (rep == 1)
    {
      if (cycleGateAfterStep("listo")) break;
    }
    else if (cycleGateAfterStep("rep-start")) break;
    progressCurrentRep = rep;
    progressStep = 0;
    repStartTime = millis();

    DBG_PRINT("---- REP ");
    DBG_PRINT(rep);
    DBG_PRINT("/");
    DBG_PRINTLN(cycles);

    // ---- 1. Holder ON (solo 1ª pieza) ----
    if (rep == 1)
    {
      progressStep = 1;
      stHolder = true;
      applyPlcOutputs();
      DBG_PRINTLN("HOLDER ON");
      pausableDelay(D_HOLDER_ON_MS);
      if (cycleGateAfterStep("holder-on")) break;
    }

    // ---- 2. Alimentación ----
    progressStep = 2;
    if (rep == 1)
    {
      // 1ª: material ya validado en waitPrefeederReadyAtStart.
      runServoFeedStep2();
      if (cycleGateAfterStep("feed"))
        break;
      if (!feedThisCycleSucceeded())
      {
        // First piece / ciclo: alimentó pero no validó → no cortar ni lineal.
        if (cycleErrorCode == E000)
        {
          if (laserValidationEnabled && feedOmLengthMet && !feedLaserSeen)
            setCycleError(E016, "feed_hose");
          else
            setCycleError(E012, "feed_incomplete");  // Alimentación fallida
        }
        lastCycleSuccess = false;
        cycleActive = false;
        break;
      }
      if (!omCommitPhase1OrFault())
      {
        lastCycleSuccess = false;
        cycleActive = false;
        break;
      }
      // Reset OM tras clamp (abajo): movimiento de pinzas no entra en Fase 2.
    }
    else if (feedHandoffReady && feedHoseSensorReadyNow())
    {
      feedHandoffReady = false;
      if (!omCommitPhase1OrFault())
      {
        lastCycleSuccess = false;
        cycleActive = false;
        break;
      }
    }
    else
    {
      if (feedHandoffReady)
      {
        DBG_PRINTLN("FEED: handoff sin sensor en vivo — no saltar");
        feedHandoffReady = false;
      }
      if (!feedEnsureReadyAtHome())
      {
        if (cycleErrorCode == E000 && !feedThisCycleSucceeded())
        {
          if (laserValidationEnabled && feedOmLengthMet && !feedLaserSeen)
            setCycleError(E016, "feed_hose");
          else
            setCycleError(E012, "feed_incomplete");
        }
        lastCycleTotalMs = cycleElapsedMs();
        lastCycleSuccess = false;
        break;
      }
      if (cycleGateAfterStep("feed-prefetch")) break;
      feedConsumeReadyMaterial();
      if (!omCommitPhase1OrFault())
      {
        lastCycleSuccess = false;
        cycleActive = false;
        break;
      }
      if (cycleGateAfterStep("feed-consume")) break;
    }

    // ---- 3. Offset (opcional) ----
    if (feedOffsetStepsThisCycleA != 0 || feedOffsetStepsThisCycleB != 0)
    {
      DBG_PRINT("OFFSET move A=");
      DBG_PRINT(feedOffsetStepsThisCycleA);
      DBG_PRINT(" B=");
      DBG_PRINTLN(feedOffsetStepsThisCycleB);
      feedApplyOffsetMove(feedOffsetStepsThisCycleA, feedOffsetStepsThisCycleB);
      if (cycleGateAfterStep("offset")) break;
    }

    // ---- 4. Pinzas cierran ----
    progressStep = 3;
    stGrippers = true;
    applyPlcOutputs();
    DBG_PRINTLN("GRIPPERS ON");
    gripToHomeMarkStart();
    pausableDelay(D_GRIPPERS_ON_MS);
    if (cycleGateAfterStep("grippers-on")) break;

    // OM cero tras clamp: Fase 2 solo mide avance del lineal (no el cierre de pinzas).
    asdaEncoderReset();
    asdaOverviewMark("omReset");

    // ---- 5. Holder abre + lineal FWD ----
    progressStep = 5;
    stHolder = false;
    applyPlcOutputs();
    DBG_PRINTLN("HOLDER OFF (lineal FWD)");
    pausableDelay(D_HOLDER_OPEN_MS);
    if (cycleGateAfterStep("holder-off")) break;

    DBG_PRINT("LINEAL FWD steps=");
    DBG_PRINTLN(stepsPerCut);
    cycleStartForwardPulses(stepsPerCut, true);
    waitCycleFinish();
    asdaOverviewMark("asdaLinearActuator");
    asdaOverviewMark("omFinal");
    (void)omValidatePhase2AndTotal();
    if (!omPieceLengthOkForCut())
    {
      if (cycleErrorCode == E000)
      {
        if (!omPhase2Ok)
          setCycleError(E012, "om_phase2");
        else if (!omTotalInternalOk)
          setCycleError(E012, "om_total");
        else
          setCycleError(E012, "om_length");
      }
      lastCycleSuccess = false;
      cycleActive = false;
      break;
    }
    if (cycleGateAfterStep("lineal-fwd")) break;

    pausableDelay(D_LINEAR_DONE_MS);
    if (cycleGateAfterStep("lineal-dwell")) break;

    // ---- 6. Holder cierra (pre-corte) ----
    stHolder = true;
    applyPlcOutputs(); 
    DBG_PRINTLN("HOLDER ON (antes corte)");
    pausableDelay(D_HOLDER_ON_MS);
    if (cycleGateAfterStep("holder-precut")) break;

    // ---- 7. Corte ----
    progressStep = 7;
    stCutters = true;
    applyPlcOutputs();
    DBG_PRINTLN("CUTTER ON");
    // All OK L+R: el prefetch (bloque 8) necesita PreFeeder listo.
    if (!peerBothAllOkForPrefetch())
    {
      Serial.println("PREFEEDER: All OK falló al accionar cortador");
      pushLog("PREFEEDER: All OK fail (cutter)");
      faultStopCycle("prefeeder_all_ok");
      break;
    }
    pausableDelay(D_CUTTER_PULSE_MS);
    if (cycleGateAfterStep("cutter-on")) break;

    stCutters = false;
    applyPlcOutputs();
    DBG_PRINTLN("CUTTER OFF");

    // Retiro del cortador antes de alimentar.
    pausableDelay(D_CUTTER_POST_MS);
    if (cycleGateAfterStep("cutter-off")) break;

    // ---- 8. Prefetch siguiente (paralelo con extra + HOME) ----
    if (rep < cycles)
    {
      startServoFeedAsync(true);
      DBG_PRINTLN("FEED: prefetch inicio (post-corte)");
    }

    // ---- 9. Extra / depósito ----
    progressStep = 8;
    {
      uint16_t batchIndex = (uint16_t)((rep - 1) / depositBatchSize + 1);
      uint16_t pieceInBatch = (uint16_t)((rep - 1) % depositBatchSize);
      int32_t extraPulses = depositExtraSignedSteps(batchIndex, pieceInBatch);
      DBG_PRINT("EXTRA deposit batch=");
      DBG_PRINT(batchIndex);
      DBG_PRINT(" piece=");
      DBG_PRINT(pieceInBatch);
      DBG_PRINT(" steps=");
      DBG_PRINTLN(extraPulses);
      if (extraPulses > 0)
        cycleStartForwardPulses((uint32_t)extraPulses, false);
      else if (extraPulses < 0)
        cycleStartBackwardPulses((uint32_t)(-extraPulses));
      if (rep >= cycles && feedStep2NeedsWait())
      {
        DBG_PRINTLN("FEED: drenando alimentacion residual (ultima pieza)");
        waitServoFeedStep2();
        feedConsumeReadyMaterial();
      }
      if (extraPulses != 0)
        waitCycleFinish();
      asdaOverviewMark("asdaDeposit");
      if (!asdaOverviewHitOk("asdaDeposit"))
      {
        if (cycleErrorCode == E000)
          setCycleError(E012, "overview_deposit");
        lastCycleSuccess = false;
        cycleActive = false;
        break;
      }
      if (cycleGateAfterStep("deposit")) break;
    }

    // ---- 10. Pinzas abren + trigger PF (pieza completada) ----
    progressStep = 9;
    stGrippers = false;
    applyPlcOutputs();
    DBG_PRINTLN("GRIPPERS RELEASED");
    prefeederTriggerAsync("grippers-off");  // Tfeed al abrir pinzas (pieza ya depositada)

    pausableDelay(D_GRIPPER_RELEASE_MS);
    if (cycleGateAfterStep("grippers-off")) break;

    // ---- 11. HOME + handoff ----
    progressStep = 11;
    DBG_PRINTLN("LINEAR RETURN");
    cycleStartBackToHome();
    if (rep < cycles)
      waitCycleFinishCoop();
    else
      waitCycleFinish();
    gripToHomeMarkEnd();
    if (cycleGateAfterStep("home")) break;

    // Pieza ya cortada y depositada: contar antes del handoff de la siguiente.
    {
      uint32_t repTime = millis() - repStartTime;
      totalCompletedTime += repTime;
      completedReps++;
      DBG_PRINT("Rep OK en ");
      DBG_PRINT(repTime / 1000);
      DBG_PRINTLN("s");
    }

    if (rep < cycles)
    {
      if (feedEnsureReadyAtHome())
      {
        DBG_PRINTLN("FEED: pieza lista en HOME");
        feedConsumeReadyMaterial();
        feedHandoffReady = true;
      }
      else
      {
        feedHandoffReady = false;
        if (!cycleActive || cycleAborted) break;
        if (cycleErrorCode == E000 && !feedThisCycleSucceeded())
          setCycleError(E012, "feed_incomplete");
        lastCycleTotalMs = cycleElapsedMs();
        lastCycleSuccess = false;
        break;
      }
    }

    // ---- 12. Asentar + post-pieza ----
    if (!feedHandoffReady)
      pausableDelay(asentarDelayMs);
    if (cycleGateAfterStep("asentar")) break;

    // Tras cada pieza: debounce/RX (sin poll); enlace TCP; holgura en HOME.
    safetyPollAfterPiece();
    if (!cycleActive || cycleAborted) break;
    if (cycleGateAfterStep("post-pieza")) break;

    if (rep < cycles)
    {
      peerCheckAfterPieceOrPause();
      if (!cycleActive || cycleAborted) break;
      if (cycleGateAfterStep("post-pieza-peer")) break;
      if (!prefeederRequireHolguraAtHome())
        break;
    }
  }

  // Capturar ANTES de limpiar cycleAborted / settle.
  const bool wasAborted = cycleAborted;
  const bool wasPaused = cyclePaused;
  const bool allRepsDone = (cycles > 0) && (completedReps >= cycles);
  const bool lotPiecesDone = !wasAborted && !wasPaused && allRepsDone
                             && cycleFaultReason[0] == '\0';

  if (wasAborted)
  {
    cycleAborted = false;
    Serial.println("Ciclo abortado — idle");
  }
  else if (wasPaused)
  {
    Serial.println("Ciclo terminado (pausado por seguridad)");
  }
  gripToHomeCancel();

  // Fin de lote: marcar éxito y notificar torre/UI antes del settle.
  if (lotPiecesDone)
  {
    lastCycleTotalMs = cycleElapsedMs();
    if (!lastCycleTotalMs)
      lastCycleTotalMs = millis() - cycleStartTime;
    lastCycleSuccess = true;
    towerNotifyLotComplete();
    Serial.printf("LOTE OK — tiempo total %lu s\n", lastCycleTotalMs / 1000UL);
  }
  else
  {
    lastCycleSuccess = false;
    if (!lastCycleTotalMs)
      lastCycleTotalMs = cycleElapsedMs();
  }

  // Rellenar buffer/holgura (servo/DeReeler) sin trigger feeder; luego desarmar.
  if (!wasAborted)
  {
    peerSettleHoldInProcess = true;
    cycleActive = false;
    cyclePaused = false;
    prefeederSettleThenDisarm(allRepsDone ? "fin-lote" : "fin-ciclo");
    // Settle ya no pone E014; las piezas hechas deben seguir como "completado".
    if (lotPiecesDone)
      lastCycleSuccess = true;
  }
  else
  {
    cycleActive = false;
    cyclePaused = false;
    peerSyncInProcessFlag();
  }
}

void procesarCorte(int longitud, int cantidad)
{
  if (cycleAborted)
  {
    Serial.println("Corte cancelado: Stop durante preparación");
    cycleAborted = false;
    cycleActive = false;
    peerSyncInProcessFlag();
    return;
  }
  float effectiveMm = 0.0f;
  uint32_t steps = cutLengthToSteps(longitud, &effectiveMm);
  if (effectiveMm <= 0.0f)
  {
    Serial.println("Corte cancelado: longitud efectiva <= 0");
    cycleActive = false;
    cyclePaused = false;
    lastCycleSuccess = false;
    peerSyncInProcessFlag();
    return;
  }
  Serial.printf("Corte: %d mm -> %lu pulsos x%d\n", longitud, (unsigned long)steps, cantidad);
  DBG_PRINTF("  lineal: linearActuator=%.1f G=%.1f pieza=%.1f spm=%.2f\n",
             effectiveMm - linearGripperAreaMm, linearGripperAreaMm, effectiveMm, linearStepsPerMm);
  cycle(cantidad, steps);
}

void prepareBeforeCut()
{
  stCutters = false;
  stGrippers = false;
  stHolder = true;  // No apagar: default ON; la rutina lo abre solo al lineal FWD
  stFgtray = false;
  stReset = false;
  applyPlcOutputs();

  cycleAborted = false;  // nuevo Start limpia abort previo
  cycleActive = true;
  peerSettleHoldInProcess = false;
  peerSyncInProcessFlag();  // Armar PF ya en homing previo al corte
  cyclePaused = false;
  cutRequested = false;
  lastCycleSuccess = false;
  lastCycleTotalMs = 0;
  progressStep = 0;
  progressTotalSteps = 0;
  progressCurrentRep = 0;
  progressTotalReps = 0;

  if (!asdaHomed)
  {
    DBG_PRINTLN("LINEAR: torque-home previo al corte");
    if (!linearRunTorqueHome())
    {
      Serial.println("LINEAR: HOME torque falló — no arrancar corte");
      cycleActive = false;
      peerSettleHoldInProcess = false;
      peerSyncInProcessFlag();
      return;
    }
  }
  else
  {
    cycleStartBackToHome();
    waitCycleFinish();
  }

  // Stop durante HOME previo: no continuar al lote.
  if (!cycleActive || cycleAborted)
  {
    cycleActive = false;
    peerSettleHoldInProcess = false;
    peerSyncInProcessFlag();
    return;
  }

  progressTotalReps = cutCantidad;
  progressCurrentRep = 0;
  progressTotalSteps = 11;
  cyclePaused = false;
  cycleActive = false;  // cycle() lo reactiva
}

static void immediatePhysicalStop()
{
  linearAbort();
  cycleState = C_IDLE;
  cycleRunning = false;
  pulsesRemaining = 0;
  skipDwellAtDestNextFwd = false;

  if (feedPhase != FEED_IDLE && feedPhase != FEED_DONE && feedPhase != FEED_ERROR)
  {
    if (!feedHaltSent)
    {
      canHalt();
      feedHaltSent = true;
    }
  }

  stCutters = false;
  stGrippers = false;
  // Holder: no forzar OFF en Stop/fault — solo rutina o manual.
  applyPlcOutputs();
  gripToHomeCancel();
}

static void resetFeedAbortState()
{
  if (feedPhase != FEED_IDLE && feedPhase != FEED_DONE && feedPhase != FEED_ERROR)
    canHalt();
  feedPhase = FEED_IDLE;
  feedHaltSent = false;
  feedPrefetch = false;
  feedPrefetchSensorLatched = false;
  feedChunksOnly = false;
  feedSensorArmed = false;
  feedSawHoseCleared = false;
  feedStableSinceMs = 0;
  feedBypassRemaining = 0;
  feedChunksFed = 0;
  feedEncoderTracking = false;
  feedSensorConfirmed = false;
  feedSensorConfirmedL = false;
  feedSensorConfirmedR = false;
  feedNeedSensorL = false;
  feedNeedSensorR = false;
  feedSsDisarmHalt();
  feedSolidPhaseActive = false;
  feedChunkTargetThisFeed = 0;
  feedChunkTargetB = 0;
  feedSolidTargetMet = false;
}

void abortCycleRoutine(bool clearSafetyError)
{
  cycleAborted = true;
  cycleActive = false;
  cyclePaused = false;
  cyclePausePending = false;
  cyclePauseBeganMs = 0;
  cyclePausedAccumMs = 0;
  setCyclePauseReason("");
  setCycleError(E000);
  setCycleFaultReason("");
  cutRequested = false;
  // Stop: reset total de progreso / tiempos.
  progressStep = 0;
  progressTotalSteps = 0;
  progressCurrentRep = 0;
  progressTotalReps = 0;
  completedReps = 0;
  totalCompletedTime = 0;
  lastCycleTotalMs = 0;
  lastCycleSuccess = false;
  cycleStartTime = 0;
  if (clearSafetyError)
    refreshSafetyErrorCode();  // no limpia alarma CAN si sensores siguen activos / offline

  stCutters = false;
  stGrippers = false;
  // Holder permanece (default ON); no apagar en abort/reset de errores.
  stFgtray = false;
  stReset = false;
  immediatePhysicalStop();
  resetFeedAbortState();
  // Parar TCM + PreFeeder ya (sin settle post-Stop).
  prefeederHardStopDisarm("Stop");
  towerForceResync = true;
  uiNotify();

  Serial.println("CICLO ABORTADO — parada inmediata, idle");
  pushLog("CICLO ABORTADO");
}

void abortCycleRoutine()
{
  abortCycleRoutine(true);
}

// ============================================================
// SECCION 14 — Web handlers
// ============================================================

void handleRoot()
{
  // HTML gzip en PROGMEM; el navegador descomprime (Content-Encoding).
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Cache-Control", "no-cache");
  server.setContentLength(index_html_gz_len);
  server.send(200, "text/html", "");
  uint8_t chunk[512];
  size_t sent = 0;
  while (sent < index_html_gz_len)
  {
    size_t n = index_html_gz_len - sent;
    if (n > sizeof(chunk)) n = sizeof(chunk);
    memcpy_P(chunk, index_html_gz + sent, n);
    server.sendContent(reinterpret_cast<const char*>(chunk), n);
    sent += n;
  }
}

static bool adminSessionValid(bool refresh = true)
{
  if (!adminSessionToken.length()) return false;
  if (millis() - adminSessionLastMs > ADMIN_SESSION_TIMEOUT_MS)
  {
    adminSessionToken = "";
    return false;
  }
  if (!server.hasArg("token") || server.arg("token") != adminSessionToken) return false;
  if (refresh) adminSessionLastMs = millis();
  return true;
}

static bool requireAdminSession()
{
  return true; // temporal: sin contraseña
  // if (adminSessionValid()) return true;
  // server.send(401, "application/json", "{\"ok\":false,\"error\":\"Sesion no autorizada o expirada\"}");
  // return false;
}

static bool validPartNumber(const String& value)
{
  return partCatalogValidPartNumber(value.c_str());
}

static String activePartJson(bool ok = true)
{
  String j = "{\"ok\":";
  j += ok ? "true" : "false";
  j += ",\"active\":\"" + peerEsc(activePartNumber) + "\"";
  j += ",\"lengthMm\":" + String(activePartLengthMm);
  j += ",\"cutOffsetMm\":" + String(cutOffsetMm, 2);
  j += "}";
  return j;
}

static void handlePartsList()
{
  static PartCatalogEntry parts[PART_CATALOG_MAX];
  const uint8_t n = partCatalogReady() ? partCatalogList(parts, PART_CATALOG_MAX) : 0;
  String j;
  j.reserve(4096);
  j = "{\"ok\":true,\"active\":\"" + peerEsc(activePartNumber) + "\",\"lengthMm\":"
    + String(activePartLengthMm)
    + ",\"cutOffsetMm\":" + String(cutOffsetMm, 2)
    + ",\"parts\":[";
  bool first = true;
  for (uint8_t i = 0; i < n; i++)
  {
    if (!first) j += ",";
    first = false;
    const bool protectedPart = isProtectedPart(String(parts[i].partNumber));
    j += "{\"pn\":\"" + peerEsc(parts[i].partNumber) + "\",\"lengthMm\":"
      + String(parts[i].lengthMm)
      + ",\"deletable\":" + String(protectedPart ? "false" : "true") + "}";
  }
  j += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

static void handlePartSelect()
{
  if (cycleActive || linearIsMoving())
  {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"No se puede cambiar modelo durante un ciclo o movimiento\"}");
    return;
  }
  if (!server.hasArg("pn"))
  {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Falta numero de parte\"}");
    return;
  }
  String pn = server.arg("pn");
  if (!selectActivePart(pn))
  {
    server.send(404, "application/json", "{\"ok\":false,\"error\":\"Numero de parte no encontrado\"}");
    return;
  }
  server.send(200, "application/json", activePartJson());
}

static void handleAdminLogin()
{
  if (!server.hasArg("user") || !server.hasArg("password")
      || server.arg("user") != "Admin" || server.arg("password") != "1234")
  {
    delay(250);
    server.send(401, "application/json", "{\"ok\":false,\"error\":\"Usuario o contrasena incorrectos\"}");
    return;
  }
  adminSessionToken = String((uint32_t)esp_random(), HEX) + String((uint32_t)esp_random(), HEX);
  adminSessionLastMs = millis();
  server.send(200, "application/json", "{\"ok\":true,\"token\":\"" + adminSessionToken + "\"}");
}

static void handleAdminStatus()
{
  bool authorized = adminSessionValid(false);
  String j = activePartJson();
  j.remove(j.length() - 1);
  j += ",\"authorized\":";
  j += authorized ? "true}" : "false}";
  server.send(200, "application/json", j);
}

static void handlePartAdd()
{
  if (!requireAdminSession()) return;
  if (cycleActive || linearIsMoving())
  {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"Ciclo o lineal activo\"}");
    return;
  }
  if (!partCatalogReady())
  {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"LittleFS no disponible\"}");
    return;
  }
  if (!server.hasArg("pn") || !server.hasArg("lengthMm"))
  {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Faltan numero de parte o longitud\"}");
    return;
  }
  String pn = server.arg("pn");
  pn.trim();
  if (isProtectedPart(pn))
    pn = "Prueba";
  else
    pn.toUpperCase();
  if (!validPartNumber(pn))
  {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Numero de parte invalido (maximo 31 caracteres, no puede empezar con _)\"}");
    return;
  }
  if (partCatalogExists(pn.c_str()))
  {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"El numero de parte ya existe; seleccionelo\"}");
    return;
  }
  int lengthMm = server.arg("lengthMm").toInt();
  if (lengthMm <= 0 || lengthMm > 60000)
  {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Longitud invalida\"}");
    return;
  }
  static PartCatalogEntry listed[PART_CATALOG_MAX];
  if (partCatalogList(listed, PART_CATALOG_MAX) >= PART_CATALOG_MAX)
  {
    server.send(507, "application/json", "{\"ok\":false,\"error\":\"Limite de 100 modelos alcanzado\"}");
    return;
  }
  PartModel pr;
  partModelSetIdentity(pr, pn.c_str(), (uint16_t)lengthMm);
  if (!partCatalogSave(pr))
  {
    server.send(507, "application/json", "{\"ok\":false,\"error\":\"No se pudo guardar el modelo en LittleFS\"}");
    return;
  }
  if (!selectActivePart(pn))
  {
    server.send(507, "application/json", "{\"ok\":false,\"error\":\"Modelo creado pero no se pudo seleccionar\"}");
    return;
  }
  server.send(200, "application/json", activePartJson());
}

static void handlePartDelete()
{
  if (!requireAdminSession()) return;
  if (cycleActive || linearIsMoving())
  {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"No se puede eliminar durante un ciclo o movimiento\"}");
    return;
  }
  String pn = server.hasArg("pn") ? server.arg("pn") : activePartNumber;
  if (!pn.length())
  {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Indique el modelo a eliminar\"}");
    return;
  }
  if (isProtectedPart(pn))
  {
    server.send(403, "application/json", "{\"ok\":false,\"error\":\"Prueba no se puede eliminar\"}");
    return;
  }
  if (!partCatalogReady() || !partCatalogExists(pn.c_str()))
  {
    server.send(404, "application/json", "{\"ok\":false,\"error\":\"Modelo no encontrado\"}");
    return;
  }
  if (!partCatalogDelete(pn.c_str()))
  {
    server.send(507, "application/json", "{\"ok\":false,\"error\":\"No se pudo eliminar el modelo en LittleFS\"}");
    return;
  }
  if (activePartNumber == pn)
  {
    if (!selectActivePart("Prueba"))
    {
      activePartNumber = "";
      activePartLengthMm = 0;
    }
  }
  Serial.printf("Modelo eliminado: %s\n", pn.c_str());
  pushLog(String("Modelo eliminado: ") + pn);
  server.send(200, "application/json", activePartJson());
}

#define CLEAN_CUT_LENGTH_MM  100

// --- 14.2 Control de ciclo (start / pause / abort / home / estado) ---
void handleStartClean()
{
  if (cycleActive)
  {
    server.send(400, "text/plain", "Ya hay un ciclo en progreso");
    return;
  }
  // Dirty OK: First piece es 1 pieza.
  if (!safetyAllowsRun())
  {
    server.send(403, "text/plain", "Seguridad activa: sensores/PreFeeder — pulse Reset (PLC) para liberar");
    return;
  }
  if (!servoCanReady)
  {
    setCycleError(E008, "servo_can");
    uiNotify();
    server.send(503, "text/plain", "Servo CAN no listo (E008)");
    return;
  }
  if (prefeederIdleModeActive())
  {
    server.send(403, "text/plain", "PreFeeder en Materialista: desactívelo (Production) para correr la maquina");
    return;
  }
  if (basePfApplyPending)
  {
    server.send(503, "text/plain", "Configuracion del PreFeeder pendiente; verifique la comunicacion");
    return;
  }

  cutLongitud = CLEAN_CUT_LENGTH_MM;
  cutCantidad = 1;
  cutUseLengthOffset = false;

  float effectiveMm = 0.0f;
  uint32_t linearSteps = cutLengthToSteps(cutLongitud, &effectiveMm);
  if (effectiveMm <= 0.0f || linearSteps == 0)
  {
    server.send(400, "text/plain", "Clean: longitud efectiva invalida");
    return;
  }

  setCycleError(E000);
  setCycleFaultReason("");
  cutRequested = true;  // antes del abort: las esperas de settle ceden de inmediato
  abortPendingPrefeederSettle("clean");
  String msg = "Clean 1st: " + String(cutLongitud) + " mm x1 -> " + String(linearSteps) + " pulsos";
  server.send(200, "text/plain", msg);
}

void handleStartCut()
{
  if (!server.hasArg("partNumber") || !server.hasArg("cantidad"))
  {
    server.send(400, "text/plain", "Faltan parametros numero de parte o cantidad");
    return;
  }
  if (cycleActive)
  {
    server.send(400, "text/plain", "Ya hay un ciclo en progreso");
    return;
  }

  int qty = server.arg("cantidad").toInt();

  if (!safetyAllowsRun())
  {
    server.send(403, "text/plain", "Seguridad activa: sensores/PreFeeder — pulse Reset (PLC) para liberar");
    return;
  }
  if (!servoCanReady)
  {
    setCycleError(E008, "servo_can");
    uiNotify();
    server.send(503, "text/plain", "Servo CAN no listo (E008)");
    return;
  }
  if (prefeederIdleModeActive())
  {
    server.send(403, "text/plain", "PreFeeder en Materialista: desactívelo (Production) para correr la maquina");
    return;
  }

  asdaSyncConfig();

  String selectedPartNumber = server.arg("partNumber");
  uint16_t selectedLengthMm = 0;
  if (!findPartLength(selectedPartNumber, selectedLengthMm))
  {
    server.send(400, "text/plain", "Numero de parte no valido");
    return;
  }
  if (activePartNumber != selectedPartNumber && !selectActivePart(selectedPartNumber))
  {
    server.send(409, "text/plain", "No se pudo activar el numero de parte");
    return;
  }
  if (basePfApplyPending)
  {
    server.send(503, "text/plain", "Configuracion del PreFeeder pendiente; verifique la comunicacion");
    return;
  }
  cutLongitud = selectedLengthMm;
  cutCantidad = qty;
  cutUseLengthOffset = true;

  if (server.hasArg("cutOffsetMm"))
  {
    cutOffsetMm = clampCutOffsetMm(server.arg("cutOffsetMm").toFloat());
    persistMachineBaseNow();
    DBG_PRINT("CUT offset longitud = ");
    DBG_PRINT(cutOffsetMm, 2);
    DBG_PRINTLN(" mm");
  }

  if (cutLongitud <= 0 || cutCantidad <= 0)
  {
    server.send(400, "text/plain", "Longitud y cantidad deben ser mayores a cero");
    return;
  }
  float effectiveMm = 0.0f;
  uint32_t linearSteps = cutLengthToSteps(cutLongitud, &effectiveMm);
  if (effectiveMm <= 0.0f)
  {
    server.send(400, "text/plain", "Longitud efectiva invalida");
    return;
  }
  if (linearSteps == 0)
  {
    server.send(400, "text/plain", "Longitud demasiado corta (min. " + String((int)linearGripperAreaMm + 1) + " mm)");
    return;
  }

  uint16_t batchesNeeded = (uint16_t)((cutCantidad + depositBatchSize - 1) / depositBatchSize);
  uint16_t maxBatch = depositMaxBatchIndex();
  if (batchesNeeded > maxBatch)
  {
    server.send(400, "text/plain",
      "Bandeja " + String((int)TRAY_LENGTH_MM) + " mm: max " + String(maxBatch)
      + " posiciones de deposito (requiere " + String(batchesNeeded) + ")");
    return;
  }


  if (server.hasArg("offsetMm"))
  {
    feedOffsetMm = clampFeedOffsetMm(server.arg("offsetMm").toFloat());
    saveFeedOffsetToNvs();
    DBG_PRINT("OFFSET alimentacion aplicado al iniciar = ");
    DBG_PRINT(feedOffsetMm);
    DBG_PRINTLN(" mm");
  }

  setCycleError(E000);
  setCycleFaultReason("");
  cutRequested = true;  // antes del abort: las esperas de settle ceden de inmediato
  abortPendingPrefeederSettle("startCut");
  uiNotify();
  String msg = "Corte " + selectedPartNumber + ": " + String(cutLongitud) + " mm";
  if (fabsf(cutOffsetMm) >= 0.05f)
  {
    msg += " (offset ";
    if (cutOffsetMm > 0.0f) msg += "+";
    msg += String(cutOffsetMm, 1) + " → " + String(effectiveMm, 1) + " mm)";
  }
  msg += " -> " + String(linearSteps) + " pulsos lineal, x" + String(cutCantidad);
  server.send(200, "text/plain", msg);
}

void handlePauseCycle()
{
  if (cyclePaused || cyclePausePending)
  {
    // Solo reanudar con enlace TCP OK (status fresco; ping solo si dudoso).
    if (!peerVerifyLinkFastOrPing())
    {
      server.send(403, "application/json", "{\"error\":\"Sin comunicación TCP con PreFeeder (o en falla). Espere reconexión para reanudar.\",\"paused\":true,\"active\":" + String(cycleActive ? "true" : "false") + "}");
      return;
    }
    if (!safetyAllowsRun())
    {
      server.send(403, "application/json", "{\"error\":\"Seguridad activa — pulse Reset (PLC) para liberar\",\"paused\":true,\"active\":" + String(cycleActive ? "true" : "false") + "}");
      return;
    }
    if (!servoCanReady)
    {
      setCycleError(E008, "servo_can");
      uiNotify();
      server.send(503, "application/json", "{\"error\":\"Servo CAN no listo\",\"errorCode\":8,\"paused\":true,\"active\":" + String(cycleActive ? "true" : "false") + "}");
      return;
    }
    if (prefeederIdleModeActive())
    {
      server.send(403, "application/json", "{\"error\":\"PreFeeder en Materialista: desactívelo (Production) para correr la maquina\",\"paused\":true,\"active\":" + String(cycleActive ? "true" : "false") + "}");
      return;
    }
    cyclePausePending = false;
    leaveCyclePaused();
    peerCommSuspect = false;
    refreshSafetyErrorCode();
    towerForceResync = true;
  }
  else
  {
    // Pause cooperativo: termina el paso actual, luego pausa (sin corte físico inmediato).
    if (cycleActive)
    {
      cyclePausePending = true;
      setCyclePauseReason("Pause manual (terminando paso)");
      Serial.println("CYCLE PAUSE pendiente — al terminar el paso");
      pushLog("CYCLE PAUSE pendiente — fin de paso");
      uiNotify();
    }
  }

  const bool pausedUi = cyclePaused || cyclePausePending;
  String response = "{\"paused\":" + String(pausedUi ? "true" : "false");
  response += ",\"pausePending\":" + String(cyclePausePending ? "true" : "false");
  response += ",\"active\":" + String(cycleActive ? "true" : "false") + "}";
  server.send(200, "application/json", response);
}

void handleAbortCycle()
{
  if (!cycleActive)
  {
    server.send(403, "application/json", "{\"ok\":false,\"error\":\"No hay ciclo activo\"}");
    return;
  }

  abortCycleRoutine(true);
  uiOfferLinearRecovery = false;
  lastCycleSuccess = false;
  lastCycleTotalMs = 0;
  String response = "{\"ok\":true,\"active\":false,\"paused\":false";
  response += ",\"atHome\":" + String(posPulses == 0 ? "true" : "false");
  response += ",\"posPulses\":" + String(posPulses);
  response += ",\"safeZoneMm\":" + String(LINEAR_SAFE_ZONE_MM);
  response += "}";
  server.send(200, "application/json", response);
  Serial.println("WEB: ciclo abortado");
}

void handleAckRecovery()
{
  uiOfferLinearRecovery = false;
  server.send(200, "application/json", "{\"ok\":true,\"offerRecovery\":false}");
}

void handleHomeAndPlcReset()
{
  // Solo HOME del lineal (sin pulso Reset PLC). Misma política que /linearHome.
  if (externalStopInputActive())
  {
    server.send(403, "application/json", "{\"error\":\"Parada externa PreFeeder (sin TCP o en falla)\"}");
    return;
  }
  if (manualLinearBlocked())
  {
    server.send(409, "application/json", "{\"error\":\"Movimiento no disponible (ciclo o lineal activo)\"}");
    return;
  }

  DBG_PRINTLN("MANUAL: HOME torque (origen +)");
  uiOfferLinearRecovery = false;
  const bool ok = linearRunTorqueHome();

  String response = "{\"ok\":";
  response += ok ? "true" : "false";
  response += ",\"alreadyHome\":false,\"posPulses\":" + String(posPulses);
  response += ",\"atHome\":" + String(posPulses == 0 ? "true" : "false");
  response += ",\"resetPulsed\":false}";
  server.send(ok ? 200 : 500, "application/json", response);
}

// Solo limpia errores enclavados (sensores / Exxx / PreFeeder). No pulsa salida RESET PLC ni mueve lineal.
void handleResetErrors()
{
  DBG_PRINTLN("RESET ERRORS: sensores + ciclo + PreFeeder (sin pulso PLC)");
  pushLog("RESET ERRORS (sin PLC)");
  resetSensorLatchesAfterFreshSnapshot();
  server.send(200, "application/json",
              String("{\"ok\":true,\"plcPulsed\":false,\"buzzerMuted\":")
              + (tcmBuzzerMuted ? "true" : "false") + "}");
}

void handleSetBuzzerMute()
{
  bool on = false;
  if (server.hasArg("on"))
    on = (server.arg("on") == "1" || server.arg("on") == "true");
  else if (server.hasArg("mute"))
    on = (server.arg("mute") == "1" || server.arg("mute") == "true");
  else
  {
    server.send(200, "application/json",
                String("{\"ok\":true,\"buzzerMuted\":") + (tcmBuzzerMuted ? "true" : "false") + "}");
    return;
  }
  applyTcmBuzzerMute(on);
  server.send(200, "application/json",
              String("{\"ok\":true,\"buzzerMuted\":") + (tcmBuzzerMuted ? "true" : "false") + "}");
}

void handleSetStepByStep()
{
  bool on = stepByStepMode;
  if (server.hasArg("on"))
    on = (server.arg("on") == "1" || server.arg("on") == "true");
  else if (server.hasArg("enabled"))
    on = (server.arg("enabled") == "1" || server.arg("enabled") == "true");
  else
  {
    server.send(200, "application/json",
                String("{\"ok\":true,\"stepByStep\":") + (stepByStepMode ? "true" : "false") + "}");
    return;
  }
  applyStepByStepMode(on);
  server.send(200, "application/json",
              String("{\"ok\":true,\"stepByStep\":") + (stepByStepMode ? "true" : "false") + "}");
}

static String buildCycleStatusJson()
{
  uint32_t elapsed = 0;
  uint32_t remaining = 0;
  int percent = 0;

  if (cycleActive)
  {
    elapsed = cycleElapsedMs();

    if (progressTotalReps > 0)
    {
      int repsRestantes = progressTotalReps - completedReps;
      uint32_t currentRepElapsed = (cyclePaused && cyclePauseBeganMs)
        ? (cyclePauseBeganMs - repStartTime)
        : (millis() - repStartTime);
      if (completedReps > 0)
      {
        uint32_t avgPerRep = totalCompletedTime / completedReps;
        if (repsRestantes > 0)
        {
          uint32_t remainingCurrentRep = (currentRepElapsed < avgPerRep) ? (avgPerRep - currentRepElapsed) : 0;
          remaining = ((repsRestantes - 1) * avgPerRep) + remainingCurrentRep;
        }
        percent = (completedReps * 100) / progressTotalReps;
        if (avgPerRep > 0)
          percent += ((currentRepElapsed * 100) / avgPerRep) / progressTotalReps;
      }
      else
      {
        if (progressStep > 0 && progressTotalSteps > 0)
        {
          uint32_t estRepMs = (currentRepElapsed * (uint32_t)progressTotalSteps) / (uint32_t)progressStep;
          uint32_t estTotalMs = estRepMs * (uint32_t)progressTotalReps;
          if (estTotalMs > elapsed)
            remaining = estTotalMs - elapsed;
          if (estTotalMs > 0)
            percent = (elapsed * 100) / estTotalMs;
        }
        else if (currentRepElapsed > 300)
        {
          uint32_t estTotalMs = currentRepElapsed * (uint32_t)progressTotalReps * 2;
          if (estTotalMs > elapsed)
            remaining = estTotalMs - elapsed;
          if (estTotalMs > 0)
            percent = (elapsed * 100) / estTotalMs;
        }
      }
      if (percent > 99) percent = 99;
    }
  }
  else if (lastCycleSuccess && lastCycleTotalMs > 0)
  {
    elapsed = lastCycleTotalMs;
    remaining = 0;
    percent = 100;
  }
  else if (progressTotalReps > 0 && cycleStartTime != 0)
  {
    // Error / safety stop: conservar progreso y tiempo congelado.
    elapsed = cycleElapsedMs();
    remaining = 0;
    percent = (completedReps * 100) / progressTotalReps;
    if (percent > 100) percent = 100;
  }

  refreshSafetyErrorCode();
  bool safeToRun = safetyAllowsRun();
  bool canOnline = sensorCanOnline();
  uint32_t ageMs = sensorEverOnline ? (millis() - sensorLastRxMs) : 0;

  String response = "{";
  response += "\"seq\":" + String(uiEventSeq);
  response += ",\"paused\":" + String((cyclePaused || cyclePausePending) ? "true" : "false");
  response += ",\"pausePending\":" + String(cyclePausePending ? "true" : "false");
  response += ",\"stepByStep\":" + String(stepByStepMode ? "true" : "false");
  response += ",\"pauseReason\":\"";
  response += peerEsc(String(cyclePauseReason));
  response += "\"";
  response += ",\"faultReason\":\"";
  response += peerEsc(String(cycleFaultReason));
  response += "\"";
  response += ",\"errorCode\":\"";
  response += cycleErrorCodeString(cycleErrorCode);
  response += "\"";
  response += ",\"errorCodeNum\":" + String((unsigned)cycleErrorCode);
  response += ",\"active\":" + String(cycleActive ? "true" : "false");
  response += ",\"aborted\":" + String(cycleAborted ? "true" : "false");
  response += ",\"step\":" + String(progressStep);
  response += ",\"totalSteps\":" + String(progressTotalSteps);
  response += ",\"currentRep\":" + String(progressCurrentRep);
  response += ",\"totalReps\":" + String(progressTotalReps);
  response += ",\"elapsed\":" + String(elapsed);
  response += ",\"remaining\":" + String(remaining);
  response += ",\"percent\":" + String(percent);
  response += ",\"lastTotalMs\":" + String(lastCycleTotalMs);
  response += ",\"lastCycleSuccess\":" + String(lastCycleSuccess ? "true" : "false");
  response += ",\"safetyError\":" + String(safetyErrorCode);
  {
    uint8_t activeErrs[ERR_ACTIVE_MAX];
    const uint8_t activeN = collectActiveErrorCodes(activeErrs, ERR_ACTIVE_MAX);
    response += ",\"activeErrors\":[";
    for (uint8_t i = 0; i < activeN; i++)
    {
      if (i) response += ",";
      response += String(activeErrs[i]);
    }
    response += "]";
    // No pisar errorCode (string Exxx del ciclo). Código unificado del catálogo UI:
    response += ",\"activeErrorCode\":";
    response += String(activeN ? activeErrs[0] : (uint8_t)0);
  }
  response += ",\"safeToRun\":" + String(safeToRun ? "true" : "false");
  response += ",\"safetyStopPending\":" + String(safetyStopPending ? "true" : "false");
  response += ",\"externalStop\":" + String(externalStopInputActive() ? "true" : "false");
  response += ",\"buzzerMuted\":" + String(tcmBuzzerMuted ? "true" : "false");
  response += ",\"canAbort\":" + String(cycleActive ? "true" : "false");
  response += ",\"offerRecovery\":" + String(uiOfferLinearRecovery ? "true" : "false");
  response += ",\"safeZoneMm\":" + String(LINEAR_SAFE_ZONE_MM);
  response += ",\"atHome\":" + String(posPulses == 0 ? "true" : "false");
  response += ",\"sensors\":{";
  response += "\"canOnline\":" + String(canOnline ? "true" : "false");
  response += ",\"stale\":" + String(sensorStaleLatched ? "true" : "false");
  response += ",\"offlineLatched\":" + String(sensorStaleLatched ? "true" : "false");
  response += ",\"ageMs\":" + String(ageMs);
  response += ",\"sequence\":" + String(sensorSequence);
  response += ",\"gripper\":" + String((sensorLatchedBitmask & SENSOR_BIT_GRIPPER) ? "true" : "false");
  response += ",\"holder\":" + String((sensorLatchedBitmask & SENSOR_BIT_HOLDER) ? "true" : "false");
  response += ",\"cutter\":" + String((sensorLatchedBitmask & SENSOR_BIT_CUTTER) ? "true" : "false");
  response += ",\"hoseA\":" + String((sensorLatchedBitmask & SENSOR_BIT_HOSE_A) ? "true" : "false");
  response += ",\"hoseB\":" + String((sensorLatchedBitmask & SENSOR_BIT_HOSE_B) ? "true" : "false");
  response += ",\"feedHoseL\":" + String(isHoseSensorActiveSide(false) ? "true" : "false");
  response += ",\"feedHoseR\":" + String(isHoseSensorActiveSide(true) ? "true" : "false");
  response += ",\"tray\":" + String((sensorLatchedBitmask & SENSOR_BIT_TRAY) ? "true" : "false");
  response += ",\"latchedMask\":" + String(sensorLatchedBitmask);
  response += ",\"liveMask\":" + String(sensorBitmask);
  response += ",\"lastAlarmCode\":" + String(safetyErrorCode);
  response += ",\"canInit\":" + String(canInitialized ? "true" : "false");
  response += "}";
  response += ",\"omValidation\":{";
  response += "\"phase1Mm\":" + String(omPhase1Mm, 2);
  response += ",\"phase2Mm\":" + String(omPhase2Mm, 2);
  response += ",\"errPhase1Mm\":" + String(omErrPhase1Mm, 2);
  response += ",\"errPhase2Mm\":" + String(omErrPhase2Mm, 2);
  response += ",\"totalMm\":" + String(omTotalMm, 2);
  response += ",\"errTotalMm\":" + String(omErrTotalMm, 2);
  response += ",\"phase1Ok\":" + String(omPhase1Ok ? "true" : "false");
  response += ",\"phase2Ok\":" + String(omPhase2Ok ? "true" : "false");
  response += ",\"totalInternalOk\":" + String(omTotalInternalOk ? "true" : "false");
  response += ",\"totalProdOk\":" + String(omTotalProdOk ? "true" : "false");
  response += ",\"cutAllowed\":" + String(omPieceLengthOkForCut() ? "true" : "false");
  response += ",\"internalTolMm\":" + String(INTERNAL_LENGTH_TOL_MM, 1);
  response += ",\"prodTolMm\":" + String(PRODUCTION_LENGTH_TOL_MM, 1);
  response += "}";
  // Log sistema TCM (pushLog) — L/R viven en la vista PreFeeder.
  response += ",\"comm\":{";
  response += "\"system\":{";
  response += "\"msg\":\"";
  response += peerEsc(lastSystemMsg);
  response += "\",\"ageMs\":";
  response += lastSystemMsgMs ? (millis() - lastSystemMsgMs) : 0;
  response += "}}";
  response += "}";
  return response;
}

void handleGetCycleStatus()
{
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", buildCycleStatusJson());
}

// Long-poll: espera cambio real (CAN/peer/ciclo) o timeout. La UI se rearma al instante.
static void handleUiWait()
{
  const uint32_t since = server.hasArg("since") ? (uint32_t)server.arg("since").toInt() : 0;
  const bool wantPf = server.hasArg("pf") && server.arg("pf") == "1";
  const bool motionBusy = linearIsMoving() || feedPhaseIsActive();
  const uint32_t maxWait = motionBusy ? (uint32_t)UI_WAIT_MOTION_MS
                          : ((cycleActive && !cyclePaused)
                               ? (uint32_t)UI_WAIT_CYCLE_MS
                               : (uint32_t)UI_WAIT_IDLE_MS);
  const uint32_t t0 = millis();
  static bool watchExtStop = false;
  static bool watchExtInit = false;
  if (!watchExtInit)
  {
    watchExtStop = externalStopInputActive();
    watchExtInit = true;
  }

  while (maxWait > 0 && (uint32_t)(millis() - t0) < maxWait)
  {
    serviceCANRx();
    peerRxDrainAll();
    const bool ext = externalStopInputActive();
    if (ext != watchExtStop)
    {
      watchExtStop = ext;
      uiNotify();
    }
    static bool watchOnline = false;
    static bool watchOnlineInit = false;
    const bool online = sensorCanOnline();
    if (!watchOnlineInit) { watchOnline = online; watchOnlineInit = true; }
    else if (online != watchOnline) { watchOnline = online; uiNotify(); }

    static uint32_t watchCycleSig = 0;
    const uint32_t sig = ((uint32_t)cycleActive)
      | ((uint32_t)cyclePaused << 1)
      | ((uint32_t)(progressStep & 0xFF) << 2)
      | ((uint32_t)(progressCurrentRep & 0xFF) << 10)
      | ((uint32_t)(safetyErrorCode & 0xFF) << 18)
      | ((uint32_t)(uiOfferLinearRecovery ? 1u : 0u) << 26);
    if (sig != watchCycleSig) { watchCycleSig = sig; uiNotify(); }

    if (uiEventSeq != since) break;
    // No re-entrar handleClient dentro del long-poll.
    delay(2);
  }

  String body = buildCycleStatusJson();
  if (wantPf)
  {
    body.remove(body.length() - 1);
    body += ",\"prefeeder\":";
    body += peerWebJson(true);
    body += "}";
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", body);
}

void handleGetCycleTime()
{
  uint32_t currentMs = 0;
  if (gripToHomeEnabled && gripToHomeActive && gripToHomeStartMs > 0)
    currentMs = millis() - gripToHomeStartMs;
  uint32_t avgMs = gripToHomeCount > 0 ? (gripToHomeTotalMs / gripToHomeCount) : 0;

  String response = "{";
  response += "\"enabled\":" + String(gripToHomeEnabled ? "true" : "false");
  response += ",\"lastMs\":" + String(gripToHomeLastMs);
  response += ",\"avgMs\":" + String(avgMs);
  response += ",\"count\":" + String(gripToHomeCount);
  response += ",\"currentMs\":" + String(currentMs);
  response += ",\"measuring\":" + String((gripToHomeEnabled && gripToHomeActive) ? "true" : "false");
  response += ",\"cycleActive\":" + String(cycleActive ? "true" : "false");
  response += ",\"cyclePaused\":" + String(cyclePaused ? "true" : "false");
  response += "}";
  server.send(200, "application/json", response);
}

void handleSetCycleTime()
{
  if (server.hasArg("reset") && server.arg("reset") == "1")
  {
    gripToHomeReset();
    gripToHomeEnabled = true;
    DBG_PRINTLN("Cycle time: estadisticas reseteadas");
  }
  else if (server.hasArg("enabled"))
  {
    // Medicion siempre activa; el flag solo se acepta por compatibilidad.
    gripToHomeEnabled = true;
    if (server.arg("enabled") != "1")
      DBG_PRINTLN("Cycle time: ignore disable (solo lectura)");
  }

  handleGetCycleTime();
}

static String plcStatusJson()
{
  String r = "{";
  r += "\"cutters\":" + String(stCutters ? "true" : "false");
  r += ",\"grippers\":" + String(stGrippers ? "true" : "false");
  r += ",\"holder\":" + String(stHolder ? "true" : "false");
  r += ",\"fgtray\":" + String(stFgtray ? "true" : "false");
  r += ",\"reset\":" + String(stReset ? "true" : "false");
  r += ",\"stepByStep\":" + String(stepByStepMode ? "true" : "false");
  r += "}";
  return r;
}

static bool setPlcOutputByName(String outName, bool state)
{
  outName.trim();
  outName.toUpperCase();
  if (outName == "CUTTER" || outName == "CUTTERS") stCutters = state;
  else if (outName == "GRIPPER" || outName == "GRIPPERS") stGrippers = state;
  else if (outName == "HOLDER") stHolder = state;
  else if (outName == "FGTRAY" || outName == "TRAY") stFgtray = state;
  else if (outName == "RESET") stReset = state;
  else return false;
  applyPlcOutputs();
  return true;
}

// --- 14.3 Salidas PLC ---
void handlePlcSet()
{
  if (!server.hasArg("out") || !server.hasArg("state"))
  {
    server.send(400, "application/json", "{\"error\":\"Faltan parametros\"}");
    return;
  }

  String outName = server.arg("out");
  outName.trim();
  outName.toUpperCase();
  bool isTray = (outName == "FGTRAY" || outName == "TRAY");
  bool isReset = (outName == "RESET");

  // Manuales OK en idle o pausa; RESET siempre (seguridad). Resto bloquea con ciclo en marcha.
  if (cycleActive && !cyclePaused && !isTray && !isReset)
  {
    server.send(409, "application/json", "{\"error\":\"Ciclo en marcha\"}");
    return;
  }
  if (cycleActive && !cyclePaused && isTray)
  {
    server.send(409, "application/json", "{\"error\":\"Ciclo en marcha\"}");
    return;
  }

  bool desired = server.arg("state").toInt() != 0;
  if (!setPlcOutputByName(server.arg("out"), desired))
  {
    server.send(400, "application/json", "{\"error\":\"Salida desconocida\"}");
    return;
  }
  // Liberar enclavamiento con CUALQUIER flanco OFF de RESET PLC
  // (btn Reset sensores, HOME+Reset, toggle manual /plcSet, etc.).
  if (outName == "RESET" && !desired)
    resetSensorLatchesAfterFreshSnapshot();
  server.send(200, "application/json", plcStatusJson());
}

void handlePlcStatus()
{
  server.send(200, "application/json", plcStatusJson());
}

// --- 14.4 Alimentación (bypass, test, modo, offset) ---
void handleGetFeedBypassSteps()
{
  String response = "{";
  response += "\"mm\":" + String(feedStepsToMm(feedBypassSteps, false), 1);
  response += ",\"mmB\":" + String(feedStepsToMm(feedBypassStepsB, true), 1);
  response += ",\"mmMin\":" + String(feedStepsToMm(FEED_REF_STEPS_MIN, false), 1);
  response += ",\"mmMax\":" + String(feedStepsToMm(FEED_REF_STEPS_MAX, false), 1);
  response += ",\"mmMinB\":" + String(feedStepsToMm(FEED_REF_STEPS_MIN, true), 1);
  response += ",\"mmMaxB\":" + String(feedStepsToMm(FEED_REF_STEPS_MAX, true), 1);
  response += ",\"steps\":" + String(feedBypassSteps);
  response += ",\"stepsB\":" + String(feedBypassStepsB);
  response += "}";
  server.send(200, "application/json", response);
}

void handleSetFeedBypassSteps()
{
  if (cycleActive)
  {
    server.send(409, "application/json", "{\"error\":\"Ciclo activo\"}");
    return;
  }
  if (!server.hasArg("mm") && !server.hasArg("steps"))
  {
    server.send(400, "application/json", "{\"error\":\"Falta parametro mm\"}");
    return;
  }

  // L/A = Izquierdo (legacy A); R/B = Derecho (legacy B). Alinea con PreFeeder L/R.
  const bool sideR = server.hasArg("side")
    && (server.arg("side") == "R" || server.arg("side") == "r"
        || server.arg("side") == "B" || server.arg("side") == "b");
  int32_t steps = server.hasArg("mm")
    ? clampFeedBypassSteps(feedMmToSteps(server.arg("mm").toFloat(), sideR))
    : clampFeedBypassSteps(server.arg("steps").toInt());
  if (sideR)
  {
    feedBypassStepsB = steps;
    if (feedCalMeasuredMmB > 0.0f)
    {
      feedRecomputeStepsPerMmB();
      saveFeedCalToNvs();
    }
  }
  else
  {
    feedBypassSteps = steps;
    if (feedCalMeasuredMm > 0.0f)
    {
      feedRecomputeStepsPerMm();
      saveFeedCalToNvs();
    }
  }
  saveFeedBypassStepsToNvs();
  DBG_PRINT("Pasos alimentacion guardados ");
  DBG_PRINT(sideR ? "R: " : "L: ");
  DBG_PRINTLN(steps);

  String response = "{\"ok\":true,\"mm\":" + String(feedStepsToMm(feedBypassSteps, false), 1);
  response += ",\"mmB\":" + String(feedStepsToMm(feedBypassStepsB, true), 1);
  response += ",\"steps\":" + String(feedBypassSteps);
  response += ",\"stepsB\":" + String(feedBypassStepsB) + "}";
  server.send(200, "application/json", response);
}

void handleGetFeedTestConfig()
{
  const float tMmL = feedStepsToMm(feedTestSolidSteps, false);
  const float tMmR = feedStepsToMm(feedTestSolidStepsR, true);
  const float vMmL = feedPpToMmS(feedSsFastPpL, false);
  const float vMmR = feedPpToMmS(feedSsFastPpR, true);
  const FeedProfilePlan planL = feedProfilePlan(tMmL, vMmL, false);
  const FeedProfilePlan planR = feedProfilePlan(tMmR, vMmR, true);

  String response = "{";
  response += "\"solidMm\":" + String(tMmL, 1);
  response += ",\"solidMmR\":" + String(tMmR, 1);
  response += ",\"velocityMmSL\":" + String(vMmL, 1);
  response += ",\"velocityMmSR\":" + String(vMmR, 1);
  response += ",\"fastMmSL\":" + String(vMmL, 1);
  response += ",\"fastMmSR\":" + String(vMmR, 1);
  response += ",\"solidMin\":" + String(feedStepsToMm(FEED_TEST_SOLID_MIN, false), 1);
  response += ",\"solidMax\":" + String(feedStepsToMm(FEED_TEST_SOLID_MAX, false), 1);
  response += ",\"solidMinR\":" + String(feedStepsToMm(FEED_TEST_SOLID_MIN, true), 1);
  response += ",\"solidMaxR\":" + String(feedStepsToMm(FEED_TEST_SOLID_MAX, true), 1);
  response += ",\"speedMin\":" + String(FEED_MM_S_MIN, 1);
  response += ",\"speedMaxL\":" + String(feedMaxMmS(false), 1);
  response += ",\"speedMaxR\":" + String(feedMaxMmS(true), 1);
  response += ",\"rampMs\":" + String(FEED_SERVO_RAMP_MS);
  response += ",\"laserValidation\":" + String(laserValidationEnabled ? "true" : "false");
  feedProfileAppendJson(planL, response, "planL");
  feedProfileAppendJson(planR, response, "planR");
  response += "}";
  server.send(200, "application/json", response);
}

// Lectura encoder servo CAN feeder (0x6064). Escala: rodillo ø50, 131072 c/rev.
void handleGetFeedCanEncoder()
{
  int32_t posL = 0;
  int32_t posR = 0;
  const bool okL = servoCanReady && canReadSdoI32(SERVO_NODE_L, 0x6064, 0x00, posL, 150);
  const bool okR = servoCanReady && canReadSdoI32(SERVO_NODE_R, 0x6064, 0x00, posR, 150);

  const int32_t dL = (okL && feedCanEncZeroLSet) ? feedCanEncSignedDelta(posL, feedCanEncZeroL) : 0;
  const int32_t dR = (okR && feedCanEncZeroRSet) ? feedCanEncSignedDelta(posR, feedCanEncZeroR) : 0;

  char buf[420];
  snprintf(buf, sizeof(buf),
           "{\"ok\":%s,\"canReady\":%s,\"rollerMm\":%.0f,\"encIncPerRev\":%lu,"
           "\"countsPerMm\":%.4f,\"mmPerCount\":%.8f,"
           "\"L\":{\"ok\":%s,\"pos\":%ld,\"zeroSet\":%s,\"delta\":%ld,\"mm\":%.3f},"
           "\"R\":{\"ok\":%s,\"pos\":%ld,\"zeroSet\":%s,\"delta\":%ld,\"mm\":%.3f}}",
           (okL || okR) ? "true" : "false",
           servoCanReady ? "true" : "false",
           FEED_ROLLER_DIAM_MM,
           (unsigned long)SERVO_ENC_INC_PER_REV,
           FEED_ENC_COUNTS_PER_MM,
           1.0f / FEED_ENC_COUNTS_PER_MM,
           okL ? "true" : "false", (long)posL,
           feedCanEncZeroLSet ? "true" : "false",
           (long)dL, okL && feedCanEncZeroLSet ? feedCanEncCountsToMm(dL) : 0.0f,
           okR ? "true" : "false", (long)posR,
           feedCanEncZeroRSet ? "true" : "false",
           (long)dR, okR && feedCanEncZeroRSet ? feedCanEncCountsToMm(dR) : 0.0f);
  server.send((okL || okR) ? 200 : 503, "application/json", buf);
}

void handleFeedCanEncoderZero()
{
  String side = server.hasArg("side") ? server.arg("side") : "LR";
  side.toUpperCase();
  bool doL = (side.indexOf('L') >= 0) || side == "BOTH" || side == "LR" || side.length() == 0;
  bool doR = (side.indexOf('R') >= 0) || side == "BOTH" || side == "LR" || side.length() == 0;
  if (!doL && !doR) { doL = true; doR = true; }

  if (!servoCanReady)
  {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"Servo CAN no listo\"}");
    return;
  }

  int32_t posL = 0;
  int32_t posR = 0;
  bool okL = true;
  bool okR = true;
  if (doL)
  {
    okL = canReadSdoI32(SERVO_NODE_L, 0x6064, 0x00, posL, 150);
    if (okL) { feedCanEncZeroL = posL; feedCanEncZeroLSet = true; }
  }
  if (doR)
  {
    okR = canReadSdoI32(SERVO_NODE_R, 0x6064, 0x00, posR, 150);
    if (okR) { feedCanEncZeroR = posR; feedCanEncZeroRSet = true; }
  }

  char buf[280];
  snprintf(buf, sizeof(buf),
           "{\"ok\":%s,\"L\":{\"ok\":%s,\"zero\":%ld},\"R\":{\"ok\":%s,\"zero\":%ld}}",
           ( (!doL || okL) && (!doR || okR) ) ? "true" : "false",
           okL ? "true" : "false", (long)feedCanEncZeroL,
           okR ? "true" : "false", (long)feedCanEncZeroR);
  server.send(( (!doL || okL) && (!doR || okR) ) ? 200 : 503, "application/json", buf);
}

void handleSetFeedTestConfig()
{
  if (cycleActive)
  {
    server.send(409, "application/json", "{\"error\":\"Ciclo activo\"}");
    return;
  }
  if (!server.hasArg("solidMm") && !server.hasArg("solid"))
  {
    server.send(400, "application/json", "{\"error\":\"Falta solidMm\"}");
    return;
  }

  if (server.hasArg("solidMm"))
    feedTestSolidSteps = clampFeedTestSolidSteps(feedMmToSteps(server.arg("solidMm").toFloat(), false));
  else
    feedTestSolidSteps = clampFeedTestSolidSteps(server.arg("solid").toInt());
  if (server.hasArg("chunk"))
    feedTestChunkSteps = clampFeedTestChunkSteps(server.arg("chunk").toInt());
  if (server.hasArg("solidMmR"))
    feedTestSolidStepsR = clampFeedTestSolidSteps(feedMmToSteps(server.arg("solidMmR").toFloat(), true));
  else if (server.hasArg("solidR"))
    feedTestSolidStepsR = clampFeedTestSolidSteps(server.arg("solidR").toInt());
  else
    feedTestSolidStepsR = feedTestSolidSteps;
  if (server.hasArg("chunkR"))
    feedTestChunkStepsR = clampFeedTestChunkSteps(server.arg("chunkR").toInt());
  else
    feedTestChunkStepsR = feedTestChunkSteps;
  if (server.hasArg("velocityMmS"))
    feedSsFastPpL = feedMmSToPp(server.arg("velocityMmS").toFloat(), false);
  else if (server.hasArg("fastMmS"))
    feedSsFastPpL = feedMmSToPp(server.arg("fastMmS").toFloat(), false);
  else if (server.hasArg("fast"))
    feedSsFastPpL = clampFeedSsSpeedPp(server.arg("fast").toInt());
  if (server.hasArg("velocityMmSR"))
    feedSsFastPpR = feedMmSToPp(server.arg("velocityMmSR").toFloat(), true);
  else if (server.hasArg("fastMmSR"))
    feedSsFastPpR = feedMmSToPp(server.arg("fastMmSR").toFloat(), true);
  else if (server.hasArg("fastR"))
    feedSsFastPpR = clampFeedSsSpeedPp(server.arg("fastR").toInt());
  else if (server.hasArg("velocityMmS") || server.hasArg("fastMmS") || server.hasArg("fast"))
    feedSsFastPpR = feedSsFastPpL;
  if (server.hasArg("laserValidation"))
  {
    String v = server.arg("laserValidation");
    v.toLowerCase();
    laserValidationEnabled = (v == "1" || v == "true" || v == "on" || v == "yes");
  }

  const float tMmL = feedStepsToMm(feedTestSolidSteps, false);
  const float tMmR = feedStepsToMm(feedTestSolidStepsR, true);
  const float vMmL = feedPpToMmS(feedSsFastPpL, false);
  const float vMmR = feedPpToMmS(feedSsFastPpR, true);
  const FeedProfilePlan planL = feedProfilePlan(tMmL, vMmL, false);
  const FeedProfilePlan planR = feedProfilePlan(tMmR, vMmR, true);
  if ((feedTestSolidSteps > 0 && !planL.valid) || (feedTestSolidStepsR > 0 && !planR.valid))
  {
    String err = "{\"ok\":false,\"error\":\"Velocity exceeds physical limit for target\"";
    if (!planL.valid && feedTestSolidSteps > 0)
      err += ",\"vMaxMmSL\":" + String(planL.vMaxMmS, 1);
    if (!planR.valid && feedTestSolidStepsR > 0)
      err += ",\"vMaxMmSR\":" + String(planR.vMaxMmS, 1);
    err += "}";
    server.send(400, "application/json", err);
    return;
  }

  saveFeedTestConfigToNvs();

  String response = "{";
  response += "\"ok\":true";
  response += ",\"solidMm\":" + String(tMmL, 1);
  response += ",\"solidMmR\":" + String(tMmR, 1);
  response += ",\"velocityMmSL\":" + String(vMmL, 1);
  response += ",\"velocityMmSR\":" + String(vMmR, 1);
  response += ",\"laserValidation\":" + String(laserValidationEnabled ? "true" : "false");
  feedProfileAppendJson(planL, response, "planL");
  feedProfileAppendJson(planR, response, "planR");
  response += "}";
  server.send(200, "application/json", response);
}

void handleFeedTestSensor()
{
  if (cycleActive || feedPhaseIsActive())
  {
    server.send(409, "application/json",
                "{\"ok\":false,\"error\":\"Ciclo o alimentacion activa\"}");
    return;
  }
  if (!servoCanReady)
  {
    server.send(503, "application/json",
                "{\"ok\":false,\"error\":\"Servo CAN no listo\",\"errorCode\":8}");
    return;
  }

  int32_t solid = feedTestSolidSteps;
  int32_t chunk = feedTestChunkSteps;
  int32_t solidR = feedTestSolidStepsR;
  int32_t chunkR = feedTestChunkStepsR;
  if (server.hasArg("solidMm"))
    solid = clampFeedTestSolidSteps(feedMmToSteps(server.arg("solidMm").toFloat(), false));
  else if (server.hasArg("solid"))
    solid = clampFeedTestSolidSteps(server.arg("solid").toInt());
  if (server.hasArg("chunk"))
    chunk = clampFeedTestChunkSteps(server.arg("chunk").toInt());
  if (server.hasArg("solidMmR"))
    solidR = clampFeedTestSolidSteps(feedMmToSteps(server.arg("solidMmR").toFloat(), true));
  else if (server.hasArg("solidR"))
    solidR = clampFeedTestSolidSteps(server.arg("solidR").toInt());
  if (server.hasArg("chunkR"))
    chunkR = clampFeedTestChunkSteps(server.arg("chunkR").toInt());

  const uint32_t savedFastL = feedSsFastPpL;
  const uint32_t savedFastR = feedSsFastPpR;
  if (server.hasArg("velocityMmS"))
    feedSsFastPpL = feedMmSToPp(server.arg("velocityMmS").toFloat(), false);
  else if (server.hasArg("fastMmS"))
    feedSsFastPpL = feedMmSToPp(server.arg("fastMmS").toFloat(), false);
  else if (server.hasArg("fast"))
    feedSsFastPpL = clampFeedSsSpeedPp(server.arg("fast").toInt());
  if (server.hasArg("velocityMmSR"))
    feedSsFastPpR = feedMmSToPp(server.arg("velocityMmSR").toFloat(), true);
  else if (server.hasArg("fastMmSR"))
    feedSsFastPpR = feedMmSToPp(server.arg("fastMmSR").toFloat(), true);
  else if (server.hasArg("fastR"))
    feedSsFastPpR = clampFeedSsSpeedPp(server.arg("fastR").toInt());

  int8_t onlySide = -1;
  if (server.hasArg("side"))
  {
    const String s = server.arg("side");
    if (s == "L" || s == "l" || s == "A" || s == "a") onlySide = 0;
    else if (s == "R" || s == "r" || s == "B" || s == "b") onlySide = 1;
  }

  const float tMmL = feedStepsToMm(solid, false);
  const float tMmR = feedStepsToMm(solidR, true);
  const float vMmL = feedPpToMmS(feedSsFastPpL, false);
  const float vMmR = feedPpToMmS(feedSsFastPpR, true);
  const bool sideL = (onlySide != 1);
  const bool sideR = (onlySide != 0);
  if ((sideL && solid > 0 && !feedProfileSideValid(tMmL, vMmL, false))
      || (sideR && solidR > 0 && !feedProfileSideValid(tMmR, vMmR, true)))
  {
    feedSsFastPpL = savedFastL;
    feedSsFastPpR = savedFastR;
    const FeedProfilePlan planL = feedProfilePlan(tMmL, vMmL, false);
    const FeedProfilePlan planR = feedProfilePlan(tMmR, vMmR, true);
    String err = "{\"ok\":false,\"error\":\"Velocity exceeds physical limit for target\"";
    if (sideL && solid > 0 && !planL.valid)
      err += ",\"vMaxMmSL\":" + String(planL.vMaxMmS, 1);
    if (sideR && solidR > 0 && !planR.valid)
      err += ",\"vMaxMmSR\":" + String(planR.vMaxMmS, 1);
    err += "}";
    server.send(400, "application/json", err);
    return;
  }

  const bool withCut = server.hasArg("cut")
    && (server.arg("cut") == "1" || server.arg("cut") == "true" || server.arg("cut") == "on");

  bool ok = runFeedTestSolidThenSensor(solid, chunk, solidR, chunkR, withCut, onlySide);
  feedSsFastPpL = savedFastL;
  feedSsFastPpR = savedFastR;

  String response = "{";
  response += "\"ok\":" + String(ok ? "true" : "false");
  response += ",\"solidSteps\":" + String(solid);
  response += ",\"chunkSteps\":" + String(chunk);
  response += ",\"solidStepsR\":" + String(solidR);
  response += ",\"chunkStepsR\":" + String(chunkR);
  response += ",\"side\":\"";
  response += (onlySide == 0) ? "L" : (onlySide == 1) ? "R" : "LR";
  response += "\"";
  response += ",\"sensorDetected\":" + String((isHoseSensorActiveSide(false) || isHoseSensorActiveSide(true)) ? "true" : "false");
  response += ",\"sensorDetectedL\":" + String(isHoseSensorActiveSide(false) ? "true" : "false");
  response += ",\"sensorDetectedR\":" + String(isHoseSensorActiveSide(true) ? "true" : "false");
  response += ",\"cut\":" + String(withCut ? "true" : "false");
  response += ",\"holderClosed\":true";
  if (!ok)
  {
    uint8_t ec = ERR_FEED_HOSE;
    const char* why = cycleFaultReason;
    if (strstr(why, "Servo CAN") != nullptr || strstr(why, "servo_can") != nullptr)
      ec = ERR_SERVO_CAN;
    else if (strstr(why, "TARGET 607A") != nullptr || strstr(why, "6064") != nullptr)
      ec = ERR_CYCLE_ABORT;
    else if (strstr(why, "laser ocupado") != nullptr)
      ec = ERR_FEED_HOSE;
    else if (strstr(why, "sensor no detectado") != nullptr)
      ec = ERR_FEED_HOSE;

    response += ",\"errorCode\":" + String((unsigned)ec);
    if (ec == ERR_SERVO_CAN)
      response += ",\"error\":\"Servo CAN no listo (E008)\"";
    else if (strstr(why, "laser ocupado") != nullptr)
      response += ",\"error\":\"Laser ocupado — retire la manguera del sensor y reintente (E016)\"";
    else if (ec == ERR_FEED_HOSE)
      response += ",\"error\":\"Sensor manguera alimentacion: no detecto a tiempo (E016)\"";
    else
      response += ",\"error\":\"Alimentacion fallida (E012)\"";
    // Test de ingeniería: no dejar Exxx enclavado (permite reintentar sin Reset).
    setCycleError(E000);
    setCycleFaultReason("");
    uiNotify();
  }
  response += "}";
  server.send(200, "application/json", response);
}

static bool manualLinearBlocked()
{
  return cycleActive || cycleRunning || cutRequested || feedPhaseIsActive() || linearIsMoving();
}

void handleGetFeedSkipEncoder()
{
  String response = "{\"skip\":";
  response += feedSkipEncoderConfirm ? "true" : "false";
  response += "}";
  server.send(200, "application/json", response);
}

void handleSetFeedSkipEncoder()
{
  if (cycleActive)
  {
    server.send(409, "application/json", "{\"error\":\"Ciclo activo\"}");
    return;
  }
  if (!server.hasArg("skip"))
  {
    server.send(400, "application/json", "{\"error\":\"Falta parametro skip\"}");
    return;
  }

  feedSkipEncoderConfirm = server.arg("skip").toInt() != 0;
  saveFeedSkipEncoderToNvs();

  DBG_PRINT("Omitir validacion encoder: ");
  DBG_PRINTLN(feedSkipEncoderConfirm ? "SI" : "NO");

  String response = "{\"ok\":true,\"skip\":";
  response += feedSkipEncoderConfirm ? "true" : "false";
  response += "}";
  server.send(200, "application/json", response);
}


static String feedOffsetJson(bool withOk)
{
  String r = "{";
  if (withOk) r += "\"ok\":true,";
  r += "\"offsetMm\":" + String(feedOffsetMm, 2);
  r += ",\"offsetMmB\":" + String(feedOffsetMmB, 2);
  r += ",\"min\":" + String(FEED_OFFSET_MM_MIN, 0);
  r += ",\"max\":" + String(FEED_OFFSET_MM_MAX, 0);
  r += ",\"stepsPerMm\":" + String(feedStepsPerMm, 2);
  r += ",\"stepsPerMmB\":" + String(feedStepsPerMmB, 2);
  r += ",\"steps\":" + String(feedOffsetSteps());
  r += ",\"stepsB\":" + String(feedOffsetStepsB());
  r += "}";
  return r;
}

void handleGetFeedOffset()
{
  server.send(200, "application/json", feedOffsetJson(false));
}

void handleSetFeedOffset()
{
  if (cycleActive)
  {
    server.send(409, "application/json", "{\"error\":\"Ciclo activo\"}");
    return;
  }
  if (!server.hasArg("offsetMm"))
  {
    server.send(400, "application/json", "{\"error\":\"Falta parametro offsetMm\"}");
    return;
  }

  const bool sideR = server.hasArg("side")
    && (server.arg("side") == "R" || server.arg("side") == "r"
        || server.arg("side") == "B" || server.arg("side") == "b");
  float mm = clampFeedOffsetMm(server.arg("offsetMm").toFloat());
  if (sideR) feedOffsetMmB = mm;
  else feedOffsetMm = mm;
  saveFeedOffsetToNvs();

  DBG_PRINT("Offset alimentacion ");
  DBG_PRINT(sideR ? "R = " : "L = ");
  DBG_PRINT(mm);
  DBG_PRINTLN(" mm");

  server.send(200, "application/json", feedOffsetJson(true));
}

static String feedCalJson(bool withOk)
{
  String r = "{";
  if (withOk) r += "\"ok\":true,";
  r += "\"nominalMm\":" + String(feedStepsToMm(feedNominalStepsForCal(), false), 1);
  r += ",\"nominalMmB\":" + String(feedStepsToMm(feedNominalStepsForCalB(), true), 1);
  r += ",\"mmMin\":" + String(feedStepsToMm(FEED_REF_STEPS_MIN, false), 1);
  r += ",\"mmMax\":" + String(feedStepsToMm(FEED_REF_STEPS_MAX, false), 1);
  r += ",\"nominalSteps\":" + String(feedNominalStepsForCal());
  r += ",\"nominalStepsB\":" + String(feedNominalStepsForCalB());
  r += ",\"measuredMm\":" + String(feedCalMeasuredMm, 2);
  r += ",\"measuredMmB\":" + String(feedCalMeasuredMmB, 2);
  r += ",\"stepsPerMm\":" + String(feedStepsPerMm, 2);
  r += ",\"stepsPerMmB\":" + String(feedStepsPerMmB, 2);
  r += "}";
  return r;
}

// --- 14.5 Calibración (feed, lineal, pieza, tolerancia) ---
void handleGetFeedCal()
{
  server.send(200, "application/json", feedCalJson(false));
}

void handleSetFeedCal()
{
  if (cycleActive)
  {
    server.send(409, "application/json", "{\"error\":\"Ciclo activo\"}");
    return;
  }
  if (!server.hasArg("mm"))
  {
    server.send(400, "application/json", "{\"error\":\"Falta parametro mm\"}");
    return;
  }

  float mm = server.arg("mm").toFloat();
  if (mm <= 0.0f)
  {
    server.send(400, "application/json", "{\"error\":\"Longitud medida invalida\"}");
    return;
  }

  const bool sideR = server.hasArg("side")
    && (server.arg("side") == "R" || server.arg("side") == "r"
        || server.arg("side") == "B" || server.arg("side") == "b");
  if (sideR)
  {
    feedCalMeasuredMmB = mm;
    feedRecomputeStepsPerMmB();
  }
  else
  {
    feedCalMeasuredMm = mm;
    feedRecomputeStepsPerMm();
  }
  saveFeedCalToNvs();

  DBG_PRINT("Calibracion feeder ");
  DBG_PRINT(sideR ? "R: " : "L: ");
  DBG_PRINT(sideR ? feedNominalStepsForCalB() : feedNominalStepsForCal());
  DBG_PRINT(" pasos / ");
  DBG_PRINT(mm);
  DBG_PRINT(" mm = ");
  DBG_PRINT(sideR ? feedStepsPerMmB : feedStepsPerMm);
  DBG_PRINTLN(" pasos/mm");

  server.send(200, "application/json", feedCalJson(true));
}

static String linearCalJson(bool withOk)
{
  String r = "{";
  if (withOk) r += "\"ok\":true,";
  r += "\"gripperAreaMm\":" + String(LINEAR_GRIPPER_AREA_DEFAULT, 1);
  r += ",\"stepsPerMm\":" + String(linearStepsPerMm, 3);
  r += ",\"offsetSteps\":" + String(linearOffsetSteps, 1);
  r += ",\"factoryStepsPerMm\":" + String(LINEAR_STEPS_PER_MM, 3);
  r += ",\"factoryRefLinearActuatorMm\":" + String(LINEAR_FACTORY_ACTUATOR_MM, 1);
  r += "}";
  return r;
}

void handleGetLinearCal()
{
  server.send(200, "application/json", linearCalJson(false));
}

void handleGetFeedTolerancePct()
{
  String response = "{";
  response += "\"percent\":" + String(feedStepsTolerancePercent);
  response += ",\"min\":" + String(FEED_TOLERANCE_PCT_MIN);
  response += ",\"max\":" + String(FEED_TOLERANCE_PCT_MAX);
  response += ",\"default\":" + String(FEED_TOLERANCE_PCT_DEFAULT);
  response += "}";
  server.send(200, "application/json", response);
}

void handleSetFeedTolerancePct()
{
  if (cycleActive)
  {
    server.send(409, "application/json", "{\"error\":\"Ciclo activo\"}");
    return;
  }
  if (!server.hasArg("percent"))
  {
    server.send(400, "application/json", "{\"error\":\"Falta parametro percent\"}");
    return;
  }

  feedStepsTolerancePercent = clampFeedTolerancePercent(server.arg("percent").toInt());
  saveFeedTolerancePctToNvs();

  DBG_PRINT("Tolerancia encoder: +/-");
  DBG_PRINT(feedStepsTolerancePercent);
  DBG_PRINTLN("%");

  String response = "{\"ok\":true,\"percent\":";
  response += String(feedStepsTolerancePercent);
  response += "}";
  server.send(200, "application/json", response);
}

// --- 14.6 Velocidad / tiempos de proceso ---
void handleGetSpeedConfig()
{
  server.send(200, "application/json", speedConfigJson(true));
}

void handleSetSpeedConfig()
{
  if (cycleActive || linearIsMoving())
  {
    server.send(409, "application/json", "{\"error\":\"Ciclo o lineal activo\"}");
    return;
  }

  if (server.hasArg("reset") && server.arg("reset") == "1")
  {
    applySpeedDefaults();
    saveSpeedConfigToNvs();
    DBG_PRINTLN("Speed config: restaurado a fabrica");
    server.send(200, "application/json", speedConfigJson(false));
    return;
  }

  static const char* keys[] = {
    "dwellAtDestMs", "linearDoneMs",
    "holderOnMs", "holderOpenMs", "grippersOnMs", "gripperReleaseMs",
    "cutterPulseMs", "cutterPostMs", "asentarMs"
  };
  bool any = false;
  for (uint8_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
  {
    if (server.hasArg(keys[i]))
    {
      parseAndApplySpeedArg(keys[i], server.arg(keys[i]));
      any = true;
    }
  }

  if (!any)
  {
    server.send(400, "application/json", "{\"error\":\"Sin parametros\"}");
    return;
  }

  saveSpeedConfigToNvs();
  DBG_PRINTLN("Speed config: guardado");
  server.send(200, "application/json", speedConfigJson(false));
}

// --- 14.7 Prefeeder / depósito ---
void handleGetPrefeederTriggerConfig()
{
  String response = "{\"everyN\":";
  response += String(prefeederTriggerEveryN);
  response += ",\"min\":";
  response += String(PREFEEDER_TRIGGER_EVERY_N_MIN);
  response += ",\"max\":";
  response += String(PREFEEDER_TRIGGER_EVERY_N_MAX);
  response += "}";
  server.send(200, "application/json", response);
}

void handleSetPrefeederTriggerConfig()
{
  if (cycleActive)
  {
    server.send(409, "application/json", "{\"error\":\"Ciclo activo\"}");
    return;
  }
  if (!server.hasArg("everyN"))
  {
    server.send(400, "application/json", "{\"error\":\"Falta parametro everyN\"}");
    return;
  }

  prefeederTriggerEveryN = clampPrefeederTriggerEveryN((uint32_t)server.arg("everyN").toInt());
  savePrefeederTriggerConfigToNvs();

  DBG_PRINT("Prefeeder disparo cada ");
  DBG_PRINT(prefeederTriggerEveryN);
  DBG_PRINTLN(" piezas");

  String response = "{\"ok\":true,\"everyN\":";
  response += String(prefeederTriggerEveryN);
  response += "}";
  server.send(200, "application/json", response);
}

void handleGetDepositConfig()
{
  String response = "{\"batchSize\":";
  response += String(depositBatchSize);
  response += ",\"extraMm\":";
  response += String(DEPOSIT_EXTRA_MM_DEFAULT, 1);
  response += ",\"extraFixed\":true";
  response += ",\"interlaceMm\":0";
  response += ",\"betweenGapMm\":";
  response += String(DEPOSIT_BETWEEN_GAP_MM, 1);
  response += ",\"gripperAreaMm\":";
  response += String(linearGripperAreaMm, 1);
  response += ",\"autoBaseFormula\":\"Pos_base=G+linearActuator+Extra\"";
  response += ",\"autoBetweenFormula\":\"Pos_entreBatch=G+linearActuator+Extra+Gap\"";
  response += ",\"formulaComment\":\"Pos(N)=Pos_base+(N-1)*Pos_entreBatch; linearActuator=max(0,L-G)\"";
  response += ",\"batchMin\":";
  response += String(DEPOSIT_BATCH_SIZE_MIN);
  response += ",\"batchMax\":";
  response += String(DEPOSIT_BATCH_SIZE_MAX);
  response += ",\"trayLengthMm\":";
  response += String((int)TRAY_LENGTH_MM);
  if (cutLongitud > 0)
  {
    float L = depositPartLengthMm();
    float linearActuator = L - linearGripperAreaMm;
    if (linearActuator < 0.0f) linearActuator = 0.0f;
    response += ",\"partLengthMm\":";
    response += String(L, 1);
    response += ",\"linearActuatorMm\":";
    response += String(linearActuator, 1);
    response += ",\"baseMm\":";
    response += String(depositBaseMm(), 1);
    response += ",\"betweenBatchMm\":";
    response += String(depositBetweenBatchAutoMm(), 1);
  }
  response += "}";
  server.send(200, "application/json", response);
}

void handleSetDepositConfig()
{
  if (cycleActive)
  {
    server.send(409, "application/json", "{\"error\":\"Ciclo activo\"}");
    return;
  }
  if (!server.hasArg("batchSize"))
  {
    server.send(400, "application/json", "{\"error\":\"Falta parametro batchSize\"}");
    return;
  }

  depositBatchSize = clampDepositBatchSize((uint32_t)server.arg("batchSize").toInt());
  depositExtraMm = DEPOSIT_EXTRA_MM_DEFAULT;
  depositInterlaceMm = 0.0f;
  depositBetweenBatchMm = DEPOSIT_BETWEEN_GAP_MM;

  saveDepositConfigToNvs();

  DBG_PRINT("Deposito: batch=");
  DBG_PRINT(depositBatchSize);
  DBG_PRINT(" extra=");
  DBG_PRINT(DEPOSIT_EXTRA_MM_DEFAULT, 1);
  DBG_PRINT(" entre-gap=");
  DBG_PRINT(DEPOSIT_BETWEEN_GAP_MM, 1);
  DBG_PRINTLN(" mm (auto por L)");

  String response = "{\"ok\":true,\"batchSize\":";
  response += String(depositBatchSize);
  response += ",\"extraMm\":";
  response += String(DEPOSIT_EXTRA_MM_DEFAULT, 1);
  response += ",\"betweenGapMm\":";
  response += String(DEPOSIT_BETWEEN_GAP_MM, 1);
  response += "}";
  server.send(200, "application/json", response);
}

void handlePrefeederTrigger()
{
  DBG_PRINTLN("WEB: prueba disparo Prefeeder TCP L+R");
  const bool ok = prefeederTriggerPulse();

  String response = "{\"ok\":";
  response += ok ? "true" : "false";
  response += ",\"via\":\"tcp\",\"peerOk\":";
  response += peerBothOk() ? "true" : "false";
  if (!ok)
    response += ",\"error\":\"Sin enlace TCP a PreFeeder L/R\"";
  response += "}";
  server.send(ok ? 200 : 503, "application/json", response);
}

static void handlePrefeederStatusApi()
{
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", peerWebJson(true));
}

static void handlePrefeederCmdApi()
{
  server.sendHeader("Cache-Control", "no-store");
  if (!server.hasArg("cmd"))
  {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"cmd\"}");
    return;
  }
  String cmd = server.arg("cmd");
  String val = server.hasArg("val") ? server.arg("val") : "";
  int8_t sideArg = server.hasArg("side") ? peerParseSideArg(server.arg("side")) : -1;

  if (cmd == "setEveryN")
  {
    prefeederTriggerEveryN = clampPrefeederTriggerEveryN((uint32_t)val.toInt());
    savePrefeederTriggerConfigToNvs();
    server.send(200, "application/json", peerWebJson(true));
    return;
  }

  if (cmd == "reconnect" || cmd == "forceReconnect")
  {
    bool ok = false;
    if (sideArg < 0)
    {
      const bool okL = peerForceReconnect(PEER_L);
      const bool okR = peerForceReconnect(PEER_R);
      ok = okL || okR;
    }
    else
    {
      ok = peerForceReconnect((uint8_t)sideArg);
    }
    server.send(ok ? 200 : 503, "application/json", peerWebJson(ok));
    return;
  }

  if (cmd == "setTriggerCfg" || cmd == "setHolguraExtra" || cmd == "setTriggerFeed"
      || cmd == "setAllCfg" || cmd == "applyAllCfg"
      || cmd == "start" || cmd == "stop" || cmd == "reset" || cmd == "trigger"
      || cmd == "halt" || cmd == "motionStop" || cmd == "hardStop"
      || cmd == "refillMaterial" || cmd == "refillDereeler" || cmd == "refillServo"
      || cmd == "refillFeeder"
      || cmd == "materialistaCall" || cmd == "setMaterialistaCall"
      || cmd == "setIdleMode" || cmd == "idleMode"
      || cmd == "setTestMode" || cmd == "testMode"
      || cmd == "setBuzzerMute" || cmd == "buzzerMute" || cmd == "buzzerMuted"
      || cmd == "setRefillPulseS" || cmd == "refillPulseS")
  {
    // setAllCfg siempre persiste en NVS del PreFeeder del lado indicado (L o R).
    // setDereelerLeadMs es solo de la UI nativa del PreFeeder (no se reenvía desde TCM).
    String peerCmd = cmd;
    if (cmd == "setTestMode" || cmd == "testMode")
      peerCmd = "setIdleMode";
    if (cmd == "materialistaCall" || cmd == "setMaterialistaCall")
      peerCmd = "setIdleMode";  // Materialista = ex-Idle

    bool ok = false;
    if (cmd == "trigger" && sideArg < 0)
    {
      // Sin side: disparo simultáneo a ambos (igual que el ciclo).
      ok = prefeederTriggerPulse();
    }
    else
    {
      uint8_t side = (sideArg >= 0) ? (uint8_t)sideArg : PEER_L;
      ok = peerSendCmd(side, peerCmd.c_str(), val);
      if (ok && (cmd == "setTriggerCfg" || cmd == "setHolguraExtra"
                 || cmd == "setTriggerFeed" || cmd == "setAllCfg" || cmd == "applyAllCfg"))
      {
        applyPrefeederCfgCsvToMirror(side, val);
        basePfApplyPending = false;
        // Forzar ese lado a BASE (no pasar por freshness de lastRx: el espejo ya es autoritativo).
        commitPrefeederMirrorSideToBase(side);
      }
      // Espejo inmediato: sin ACK, el status/event confirma después.
      if (ok && (cmd == "setIdleMode" || cmd == "idleMode"
                 || cmd == "setTestMode" || cmd == "testMode"
                 || cmd == "materialistaCall" || cmd == "setMaterialistaCall"))
      {
        const bool on = (val == "1" || val == "true" || val == "on");
        peers[side].mirror.idleMode = on;
      }
      if (ok && (cmd == "setBuzzerMute" || cmd == "buzzerMute" || cmd == "buzzerMuted"))
      {
        const bool on = (val == "1" || val == "true" || val == "on");
        applyTcmBuzzerMute(on);
      }
      if (ok && (cmd == "setRefillPulseS" || cmd == "refillPulseS"))
      {
        float s = val.toFloat();
        if (s < 0.2f) s = 0.2f;
        if (s > 10.0f) s = 10.0f;
        peers[side].mirror.refillPulseS = s;
      }
      if (ok && (cmd == "refillMaterial" || cmd == "refillDereeler" || cmd == "refillServo"
                 || cmd == "refillFeeder"))
      {
        const bool on = (val == "1" || val == "true" || val == "on");
        PrefeederMirror& m = peers[side].mirror;
        if (cmd == "refillMaterial")
        {
          m.refillMaterial = on;
          m.refillDereeler = on;
          m.refillServo = on;
          m.refillFeeder = on;
        }
        else if (cmd == "refillDereeler")
        {
          m.refillDereeler = on;
          m.refillMaterial = m.refillDereeler && m.refillServo && m.refillFeeder;
        }
        else if (cmd == "refillServo")
        {
          m.refillServo = on;
          m.refillMaterial = m.refillDereeler && m.refillServo && m.refillFeeder;
        }
        else if (cmd == "refillFeeder")
        {
          m.refillFeeder = on;
          m.refillMaterial = m.refillDereeler && m.refillServo && m.refillFeeder;
        }
      }
    }
    server.send(ok ? 200 : 503, "application/json", peerWebJson(ok));
    return;
  }

  server.send(400, "application/json", "{\"ok\":false,\"error\":\"unknown\"}");
}

static void handleSlaveUiEnterApi()
{
  server.sendHeader("Cache-Control", "no-store");
  const String kind = server.hasArg("kind") ? server.arg("kind") : "pf";
  beginSlaveUiHold(kind.c_str());
  // Si ya hay espejo fresco, absorber ya (no esperar a que expire el hold).
  if (kind == "pf" || kind == "prefeeder" || kind == "L" || kind == "R")
    persistPrefeederMirrorToBase();
  server.send(200, "application/json",
              String("{\"ok\":true,\"kind\":\"") + kind + "\",\"holdMs\":"
              + String((unsigned long)PF_SLAVE_UI_HOLD_MS) + "}");
}

// --- 14.8 Movimiento lineal manual (home / safe zone / move) ---
void handleLinearHome()
{
  if (externalStopInputActive())
  {
    server.send(403, "application/json", "{\"error\":\"Parada externa PreFeeder (sin TCP o en falla)\"}");
    return;
  }
  if (manualLinearBlocked())
  {
    server.send(409, "application/json", "{\"error\":\"Movimiento no disponible (ciclo o lineal activo)\"}");
    return;
  }

  DBG_PRINTLN("MANUAL LINEAR: HOME torque (origen +)");
  const bool ok = linearRunTorqueHome();

  String response = "{\"ok\":";
  response += ok ? "true" : "false";
  response += ",\"alreadyHome\":false,\"posPulses\":" + String(posPulses);
  response += ",\"atHome\":" + String(posPulses == 0 ? "true" : "false");
  response += ",\"moving\":false}";
  server.send(ok ? 200 : 500, "application/json", response);
}

void handleLinearSafeZone()
{
  if (externalStopInputActive())
  {
    server.send(403, "application/json", "{\"error\":\"Parada externa PreFeeder (sin TCP o en falla)\"}");
    return;
  }
  if (manualLinearBlocked())
  {
    server.send(409, "application/json", "{\"error\":\"Movimiento no disponible (ciclo o lineal activo)\"}");
    return;
  }

  DBG_PRINT("MANUAL LINEAR: SAFE ZONE ");
  DBG_PRINT(LINEAR_SAFE_ZONE_MM);
  DBG_PRINTLN(" mm");
  uint32_t target = mmToLinearStepsAbs((float)LINEAR_SAFE_ZONE_MM);
  const bool already = (posPulses == target);
  if (!already)
    linearMoveToPulses(target);

  String response = "{\"ok\":true,\"safeZoneMm\":" + String(LINEAR_SAFE_ZONE_MM);
  response += ",\"alreadyAtSafeZone\":" + String(already ? "true" : "false");
  response += ",\"targetSteps\":" + String(target);
  response += ",\"posPulses\":" + String(posPulses);
  response += "}";
  server.send(200, "application/json", response);
}

void handleLinearMove()
{
  if (externalStopInputActive())
  {
    server.send(403, "application/json", "{\"error\":\"Parada externa PreFeeder (sin TCP o en falla)\"}");
    return;
  }
  if (manualLinearBlocked())
  {
    server.send(409, "application/json", "{\"error\":\"Movimiento no disponible (ciclo o lineal activo)\"}");
    return;
  }
  if (!server.hasArg("longitud"))
  {
    server.send(400, "application/json", "{\"error\":\"Falta parametro longitud (mm)\"}");
    return;
  }

  int mm = server.arg("longitud").toInt();
  if (mm < 0)
  {
    server.send(400, "application/json", "{\"error\":\"Longitud invalida\"}");
    return;
  }
  // Movimiento directo del actuador: mm programados = recorrido fisico (sin restar G), modelo afin.
  // factory=1: usa la referencia FIJA de fabrica (para el movimiento de prueba de
  // calibracion, asi no arrastra una calibracion guardada posiblemente corrupta).
  bool useFactory = server.hasArg("factory") && server.arg("factory") == "1";
  uint32_t target = useFactory ? mmToLinearStepsFactory((float)mm)
                               : mmToLinearStepsAbs((float)mm);
  if (mm > 0 && target == 0)
  {
    server.send(400, "application/json", "{\"error\":\"Longitud demasiado corta\"}");
    return;
  }
  DBG_PRINT("MANUAL LINEAR: ");
  DBG_PRINT(mm);
  DBG_PRINT(" mm -> ");
  DBG_PRINT(target);
  DBG_PRINTLN(" pulsos");

  linearMoveToPulses(target);

  String response = "{\"ok\":true,\"targetMm\":" + String(mm);
  response += ",\"targetSteps\":" + String(target);
  response += ",\"posPulses\":" + String(posPulses);
  response += ",\"moving\":false}";
  server.send(200, "application/json", response);
}

static void serviceWebClients(uint8_t maxPasses)
{
  for (uint8_t i = 0; i < maxPasses; i++)
    server.handleClient();
}

static void serviceBackgroundTick(uint32_t& lastCanMs, uint32_t& lastWebMs)
{
  wifiService();
  peerService();
  uint32_t now = millis();
  const bool linBusy = linearIsMoving();
  const bool feedBusy = feedPhaseIsActive();
  const bool cycleBusy = cycleActive && !cyclePaused;
  uint32_t canIv = (linBusy || feedBusy) ? STEP_BUSY_CAN_INTERVAL_MS : STEP_CAN_INTERVAL_MS;
  uint32_t webIv = STEP_WEB_INTERVAL_MS;
  if (linBusy || feedBusy)
    webIv = STEP_BUSY_WEB_INTERVAL_MS;
  else if (cycleBusy)
    webIv = STEP_CYCLE_WEB_INTERVAL_MS;
  if (now - lastCanMs >= canIv)
  {
    lastCanMs = now;
    serviceCANRx();
    servicePeerFaultLatches();
    serviceTowerCommandTx();
  }
  if (now - lastWebMs >= webIv)
  {
    lastWebMs = now;
    serviceWebClients((linBusy || feedBusy || cycleBusy) ? WEB_BUSY_PASSES : 4);
  }
}

// ============================================================
// SECCION 15 — Setup / Loop
// ============================================================
void setup()
{
  Serial.begin(115200);
  delay(200);
  Serial.println("Cortador de tubo v17 — CAN + I/O + lineal + feeder");
  Serial.printf("Reset reason=%d (1=POWERON 3=SW 4=PANIC 5=INT_WDT 6=TASK_WDT 7=WDT 9=BROWNOUT 10=SDIO)\n",
                (int)esp_reset_reason());

  // NVS/RMT antes de WiFi.
  Serial.println("boot: IO...");
  setupIoPins();
  printPlcStates();
  Serial.println("boot: NVS...");
  loadPersistedSettings();
  Serial.println("boot: LittleFS models...");
  if (!partCatalogBegin())
    Serial.println("WARN: catalogo de modelos no disponible");
  Serial.println("boot: BASE + modelos...");
  initializeMachineBaseAndParts();
  // Lineal: flags; ON+HOME tras WiFi.
  Serial.println("boot: lineal ASDA...");
  setupLinearActuator();

  Serial.println("boot: WiFi STA...");
  if (!startStationWifi())
    Serial.println("WARN: STA sin router al arranque");

  canMutex = xSemaphoreCreateMutex();
  feedHaltTaskStart();

  Serial.println("Esperando alimentacion servo CAN...");
  delay(SERVO_POWERUP_MS);

  Serial.println("Iniciando MCP2515...");
  if (initCANBus(CAN_INIT_MAX_RETRIES))
    Serial.println("MCP2515 inicializado OK");
  else
    Serial.println("Error inicializando MCP2515");

  if (canInitialized)
  {
    Serial.println("Setup servo feeder (CiA402 PP)...");
    for (uint8_t attempt = 1; attempt <= SERVO_SETUP_MAX_RETRIES; attempt++)
    {
      if (setupServoFeeder()) break;
      Serial.println("WARN: servo sin confirmar, reintento...");
      delay(500);
      if (attempt < SERVO_SETUP_MAX_RETRIES)
        initCANBus(2);
    }
  }

  prepareServoFeeder();

  server.on("/", handleRoot);
  server.on("/startCut", handleStartCut);
  server.on("/startClean", handleStartClean);
  server.on("/pauseCycle", handlePauseCycle);
  server.on("/abortCycle", handleAbortCycle);
  server.on("/ackRecovery", handleAckRecovery);
  server.on("/homeAndPlcReset", handleHomeAndPlcReset);
  server.on("/resetErrors", handleResetErrors);
  server.on("/setBuzzerMute", handleSetBuzzerMute);
  server.on("/getBuzzerMute", handleSetBuzzerMute);
  server.on("/setStepByStep", handleSetStepByStep);
  server.on("/getStepByStep", handleSetStepByStep);
  server.on("/getCycleStatus", handleGetCycleStatus);
  server.on("/api/uiWait", handleUiWait);
  server.on("/getCycleTime", handleGetCycleTime);
  server.on("/setCycleTime", handleSetCycleTime);
  server.on("/plcSet", handlePlcSet);
  server.on("/plcStatus", handlePlcStatus);
  server.on("/getFeedBypassSteps", handleGetFeedBypassSteps);
  server.on("/setFeedBypassSteps", handleSetFeedBypassSteps);
  server.on("/getFeedTestConfig", handleGetFeedTestConfig);
  server.on("/setFeedTestConfig", handleSetFeedTestConfig);
  server.on("/api/feed/encoder", HTTP_GET, handleGetFeedCanEncoder);
  server.on("/api/feed/encoder/zero", HTTP_POST, handleFeedCanEncoderZero);
  server.on("/feedTestSensor", handleFeedTestSensor);
  server.on("/getFeedSkipEncoder", handleGetFeedSkipEncoder);
  server.on("/setFeedSkipEncoder", handleSetFeedSkipEncoder);
  server.on("/getFeedOffset", handleGetFeedOffset);
  server.on("/setFeedOffset", handleSetFeedOffset);
  server.on("/getFeedCal", handleGetFeedCal);
  server.on("/setFeedCal", handleSetFeedCal);
  server.on("/getFeedTolerancePct", handleGetFeedTolerancePct);
  server.on("/setFeedTolerancePct", handleSetFeedTolerancePct);
  server.on("/getLinearCal", handleGetLinearCal);
  server.on("/linearHome", handleLinearHome);
  server.on("/linearSafeZone", handleLinearSafeZone);
  server.on("/linearMove", handleLinearMove);
  server.on("/getPrefeederTriggerConfig", handleGetPrefeederTriggerConfig);
  server.on("/setPrefeederTriggerConfig", handleSetPrefeederTriggerConfig);
  server.on("/getDepositConfig", handleGetDepositConfig);
  server.on("/setDepositConfig", handleSetDepositConfig);
  server.on("/prefeederTrigger", handlePrefeederTrigger);
  server.on("/api/prefeeder/status", handlePrefeederStatusApi);
  server.on("/api/prefeeder/cmd", handlePrefeederCmdApi);
  server.on("/api/slaveUi/enter", handleSlaveUiEnterApi);
  server.on("/api/parts", HTTP_GET, handlePartsList);
  server.on("/api/parts/select", HTTP_POST, handlePartSelect);
  server.on("/api/parts/add", HTTP_POST, handlePartAdd);
  server.on("/api/parts/delete", HTTP_POST, handlePartDelete);
  server.on("/api/admin/login", HTTP_POST, handleAdminLogin);
  server.on("/api/admin/status", HTTP_GET, handleAdminStatus);
  server.on("/getSpeedConfig", handleGetSpeedConfig);
  server.on("/setSpeedConfig", handleSetSpeedConfig);
  if (wifiIsUp())
    server.begin();

  asdaEnsureHomeAtBoot();

  Serial.printf("Operador: %s -> http://%s\n", WIFI_SSID, STA_IP_TCM);
  Serial.print("canOk=");       Serial.print(canInitialized ? "true" : "false");
  Serial.print(" servoReady="); Serial.print(servoCanReady ? "true" : "false");
  Serial.print(" posPulses=");  Serial.println(posPulses);

  // Grace de torre/alarmas: cuenta desde aquí (no desde power-on) para dar
  // tiempo real a STA + TCP de los PreFeeders tras el setup bloqueante.
  towerBootGraceUntilMs = millis() + TOWER_BOOT_GRACE_MS;
  Serial.printf("TOWER boot grace %lu ms\n", (unsigned long)TOWER_BOOT_GRACE_MS);
}

void loop()
{
  wifiService();
  asdaEnsureHomeAtBoot();
  linearService();
  peerService();
  peerTryReconnect();
  peerSyncInProcessFlag();
  serviceMachineBasePrefeeder();
  serviceExternalStopInput();
  serviceTowerCommandTx();
  if (cutRequested && !cycleActive)
  {
    cutRequested = false;
    abortPendingPrefeederSettle("dispatch");
    if (prefeederIdleModeActive())
    {
      Serial.println("Corte cancelado: PreFeeder en Materialista");
      pushLog("Corte cancelado: PreFeeder Materialista");
      releasePrefeederSettleHold();
    }
    // Status TCP fresco = OK.
    else if (safetyValidateAtStart() && peerVerifyLinkFastOrPing())
    {
      prepareBeforeCut();
      procesarCorte(cutLongitud, cutCantidad);
    }
    else
    {
      releasePrefeederSettleHold();
      // Clasificar sin re-poll: estado tras el intento fallido.
      if (sensorStaleLatched || !sensorCanOnline())
        setCycleError(E027, "sensors_can");  // Sensores CAN offline
      else if (!servoCanReady)
        setCycleError(E008, "servo_can");    // Servo feeder CAN
      else if (!safetyAllowsRun())
        setCycleError(E026, "safety_start");  // Parada externa / safety
      else
      {
        setCycleError(E000);
        Serial.println("Corte cancelado: PreFeeder L/R no OK (ver E020/E030)");
        pushLog("Corte cancelado: PreFeeder sin enlace OK");
      }
      if (cycleErrorCode != E000)
        Serial.println("Corte cancelado: seguridad/sensores CAN o TCP PreFeeder");
      uiNotify();
    }
  }
  else
  {
    servicePendingPrefeederSettle();
  }

  serviceCycle();
  if (linearIsMoving())
    linearService();

  if (!linearIsMoving() || feedPhaseIsActive())
    serviceServoFeed();

  if (!cycleRunning)
  {
    serviceWebClients(WEB_IDLE_PASSES);
    serviceCANRx();
  }
  else
  {
    static uint32_t lastCanMs = 0;
    static uint32_t lastWebMs = 0;
    serviceBackgroundTick(lastCanMs, lastWebMs);
  }
}
