import type { BackendSnapshot } from './backendTypes';
import type {
  AndonState,
  ConnectionState,
  CycleConfig,
  LogEntry,
  MachineState,
  MotionState,
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

let logCounter = 0;

function parseLogLine(line: string, module: LogEntry['module']): LogEntry {
  const match = line.match(/^\[(\d{2}:\d{2}:\d{2})\]\s*(.*)$/);
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

export function mapMachineState(
  snap: BackendSnapshot,
  targetQty: number
): MachineState {
  const model = snap.models[snap.selectedModel];
  const cycle = snap.cycle;
  const qty = targetQty || model?.qty || model?.cantidad || 1;

  const piecesDone = cycle.piecesDone ?? 0;
  const targetReps =
    cycle.totalReps > 0 ? cycle.totalReps : qty;

  return {
    model: model?.name ?? '—',
    offsetMm: cycle.config?.cutOffsetMm ?? 0,
    mm: snap.mm,
    rpm: snap.rpm,
    statusText: snap.banner.text,
    isRunning: cycle.active && !cycle.paused,
    isPaused: cycle.paused,
    cycleActive: cycle.active,
    cycleStep: cycle.step ?? 0,
    cycleStepLabel: cycle.stepLabel ?? '',
    cycleMaterialist: cycle.materialist ?? false,
    stepByStep: cycle.stepByStep ?? false,
    trialMode: cycle.trialMode ?? false,
    pauseEnabled: cycle.active && !cycle.paused,
    progress: cycle.completed
      ? 100
      : cycle.active && targetReps > 0
        ? Math.max(
            snap.progress,
            Math.round((piecesDone / targetReps) * 100)
          )
        : cycle.lastOk
          ? Math.max(snap.progress, Math.round((piecesDone / Math.max(targetReps, 1)) * 100))
          : snap.progress,
    cycleTimeSec: cycle.elapsedSec ?? 0,
    // Piezas terminadas (no el rep en curso — eso confundía 1/15 al empezar)
    piecesCount: cycle.completed ? targetReps : piecesDone,
    targetPieces: targetReps,
    cycleCompleted: cycle.completed ?? false,
    safetyExhaust: !!snap.motion.safetyExhaust,
    fault: snap.error?.active ? snap.error.ui : cycle.fault || undefined,
    faultClass: snap.error?.class || cycle.faultClass || undefined,
    errorNeedsConfirm: !!snap.error?.needsConfirm && !snap.error?.confirmed,
    errorNeedsHome: !!snap.error?.needsHome,
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
    blowerSec: Number(snap.plc.blowerSec ?? 2),
    valves,
  };
}

function buildSensors(
  snap: BackendSnapshot,
  side: 'L' | 'R'
): PreFeederSensor[] {
  return Object.values(snap.prefeeder.errors)
    .filter((e) => e.label.endsWith(` ${side}`))
    .map((e) => ({
      id: PF_SENSOR_IDS[e.label] ?? e.label.toLowerCase().replace(/\s+/g, '-'),
      name: e.label,
      active: e.active === true,
      status: (e.active === true ? 'error' : e.active === false ? 'ok' : 'idle') as PreFeederSensor['status'],
    }));
}

export function mapPreFeederState(snap: BackendSnapshot): PreFeederState {
  const pf = snap.prefeeder;
  const running =
    pf.status?.text?.toLowerCase().includes('ocupado') ||
    pf.status?.text?.toLowerCase().includes('busy');

  return {
    connection: {
      connected: snap.pfLink.connected,
      ip: snap.pfLink.host,
      port: snap.pfLink.port,
    },
    isRunning: !!running,
    statusText: pf.status?.text ?? '',
    sensorsL: buildSensors(snap, 'L'),
    sensorsR: buildSensors(snap, 'R'),
  };
}

export function mapAndonConnection(snap: BackendSnapshot): ConnectionState {
  const link = snap.andonLink;
  return {
    connected: !!link?.connected,
    ip: link?.host ?? '10.10.32.60',
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
  return { ...cfg };
}

export function valveByteFromId(
  valves: ValveItem[],
  valveId: string
): number | undefined {
  return valves.find((v) => v.id === valveId)?.byte;
}
