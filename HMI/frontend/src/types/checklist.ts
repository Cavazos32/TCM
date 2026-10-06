export type ChecklistStepId =
  | 'cycle_counter'
  | 'air_pressure'
  | 'pf_holgura'
  | 'asda_roundtrip'
  | 'feeder_can_purge'
  | 'piece_measure'
  | '';

export interface ChecklistStepStatus {
  ready: boolean;
  detail?: string;
  overLimit?: boolean;
  limit?: number;
  testActive?: boolean;
  pressuresOk?: boolean;
  airTestVerified?: boolean;
  plcValvesOk?: boolean;
  stage2Ok?: boolean;
  purgeIdle?: boolean;
  piecesDone?: number;
  measurements?: { l: number | null; r: number | null };
  motionBlocked?: boolean;
  asdaOk?: boolean;
}

export interface ChecklistSession {
  employeeId: string;
  startedAt: string;
  currentStepIndex: number;
  completedSteps?: string[];
  stage2Ok?: boolean;
  asdaOk?: boolean;
  airTestVerified?: boolean;
  pieceTestStarted?: boolean;
  measurements?: { l: number | null; r: number | null };
}

export interface ChecklistState {
  required: boolean;
  lastCompletedAt: string | null;
  currentStepId: ChecklistStepId;
  session: ChecklistSession | null;
  workerBusy: boolean;
  workerPhase: string;
  workerError: string;
  stepError: string;
  airTestActive: boolean;
  pieceTestStarted: boolean;
  stepStatus: ChecklistStepStatus;
}

/** Longitud de pieza física = medida del operador incluye estos 55 mm. */
export const CHECKLIST_PIECE_EXTRA_MM = 55;

export const CHECKLIST_STEP_ORDER: ChecklistStepId[] = [
  'cycle_counter',
  'air_pressure',
  'pf_holgura',
  'asda_roundtrip',
  'feeder_can_purge',
  'piece_measure',
];
