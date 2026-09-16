import { useCallback, useEffect, useRef, useState } from 'react';
import type { BackendSnapshot } from '../api/backendTypes';
import * as api from '../api/hmiApi';
import {
  mapAndonConnection,
  mapAndonState,
  mapAppConfig,
  mapCycleConfig,
  mapMachineState,
  mapMotionState,
  mapPlcState,
  mapPreFeederState,
  mergeAllLogs,
  parseLogs,
  valveByteFromId,
} from '../api/mappers';
import type {
  AndonState,
  ConnectionState,
  CycleConfig,
  DebugTrailsSide,
  DebugTrailsState,
  LogEntry,
  MachineState,
  MotionState,
  PlcState,
  PreFeederState,
  TabType,
} from '../types';

/** Bloqueo tras toggle: evita doble click ON→OFF antes de que el KEEP/pulso asiente. */
const VALVE_TOGGLE_LOCK_MS = 450;

const DEFAULT_TRAILS: DebugTrailsState = {
  active: false,
  stage: 'stage1',
  side: 'Both',
  numTests: 0,
  waitTimeS: 0,
  currentTest: 0,
  phase: 'idle',
  lastOk: false,
  fault: '',
  records: [],
  recordCount: 0,
};

export interface HmiViewState {
  connected: boolean;
  machineState: MachineState;
  motionState: MotionState;
  plcState: PlcState;
  preFeederState: PreFeederState;
  andonState: AndonState;
  andonConn: ConnectionState;
  andonBuzzerMute: boolean;
  cycleConfig: CycleConfig;
  cycleStep: number;
  cycleActive: boolean;
  cycleFlow: BackendSnapshot['cycle']['flow'];
  debugTrails: DebugTrailsState;
  resumeEnabled: boolean;
  logs: LogEntry[];
  models: { name: string; mm: number; rpm: number; qty?: number }[];
  selectedModelIndex: number;
}

const DEFAULT_MACHINE: MachineState = {
  model: '—',
  offsetMm: 0,
  mm: -45,
  rpm: 1200,
  statusText: 'Conectando…',
  isRunning: false,
  isPaused: false,
  cycleActive: false,
  cycleStep: 0,
  cycleStepLabel: '',
  cycleMaterialist: false,
  stepByStep: false,
  trialMode: false,
  pauseEnabled: false,
  progress: 0,
  cycleTimeSec: 0,
  piecesCount: 0,
  targetPieces: 1,
  cycleCompleted: false,
  safetyExhaust: false,
};

export function useHmiState() {
  const [view, setView] = useState<HmiViewState>({
    connected: false,
    machineState: DEFAULT_MACHINE,
    motionState: {
      connection: { connected: false, ip: '', port: 0 },
      targetPositionMm: -45,
      currentPositionMm: 0,
      rpm: 1200,
      actualRpm: 0,
      isMoving: false,
      hasError: false,
      inPosition: true,
      encoderR: null,
      encoderL: null,
      feederCanLTesting: false,
      feederCanRTesting: false,
      offsetL: 0,
      offsetR: 0,
      laserR: false,
      laserL: false,
    },
    plcState: {
      connection: { connected: false, ip: '', port: 0 },
      blowerSec: 2,
      valves: [],
    },
    preFeederState: {
      connection: { connected: false, ip: '', port: 0 },
      isRunning: false,
      sensorsL: [],
      sensorsR: [],
    },
    andonState: {
      connection: { connected: false, ip: '10.10.32.60', port: 8769 },
      green: false,
      yellow: false,
      red: false,
      buzzer: false,
      manual: false,
    },
    andonConn: { connected: false, ip: '10.10.32.60', port: 8769 },
    andonBuzzerMute: false,
    cycleConfig: {} as CycleConfig,
    cycleStep: 0,
    cycleActive: false,
    cycleFlow: [],
    debugTrails: DEFAULT_TRAILS,
    resumeEnabled: false,
    logs: [],
    models: [],
    selectedModelIndex: 0,
  });

  const targetQtyRef = useRef(1);
  const snapRef = useRef<BackendSnapshot | null>(null);
  const encPollRef = useRef(false);
  /** Estado lógico local por válvula (fuente de verdad entre clicks). */
  const valveOnRef = useRef<Record<string, boolean>>({});
  const valveLockRef = useRef<Record<string, boolean>>({});
  const [valveBusy, setValveBusy] = useState<Record<string, boolean>>({});

  const applySnapshot = useCallback((snap: BackendSnapshot) => {
    snapRef.current = snap;
    const model = snap.models[snap.selectedModel];
    if (model?.qty && targetQtyRef.current <= 1) {
      targetQtyRef.current = model.qty;
    }
    const targetQty = targetQtyRef.current;
    const plc = mapPlcState(snap);
    // Sincronizar lectura ON/OFF desde servidor solo si la válvula no está bloqueada.
    for (const v of plc.valves) {
      if (!valveLockRef.current[v.id]) {
        valveOnRef.current[v.id] = v.active;
      }
    }
    setView({
      connected: true,
      machineState: mapMachineState(snap, targetQty),
      motionState: mapMotionState(snap),
      plcState: plc,
      preFeederState: mapPreFeederState(snap),
      andonState: mapAndonState(snap),
      andonConn: mapAndonConnection(snap),
      andonBuzzerMute: mapAppConfig(snap).andonBuzzerMute,
      cycleConfig: mapCycleConfig(snap.cycle.config),
      cycleStep: snap.cycle.step,
      cycleActive: snap.cycle.active,
      cycleFlow: snap.cycle.flow,
      debugTrails: snap.debugTrails
        ? {
            active: !!snap.debugTrails.active,
            stage: snap.debugTrails.stage || 'stage1',
            side: (snap.debugTrails.side as DebugTrailsSide) || 'Both',
            numTests: snap.debugTrails.numTests || 0,
            waitTimeS: snap.debugTrails.waitTimeS || 0,
            currentTest: snap.debugTrails.currentTest || 0,
            phase: snap.debugTrails.phase || 'idle',
            lastOk: !!snap.debugTrails.lastOk,
            fault: snap.debugTrails.fault || '',
            records: Array.isArray(snap.debugTrails.records)
              ? snap.debugTrails.records.map((r) => ({
                  testNum: r.testNum,
                  side: r.side,
                  measureMm: r.measureMm,
                  status: r.status,
                  error: r.error || '',
                  timestamp: r.timestamp || '',
                  phase: r.phase || '',
                }))
              : [],
            recordCount: snap.debugTrails.recordCount || 0,
          }
        : DEFAULT_TRAILS,
      resumeEnabled: snap.resumeEnabled,
      logs: mergeAllLogs(snap),
      models: snap.models,
      selectedModelIndex: snap.selectedModel,
    });
  }, []);

  useEffect(() => {
    let closed = false;
    let es: EventSource | null = null;
    let retryTimer: ReturnType<typeof setTimeout> | undefined;
    let pollTimer: ReturnType<typeof setInterval> | undefined;

    const connect = () => {
      if (closed) return;
      es = new EventSource('/api/events');
      es.onopen = () => {
        setView((prev) => (prev.connected ? prev : { ...prev, connected: true }));
      };
      es.onmessage = (ev) => {
        try {
          applySnapshot(JSON.parse(ev.data));
        } catch {
          // ignore
        }
      };
      es.onerror = () => {
        setView((prev) => ({ ...prev, connected: false }));
        // Solo recrear si el browser cerró el stream (no pelear con reconnect nativo).
        if (es && es.readyState === EventSource.CLOSED) {
          es.close();
          es = null;
          if (!closed) {
            retryTimer = setTimeout(connect, 1500);
          }
        }
      };
    };

    api.getState().then(applySnapshot).catch(() => {});
    connect();

    // Respaldo: si el SSE se queda mudo, el poll recupera progreso/completado sin F5.
    pollTimer = setInterval(() => {
      if (closed) return;
      api.getState().then(applySnapshot).catch(() => {});
    }, 2000);

    return () => {
      closed = true;
      if (retryTimer) clearTimeout(retryTimer);
      if (pollTimer) clearInterval(pollTimer);
      es?.close();
    };
  }, [applySnapshot]);

  const ensureEncPoll = useCallback(() => {
    if (!encPollRef.current) {
      encPollRef.current = true;
      api.setEncPoll(true).catch(() => {});
      api.getFeedOffset().catch(() => {});
    }
  }, []);

  const onTabChange = useCallback(
    (tab: TabType) => {
      if (tab === 'motion') ensureEncPoll();
    },
    [ensureEncPoll]
  );

  const selectModel = useCallback((index: number) => {
    api.selectModel(index).catch(() => {});
  }, []);

  const setTargetQty = useCallback((qty: number) => {
    targetQtyRef.current = Math.max(1, qty);
    if (snapRef.current) {
      applySnapshot(snapRef.current);
    }
  }, [applySnapshot]);

  const start = useCallback(() => {
    api.startCycle(targetQtyRef.current).catch(() => {});
  }, []);

  const stop = useCallback(() => {
    api.stopMachine().catch(() => {});
  }, []);

  const resume = useCallback(() => {
    api.resumeMachine().catch(() => {});
  }, []);

  const pauseCycle = useCallback(() => {
    api.pauseCycle().catch(() => {});
  }, []);

  const resetCycleCmd = useCallback(async () => {
    const snap = snapRef.current;
    const err = snap?.error;
    if (err?.active && err.needsConfirm && !err.confirmed) {
      const ok = window.confirm(
        `${err.ui}\n\n¿Confirmar reset y homing general?`
      );
      if (!ok) return;
      try {
        await api.confirmError();
      } catch {
        /* ignore */
      }
      api.resetError({ confirm: true, doHome: true }).catch(() => {});
      return;
    }
    if (err?.active && err.needsHome) {
      const ok = window.confirm(
        `${err.ui}\n\nReset errores y ejecutar homing general?`
      );
      if (!ok) return;
      api.resetError({ confirm: true, doHome: true }).catch(() => {});
      return;
    }
    api.resetCycle({ confirm: true, doHome: false }).catch(() => {});
  }, []);

  const setCutOffset = useCallback(async (mm: number) => {
    const res = await api.setCycleConfig({ cutOffsetMm: mm });
    return res.config;
  }, []);

  const toggleCycleMaterialist = useCallback(() => {
    const snap = snapRef.current;
    const next = !(snap?.cycle.materialist ?? false);
    api.cycleMaterialist(next).catch(() => {});
  }, []);

  const setCycleStepByStep = useCallback((on: boolean) => {
    api.setCycleStepByStep(on).catch(() => {});
  }, []);

  const setCycleTrialMode = useCallback((on: boolean) => {
    api.setCycleTrialMode(on).catch(() => {});
  }, []);

  const reloadFeedOffset = useCallback(async () => {
    const res = await api.getFeedOffset();
    if (res.ok) {
      setView((prev) => ({
        ...prev,
        motionState: {
          ...prev.motionState,
          offsetL: res.feedOffsetMmL ?? prev.motionState.offsetL,
          offsetR: res.feedOffsetMmR ?? prev.motionState.offsetR,
        },
      }));
    }
  }, []);

  const saveCycleConfig = useCallback(async (cfg: Partial<CycleConfig>) => {
    const res = await api.setCycleConfig(cfg);
    return res.config;
  }, []);

  const reloadCycleConfig = useCallback(async () => {
    const res = await api.reloadCycleConfigFromDisk();
    return res.config;
  }, []);

  const setMmRpm = useCallback((mm: number, rpm: number) => {
    api.setMmRpm(mm, rpm).catch(() => {});
  }, []);

  const motionMove = useCallback((mm: number, rpm: number) => {
    api.setMmRpm(mm, rpm).catch(() => {});
    api.motionAction('move', { mm, rpm }).catch(() => {});
  }, []);

  const motionStop = useCallback(() => {
    api.motionAction('stop').catch(() => {});
  }, []);

  const motionServoOn = useCallback(() => {
    api.motionAction('on').catch(() => {});
  }, []);

  const motionServoOff = useCallback(() => {
    api.motionAction('off').catch(() => {});
  }, []);

  const motionMoveZero = useCallback(() => {
    const snap = snapRef.current;
    const rpm = snap?.rpm ?? view.motionState.rpm;
    api.motionAction('move_zero', { rpm }).catch(() => {});
  }, [view.motionState.rpm]);

  const motionSearchHome = useCallback(() => {
    api.motionAction('home').catch(() => {});
  }, []);

  const motionReset = useCallback(() => {
    api.motionAction('motion_reset').catch(() => {});
  }, []);

  const encSetZeroR = useCallback(() => {
    api.motionAction('enc_set0_r').catch(() => {});
  }, []);

  const encSetZeroL = useCallback(() => {
    api.motionAction('enc_set0_l').catch(() => {});
  }, []);

  const feedL = useCallback(() => {
    api.motionAction('feed_l').catch(() => {});
  }, []);

  const feedR = useCallback(() => {
    api.motionAction('feed_r').catch(() => {});
  }, []);

  const saveFeedOffset = useCallback((l: number, r: number) => {
    api.setFeedOffset(l, r).catch(() => {});
  }, []);

  const toggleValve = useCallback((valveId: string) => {
    if (valveLockRef.current[valveId]) return;

    const snap = snapRef.current;
    if (!snap) return;
    const valves = mapPlcState(snap).valves;
    const byte = valveByteFromId(valves, valveId);
    if (byte == null) return;

    const current = snap.plc.valves[String(byte)];
    const knownOn =
      valveOnRef.current[valveId] ??
      (current?.on === true);
    const newOn = !knownOn;

    valveLockRef.current[valveId] = true;
    valveOnRef.current[valveId] = newOn;
    setValveBusy((prev) => ({ ...prev, [valveId]: true }));

    const extra: Record<string, unknown> = { byte, on: newOn };
    if (valveId === 'blower' && newOn) {
      const sec = Number(
        snapRef.current?.plc?.blowerSec ?? snap.plc.blowerSec ?? 2
      );
      extra.durationSec = Number.isFinite(sec) ? Math.max(0.2, sec) : 2;
    }
    if (snap.plc.valves[String(byte)]) {
      snap.plc.valves[String(byte)] = {
        ...snap.plc.valves[String(byte)],
        on: newOn,
      };
    }
    setView((prev) => ({
      ...prev,
      plcState: {
        ...prev.plcState,
        valves: prev.plcState.valves.map((v) =>
          v.id === valveId ? { ...v, active: newOn } : v
        ),
      },
    }));
    api.plcAction('valve', extra).catch(() => {});

    window.setTimeout(() => {
      valveLockRef.current[valveId] = false;
      setValveBusy((prev) => {
        const next = { ...prev };
        delete next[valveId];
        return next;
      });
      // Releer estado del snap (puede haber llegado evento OFF del blower, etc.)
      const latest = snapRef.current?.plc.valves[String(byte)];
      if (latest && latest.on !== null && latest.on !== undefined) {
        valveOnRef.current[valveId] = !!latest.on;
      }
    }, VALVE_TOGGLE_LOCK_MS);
  }, []);

  const setBlowerSec = useCallback((sec: number) => {
    const v = Math.max(0.2, Math.min(300, Number(sec)));
    if (!Number.isFinite(v)) return;
    if (snapRef.current?.plc) {
      snapRef.current.plc.blowerSec = v;
    }
    setView((prev) => ({
      ...prev,
      plcState: { ...prev.plcState, blowerSec: v },
    }));
    api.plcAction('set_blower_sec', { blowerSec: v }).catch(() => {});
  }, []);

  const plcReset = useCallback(() => {
    api.plcAction('reset').catch(() => {});
  }, []);

  const plcAllOff = useCallback(() => {
    api.plcAction('all_off').catch(() => {});
  }, []);

  const pfStart = useCallback(() => {
    api.prefeederAction('start').catch(() => {});
  }, []);

  const pfStop = useCallback(() => {
    api.prefeederAction('stop').catch(() => {});
  }, []);

  const pfReset = useCallback(() => {
    api.prefeederAction('reset').catch(() => {});
  }, []);

  const pfMaterialist = useCallback(() => {
    api.prefeederAction('materialist').catch(() => {});
  }, []);

  const pfTriggerR = useCallback(() => {
    api.prefeederAction('trigger_r').catch(() => {});
  }, []);

  const pfTriggerL = useCallback(() => {
    api.prefeederAction('trigger_l').catch(() => {});
  }, []);

  const andonSetOut = useCallback((out: 'green' | 'yellow' | 'red' | 'buzzer', on: boolean) => {
    api.andonAction('set_out', { out, on }).catch(() => {});
  }, []);

  const andonAllOff = useCallback(() => {
    api.andonAction('all_off').catch(() => {});
  }, []);

  const andonResumeAuto = useCallback(() => {
    api.andonAction('resume_auto').catch(() => {});
  }, []);

  const andonMachineState = useCallback((byte: number) => {
    api.andonAction('state', { byte }).catch(() => {});
  }, []);

  const clearLogs = useCallback((target: 'main' | 'motion' | 'plc' | 'prefeeder' | 'andon' | 'all') => {
    setView((prev) => ({ ...prev, logs: [] }));
    api.clearLog(target).catch(() => {});
  }, []);

  const setAndonBuzzerMute = useCallback(async (mute: boolean) => {
    setView((prev) => ({ ...prev, andonBuzzerMute: mute }));
    try {
      const res = await api.setAppConfig({ andonBuzzerMute: mute });
      if (res?.config) {
        setView((prev) => ({
          ...prev,
          andonBuzzerMute: !!res.config.andonBuzzerMute,
        }));
      }
    } catch {
      // ignore
    }
  }, []);

  const filterLogs = useCallback(
    (module: LogEntry['module'] | 'ALL'): LogEntry[] => {
      if (module === 'ALL') return view.logs;
      return view.logs.filter((l) => l.module === module || l.module === 'SYSTEM');
    },
    [view.logs]
  );

  const reconnectNetwork = useCallback(async () => {
    await api.reconnectNetwork().catch(() => {});
  }, []);

  const startDebugTrails = useCallback(
    async (side: DebugTrailsSide, numTests: number, waitTimeS: number) => {
      const res = await api.debugTrailsStart({ side, numTests, waitTimeS });
      if (!res.ok) {
        throw new Error(res.error || 'No se pudo iniciar Debug Trails');
      }
    },
    []
  );

  const stopDebugTrails = useCallback(async () => {
    await api.debugTrailsStop();
  }, []);

  const clearDebugTrails = useCallback(async () => {
    const res = await api.debugTrailsClear();
    if (!res.ok) {
      throw new Error(res.error || 'No se pudo borrar el registro');
    }
  }, []);

  const exportDebugTrails = useCallback(() => {
    const a = document.createElement('a');
    a.href = api.debugTrailsExportUrl();
    a.download = 'debug_trails_stage1.csv';
    document.body.appendChild(a);
    a.click();
    a.remove();
  }, []);

  return {
    view,
    onTabChange,
    reconnectNetwork,
    selectModel,
    setTargetQty,
    start,
    stop,
    resume,
    pauseCycle,
    resetCycleCmd,
    setCutOffset,
    toggleCycleMaterialist,
    setCycleStepByStep,
    setCycleTrialMode,
    startDebugTrails,
    stopDebugTrails,
    clearDebugTrails,
    exportDebugTrails,
    reloadFeedOffset,
    saveCycleConfig,
    reloadCycleConfig,
    setMmRpm,
    motionMove,
    motionStop,
    motionServoOn,
    motionServoOff,
    motionSearchHome,
    motionMoveZero,
    motionReset,
    encSetZeroR,
    encSetZeroL,
    feedL,
    feedR,
    saveFeedOffset,
    toggleValve,
    valveBusy,
    setBlowerSec,
    plcReset,
    plcAllOff,
    pfStart,
    pfStop,
    pfReset,
    pfMaterialist,
    pfTriggerR,
    pfTriggerL,
    andonSetOut,
    andonAllOff,
    andonResumeAuto,
    andonMachineState,
    clearLogs,
    setAndonBuzzerMute,
    filterLogs,
    parseLogs,
  };
}
