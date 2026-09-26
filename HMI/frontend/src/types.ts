export type TabType = 'maquina' | 'cycle' | 'motion' | 'plc' | 'prefeeder' | 'andon';

/** Purga: Long feed desde await_feed / after_feed (skipValidate en Motion). */
export const REFILL_LONG_FEED_MM = 100;

export interface CycleConfig {
  holderOnMs: number;
  holderOpenMs: number;
  grippersOnMs: number;
  gripperReleaseMs: number;
  cutterPulseMs: number;
  cutterPostMs: number;
  linearDoneMs: number;
  asentarMs: number;
  dwellAtDestMs: number;
  depositBatchSize: number;
  depositExtraMm: number;
  /** Gap entre batches: depósito(n) = depósito(n−1) + |L| + gap. */
  depositStackGapMm?: number;
  /** Tope carrera ASDA (mm). Start rechaza si último batch + despeje lo supera. */
  depositMaxTravelMm?: number;
  /** Avance corto ASDA tras abrir pinzas; entra en ref WIP soplo fin. */
  gripperClearanceMm?: number;
  cutOffsetMm?: number;
  /** Offset blower desde cada punta hacia el centro (fin=start−off, inicio=+off). */
  wipBlowerInicioOffsetMm?: number;
  motionWaitTimeoutS: number;
  feedWaitTimeoutS: number;
  pfReadyTimeoutS: number;
  /** Watchdog de pieza (s). Si no cierra (Pause excluida) → E008/E009. Feed post-HOME de la siguiente excluido. */
  pieceWatchTimeoutS?: number;
  /** Feed / Stage2 OM: 'L' | 'R' | 'LR' */
  feedSides: 'L' | 'R' | 'LR';
  /** Tfeed entre piezas (paso 3). false = omitir; solo helper holgura. */
  pfTriggerEnabled?: boolean;
  /** Longitud de purga/refill (mm). Retry = este valor; Long feed = REFILL_LONG_FEED_MM. */
  refillMm?: number;
  /** Posición park ASDA antes del refill (convención firmada HMI). */
  refillAsdaMm?: number;
}

export type CycleStepType = 'action' | 'delay' | 'background' | 'join';

export interface CycleStep {
  id: number;
  title: string;
  type: CycleStepType;
  delayKey?: keyof CycleConfig;
  defaultDurationMs?: number;
  description?: string;
  note?: string;
  badge?: string;
  /** Checkpoint físico en paso a paso (exige Next). Ausente/false = auto. */
  sbsPause?: boolean;
  /** False = delay fijo (no editable en UI). Default true si hay delayKey. */
  delayEditable?: boolean;
}

export interface LogEntry {
  id: string;
  timestamp: string;
  type: 'info' | 'cmd' | 'warn' | 'error' | 'rx';
  module: 'MAQUINA' | 'MOTION' | 'PLC' | 'PREFEEDER' | 'ANDON' | 'SYSTEM';
  code?: string;
  message: string;
}

export interface ValveItem {
  id: string;
  name: string;
  hexCode: string;
  byte?: number;
  active: boolean;
  hasError: boolean;
  errorMessage?: string;
}

export interface PreFeederSensor {
  id: string;
  name: string;
  active: boolean;
  status: 'ok' | 'warning' | 'error' | 'idle';
}

export interface ConnectionState {
  connected: boolean;
  ip: string;
  port: number;
  latencyMs?: number;
  lastHeartbeat?: string;
}

export interface MachineState {
  model: string;
  offsetMm: number;
  mm: number;
  rpm: number;
  statusText: string;
  /** Banner de máquina (info/ok/warn/error). Error de interlock no es EXXX. */
  statusKind?: 'info' | 'ok' | 'warn' | 'error';
  /** Estado máquina 0x40–0x49 (Andon). */
  machineByte?: number;
  machineName?: string;
  isRunning: boolean;
  isPaused: boolean;
  cycleActive: boolean;
  cycleStep: number;
  cycleStepLabel: string;
  cycleMaterialist: boolean;
  cycleBusy: boolean;
  refillActive: boolean;
  refillAwaitingConfirm: boolean;
  /** working | await_feed | after_feed | after_cut | "" */
  refillPrompt: string;
  /** review_piece | continue_cycle | e050_* | "" */
  recoveryPrompt: string;
  recoveryAwaitingConfirm: boolean;
  recoveryAfterError: boolean;
  e050Lot: boolean;
  e050FinishPiece: boolean;
  refillSkipCut: boolean;
  stepByStep: boolean;
  pauseEnabled: boolean;
  progress: number;
  cycleTimeSec: number;
  /** Última pieza completada (s). */
  lastPieceSec: number;
  /** Promedio por pieza del lote (s). */
  avgPieceSec: number;
  piecesCount: number;
  targetPieces: number;
  cycleCompleted: boolean;
  safetyExhaust: boolean;
  /** EXXX UI completo; detalle del panel de recovery, no del estado general. */
  fault?: string;
  faultCode?: string;
  /** EXXX del Set, visible tras Res mientras el lote sigue en recuperación. */
  lastFault?: string;
  faultModule?: string;
  faultDescription?: string;
  errorActive?: boolean;
  /** Latch EXXX o fallo PLC (ErrorState / sensor) — bloquea Start/Refill. */
  workBlocked?: boolean;
  /** Status del módulo del EXXX (Motion/PLC/PreFeeder). */
  faultModuleStatus?: string;
  /** Cola de EXXX activos (el primero es el que se muestra). */
  faultQueue?: { code: string; ui: string }[];
}

export interface MotionState {
  connection: ConnectionState;
  targetPositionMm: number;
  currentPositionMm: number;
  rpm: number;
  actualRpm: number;
  isMoving: boolean;
  hasError: boolean;
  errorCode?: string;
  inPosition: boolean;
  encoderR: number | null;
  encoderL: number | null;
  feederCanLTesting: boolean;
  feederCanRTesting: boolean;
  offsetL: number;
  offsetR: number;
  laserR: boolean;
  laserL: boolean;
  statusText?: string;
}

export interface PlcState {
  connection: ConnectionState;
  statusText?: string;
  hasError?: boolean;
  blowerSec: number;
  valves: ValveItem[];
}

export type PfRefillChannel = 'material' | 'dereeler' | 'servo' | 'feeder';

export interface PfRefillState {
  material: boolean;
  dereeler: boolean;
  servo: boolean;
  feeder: boolean;
  /** Duración del pulso (s) desde el HTML local del PreFeeder. */
  pulseS?: number;
}

export interface PreFeederState {
  connection: ConnectionState;
  isRunning: boolean;
  statusText?: string;
  hasError?: boolean;
  /** Materialista real del PreFeeder (idleMode L|R del HTML local). */
  idleMode?: boolean;
  sensorsL: PreFeederSensor[];
  sensorsR: PreFeederSensor[];
  refillL: PfRefillState;
  refillR: PfRefillState;
}

export interface AppConfigState {
  andonBuzzerMute: boolean;
}

export interface AndonState {
  connection: ConnectionState;
  green: boolean;
  yellow: boolean;
  red: boolean;
  buzzer: boolean;
  manual: boolean;
  machineByte?: number;
  pressure?: boolean;
}
