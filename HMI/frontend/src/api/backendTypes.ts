/** Tipos del snapshot JSON del backend Flask (state.py). */

export interface BackendBanner {
  text: string;
  kind: 'info' | 'warn' | 'error' | 'ok';
}

export interface BackendModel {
  name: string;
  mm: number;
  rpm: number;
  qty?: number;
  cantidad?: number;
}

export interface BackendCycleConfig {
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

export interface BackendFlowStep {
  id: number;
  key: string;
  label: string;
  kind: 'action' | 'wait' | 'parallel';
  delayKey?: string;
  parallelRole?: 'start' | 'join';
}

export interface BackendCycleSnapshot {
  byte: number;
  name: string;
  active: boolean;
  paused: boolean;
  materialist: boolean;
  stepByStep: boolean;
  trialMode?: boolean;
  step: number;
  stepName: string;
  stepLabel: string;
  parallelGroup?: string;
  rep: number;
  piecesDone?: number;
  totalReps: number;
  progress: number;
  elapsedSec?: number;
  completed?: boolean;
  lastOk: boolean;
  fault: string;
  faultClass?: string;
  recovery?: string;
  c3Pending?: boolean;
  config: BackendCycleConfig;
  flow: BackendFlowStep[];
}

export interface BackendDebugTrailsRecord {
  testNum: number;
  side: 'R' | 'L';
  measureMm: number | null;
  status: string;
  error: string;
  timestamp: string;
  phase: string;
}

export interface BackendDebugTrails {
  active: boolean;
  stage: string;
  side: 'R' | 'L' | 'Both';
  numTests: number;
  waitTimeS: number;
  currentTest: number;
  phase: string;
  lastOk: boolean;
  fault: string;
  records: BackendDebugTrailsRecord[];
  recordCount: number;
}

export interface BackendLink {
  connected: boolean;
  host: string;
  port: number;
}

export interface BackendMotion {
  connected: boolean;
  status: BackendBanner;
  asdaPositionMm: number | null;
  enc_r: string;
  enc_l: string;
  feedOffsetMmL: number;
  feedOffsetMmR: number;
  laserR?: boolean;
  laserL?: boolean;
  safetyExhaust?: boolean;
}

export interface BackendValve {
  label: string;
  on: boolean | null;
  error: boolean | null;
}

export interface BackendPlc {
  connected: boolean;
  status: BackendBanner;
  last_state_byte: number | null;
  blowerSec?: number;
  valves: Record<string, BackendValve>;
}

export interface BackendPfError {
  label: string;
  active: boolean | null;
}

export interface BackendPreFeeder {
  connected: boolean;
  status: BackendBanner;
  last_state_byte: number | null;
  errors: Record<string, BackendPfError>;
}

export interface BackendErrorLatch {
  active: boolean;
  code: string;
  byte: number;
  module: string;
  description: string;
  class: string;
  ui: string;
  needsConfirm: boolean;
  needsHome: boolean;
  recovery: string;
  confirmed: boolean;
}

export interface BackendSnapshot {
  models: BackendModel[];
  selectedModel: number;
  mm: number;
  rpm: number;
  banner: BackendBanner;
  error?: BackendErrorLatch;
  progress: number;
  resumeEnabled: boolean;
  cycle: BackendCycleSnapshot;
  debugTrails?: BackendDebugTrails;
  motionLink: BackendLink;
  plcLink: BackendLink;
  pfLink: BackendLink;
  andonLink?: BackendLink;
  appConfig?: {
    andonBuzzerMute: boolean;
  };
  andon?: {
    connected: boolean;
    green: boolean;
    yellow: boolean;
    red: boolean;
    buzzer: boolean;
    manual: boolean;
  };
  motion: BackendMotion;
  plc: BackendPlc;
  prefeeder: BackendPreFeeder;
  logs: {
    main: string[];
    motion: string[];
    plc: string[];
    prefeeder: string[];
    andon?: string[];
  };
}
