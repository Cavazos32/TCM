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
  depositStackGapMm?: number;
  depositMaxTravelMm?: number;
  gripperClearanceMm?: number;
  cutOffsetMm?: number;
  wipBlowerInicioOffsetMm?: number;
  motionWaitTimeoutS: number;
  feedWaitTimeoutS: number;
  pfReadyTimeoutS: number;
  pieceWatchTimeoutS?: number;
  feedSides?: 'L' | 'R' | 'LR' | string;
  pfTriggerEnabled?: boolean;
  refillMm?: number;
  refillAsdaMm?: number;
}

export interface BackendFlowStep {
  id: number;
  key: string;
  label: string;
  kind: 'action' | 'wait' | 'parallel';
  delayKey?: string;
  parallelRole?: 'start' | 'join';
  /** Si true, Step by Step pausa tras este paso (checkpoint físico). */
  sbsPause?: boolean;
}

export interface BackendCycleSnapshot {
  byte: number;
  name: string;
  active: boolean;
  paused: boolean;
  materialist: boolean;
  busy?: boolean;
  stepByStep: boolean;
  refillActive?: boolean;
  refillAwaitingConfirm?: boolean;
  refillPrompt?: string;
  step: number;
  stepName: string;
  stepLabel: string;
  parallelGroup?: string;
  rep: number;
  piecesDone?: number;
  totalReps: number;
  progress: number;
  elapsedSec?: number;
  lastPieceSec?: number;
  avgPieceSec?: number;
  completed?: boolean;
  lastOk: boolean;
  fault: string;
  faultClass?: string;
  recovery?: string;
  recoveryAfterError?: boolean;
  recoveryPrompt?: string;
  recoveryAwaitingConfirm?: boolean;
  e050FinishPiece?: boolean;
  refillSkipCut?: boolean;
  c3Pending?: boolean;
  config: BackendCycleConfig;
  /** Ausente en SSE slim (la UI conserva el último flow). */
  flow?: BackendFlowStep[];
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

export interface BackendPfSide {
  autoState?: string | null;
  triggerActive?: boolean | null;
  idleMode?: boolean;
  refillMaterial?: boolean;
  refillDereeler?: boolean;
  refillServo?: boolean;
  refillFeeder?: boolean;
  refillPulseS?: number;
}

export interface BackendPreFeeder {
  connected: boolean;
  status: BackendBanner;
  last_state_byte: number | null;
  errors: Record<string, BackendPfError>;
  fault_active?: Record<string, boolean>;
  sides?: {
    L?: BackendPfSide;
    R?: BackendPfSide;
  };
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
  /** EXXX ya reseteado; contexto de recuperación C2/C3. */
  last?: BackendErrorLatch;
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
