import type { BackendCycleConfig, BackendSnapshot } from './backendTypes';
import type {
  AndonState,
  ConnectionState,
  CycleConfig,
  LogEntry,
  MachineState,
  MotionState,
  PfRefillState,
  PlcState,
  PreFeederState,
  PreFeederSensor,
  ValveItem,
} from '../types';

const VALVE_MAP: { byte: number; id: string }[] = [
  { byte: 0x19, id: 'cutter-r' },
  { byte: 0x1a, id: 'cutter-l' },
  { byte: 0x1b, id: 'gripper' },
  { byte: 0x1c, id: 'holder' },
  { byte: 0x1d, id: 'encoder' },
  { byte: 0x23, id: 'blower' },
];

const PF_SENSOR_IDS: Record<string, string> = {
  'Buffer Full L': 'buf-full-l',
  'Buffer Max L': 'buf-max-l',
  'Tensioner L': 'tensioner-l',
  'Cilindro L': 'cilindro-l',
  'Manguera L': 'manguera-l',
  'Holgura L': 'holgura-l',
  'Buffer Full R': 'buf-full-r',
  'Buffer Max R': 'buf-max-r',
  'Tensioner R': 'tensioner-r',
  'Cilindro R': 'cilindro-r',
  'Manguera R': 'manguera-r',
  'Holgura R': 'holgura-r',
};

// Sensor ON = OK (igual que HTML L/R). EXXX solo con fault_active (timeout).
const PF_OK_WHEN_ACTIVE: Record<string, number> = {
  'Buffer Full L': 0x33,
  'Buffer Full R': 0x2d,
  'Holgura L': 0x38,
  'Holgura R': 0x32,
};

let logCounter = 0;

function parseLogLine(line: string, module: LogEntry['module']): LogEntry {
  const match = line.match(/^\[(\d{2}:\d{2}:\d{2}(?:\.\d{3})?)\]\s*(.*)$/);
  const timestamp = match?.[1] ?? '';
  const message = match?.[2] ?? line;
  const codeMatch = message.match(/\(0x[0-9A-Fa-f]+\)/);
  const code = codeMatch?.[0]?.replace(/[()]/g, '');

  let type: LogEntry['type'] = 'info';
  const lower = message.toLowerCase();
  if (lower.includes('error') || lower.includes('sin enlace') || lower.includes('ng')) {
    type = 'error';
  } else if (lower.includes('stop') || lower.includes('parada') || lower.includes('abort')) {
    type = 'warn';
  } else if (lower.includes('comando') || lower.includes('start') || lower.includes('reset')) {
    type = 'cmd';
  } else if (lower.includes('ok') || lower.includes('reached') || lower.includes('ack')) {
    type = 'rx';
  }

  return {
    id: `log-${++logCounter}`,
    timestamp,
    module,
    type,
    message,
    code,
  };
}

export function parseLogs(
  lines: string[],
  module: LogEntry['module']
): LogEntry[] {
  return lines.map((line) => parseLogLine(line, module));
}

export function mergeAllLogs(snap: BackendSnapshot): LogEntry[] {
  return [
    ...parseLogs(snap.logs.main, 'MAQUINA'),
    ...parseLogs(snap.logs.motion, 'MOTION'),
    ...parseLogs(snap.logs.plc, 'PLC'),
    ...parseLogs(snap.logs.prefeeder, 'PREFEEDER'),
    ...parseLogs(snap.logs.andon ?? [], 'ANDON'),
  ].sort((a, b) => a.timestamp.localeCompare(b.timestamp));
}

const MAX_UI_LOGS = 400;

function logKey(e: LogEntry): string {
  return `${e.timestamp}|${e.module}|${e.message}`;
}

/** Une cola SSE/poll (tail) con lo ya mostrado. Misma ref si no hay líneas nuevas. */
export function mergeLogTail(prev: LogEntry[], snap: BackendSnapshot): LogEntry[] {
  const incoming = mergeAllLogs(snap);
  if (!prev.length) return incoming;
  const seen = new Set(prev.map(logKey));
  const added = incoming.filter((e) => !seen.has(logKey(e)));
  if (!added.length) return prev;
  const next = prev.concat(added);
  return next.length > MAX_UI_LOGS ? next.slice(next.length - MAX_UI_LOGS) : next;
}

export function mapMachineState(
  snap: BackendSnapshot,
  targetQty: number
): MachineState {
  const model = snap.models[snap.selectedModel];
  const cycle = snap.cycle;
  const qty = targetQty || model?.qty || model?.cantidad || 1;

  const piecesDone = cycle.piecesDone ?? 0;
  // Tamaño del lote en curso / último lote (progreso). No es el valor editable.
  const lotTarget =
    cycle.totalReps > 0 ? cycle.totalReps : qty;
  // Editable: qty del operador. Solo durante ciclo activo mostramos el lote real
  // (input deshabilitado). Si no, totalReps residual tras FinishParts pisaba el input.
  const targetPieces =
    cycle.active && cycle.totalReps > 0 ? cycle.totalReps : qty;

  const errorActive = !!snap.error?.active;
  const lastErr = snap.error?.last;
  const inRecovery =
    !!cycle.recoveryAfterError ||
    String(cycle.recovery || '') === 'e050_material' ||
    !!cycle.e050FinishPiece ||
    String(cycle.recoveryPrompt || '').startsWith('e050_');
  const plcFault =
    snap.plc.status?.kind === 'error' ||
    Object.values(snap.plc.valves || {}).some((v) => v.error === true);
  const faultModule = errorActive ? snap.error?.module || '' : '';
  const modKey = faultModule.toLowerCase();
  let faultModuleStatus = '';
  if (modKey.includes('motion')) {
    faultModuleStatus = snap.motion.status?.text ?? '';
  } else if (modKey.includes('plc')) {
    faultModuleStatus = snap.plc.status?.text ?? '';
  } else if (modKey.includes('pre') || modKey.includes('feeder')) {
    faultModuleStatus = pfVisibleStatusText(snap.prefeeder.status?.text);
  }

  // Estado general de máquina: no mezclar EXXX (va al panel de recovery).
  const generalStatus = errorActive
    ? cycle.name || snap.banner.text
    : snap.banner.text;

  return {
    model: model?.name ?? '—',
    offsetMm: cycle.config?.cutOffsetMm ?? 0,
    mm: snap.mm,
    rpm: snap.rpm,
    statusText: generalStatus,
    statusKind: errorActive ? 'error' : snap.banner.kind,
    isRunning: cycle.active && !cycle.paused,
    isPaused: cycle.paused,
    cycleActive: cycle.active,
    cycleStep: cycle.step ?? 0,
    cycleStepLabel: cycle.stepLabel ?? '',
    cycleMaterialist: cycle.materialist ?? false,
    cycleBusy: !!(cycle.busy ?? false),
    refillActive: !!cycle.refillActive,
    refillAwaitingConfirm: !!cycle.refillAwaitingConfirm,
    refillPrompt: String(cycle.refillPrompt || ''),
    recoveryPrompt: String(cycle.recoveryPrompt || ''),
    recoveryAwaitingConfirm: !!cycle.recoveryAwaitingConfirm,
    recoveryAfterError: !!cycle.recoveryAfterError,
    e050Lot:
      String(cycle.recovery || '') === 'e050_material' ||
      !!cycle.e050FinishPiece ||
      String(cycle.recoveryPrompt || '').startsWith('e050_'),
    e050FinishPiece: !!cycle.e050FinishPiece,
    refillSkipCut: !!cycle.refillSkipCut,
    stepByStep: cycle.stepByStep ?? false,
    pauseEnabled:
      cycle.active &&
      !cycle.paused &&
      !cycle.refillAwaitingConfirm &&
      !cycle.recoveryAwaitingConfirm,
    progress: cycle.completed
      ? 100
      : cycle.active && lotTarget > 0
        ? Math.max(
            snap.progress,
            Math.round((piecesDone / lotTarget) * 100)
          )
        : cycle.lastOk
          ? Math.max(snap.progress, Math.round((piecesDone / Math.max(lotTarget, 1)) * 100))
          : snap.progress,
    cycleTimeSec: cycle.elapsedSec ?? 0,
    lastPieceSec: cycle.lastPieceSec ?? 0,
    avgPieceSec: cycle.avgPieceSec ?? 0,
    // Piezas terminadas (no el rep en curso — eso confundía 1/15 al empezar)
    piecesCount: cycle.completed ? lotTarget : piecesDone,
    targetPieces,
    cycleCompleted: cycle.completed ?? false,
    safetyExhaust: !!snap.motion.safetyExhaust,
    errorActive,
    workBlocked:
      errorActive ||
      plcFault ||
      (!!cycle.fault && !cycle.lastOk && !cycle.active),
    // Latch HMI, fault de ciclo activo, o lote NO OK con EXXX (el aborto
    // no debe desaparecer al poner _active=false / PF Idle).
    fault: errorActive
      ? snap.error?.ui
      : cycle.active
        ? cycle.fault || undefined
        : !cycle.lastOk && cycle.fault
          ? cycle.fault
          : undefined,
    faultCode: errorActive ? snap.error?.code : undefined,
    lastFault:
      !errorActive && inRecovery && lastErr?.ui
        ? String(lastErr.ui)
        : undefined,
    faultModule: faultModule || undefined,
    faultDescription: errorActive ? snap.error?.description : undefined,
    faultModuleStatus: faultModuleStatus || undefined,
  };
}

function parseEncoder(val: string): number | null {
  if (!val || val === '—') return null;
  const n = parseFloat(val);
  return Number.isFinite(n) ? n : null;
}

export function mapMotionState(snap: BackendSnapshot): MotionState {
  const m = snap.motion;
  const statusText = m.status?.text ?? '';
  const isMoving =
    statusText.toLowerCase().includes('ocupado') ||
    statusText.toLowerCase().includes('busy');

  return {
    connection: {
      connected: snap.motionLink.connected,
      ip: snap.motionLink.host,
      port: snap.motionLink.port,
    },
    targetPositionMm: snap.mm,
    currentPositionMm: m.asdaPositionMm ?? 0,
    rpm: snap.rpm,
    actualRpm: isMoving ? snap.rpm : 0,
    isMoving,
    hasError: m.status?.kind === 'error',
    inPosition: !isMoving && m.status?.kind !== 'error',
    encoderR: parseEncoder(m.enc_r),
    encoderL: parseEncoder(m.enc_l),
    feederCanLTesting: false,
    feederCanRTesting: false,
    offsetL: m.feedOffsetMmL ?? 0,
    offsetR: m.feedOffsetMmR ?? 0,
    laserR: !!m.laserR,
    laserL: !!m.laserL,
    statusText,
  };
}

export function mapPlcState(snap: BackendSnapshot): PlcState {
  const valves: ValveItem[] = VALVE_MAP.map(({ byte, id }) => {
    const v = snap.plc.valves[String(byte)] ?? {
      label: id,
      on: null,
      error: null,
    };
    return {
      id,
      name: v.label,
      hexCode: `0x${byte.toString(16).toUpperCase().padStart(2, '0')}`,
      byte,
      active: v.on === true,
      hasError: v.error === true,
    };
  });

  return {
    connection: {
      connected: snap.plcLink.connected,
      ip: snap.plcLink.host,
      port: snap.plcLink.port,
    },
    statusText: snap.plc.status?.text ?? '',
    hasError: snap.plc.status?.kind === 'error',
    blowerSec: Number(snap.plc.blowerSec ?? 2),
    valves,
  };
}

function buildSensors(
  snap: BackendSnapshot,
  side: 'L' | 'R'
): PreFeederSensor[] {
  const faults = snap.prefeeder.fault_active || {};
  return Object.values(snap.prefeeder.errors)
    .filter((e) => e.label.endsWith(` ${side}`))
    .map((e) => {
      const okByte = PF_OK_WHEN_ACTIVE[e.label];
      const fault = okByte !== undefined && faults[String(okByte)] === true;
      if (okByte !== undefined) {
        return {
          id: PF_SENSOR_IDS[e.label] ?? e.label.toLowerCase().replace(/\s+/g, '-'),
          name: e.label,
          active: fault,
          status: (fault ? 'error' : e.active === true ? 'ok' : e.active === false ? 'warning' : 'idle') as PreFeederSensor['status'],
        };
      }
      return {
        id: PF_SENSOR_IDS[e.label] ?? e.label.toLowerCase().replace(/\s+/g, '-'),
        name: e.label,
        active: e.active === true,
        status: (e.active === true ? 'error' : e.active === false ? 'ok' : 'idle') as PreFeederSensor['status'],
      };
    });
}

function mapPfRefill(side?: { refillMaterial?: boolean; refillDereeler?: boolean; refillServo?: boolean; refillFeeder?: boolean; refillPulseS?: number } | null): PfRefillState {
  const pulse = Number(side?.refillPulseS);
  return {
    material: !!side?.refillMaterial,
    dereeler: !!side?.refillDereeler,
    servo: !!side?.refillServo,
    feeder: !!side?.refillFeeder,
    pulseS: Number.isFinite(pulse) ? Math.max(0.2, Math.min(10, pulse)) : 1,
  };
}

export function isPfGenericErrorText(text: string | undefined): boolean {
  const s = (text || '').toLowerCase();
  return s.includes('errorstate') || s.includes('0x03c');
}

export function pfVisibleStatusText(text: string | undefined): string {
  if (isPfGenericErrorText(text)) return '';
  return (text || '').trim();
}

export function mapPreFeederState(snap: BackendSnapshot): PreFeederState {
  const pf = snap.prefeeder;
  const rawStatus = pf.status?.text ?? '';
  const statusText = pfVisibleStatusText(rawStatus);
  const running =
    rawStatus.toLowerCase().includes('ocupado') ||
    rawStatus.toLowerCase().includes('busy');

  return {
    connection: {
      connected: snap.pfLink.connected,
      ip: snap.pfLink.host,
      port: snap.pfLink.port,
    },
    isRunning: !!running,
    statusText,
    hasError: pf.status?.kind === 'error' && !!statusText,
    idleMode: !!(pf.sides?.L?.idleMode || pf.sides?.R?.idleMode),
    sensorsL: buildSensors(snap, 'L'),
    sensorsR: buildSensors(snap, 'R'),
    refillL: mapPfRefill(pf.sides?.L),
    refillR: mapPfRefill(pf.sides?.R),
  };
}

export function mapAndonConnection(snap: BackendSnapshot): ConnectionState {
  const link = snap.andonLink;
  return {
    connected: !!link?.connected,
    ip: link?.host ?? '10.10.32.61',
    port: link?.port ?? 8769,
  };
}

export function mapAndonState(snap: BackendSnapshot): AndonState {
  const a = snap.andon;
  const conn = mapAndonConnection(snap);
  return {
    connection: conn,
    green: !!a?.green,
    yellow: !!a?.yellow,
    red: !!a?.red,
    buzzer: !!a?.buzzer,
    manual: !!a?.manual,
  };
}

export function mapAppConfig(snap: BackendSnapshot): { andonBuzzerMute: boolean } {
  return {
    andonBuzzerMute: !!snap.appConfig?.andonBuzzerMute,
  };
}

export function mapCycleConfig(cfg: BackendSnapshot['cycle']['config']): CycleConfig {
  const raw = (cfg as BackendCycleConfig)?.feedSides;
  const feedSides =
    raw === 'L' || raw === 'R' || raw === 'LR' ? raw : 'LR';
  const pfTriggerEnabled = (cfg as BackendCycleConfig)?.pfTriggerEnabled !== false;
  return { ...(cfg as CycleConfig), feedSides, pfTriggerEnabled };
}

export function valveByteFromId(
  valves: ValveItem[],
  valveId: string
): number | undefined {
  return valves.find((v) => v.id === valveId)?.byte;
}
