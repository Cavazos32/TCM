export type TabType = 'maquina' | 'cycle' | 'motion' | 'plc' | 'prefeeder' | 'andon' | 'debug-trails';

export type DebugTrailsSide = 'R' | 'L' | 'Both';

export interface DebugTrailsRecord {
  testNum: number;
  side: 'R' | 'L';
  measureMm: number | null;
  status: string;
  error: string;
  timestamp: string;
  phase: string;
}

export interface DebugTrailsState {
  active: boolean;
  stage: string;
  side: DebugTrailsSide;
  numTests: number;
  waitTimeS: number;
  currentTest: number;
  phase: string;
  lastOk: boolean;
  fault: string;
  records: DebugTrailsRecord[];
  recordCount: number;
}

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
  cutOffsetMm?: number;
  motionWaitTimeoutS: number;
  feedWaitTimeoutS: number;
  pfReadyTimeoutS: number;
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
  stepByStep: boolean;
  trialMode: boolean;
  pauseEnabled: boolean;
  progress: number;
  cycleTimeSec: number;
  piecesCount: number;
  targetPieces: number;
  cycleCompleted: boolean;
  safetyExhaust: boolean;
  fault?: string;
  faultClass?: string;
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
  blowerSec: number;
  valves: ValveItem[];
}

export interface PreFeederState {
  connection: ConnectionState;
  isRunning: boolean;
  statusText?: string;
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
