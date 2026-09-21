import React, { useState, useEffect, useMemo, useRef, useCallback } from 'react';
import {
  Timer,
  CheckCircle2,
  Save,
  RotateCcw,
  Sliders,
  Clock,
  Plus,
  Minus,
  ShieldAlert,
  Boxes,
  ChevronLeft,
  ChevronRight,
  Footprints,
  X,
  Droplets,
  Check,
} from 'lucide-react';
import { CycleConfig, CycleStep, MachineState } from '../types';
import { useApp } from '../context/AppContext';
import type { BackendFlowStep } from '../api/backendTypes';

export const DEFAULT_CYCLE_CONFIG: CycleConfig = {
  holderOnMs: 120,
  holderOpenMs: 60,
  grippersOnMs: 60,
  gripperReleaseMs: 200,
  cutterPulseMs: 200,
  cutterPostMs: 100,
  linearDoneMs: 60,
  asentarMs: 30,
  dwellAtDestMs: 100,
  depositBatchSize: 50,
  depositExtraMm: 30,
  cutOffsetMm: 0,
  motionWaitTimeoutS: 120,
  feedWaitTimeoutS: 30,
  pfReadyTimeoutS: 10,
  feedSides: 'L',
  refillMm: 55,
  refillAsdaMm: -300,
};

/** Fallback local si el backend aún no envió flow (arranque). */
export const CYCLE_STEPS_DEFINITION: CycleStep[] = [
  { id: 1, title: 'Holder+Encoder ON (solo 1ª pieza)', type: 'action', sbsPause: false },
  { id: 2, title: 'Delay Holder ON', type: 'delay', delayKey: 'holderOnMs', defaultDurationMs: 200, sbsPause: true },
  { id: 3, title: 'Alimentación (feed / handoff)', type: 'action', sbsPause: true },
  { id: 4, title: 'Offset alimentación (si hay)', type: 'action', sbsPause: false },
  { id: 5, title: 'Pinzas cierran', type: 'action', sbsPause: false },
  { id: 6, title: 'Delay tras cerrar pinzas', type: 'delay', delayKey: 'grippersOnMs', defaultDurationMs: 100, sbsPause: false },
  { id: 7, title: 'OM ref (Stage2 RESET)', type: 'action', sbsPause: true },
  { id: 8, title: 'Holder+Encoder OFF (abre para lineal)', type: 'action', sbsPause: false },
  { id: 9, title: 'Delay Holder/Encoder OFF', type: 'delay', delayKey: 'holderOpenMs', defaultDurationMs: 100, sbsPause: true },
  { id: 10, title: 'Stage2 lineal ASDA (0→ABS)', type: 'action', sbsPause: false },
  { id: 11, title: 'Delay antes del corte', type: 'delay', delayKey: 'linearDoneMs', defaultDurationMs: 100, sbsPause: true },
  { id: 12, title: 'Holder ON / Encoder ON (pre-corte)', type: 'action', sbsPause: false },
  { id: 13, title: 'Delay tras cerrar holder', type: 'delay', delayKey: 'holderOnMs', defaultDurationMs: 200, sbsPause: true },
  { id: 14, title: 'Cortador ON (+ All OK PreFeeder)', type: 'action', sbsPause: false },
  { id: 15, title: 'Delay entre Set y Res cortador', type: 'delay', delayKey: 'cutterPulseMs', defaultDurationMs: 200, sbsPause: false },
  { id: 16, title: 'Cortador OFF', type: 'action', sbsPause: false },
  { id: 17, title: 'Delay post-corte', type: 'delay', delayKey: 'cutterPostMs', defaultDurationMs: 100, sbsPause: true },
  { id: 18, title: 'Extra / depósito lineal', type: 'action', sbsPause: false },
  { id: 19, title: 'Delay tras depósito', type: 'delay', delayKey: 'dwellAtDestMs', defaultDurationMs: 150, sbsPause: true },
  {
    id: 20,
    title: 'Prefetch feed — arranca en background',
    type: 'background',
    badge: 'background',
    note: 'Depósito ya hecho (manguera fuera). Prefetch en background; la secuencia continúa (pinzas → HOME), no todo a la vez.',
    sbsPause: false,
  },
  { id: 21, title: 'Pinzas abren', type: 'action', sbsPause: false },
  { id: 22, title: 'Trigger PreFeeder (Tfeed)', type: 'action', sbsPause: false },
  { id: 23, title: 'Delay antes de HOME', type: 'delay', delayKey: 'gripperReleaseMs', defaultDurationMs: 350, sbsPause: true },
  { id: 24, title: 'HOME: WIP blower fin/inicio + 0', type: 'action', sbsPause: true },
  { id: 25, title: 'Join — espera fin del prefetch (handoff)', type: 'join', badge: 'join', sbsPause: false },
  { id: 26, title: 'Delay asentar', type: 'delay', delayKey: 'asentarMs', defaultDurationMs: 50, sbsPause: false },
  { id: 27, title: 'Post-pieza (safety / peer / holgura)', type: 'action', sbsPause: false },
];

function stepsFromFlow(flow: BackendFlowStep[]): CycleStep[] {
  return flow.map((s) => {
    const delayKey = s.delayKey as keyof CycleConfig | undefined;
    const defaultDurationMs =
      delayKey && typeof DEFAULT_CYCLE_CONFIG[delayKey] === 'number'
        ? (DEFAULT_CYCLE_CONFIG[delayKey] as number)
        : undefined;
    const sbsPause = s.sbsPause === true;
    if (s.kind === 'wait') {
      return {
        id: s.id,
        title: s.label,
        type: 'delay' as const,
        delayKey,
        defaultDurationMs,
        sbsPause,
      };
    }
    if (s.kind === 'parallel') {
      const isJoin = s.parallelRole === 'join';
      return {
        id: s.id,
        title: s.label,
        type: (isJoin ? 'join' : 'background') as CycleStep['type'],
        badge: isJoin ? 'join' : 'background',
        note: isJoin
          ? undefined
          : 'Depósito ya hecho (manguera fuera). Prefetch en background; la secuencia continúa (pinzas → HOME).',
        sbsPause,
      };
    }
    return {
      id: s.id,
      title: s.label,
      type: 'action' as const,
      sbsPause,
    };
  });
}

interface CycleTabProps {
  machineState: MachineState;
  cycleConfig: CycleConfig;
  cycleStep: number;
  cycleActive: boolean;
  cycleFlow?: BackendFlowStep[];
  onSaveConfig: (cfg: Partial<CycleConfig>) => Promise<CycleConfig>;
  onReloadConfig: () => Promise<CycleConfig>;
  onPause?: () => void;
  onReset?: () => void;
  onMaterialist?: () => void;
  onSetStepByStep?: (on: boolean) => void;
  onStart?: () => void;
  onResume?: () => void;
  resumeEnabled?: boolean;
  onRefill?: () => void;
  onRefillConfirm?: (ok: boolean) => void;
  onRefillRetry?: () => void;
}

export const CycleTab: React.FC<CycleTabProps> = ({
  machineState,
  cycleConfig,
  cycleStep,
  cycleActive,
  cycleFlow,
  onSaveConfig,
  onReloadConfig,
  onPause,
  onReset,
  onMaterialist,
  onSetStepByStep,
  onStart,
  onResume,
  resumeEnabled = false,
  onRefill,
  onRefillConfirm,
  onRefillRetry,
}) => {
  const { t } = useApp();

  const [config, setConfig] = useState<CycleConfig>(cycleConfig || DEFAULT_CYCLE_CONFIG);
  const [configDirty, setConfigDirty] = useState(false);
  const saveTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const pendingSaveRef = useRef<CycleConfig | null>(null);
  const savingRef = useRef(false);
  /** Evita que un SSE/poll viejo con LR pise L/R recién guardado. */
  const confirmedFeedSidesRef = useRef<'L' | 'R' | 'LR' | null>(null);

  const sequenceSteps = useMemo(
    () => (cycleFlow && cycleFlow.length > 0 ? stepsFromFlow(cycleFlow) : CYCLE_STEPS_DEFINITION),
    [cycleFlow],
  );
  const maxStepId = sequenceSteps.length > 0 ? sequenceSteps[sequenceSteps.length - 1].id : 27;

  useEffect(() => {
    if (configDirty || savingRef.current) return;
    if (!cycleConfig || Object.keys(cycleConfig).length === 0) return;
    const confirmed = confirmedFeedSidesRef.current;
    if (confirmed && cycleConfig.feedSides !== confirmed) {
      setConfig({ ...cycleConfig, feedSides: confirmed });
      return;
    }
    confirmedFeedSidesRef.current = null;
    setConfig(cycleConfig);
  }, [cycleConfig, configDirty]);

  useEffect(() => {
    return () => {
      if (saveTimerRef.current) clearTimeout(saveTimerRef.current);
    };
  }, []);

  const [toastMessage, setToastMessage] = useState<string | null>(null);
  const [filterType, setFilterType] = useState<'all' | 'action' | 'delay' | 'parallel'>('all');
  const [activeStepNum, setActiveStepNum] = useState<number | null>(null);

  const stepModeActive = machineState.stepByStep;

  const persistConfig = useCallback(
    async (next: CycleConfig, toastKey: 'cycle_saved_success' | 'cycle_delay_saved') => {
      savingRef.current = true;
      confirmedFeedSidesRef.current = next.feedSides;
      try {
        const saved = await onSaveConfig(next);
        const feedSides =
          saved.feedSides === 'L' || saved.feedSides === 'R' || saved.feedSides === 'LR'
            ? saved.feedSides
            : next.feedSides;
        const merged = { ...saved, feedSides };
        confirmedFeedSidesRef.current = feedSides;
        setConfig(merged);
        setConfigDirty(false);
        setToastMessage(t(toastKey));
        setTimeout(() => setToastMessage(null), 2500);
        return merged;
      } catch (err) {
        confirmedFeedSidesRef.current = null;
        throw err;
      } finally {
        savingRef.current = false;
      }
    },
    [onSaveConfig, t],
  );

  useEffect(() => {
    if (cycleActive && cycleStep > 0) {
      setActiveStepNum(cycleStep);
    }
  }, [cycleActive, cycleStep]);

  const currentRunningStep = cycleActive ? cycleStep : activeStepNum;

  /** Pausado en paso a paso → Resume ejecuta el siguiente paso real. */
  const showStepNext =
    stepModeActive && cycleActive && machineState.isPaused && resumeEnabled;
  /** Idle en paso a paso → Siguiente arranca el lote (no solo cambia el highlight). */
  const canStartStepRun = stepModeActive && !cycleActive && Boolean(onStart);
  const canExecuteNext = showStepNext || canStartStepRun;

  const handleStartStepMode = () => {
    onSetStepByStep?.(true);
    if (!cycleActive) setActiveStepNum(1);
  };

  const handleNextStep = () => {
    if (showStepNext) {
      onResume?.();
      return;
    }
    if (canStartStepRun) {
      setActiveStepNum(1);
      onStart?.();
      return;
    }
    // Ciclo corriendo (aún no pausó): no fingir avance de UI.
  };

  const handlePrevStep = () => {
    if (cycleActive) return;
    const prevStep = activeStepNum ? Math.max(1, activeStepNum - 1) : 1;
    setActiveStepNum(prevStep);
  };

  const handleExitStepMode = () => {
    onSetStepByStep?.(false);
    if (!cycleActive) setActiveStepNum(null);
  };

  const handleSave = async () => {
    if (saveTimerRef.current) {
      clearTimeout(saveTimerRef.current);
      saveTimerRef.current = null;
    }
    try {
      await persistConfig(pendingSaveRef.current ?? config, 'cycle_saved_success');
      pendingSaveRef.current = null;
    } catch {
      // ignore
    }
  };

  const handleReload = async () => {
    if (saveTimerRef.current) {
      clearTimeout(saveTimerRef.current);
      saveTimerRef.current = null;
    }
    pendingSaveRef.current = null;
    try {
      const cfg = await onReloadConfig();
      setConfig(cfg);
      setConfigDirty(false);
      setToastMessage(t('cycle_reloaded_success'));
      setTimeout(() => setToastMessage(null), 3000);
    } catch {
      // ignore
    }
  };

  /** Auto-guarda (debounce) para que Start (reload desde disco) aplique el valor. */
  const schedulePersist = useCallback(
    (next: CycleConfig, toastKey: 'cycle_saved_success' | 'cycle_delay_saved') => {
      pendingSaveRef.current = next;
      if (saveTimerRef.current) clearTimeout(saveTimerRef.current);
      saveTimerRef.current = setTimeout(() => {
        const toSave = pendingSaveRef.current;
        if (!toSave || savingRef.current) return;
        persistConfig(toSave, toastKey).catch(() => {});
      }, 450);
    },
    [persistConfig],
  );

  const handleUpdateDelay = (delayKey?: keyof CycleConfig, value?: number) => {
    if (!delayKey || value === undefined) return;
    const cleanVal = Math.max(0, isNaN(value) ? 0 : value);
    setConfigDirty(true);
    setConfig((prev) => {
      const next = { ...prev, [delayKey]: cleanVal };
      schedulePersist(next, 'cycle_delay_saved');
      return next;
    });
  };

  const handleFeedSides = (side: 'L' | 'R' | 'LR') => {
    setConfigDirty(true);
    setConfig((prev) => {
      const next = { ...prev, feedSides: side };
      schedulePersist(next, 'cycle_saved_success');
      return next;
    });
  };

  const updateConfigField = <K extends keyof CycleConfig>(key: K, value: CycleConfig[K]) => {
    setConfigDirty(true);
    setConfig((prev) => {
      const next = { ...prev, [key]: value };
      schedulePersist(next, 'cycle_saved_success');
      return next;
    });
  };

  const totalDelaysMs =
    config.holderOnMs * 2 +
    config.grippersOnMs +
    config.holderOpenMs +
    config.linearDoneMs +
    config.cutterPulseMs +
    config.cutterPostMs +
    config.dwellAtDestMs +
    config.gripperReleaseMs +
    config.asentarMs;

  const estimatedTotalTimeSec = ((totalDelaysMs + 2400) / 1000).toFixed(2);

  const filteredSteps = sequenceSteps.filter((step) => {
    if (filterType === 'action') return step.type === 'action';
    if (filterType === 'delay') return step.type === 'delay';
    if (filterType === 'parallel') return step.type === 'background' || step.type === 'join';
    return true;
  });

  return (
    <div className="space-y-5">
      {toastMessage && (
        <div className="flex items-center gap-2 rounded-lg bg-emerald-500 text-white px-4 py-2.5 shadow-md">
          <CheckCircle2 className="h-5 w-5 shrink-0" />
          <span className="text-sm font-semibold">{toastMessage}</span>
        </div>
      )}

      {/* Controles de ciclo en vivo */}
      <div className="flex flex-wrap items-center gap-2 rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 px-4 py-3 shadow-2xs">
        <span
          className={`text-xs font-semibold mr-2 ${
            machineState.fault
              ? 'text-red-700 dark:text-red-300'
              : 'text-slate-600 dark:text-slate-400'
          }`}
        >
          {machineState.fault
            ? machineState.fault
            : cycleActive
            ? `${t('cycle_step_status').replace('{step}', String(cycleStep))}${machineState.cycleStepLabel ? ` · ${machineState.cycleStepLabel}` : ''}`
            : machineState.statusText || t('state_ready')}
        </span>
        {stepModeActive && (
          <span className="rounded-md border border-teal-300 dark:border-teal-800 bg-teal-50 dark:bg-teal-950/40 px-2 py-0.5 text-[10px] font-bold uppercase tracking-wider text-teal-700 dark:text-teal-300">
            {t('step_by_step_active')}
          </span>
        )}
        {stepModeActive && !cycleActive && (
          <span className="text-[10px] text-slate-500 dark:text-slate-400 max-w-md">
            {t('step_by_step_hint')}
          </span>
        )}
        {showStepNext ? (
          <button
            id="btn-cycle-next-step"
            onClick={onResume}
            className="flex items-center gap-1 rounded-lg border border-teal-300 dark:border-teal-700 bg-teal-600 hover:bg-teal-500 px-3 py-1.5 text-xs font-bold text-white"
          >
            <ChevronRight className="h-3.5 w-3.5" />
            {t('btn_next_step')}
          </button>
        ) : (
          <button
            id="btn-cycle-pause"
            onClick={onPause}
            disabled={!machineState.pauseEnabled}
            className="flex items-center gap-1 rounded-lg border border-amber-200 dark:border-amber-800 bg-amber-50 dark:bg-amber-950/40 px-3 py-1.5 text-xs font-bold text-amber-800 dark:text-amber-200 disabled:opacity-40"
          >
            <Timer className="h-3.5 w-3.5" />
            {t('btn_pause')}
          </button>
        )}
        <button
          id="btn-cycle-reset"
          onClick={onReset}
          className="flex items-center gap-1 rounded-lg border border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 px-3 py-1.5 text-xs font-bold text-slate-700 dark:text-slate-200"
        >
          <RotateCcw className="h-3.5 w-3.5" />
          {t('btn_reset_cycle')}
          <span className="font-mono text-[10px]">0x043</span>
        </button>
        {onMaterialist && (
          <button
            id="btn-cycle-materialist"
            onClick={onMaterialist}
            className={`flex items-center gap-1 rounded-lg border px-3 py-1.5 text-xs font-bold ${
              machineState.cycleMaterialist
                ? 'border-violet-400 bg-violet-100 dark:bg-violet-950/50 text-violet-800 dark:text-violet-200'
                : 'border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 text-slate-700 dark:text-slate-200'
            }`}
          >
            {t('btn_materialist_cycle')}
            <span className="font-mono text-[10px]">0x049</span>
          </button>
        )}
        {onRefill && (
          <button
            id="btn-cycle-refill"
            type="button"
            onClick={onRefill}
            disabled={machineState.isRunning || machineState.refillActive}
            className={`flex items-center gap-1 rounded-lg border px-3 py-1.5 text-xs font-bold ${
              machineState.isRunning || machineState.refillActive
                ? 'border-slate-200 dark:border-slate-700 bg-slate-100 dark:bg-slate-800 text-slate-400 cursor-not-allowed'
                : 'border-sky-300 dark:border-sky-800 bg-sky-50 dark:bg-sky-950/40 text-sky-800 dark:text-sky-200'
            }`}
          >
            <Droplets className="h-3.5 w-3.5" />
            {t('btn_refill')}
          </button>
        )}
      </div>

      {machineState.refillAwaitingConfirm && onRefillConfirm && (
        <div className="flex flex-wrap items-center gap-3 rounded-xl border border-sky-300 dark:border-sky-700 bg-sky-50 dark:bg-sky-950/50 px-4 py-3 shadow-2xs">
          <div className="min-w-0 flex-1">
            <p className="text-xs font-bold text-sky-900 dark:text-sky-100">
              {machineState.refillPrompt === 'after_cut'
                ? t('refill_confirm_title_cut')
                : t('refill_confirm_title_feed')}
            </p>
            <p className="text-[11px] text-sky-800/80 dark:text-sky-200/80 mt-0.5">
              {machineState.refillPrompt === 'after_cut'
                ? t('refill_confirm_hint_cut')
                : t('refill_confirm_hint_feed')}
            </p>
          </div>
          {machineState.refillPrompt === 'after_feed' && onRefillRetry && (
            <button
              id="btn-cycle-refill-retry"
              type="button"
              onClick={onRefillRetry}
              className="flex items-center gap-1.5 rounded-lg bg-amber-500 hover:bg-amber-600 px-3 py-1.5 text-xs font-bold text-white"
            >
              <RotateCcw className="h-3.5 w-3.5" />
              {t('btn_refill_confirm_retry')}
            </button>
          )}
          <button
            id="btn-cycle-refill-yes"
            type="button"
            onClick={() => onRefillConfirm(true)}
            className="flex items-center gap-1.5 rounded-lg bg-emerald-600 hover:bg-emerald-700 px-3 py-1.5 text-xs font-bold text-white"
          >
            <Check className="h-3.5 w-3.5" />
            {machineState.refillPrompt === 'after_cut'
              ? t('btn_refill_confirm_yes')
              : t('btn_refill_confirm_next_cut')}
          </button>
          <button
            id="btn-cycle-refill-no"
            type="button"
            onClick={() => onRefillConfirm(false)}
            className="flex items-center gap-1.5 rounded-lg border border-slate-300 dark:border-slate-600 bg-white dark:bg-slate-800 px-3 py-1.5 text-xs font-bold text-slate-700 dark:text-slate-200"
          >
            <X className="h-3.5 w-3.5" />
            {t('btn_refill_confirm_no')}
          </button>
        </div>
      )}

      {/* SECTION 1: CYCLE SEQUENCE */}
      <div className="rounded-xl border border-slate-200 dark:border-slate-800/80 bg-white dark:bg-slate-900 shadow-2xs">
        <div className="border-b border-slate-200 dark:border-slate-800 p-4 sm:px-6 flex flex-wrap items-center justify-between gap-3">
          <div>
            <div className="flex items-center gap-2.5 flex-wrap">
              <div className="flex items-center gap-2">
                <Sliders className="h-4 w-4 text-teal-600 dark:text-teal-400" />
                <h2 className="text-sm font-bold tracking-wider uppercase text-slate-900 dark:text-slate-100">
                  {t('cycle_sequence_title')}
                </h2>
              </div>
              <span className="flex items-center gap-1 bg-slate-100 dark:bg-slate-800 text-slate-700 dark:text-slate-200 px-2.5 py-0.5 rounded-md border border-slate-200 dark:border-slate-700 text-xs font-mono shadow-2xs">
                <Clock className="h-3.5 w-3.5 text-sky-500" />
                <span>Est. Ciclo: <strong className="text-teal-600 dark:text-teal-400">~{estimatedTotalTimeSec}s</strong></span>
              </span>
            </div>
          </div>

          <div className="flex items-center gap-2 flex-wrap">
            <div className="flex items-center bg-slate-100 dark:bg-slate-800 p-0.5 rounded-lg border border-slate-200 dark:border-slate-700 text-xs">
              <button
                onClick={() => setFilterType('all')}
                className={`px-2.5 py-1 rounded-md transition cursor-pointer ${
                  filterType === 'all'
                    ? 'bg-white dark:bg-slate-700 font-bold text-slate-900 dark:text-white shadow-2xs'
                    : 'text-slate-600 dark:text-slate-400 hover:text-slate-900'
                }`}
              >
                Todos ({sequenceSteps.length})
              </button>
              <button
                onClick={() => setFilterType('action')}
                className={`px-2.5 py-1 rounded-md transition cursor-pointer ${
                  filterType === 'action'
                    ? 'bg-white dark:bg-slate-700 font-bold text-slate-900 dark:text-white shadow-2xs'
                    : 'text-slate-600 dark:text-slate-400 hover:text-slate-900'
                }`}
              >
                Acciones
              </button>
              <button
                onClick={() => setFilterType('delay')}
                className={`px-2.5 py-1 rounded-md transition cursor-pointer ${
                  filterType === 'delay'
                    ? 'bg-white dark:bg-slate-700 font-bold text-teal-600 dark:text-teal-400 shadow-2xs'
                    : 'text-slate-600 dark:text-slate-400 hover:text-slate-900'
                }`}
              >
                Delays (⏱)
              </button>
              <button
                onClick={() => setFilterType('parallel')}
                className={`px-2.5 py-1 rounded-md transition cursor-pointer ${
                  filterType === 'parallel'
                    ? 'bg-white dark:bg-slate-700 font-bold text-amber-600 dark:text-amber-400 shadow-2xs'
                    : 'text-slate-600 dark:text-slate-400 hover:text-slate-900'
                }`}
              >
                || Prefetch/Join
              </button>
            </div>

            {!stepModeActive ? (
              <button
                id="btn-step-by-step"
                onClick={handleStartStepMode}
                className="flex items-center gap-1.5 px-3 py-1.5 rounded-lg text-xs font-semibold border transition shadow-2xs cursor-pointer bg-teal-50 dark:bg-teal-950/40 text-teal-700 dark:text-teal-300 border-teal-300 dark:border-teal-800 hover:bg-teal-100 dark:hover:bg-teal-900/60"
              >
                <Footprints className="h-3.5 w-3.5" />
                <span>{t('btn_step_by_step')}</span>
              </button>
            ) : (
              <div className="flex items-center gap-1 bg-teal-500/10 dark:bg-teal-950/60 border border-teal-500/40 rounded-lg p-0.5 sm:p-1 shadow-2xs animate-in fade-in">
                <button
                  id="btn-step-prev"
                  onClick={handlePrevStep}
                  disabled={cycleActive || activeStepNum === 1}
                  title={t('btn_prev_step')}
                  className="flex items-center gap-0.5 px-2 py-1 rounded text-xs font-bold text-teal-700 dark:text-teal-300 hover:bg-teal-500/20 disabled:opacity-40 disabled:cursor-not-allowed transition cursor-pointer"
                >
                  <ChevronLeft className="h-3.5 w-3.5" />
                  <span className="hidden sm:inline">{t('btn_prev_step')}</span>
                </button>

                <span className="font-mono text-xs font-bold text-teal-900 dark:text-teal-100 px-2 py-0.5 bg-white dark:bg-slate-800 rounded border border-teal-500/30">
                  {(cycleActive ? cycleStep : activeStepNum) ?? 1}/{maxStepId}
                </span>

                <button
                  id="btn-step-next"
                  onClick={handleNextStep}
                  disabled={!canExecuteNext}
                  title={
                    showStepNext
                      ? t('btn_next_step')
                      : canStartStepRun
                      ? t('btn_next_step_start')
                      : t('btn_next_step')
                  }
                  className="flex items-center gap-0.5 px-2.5 py-1 rounded text-xs font-bold bg-teal-600 hover:bg-teal-500 text-white disabled:opacity-40 disabled:cursor-not-allowed transition cursor-pointer"
                >
                  <span>{t('btn_next_step')}</span>
                  <ChevronRight className="h-3.5 w-3.5" />
                </button>

                <button
                  id="btn-step-exit"
                  onClick={handleExitStepMode}
                  title={t('btn_exit_step_mode')}
                  className="flex h-6 w-6 items-center justify-center rounded hover:bg-rose-500/20 text-slate-400 hover:text-rose-500 transition cursor-pointer"
                >
                  <X className="h-3.5 w-3.5" />
                </button>
              </div>
            )}

            <button
              onClick={handleSave}
              title={t('btn_save')}
              className="flex items-center gap-1.5 rounded-lg bg-teal-600 hover:bg-teal-500 text-white px-3 py-1.5 text-xs font-bold shadow-2xs transition active:scale-95 cursor-pointer"
            >
              <Save className="h-3.5 w-3.5" />
              <span>{t('btn_save')}</span>
            </button>
          </div>
        </div>

        {configDirty && (
          <div className="mx-4 sm:mx-6 mt-3 flex items-center gap-2 rounded-lg border border-amber-300/80 bg-amber-50 dark:border-amber-700/60 dark:bg-amber-950/40 px-3 py-2 text-xs text-amber-900 dark:text-amber-200">
            <ShieldAlert className="h-4 w-4 shrink-0" />
            <span>{t('cycle_unsaved_warning')}</span>
          </div>
        )}

        <div className="max-h-[500px] overflow-y-auto p-2 sm:p-4 space-y-1.5 divide-y divide-slate-100 dark:divide-slate-800/60 font-sans">
          {filteredSteps.map((step) => {
            const isCurrent = currentRunningStep === step.id;
            const stepDelayVal = step.delayKey ? config[step.delayKey] : step.defaultDurationMs;

            return (
              <div
                key={step.id}
                className={`group flex items-center justify-between p-2 sm:px-3.5 rounded-lg transition-all ${
                  isCurrent
                    ? 'bg-teal-500/10 dark:bg-teal-950/60 border border-teal-500/40 shadow-xs ring-1 ring-teal-500/20'
                    : step.type === 'background'
                    ? 'bg-amber-50/60 dark:bg-amber-950/20 border border-amber-200/60 dark:border-amber-900/40'
                    : step.type === 'join'
                    ? 'bg-teal-50/60 dark:bg-teal-950/30 border border-teal-200/60 dark:border-teal-900/40'
                    : step.type === 'delay'
                    ? 'bg-slate-50/40 dark:bg-slate-900/40 hover:bg-slate-100/70 dark:hover:bg-slate-800/60'
                    : 'hover:bg-slate-50 dark:hover:bg-slate-800/40'
                }`}
              >
                <div className="flex items-start sm:items-center gap-2.5 sm:gap-3.5 min-w-0 flex-1">
                  {step.type === 'action' && (
                    <div className={`flex h-6 w-6 shrink-0 items-center justify-center rounded-full text-xs font-mono font-bold border ${
                      isCurrent
                        ? 'bg-teal-500 text-white border-teal-600 ring-2 ring-teal-300'
                        : 'border-teal-500/40 text-teal-600 dark:text-teal-400 bg-teal-50/50 dark:bg-teal-950/30'
                    }`}>
                      {step.id}
                    </div>
                  )}

                  {step.type === 'delay' && (
                    <div className={`flex h-6 w-6 shrink-0 items-center justify-center rounded text-xs font-mono font-medium ${
                      isCurrent
                        ? 'bg-teal-500 text-white ring-2 ring-teal-300'
                        : 'text-teal-600 dark:text-teal-400 bg-teal-50/80 dark:bg-teal-950/40'
                    }`}>
                      <Timer className="h-4 w-4" />
                    </div>
                  )}

                  {step.type === 'background' && (
                    <div className="flex h-6 w-6 shrink-0 items-center justify-center rounded bg-amber-100 dark:bg-amber-950 text-amber-700 dark:text-amber-400 font-mono font-black text-xs border border-amber-300 dark:border-amber-800">
                      ||
                    </div>
                  )}

                  {step.type === 'join' && (
                    <div className="flex h-6 w-6 shrink-0 items-center justify-center rounded bg-teal-100 dark:bg-teal-950 text-teal-700 dark:text-teal-400 font-mono font-black text-xs border border-teal-300 dark:border-teal-800">
                      ||
                    </div>
                  )}

                  <span className="font-mono text-xs font-semibold text-slate-400 dark:text-slate-500 min-w-[20px]">
                    {step.id}
                  </span>

                  <div className="min-w-0 flex-1">
                    <div className="flex items-center gap-2 flex-wrap">
                      <span className={`text-xs sm:text-sm font-medium ${
                        isCurrent
                          ? 'font-bold text-teal-900 dark:text-teal-100'
                          : step.type === 'delay'
                          ? 'text-slate-700 dark:text-slate-300'
                          : 'text-slate-800 dark:text-slate-200'
                      }`}>
                        {step.title}
                      </span>

                      {step.badge === 'background' && (
                        <span className="rounded bg-amber-500/20 text-amber-700 dark:text-amber-400 border border-amber-500/30 px-1.5 py-0.2 font-mono text-[10px] font-bold uppercase">
                          background
                        </span>
                      )}

                      {step.badge === 'join' && (
                        <span className="rounded bg-teal-500/20 text-teal-700 dark:text-teal-400 border border-teal-500/30 px-1.5 py-0.2 font-mono text-[10px] font-bold uppercase">
                          join
                        </span>
                      )}

                      {stepModeActive && step.sbsPause && (
                        <span className="rounded bg-teal-600/15 text-teal-800 dark:text-teal-300 border border-teal-500/25 px-1.5 py-0.2 font-mono text-[10px] font-bold uppercase">
                          {t('step_sbs_checkpoint')}
                        </span>
                      )}

                      {stepModeActive && !step.sbsPause && (
                        <span className="rounded bg-slate-500/10 text-slate-500 dark:text-slate-400 border border-slate-400/20 px-1.5 py-0.2 font-mono text-[10px] font-bold uppercase">
                          {t('step_sbs_auto')}
                        </span>
                      )}

                      {isCurrent && (
                        <span className="inline-flex items-center gap-1 rounded bg-teal-500 text-white px-2 py-0.2 font-mono text-[10px] font-bold animate-pulse">
                          EN EJECUCIÓN
                        </span>
                      )}
                    </div>

                    {step.note && (
                      <p className="mt-1 text-[11px] italic text-slate-500 dark:text-slate-400">
                        • {step.note}
                      </p>
                    )}
                  </div>
                </div>

                <div className="flex items-center gap-2 shrink-0 ml-2">
                  {step.type === 'delay' && step.delayKey && (
                    <div className="flex items-center gap-1 bg-white dark:bg-slate-800 border border-slate-300 dark:border-slate-700 hover:border-teal-500 dark:hover:border-teal-500 rounded-lg p-0.5 shadow-2xs transition">
                      <button
                        type="button"
                        onClick={() => handleUpdateDelay(step.delayKey, Math.max(0, (stepDelayVal || 0) - 25))}
                        title="Restar 25ms"
                        className="flex h-6 w-6 items-center justify-center rounded hover:bg-slate-100 dark:hover:bg-slate-700 text-slate-500 hover:text-slate-900 dark:text-slate-400 dark:hover:text-white transition active:scale-95 cursor-pointer"
                      >
                        <Minus className="h-3 w-3" />
                      </button>

                      <div className="flex items-center gap-0.5 px-1">
                        <input
                          id={`input-delay-step-${step.id}`}
                          type="number"
                          min={0}
                          step={10}
                          value={stepDelayVal ?? ''}
                          onChange={(e) => handleUpdateDelay(step.delayKey, Number(e.target.value))}
                          className="w-16 sm:w-20 bg-transparent text-center font-mono font-bold text-xs sm:text-sm text-teal-600 dark:text-teal-400 focus:outline-hidden focus:ring-1 focus:ring-teal-500 rounded"
                        />
                        <span className="text-[11px] font-mono text-slate-400 dark:text-slate-500 select-none">
                          ms
                        </span>
                      </div>

                      <button
                        type="button"
                        onClick={() => handleUpdateDelay(step.delayKey, (stepDelayVal || 0) + 25)}
                        title="Sumar 25ms"
                        className="flex h-6 w-6 items-center justify-center rounded hover:bg-slate-100 dark:hover:bg-slate-700 text-slate-500 hover:text-slate-900 dark:text-slate-400 dark:hover:text-white transition active:scale-95 cursor-pointer"
                      >
                        <Plus className="h-3 w-3" />
                      </button>
                    </div>
                  )}
                </div>
              </div>
            );
          })}
        </div>
      </div>

      {/* Helpers: refill params (debajo de la secuencia) */}
      <div className="rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 p-4 sm:px-6 shadow-2xs">
        <div className="mb-3 flex items-center gap-2">
          <Droplets className="h-4 w-4 text-sky-600 dark:text-sky-400" />
          <h2 className="text-sm font-bold tracking-wider uppercase text-slate-900 dark:text-slate-100">
            {t('refill_helpers_title')}
          </h2>
        </div>
        <p className="text-xs text-slate-500 dark:text-slate-400 mb-3">
          {t('refill_helpers_subtitle')}
        </p>
        <div className="grid grid-cols-1 sm:grid-cols-2 gap-4 max-w-lg">
          <div className="space-y-1.5">
            <label className="text-xs font-semibold text-slate-700 dark:text-slate-300">
              {t('cfg_refill_mm')}
            </label>
            <input
              type="number"
              min={1}
              step={0.5}
              value={config.refillMm ?? 55}
              onChange={(e) => updateConfigField('refillMm', Number(e.target.value))}
              className="w-full rounded-lg border border-slate-300 dark:border-slate-700 bg-slate-50 dark:bg-slate-800/80 px-3 py-2 text-sm font-mono text-slate-900 dark:text-slate-100 focus:border-teal-500 focus:outline-hidden focus:ring-1 focus:ring-teal-500"
            />
          </div>
          <div className="space-y-1.5">
            <label className="text-xs font-semibold text-slate-700 dark:text-slate-300">
              {t('cfg_refill_asda')}
            </label>
            <input
              type="number"
              step={1}
              value={config.refillAsdaMm ?? -300}
              onChange={(e) => updateConfigField('refillAsdaMm', Number(e.target.value))}
              className="w-full rounded-lg border border-slate-300 dark:border-slate-700 bg-slate-50 dark:bg-slate-800/80 px-3 py-2 text-sm font-mono text-slate-900 dark:text-slate-100 focus:border-teal-500 focus:outline-hidden focus:ring-1 focus:ring-teal-500"
            />
          </div>
        </div>
        <p className="text-[11px] text-slate-500 dark:text-slate-400 mt-2">
          {t('cfg_refill_hint')}
        </p>
      </div>

      {/* SECTIONS 2 & 3: MATERIAL HANDLING & TIMEOUTS */}
      <div className="grid grid-cols-1 lg:grid-cols-12 gap-5">
        <div className="lg:col-span-5 flex flex-col justify-between rounded-xl border border-slate-200 dark:border-slate-800/80 bg-white dark:bg-slate-900 p-4 sm:p-6 shadow-2xs">
          <div>
            <div className="mb-5 pb-3 border-b border-slate-100 dark:border-slate-800">
              <div className="flex items-center gap-2">
                <Boxes className="h-4 w-4 text-teal-600 dark:text-teal-400" />
                <h3 className="text-sm font-bold tracking-wider uppercase text-slate-900 dark:text-slate-100">
                  {t('material_handling_title')}
                </h3>
              </div>
              <p className="text-xs text-slate-500 dark:text-slate-400 mt-0.5 font-mono">
                {t('material_handling_subtitle')}
              </p>
            </div>

            <div className="mb-4 space-y-1.5">
              <label className="text-xs font-semibold text-slate-700 dark:text-slate-300">
                {t('cfg_feed_sides')}
              </label>
              <div className="flex items-center gap-1.5">
                {(['L', 'R', 'LR'] as const).map((side) => (
                  <button
                    key={side}
                    type="button"
                    onClick={() => handleFeedSides(side)}
                    className={`rounded-md px-3 py-1.5 text-xs font-mono font-semibold border transition ${
                      config.feedSides === side
                        ? 'bg-slate-900 dark:bg-slate-100 text-white dark:text-slate-900 border-slate-900 dark:border-white'
                        : 'bg-slate-100 dark:bg-slate-800 text-slate-700 dark:text-slate-300 border-slate-200 dark:border-slate-700 hover:bg-slate-200 dark:hover:bg-slate-700'
                    }`}
                  >
                    {side === 'LR' ? t('cfg_feed_sides_both') : side}
                  </button>
                ))}
              </div>
              <p className="text-[11px] text-slate-500 dark:text-slate-400">
                {t('cfg_feed_sides_hint')}
              </p>
            </div>

            <div className="grid grid-cols-1 sm:grid-cols-3 gap-4">
              <div className="space-y-1.5">
                <label className="text-xs font-semibold text-slate-700 dark:text-slate-300">
                  {t('cfg_deposito_batch_size')}
                </label>
                <input
                  type="number"
                  min={1}
                  value={config.depositBatchSize}
                  onChange={(e) => updateConfigField('depositBatchSize', Number(e.target.value))}
                  className="w-full rounded-lg border border-slate-300 dark:border-slate-700 bg-slate-50 dark:bg-slate-800/80 px-3 py-2 text-sm font-mono text-slate-900 dark:text-slate-100 focus:border-teal-500 focus:outline-hidden focus:ring-1 focus:ring-teal-500"
                />
              </div>

              <div className="space-y-1.5">
                <label className="text-xs font-semibold text-slate-700 dark:text-slate-300">
                  {t('cfg_deposito_extra')}
                </label>
                <input
                  type="number"
                  step={0.1}
                  value={config.depositExtraMm}
                  onChange={(e) => updateConfigField('depositExtraMm', Number(e.target.value))}
                  className="w-full rounded-lg border border-slate-300 dark:border-slate-700 bg-slate-50 dark:bg-slate-800/80 px-3 py-2 text-sm font-mono text-slate-900 dark:text-slate-100 focus:border-teal-500 focus:outline-hidden focus:ring-1 focus:ring-teal-500"
                />
              </div>

              <div className="space-y-1.5">
                <label className="text-xs font-semibold text-slate-700 dark:text-slate-300">
                  {t('cfg_cut_offset')}
                </label>
                <input
                  type="number"
                  step={0.1}
                  value={config.cutOffsetMm ?? 0}
                  onChange={(e) => updateConfigField('cutOffsetMm', Number(e.target.value))}
                  className="w-full rounded-lg border border-slate-300 dark:border-slate-700 bg-slate-50 dark:bg-slate-800/80 px-3 py-2 text-sm font-mono text-slate-900 dark:text-slate-100 focus:border-teal-500 focus:outline-hidden focus:ring-1 focus:ring-teal-500"
                />
              </div>
            </div>
          </div>
        </div>

        <div className="lg:col-span-7 flex flex-col justify-between rounded-xl border border-slate-200 dark:border-slate-800/80 bg-white dark:bg-slate-900 p-4 sm:p-6 shadow-2xs">
          <div>
            <div className="mb-5 pb-3 border-b border-slate-100 dark:border-slate-800">
              <div className="flex items-center gap-2">
                <ShieldAlert className="h-4 w-4 text-teal-600 dark:text-teal-400" />
                <h3 className="text-sm font-bold tracking-wider uppercase text-slate-900 dark:text-slate-100">
                  {t('timeouts_title')}
                </h3>
              </div>
              <p className="text-xs text-slate-500 dark:text-slate-400 mt-0.5 font-mono">
                {t('timeouts_subtitle')}
              </p>
            </div>

            <div className="grid grid-cols-1 sm:grid-cols-3 gap-4">
              <div className="space-y-1.5">
                <label className="text-xs font-semibold text-slate-700 dark:text-slate-300">
                  {t('cfg_timeout_motion')}
                </label>
                <input
                  type="number"
                  min={1}
                  value={config.motionWaitTimeoutS}
                  onChange={(e) => updateConfigField('motionWaitTimeoutS', Number(e.target.value))}
                  className="w-full rounded-lg border border-slate-300 dark:border-slate-700 bg-slate-50 dark:bg-slate-800/80 px-3 py-2 text-sm font-mono text-slate-900 dark:text-slate-100 focus:border-teal-500 focus:outline-hidden focus:ring-1 focus:ring-teal-500"
                />
              </div>

              <div className="space-y-1.5">
                <label className="text-xs font-semibold text-slate-700 dark:text-slate-300">
                  {t('cfg_timeout_feed')}
                </label>
                <input
                  type="number"
                  min={1}
                  value={config.feedWaitTimeoutS}
                  onChange={(e) => updateConfigField('feedWaitTimeoutS', Number(e.target.value))}
                  className="w-full rounded-lg border border-slate-300 dark:border-slate-700 bg-slate-50 dark:bg-slate-800/80 px-3 py-2 text-sm font-mono text-slate-900 dark:text-slate-100 focus:border-teal-500 focus:outline-hidden focus:ring-1 focus:ring-teal-500"
                />
              </div>

              <div className="space-y-1.5">
                <label className="text-xs font-semibold text-slate-700 dark:text-slate-300">
                  {t('cfg_timeout_pf_ready')}
                </label>
                <input
                  type="number"
                  min={1}
                  value={config.pfReadyTimeoutS}
                  onChange={(e) => updateConfigField('pfReadyTimeoutS', Number(e.target.value))}
                  className="w-full rounded-lg border border-slate-300 dark:border-slate-700 bg-slate-50 dark:bg-slate-800/80 px-3 py-2 text-sm font-mono text-slate-900 dark:text-slate-100 focus:border-teal-500 focus:outline-hidden focus:ring-1 focus:ring-teal-500"
                />
              </div>
            </div>
          </div>
        </div>
      </div>

      <div className="flex items-center gap-3">
        <button
          id="btn-save-cycle-config"
          onClick={handleSave}
          className="flex items-center gap-2 rounded-lg bg-teal-600 hover:bg-teal-500 text-white px-5 py-2 text-sm font-bold shadow-2xs transition active:scale-95 cursor-pointer"
        >
          <Save className="h-4 w-4" />
          <span>{t('btn_save')}</span>
        </button>

        <button
          id="btn-reload-cycle-config"
          onClick={handleReload}
          className="flex items-center gap-2 rounded-lg border border-slate-300 dark:border-slate-700 bg-slate-100 dark:bg-slate-800 hover:bg-slate-200 dark:hover:bg-slate-700 text-slate-700 dark:text-slate-200 px-4 py-2 text-sm font-semibold shadow-2xs transition active:scale-95 cursor-pointer"
        >
          <RotateCcw className="h-4 w-4 text-slate-500" />
          <span>{t('btn_reload')}</span>
        </button>
      </div>
    </div>
  );
};
