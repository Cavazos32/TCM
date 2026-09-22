export type TabType = 'maquina' | 'cycle' | 'motion' | 'plc' | 'prefeeder' | 'andon';

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
  /** Avance corto ASDA tras abrir pinzas; entra en ref WIP soplo fin. */
  gripperClearanceMm?: number;
  cutOffsetMm?: number;
  /** Offset blower desde cada punta hacia el centro (fin=start−off, inicio=+off). */
  wipBlowerInicioOffsetMm?: number;
  motionWaitTimeoutS: number;
  feedWaitTimeoutS: number;
  pfReadyTimeoutS: number;
  /** Feed / Stage2 OM: 'L' | 'R' | 'LR' */
  feedSides: 'L' | 'R' | 'LR';
  /** Longitud de purga/refill (mm). Motion FEED físico = 55. */
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
  isRunning: boolean;
  isPaused: boolean;
  cycleActive: boolean;
  cycleStep: number;
  cycleStepLabel: string;
  cycleMaterialist: boolean;
  cycleBusy: boolean;
  refillActive: boolean;
  refillAwaitingConfirm: boolean;
  /** working | after_feed | after_cut | "" */
  refillPrompt: string;
  stepByStep: boolean;
  trialMode: boolean;
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
  faultClass?: string;
  faultCode?: string;
  faultModule?: string;
  faultDescription?: string;
  errorActive?: boolean;
  /** Status del módulo del EXXX (Motion/PLC/PreFeeder). */
  faultModuleStatus?: string;
  errorNeedsConfirm?: boolean;
  errorNeedsHome?: boolean;
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

export interface PreFeederState {
  connection: ConnectionState;
  isRunning: boolean;
  statusText?: string;
  hasError?: boolean;
  sensorsL: PreFeederSensor[];
  sensorsR: PreFeederSensor[];
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
}
