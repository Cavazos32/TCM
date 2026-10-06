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
import {
  hmiAssistHint,
  hmiAssistTitle,
  hmiModuleBtnChip,
  hmiModuleBtnChipOff,
  hmiModuleBtnChipOn,
  hmiModuleBtnDanger,
  hmiModuleBtnGroup,
  hmiModuleBtnPrimary,
  hmiModuleBtnSecondary,
  hmiModuleBtnSeg,
  hmiModuleBtnSegActive,
  hmiModuleBtnSegIdle,
  hmiModuleBtnSegWrap,
  hmiModuleBtnSuccess,
  hmiModuleBtnTealSoft,
  hmiModuleBtnWarning,
  hmiModuleCenterSectionTitle,
  hmiModuleCompactFieldInput,
  hmiModuleFieldLabel,
  hmiModuleIconBtn,
  hmiModuleIconSection,
  hmiBtnToolbarWrap,
  hmiCfgGrid,
  hmiCycleStepBadge,
  hmiCycleStepIcon,
  hmiCycleStepNote,
  hmiCycleStepRow,
  hmiCycleStepTitle,
  hmiCycleTimeChip,
  hmiPanelCls,
  hmiPanelHeader,
  hmiPanelPadding,
  hmiSectionTitleMb,
} from '../styles/hmiUi';

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
  depositStackGapMm: 20,
  depositMaxTravelMm: 1500,
  gripperClearanceMm: 10,
  cutOffsetMm: 0,
  wipBlowerInicioOffsetMm: 8,
  motionWaitTimeoutS: 25,
  feedWaitTimeoutS: 15,
  pfReadyTimeoutS: 15,
  pieceWatchTimeoutS: 20,
  feedSides: 'LR',
  pfTriggerEnabled: true,
  refillMm: 55,
  refillAsdaMm: -300,
};

/** Fallback local si el backend aún no envió flow (arranque). */
export const CYCLE_STEPS_DEFINITION: CycleStep[] = [
  { id: 1, title: 'Sujetar tubo (primera pieza)', type: 'action', sbsPause: false },
  { id: 2, title: 'Espera: sujetador', type: 'delay', delayKey: 'holderOnMs', defaultDurationMs: 200, sbsPause: true },
  { id: 3, title: 'Pedir material al alimentador', type: 'action', sbsPause: false },
  { id: 4, title: 'Cargar material al área de corte', type: 'action', sbsPause: true },
  { id: 5, title: 'Ajustar longitud del tubo', type: 'action', sbsPause: false },
  { id: 6, title: 'Cerrar pinzas', type: 'action', sbsPause: false },
  { id: 7, title: 'Espera: pinzas cerradas', type: 'delay', delayKey: 'grippersOnMs', defaultDurationMs: 100, sbsPause: false },
  { id: 8, title: 'Referencia de medida', type: 'action', sbsPause: true },
  { id: 9, title: 'Liberar tubo para avance', type: 'action', sbsPause: false },
  { id: 10, title: 'Espera: tubo liberado', type: 'delay', delayKey: 'holderOpenMs', defaultDurationMs: 100, sbsPause: true },
  { id: 11, title: 'Avance lineal al corte', type: 'action', sbsPause: false },
  { id: 12, title: 'Espera antes del corte', type: 'delay', delayKey: 'linearDoneMs', defaultDurationMs: 100, sbsPause: true },
  { id: 13, title: 'Sujetar antes del corte', type: 'action', sbsPause: false },
  { id: 14, title: 'Espera: listo para cortar', type: 'delay', delayKey: 'holderOnMs', defaultDurationMs: 200, sbsPause: true },
  { id: 15, title: 'Cortar tubo', type: 'action', sbsPause: false },
  {
    id: 16,
    title: 'Tiempo de corte (fijo)',
    type: 'delay',
    delayKey: 'cutterPulseMs',
    defaultDurationMs: 200,
    sbsPause: false,
    delayEditable: false,
    note: 'Duración fija del corte — no se puede cambiar',
  },
  { id: 17, title: 'Apagar cortador', type: 'action', sbsPause: false },
  { id: 18, title: 'Espera tras el corte', type: 'delay', delayKey: 'cutterPostMs', defaultDurationMs: 100, sbsPause: true },
  { id: 19, title: 'Depositar pieza', type: 'action', sbsPause: false },
  { id: 20, title: 'Espera en depósito', type: 'delay', delayKey: 'dwellAtDestMs', defaultDurationMs: 150, sbsPause: true },
  { id: 21, title: 'Abrir pinzas', type: 'action', sbsPause: false },
  { id: 22, title: 'Espera: pinzas abiertas', type: 'delay', delayKey: 'gripperReleaseMs', defaultDurationMs: 350, sbsPause: true },
  { id: 23, title: 'Retroceso de seguridad', type: 'action', sbsPause: true },
  { id: 24, title: 'Volver a posición inicial', type: 'action', sbsPause: true },
  {
    id: 25,
    title: 'Preparar siguiente pieza',
    type: 'action',
    sbsPause: true,
    note: 'Solo con la máquina en posición inicial. En la última pieza del lote se omite.',
  },
  { id: 26, title: 'Espera: material asentado', type: 'delay', delayKey: 'asentarMs', defaultDurationMs: 50, sbsPause: false },
  { id: 27, title: 'Comprobar pieza terminada', type: 'action', sbsPause: false },
]

function stepsFromFlow(flow: BackendFlowStep[]): CycleStep[] {
  return flow.map((s) => {
    const delayKey = s.delayKey as keyof CycleConfig | undefined;
    const defaultDurationMs =
      delayKey && typeof DEFAULT_CYCLE_CONFIG[delayKey] === 'number'
        ? (DEFAULT_CYCLE_CONFIG[delayKey] as number)
        : undefined;
    const sbsPause = s.sbsPause === true;
    const note = s.note;
    const delayEditable = s.delayEditable;

    if (s.kind === 'wait') {
      return {
        id: s.id,
        title: s.label,
        type: 'delay' as const,
        delayKey,
        defaultDurationMs,
        sbsPause,
        note,
        delayEditable,
      };
    }
    if (s.kind === 'parallel') {
      const isJoin = s.parallelRole === 'join';
      return {
        id: s.id,
        title: s.label,
        type: (isJoin ? 'join' : 'background') as CycleStep['type'],
        badge: isJoin ? 'join' : 'background',
        note:
          note ??
          (isJoin
            ? undefined
            : 'Paso en paralelo — el material se prepara al volver a inicio.'),
        sbsPause,
      };
    }
    return {
      id: s.id,
      title: s.label,
      type: 'action' as const,
      sbsPause,
      note,
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
  onRefillLongFeed?: () => void;
  onRecoveryReview?: (ok: boolean) => void;
}

export const CycleTab: React.FC<CycleTabProps> = ({
  machineState,
  cycleConfig,
  cycleStep,
  cycleActive,
  cycleFlow,
  onSaveConfig,
  onReloadConfig,
  onSetStepByStep,
  onStart,
  onResume,
  resumeEnabled = false,
  onRefillConfirm,
  onRefillRetry,
  onRefillLongFeed,
  onRecoveryReview,
}) => {
  const { t } = useApp();
  const recoveryStage =
    machineState.refillActive && machineState.refillPrompt
      ? machineState.refillPrompt
      : machineState.recoveryPrompt ||
        (machineState.recoveryAfterError || machineState.e050Lot
          ? machineState.refillPrompt ||
            (machineState.refillActive ? 'working' : '')
          : '');
  const showManualRefill =
    !machineState.recoveryAfterError &&
    !machineState.e050Lot &&
    !!machineState.refillActive &&
    !!machineState.refillPrompt &&
    !!onRefillConfirm;
  const skipCut = !!machineState.refillSkipCut;
  const e050Ask = recoveryStage === 'e050_materialist';
  const trayFull = recoveryStage === 'tray_full';
  const recoveryTitle = trayFull
    ? t('tray_full_title')
    : e050Ask
    ? t('e050_materialist_title')
    : recoveryStage === 'e050_finishing'
      ? t('e050_materialist_finish_title')
    : recoveryStage === 'e050_materialist_wait'
      ? t('e050_materialist_wait_title')
      : recoveryStage === 'abort_decide'
        ? t('recovery_abort_title')
      : recoveryStage === 'pre_purge_decide'
        ? t('recovery_pre_purge_title')
      : recoveryStage === 'review_piece'
        ? t('recovery_review_title')
        : recoveryStage === 'verify_piece'
          ? t('verify_piece_title')
        : recoveryStage === 'purge_decide'
          ? t('recovery_purge_title')
          : recoveryStage === 'continue_cycle'
          ? t('recovery_continue_title')
          : recoveryStage === 'await_feed'
            ? t('refill_confirm_title_await')
            : t('refill_confirm_title_feed');
  const recoveryHint = trayFull
    ? t('tray_full_hint').replace('{n}', String(machineState.trayPieces || ''))
    : e050Ask
    ? machineState.e050FinishPiece
      ? `${t('e050_materialist_hint')} ${t('lot_recover_hint_e050_finish')}`
      : t('e050_materialist_hint')
    : recoveryStage === 'e050_finishing'
      ? t('e050_materialist_finish_hint')
    : recoveryStage === 'e050_materialist_wait'
      ? t('e050_materialist_wait_hint')
      : recoveryStage === 'abort_decide'
      ? t('recovery_abort_hint')
      : recoveryStage === 'pre_purge_decide'
      ? t('recovery_pre_purge_hint')
      : recoveryStage === 'review_piece'
      ? t('recovery_review_hint')
      : recoveryStage === 'verify_piece'
        ? t('verify_piece_hint')
      : recoveryStage === 'purge_decide'
        ? t('recovery_purge_hint')
        : recoveryStage === 'continue_cycle'
        ? t('recovery_continue_hint')
        : recoveryStage === 'await_feed'
          ? skipCut
            ? t('refill_confirm_hint_await_nocut')
            : t('refill_confirm_hint_await')
          : skipCut
            ? t('refill_confirm_hint_feed_nocut')
            : t('refill_confirm_hint_feed');
  // Refill manual: solo banner sky. Amber = recovery/E050/tray.
  const showRecoveryActions =
    !showManualRefill &&
    (recoveryStage === 'await_feed' ||
      recoveryStage === 'after_feed' ||
      (e050Ask && !!machineState.recoveryAwaitingConfirm) ||
      recoveryStage === 'e050_finishing' ||
      recoveryStage === 'e050_materialist_wait' ||
      recoveryStage === 'abort_decide' ||
      recoveryStage === 'pre_purge_decide' ||
      recoveryStage === 'review_piece' ||
      recoveryStage === 'verify_piece' ||
      recoveryStage === 'purge_decide' ||
      recoveryStage === 'continue_cycle' ||
      trayFull);

  const [config, setConfig] = useState<CycleConfig>(cycleConfig || DEFAULT_CYCLE_CONFIG);
  const [configDirty, setConfigDirty] = useState(false);
  const saveTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const pendingSaveRef = useRef<CycleConfig | null>(null);
  const savingRef = useRef(false);
  /** Evita que un SSE/poll viejo con LR pise L/R recién guardado. */
  const confirmedFeedSidesRef = useRef<'L' | 'R' | 'LR' | null>(null);

  const backendHasPrefetch = useMemo(
    () =>
      (cycleFlow || []).some(
        (s) =>
          s.key === 'prefetch_start' ||
          String(s.label || '')
            .toLowerCase()
            .includes('prefetch'),
      ),
    [cycleFlow],
  );
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
      if (saveTimerRef.current) {
        clearTimeout(saveTimerRef.current);
        saveTimerRef.current = null;
      }
      // Al desmontar (cambio de tab): no perder edits pendientes del debounce.
      const pending = pendingSaveRef.current;
      if (pending && !savingRef.current) {
        pendingSaveRef.current = null;
        void onSaveConfig(pending).catch(() => {});
      }
    };
  }, [onSaveConfig]);

  const [toastMessage, setToastMessage] = useState<string | null>(null);
  const [filterType, setFilterType] = useState<'all' | 'delay'>('all');
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
        const pfTriggerEnabled =
          typeof saved.pfTriggerEnabled === 'boolean'
            ? saved.pfTriggerEnabled
            : next.pfTriggerEnabled;
        const merged = { ...saved, feedSides, pfTriggerEnabled };
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

  const workBlocked = !!(
    machineState.errorActive ||
    machineState.fault ||
    machineState.workBlocked
  );
  /** Pausado en paso a paso → Resume ejecuta el siguiente paso real. */
  const showStepNext =
    stepModeActive &&
    cycleActive &&
    machineState.isPaused &&
    resumeEnabled &&
    !workBlocked;
  /** Idle en paso a paso → Siguiente arranca el lote (no solo cambia el highlight). */
  const canStartStepRun =
    stepModeActive && !cycleActive && Boolean(onStart) && !workBlocked;
  const canExecuteNext = (showStepNext || canStartStepRun) && !machineState.purgeBusy;

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

  /** Delays fijados por contrato PLC/Motion — no editables en Cycle. */
  const LOCKED_DELAY_KEYS = new Set<keyof CycleConfig>(['cutterPulseMs']);

  const handleUpdateDelay = (delayKey?: keyof CycleConfig, value?: number) => {
    if (!delayKey || value === undefined) return;
    if (LOCKED_DELAY_KEYS.has(delayKey)) return;
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
  const liveCtSec = machineState.cycleTimeSec ?? 0;
  const lastPieceSec = machineState.lastPieceSec ?? 0;
  const avgPieceSec = machineState.avgPieceSec ?? 0;
  const showLiveCt = cycleActive || liveCtSec > 0 || lastPieceSec > 0;

  const filteredSteps = sequenceSteps.filter((step) => {
    if (filterType === 'delay') return step.type === 'delay';
    return true;
  });
  const cfgLabel = `${hmiModuleFieldLabel} mb-1`;

  return (
    <div className="space-y-3">
      {toastMessage && (
        <div className="flex items-center gap-2 rounded-lg bg-emerald-500 text-white px-4 py-2.5 shadow-md">
          <CheckCircle2 className="h-5 w-5 shrink-0" />
          <span className="text-sm font-semibold">{toastMessage}</span>
        </div>
      )}

      {showRecoveryActions && (
        <div className="flex flex-wrap items-center gap-3 rounded-md border border-amber-300 dark:border-amber-700 bg-amber-50 dark:bg-amber-950/50 px-3 py-3 shadow-2xs">
          <div className="min-w-0 flex-1">
            <p className={`${hmiAssistTitle} text-amber-900 dark:text-amber-100`}>
              {recoveryTitle}
            </p>
            <p className={`mt-1 ${hmiAssistHint} text-amber-800/80 dark:text-amber-200/80`}>
              {recoveryHint}
            </p>
          </div>
          {recoveryStage === 'await_feed' && onRefillRetry && (
            <button
              id="btn-cycle-recovery-retry"
              type="button"
              onClick={onRefillRetry}
              disabled={!machineState.refillAwaitingConfirm}
              className={hmiModuleBtnWarning}
            >
              <RotateCcw className="h-3.5 w-3.5" />
              {t('btn_refill_confirm_retry')}
            </button>
          )}
          {(recoveryStage === 'after_feed' || recoveryStage === 'await_feed') &&
            onRefillConfirm && (
            <button
              id="btn-cycle-recovery-next-cut"
              type="button"
              onClick={() => onRefillConfirm(true)}
              disabled={!machineState.refillAwaitingConfirm}
              className={hmiModuleBtnSuccess}
            >
              <Check className="h-3.5 w-3.5" />
              {t('btn_refill_confirm_next_cut')}
            </button>
          )}
          {e050Ask && machineState.recoveryAwaitingConfirm && onRecoveryReview && (
            <>
              <button
                id="btn-cycle-e050-materialist-no"
                type="button"
                onClick={() => onRecoveryReview(false)}
                disabled={!machineState.recoveryAwaitingConfirm}
                className={hmiModuleBtnSecondary}
              >
                <X className="h-3.5 w-3.5" />
                {t('btn_e050_no_materialist')}
              </button>
              <button
                id="btn-cycle-e050-materialist-yes"
                type="button"
                onClick={() => onRecoveryReview(true)}
                disabled={!machineState.recoveryAwaitingConfirm}
                className={hmiModuleBtnSuccess}
              >
                <Check className="h-3.5 w-3.5" />
                {t('btn_e050_yes_materialist')}
              </button>
            </>
          )}
          {recoveryStage === 'abort_decide' && onRecoveryReview && (
            <>
              <button
                id="btn-cycle-recovery-abort"
                type="button"
                onClick={() => onRecoveryReview(false)}
                disabled={!machineState.recoveryAwaitingConfirm}
                className={hmiModuleBtnDanger}
              >
                <X className="h-3.5 w-3.5" />
                {t('btn_recovery_abort')}
              </button>
              <button
                id="btn-cycle-recovery-abort-continue"
                type="button"
                onClick={() => onRecoveryReview(true)}
                disabled={!machineState.recoveryAwaitingConfirm}
                className={hmiModuleBtnSuccess}
              >
                <Check className="h-3.5 w-3.5" />
                {t('btn_recovery_abort_continue')}
              </button>
            </>
          )}
          {(recoveryStage === 'purge_decide' ||
            recoveryStage === 'pre_purge_decide') &&
            onRecoveryReview && (
            <>
              <button
                id="btn-cycle-recovery-purge-yes"
                type="button"
                onClick={() => onRecoveryReview(true)}
                disabled={!machineState.recoveryAwaitingConfirm}
                className={hmiModuleBtnSuccess}
              >
                <Check className="h-3.5 w-3.5" />
                {t('btn_recovery_purge_yes')}
              </button>
              <button
                id="btn-cycle-recovery-purge-no"
                type="button"
                onClick={() => onRecoveryReview(false)}
                disabled={!machineState.recoveryAwaitingConfirm}
                className={hmiModuleBtnSecondary}
              >
                <X className="h-3.5 w-3.5" />
                {t('btn_recovery_purge_no')}
              </button>
            </>
          )}
          {(recoveryStage === 'review_piece' ||
            recoveryStage === 'verify_piece' ||
            recoveryStage === 'continue_cycle') &&
            onRecoveryReview && (
            <button
              id="btn-cycle-recovery-review-ok"
              type="button"
              onClick={() => onRecoveryReview(true)}
              disabled={!machineState.recoveryAwaitingConfirm}
              className={hmiModuleBtnSuccess}
            >
              <Check className="h-3.5 w-3.5" />
              {recoveryStage === 'continue_cycle'
                ? t('btn_recovery_continue')
                : recoveryStage === 'verify_piece'
                  ? t('btn_verify_piece_continue')
                : t('btn_recovery_review_ok')}
            </button>
          )}
          {trayFull && onRecoveryReview && (
            <button
              id="btn-cycle-tray-emptied"
              type="button"
              onClick={() => onRecoveryReview(true)}
              disabled={!machineState.recoveryAwaitingConfirm}
              className={hmiModuleBtnSuccess}
            >
              <Check className="h-3.5 w-3.5" />
              {t('btn_tray_emptied')}
            </button>
          )}
        </div>
      )}
      {showManualRefill && (
        <div className="flex flex-wrap items-center gap-3 rounded-md border border-sky-300 dark:border-sky-700 bg-sky-50 dark:bg-sky-950/50 px-3 py-3 shadow-2xs">
          <div className="min-w-0 flex-1">
            <p className={`${hmiAssistTitle} text-sky-900 dark:text-sky-100`}>
              {machineState.refillPrompt === 'working'
                ? t('refill_confirm_title_working')
                : machineState.refillPrompt === 'await_feed'
                  ? t('refill_confirm_title_await')
                  : t('refill_confirm_title_feed')}
            </p>
            <p className={`mt-1 ${hmiAssistHint} text-sky-800/80 dark:text-sky-200/80`}>
              {machineState.refillPrompt === 'working'
                ? t('refill_confirm_hint_working')
                : machineState.refillPrompt === 'await_feed'
                  ? t('refill_confirm_hint_await')
                  : t('refill_confirm_hint_feed')}
            </p>
          </div>
          {machineState.refillPrompt === 'await_feed' && onRefillRetry && (
            <button
              id="btn-cycle-refill-retry"
              type="button"
              onClick={onRefillRetry}
              disabled={!machineState.refillAwaitingConfirm}
              className={hmiModuleBtnWarning}
            >
              <RotateCcw className="h-3.5 w-3.5" />
              {t('btn_refill_confirm_retry')}
            </button>
          )}
          {machineState.refillPrompt !== 'working' && (
            <button
              id="btn-cycle-refill-yes"
              type="button"
              onClick={() => onRefillConfirm(true)}
              disabled={!machineState.refillAwaitingConfirm}
              className={hmiModuleBtnSuccess}
            >
              <Check className="h-3.5 w-3.5" />
              {t('btn_refill_confirm_next_cut')}
            </button>
          )}
        </div>
      )}

      {/* SECTION 1: CYCLE SEQUENCE */}
      <div className={hmiPanelCls}>
        <div className={`${hmiPanelHeader} ${hmiPanelPadding} !mb-0 border-slate-200 dark:border-slate-800`}>
          <div>
            <div className="flex flex-wrap items-center gap-2.5">
              <div className="flex items-center gap-2">
                <Sliders className={`${hmiModuleIconSection} text-teal-600 dark:text-teal-400`} />
                <h2 className={`${hmiModuleCenterSectionTitle} text-slate-900 dark:text-slate-100`}>
                  {t('cycle_sequence_title')}
                </h2>
              </div>
              <span className={`${hmiCycleTimeChip} border-slate-200 bg-slate-100 text-slate-700 dark:border-slate-700 dark:bg-slate-800 dark:text-slate-200`}>
                <Clock className="h-4 w-4 text-sky-500 sm:h-5 sm:w-5" />
                <span>Est. delays: <strong className="text-teal-600 dark:text-teal-400">~{estimatedTotalTimeSec}s</strong></span>
              </span>
              {showLiveCt && (
                <span
                  className={`${hmiCycleTimeChip} border-teal-200 bg-teal-50 text-teal-800 dark:border-teal-800 dark:bg-teal-950/40 dark:text-teal-200`}
                  title={t('cycle_time_hint')}
                >
                  <Timer className="h-4 w-4 sm:h-5 sm:w-5" />
                  <span>
                    {t('cycle_time_label')}:{' '}
                    <strong>{liveCtSec}s</strong>
                    {lastPieceSec > 0 ? ` · pz ${lastPieceSec.toFixed(1)}s` : ''}
                    {avgPieceSec > 0 && (machineState.targetPieces || 0) > 1
                      ? ` · avg ${avgPieceSec.toFixed(1)}s`
                      : ''}
                  </span>
                </span>
              )}
            </div>
          </div>

          <div className={hmiModuleBtnGroup}>
            <div className={hmiModuleBtnSegWrap}>
              <button
                type="button"
                onClick={() => setFilterType('all')}
                className={`${hmiModuleBtnSeg} ${filterType === 'all' ? hmiModuleBtnSegActive : hmiModuleBtnSegIdle}`}
              >
                {t('cycle_sequence_title')} ({sequenceSteps.length})
              </button>
              <button
                type="button"
                onClick={() => setFilterType('delay')}
                className={`${hmiModuleBtnSeg} ${
                  filterType === 'delay'
                    ? `${hmiModuleBtnSegActive} text-teal-600 dark:text-teal-400`
                    : hmiModuleBtnSegIdle
                }`}
              >
                {t('cycle_step_filter_delays')}
              </button>
            </div>

            {!stepModeActive ? (
              <button
                id="btn-step-by-step"
                type="button"
                onClick={handleStartStepMode}
                className={hmiModuleBtnTealSoft}
              >
                <Footprints className={hmiModuleIconBtn} />
                <span>{t('btn_step_by_step')}</span>
              </button>
            ) : (
              <div className={hmiBtnToolbarWrap}>
                <button
                  id="btn-step-prev"
                  type="button"
                  onClick={handlePrevStep}
                  disabled={cycleActive || activeStepNum === 1}
                  title={t('btn_prev_step')}
                  className={`${hmiModuleBtnSeg} text-teal-700 hover:bg-teal-500/20 dark:text-teal-300`}
                >
                  <ChevronLeft className={hmiModuleIconBtn} />
                  <span className="hidden sm:inline">{t('btn_prev_step')}</span>
                </button>

                <span className={`${hmiModuleBtnSeg} ${hmiModuleBtnSegActive} font-mono`}>
                  {(cycleActive ? cycleStep : activeStepNum) ?? 1}/{maxStepId}
                </span>

                <button
                  id="btn-step-next"
                  type="button"
                  onClick={handleNextStep}
                  disabled={!canExecuteNext}
                  title={
                    machineState.purgeBusy
                      ? t('purge_busy_locked')
                      : showStepNext
                      ? t('btn_next_step')
                      : canStartStepRun
                      ? t('btn_next_step_start')
                      : t('btn_next_step')
                  }
                  className={hmiModuleBtnPrimary}
                >
                  <span>{t('btn_next_step')}</span>
                  <ChevronRight className={hmiModuleIconBtn} />
                </button>

                <button
                  id="btn-step-exit"
                  type="button"
                  onClick={handleExitStepMode}
                  title={t('btn_exit_step_mode')}
                  className={`${hmiModuleBtnSeg} text-slate-400 hover:bg-rose-500/20 hover:text-rose-500`}
                >
                  <X className={hmiModuleIconBtn} />
                </button>
              </div>
            )}

            <button
              type="button"
              onClick={handleSave}
              title={t('btn_save')}
              className={hmiModuleBtnPrimary}
            >
              <Save className={hmiModuleIconBtn} />
              <span>{t('btn_save')}</span>
            </button>
          </div>
        </div>

        {backendHasPrefetch && (
          <div className="mx-3 mt-3 flex items-center gap-2 rounded-md border border-rose-300/80 bg-rose-50 px-3 py-2 text-sm text-rose-900 dark:border-rose-700/60 dark:bg-rose-950/40 dark:text-rose-200 sm:mx-4">
            <ShieldAlert className="h-4 w-4 shrink-0" />
            <span>{t('cycle_stale_prefetch_warning')}</span>
          </div>
        )}
        {configDirty && (
          <div className="mx-3 mt-3 flex items-center gap-2 rounded-md border border-amber-300/80 bg-amber-50 px-3 py-2 text-sm text-amber-900 dark:border-amber-700/60 dark:bg-amber-950/40 dark:text-amber-200 sm:mx-4">
            <ShieldAlert className="h-4 w-4 shrink-0" />
            <span>{t('cycle_unsaved_warning')}</span>
          </div>
        )}

        <div className="max-h-[min(520px,52vh)] overflow-y-auto space-y-1 p-3 sm:p-4">
          {filteredSteps.map((step) => {
            const isCurrent = currentRunningStep === step.id;
            const stepDelayVal = step.delayKey
              ? Number(config[step.delayKey])
              : Number(step.defaultDurationMs ?? 0);
            const delayMs = Number.isFinite(stepDelayVal) ? stepDelayVal : 0;

            return (
              <div
                key={step.id}
                className={`${hmiCycleStepRow} ${
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
                <div className="flex items-start sm:items-center gap-3 sm:gap-4 min-w-0 flex-1">
                  {step.type === 'action' && (
                    <div className={`${hmiCycleStepIcon} ${
                      isCurrent
                        ? 'bg-teal-500 text-white border-teal-600 ring-2 ring-teal-300'
                        : 'border-teal-500/40 text-teal-600 dark:text-teal-400 bg-teal-50/50 dark:bg-teal-950/30'
                    }`}>
                      {step.id}
                    </div>
                  )}

                  {step.type === 'delay' && (
                    <div className={`${hmiCycleStepIcon} rounded-md ${
                      isCurrent
                        ? 'bg-teal-500 text-white ring-2 ring-teal-300 border-transparent'
                        : 'text-teal-600 dark:text-teal-400 bg-teal-50/80 dark:bg-teal-950/40 border-teal-500/30'
                    }`}>
                      <Timer className="h-5 w-5 sm:h-6 sm:w-6" />
                    </div>
                  )}

                  {step.type === 'background' && (
                    <div className={`${hmiCycleStepIcon} rounded-md bg-amber-100 dark:bg-amber-950 text-amber-700 dark:text-amber-400 border-amber-300 dark:border-amber-800`}>
                      ||
                    </div>
                  )}

                  {step.type === 'join' && (
                    <div className={`${hmiCycleStepIcon} rounded-md bg-teal-100 dark:bg-teal-950 text-teal-700 dark:text-teal-400 border-teal-300 dark:border-teal-800`}>
                      ||
                    </div>
                  )}

                  <span className="font-mono text-sm font-semibold text-slate-400 dark:text-slate-500 min-w-[1.75rem] sm:text-base">
                    {step.id}
                  </span>

                  <div className="min-w-0 flex-1">
                    <div className="flex items-center gap-2.5 flex-wrap">
                      <span className={`${hmiCycleStepTitle} ${
                        isCurrent
                          ? 'font-bold text-teal-900 dark:text-teal-100'
                          : step.type === 'delay'
                          ? 'text-slate-700 dark:text-slate-300'
                          : 'text-slate-800 dark:text-slate-200'
                      }`}>
                        {step.title}
                      </span>

                      {step.badge === 'background' && (
                        <span className={`${hmiCycleStepBadge} bg-amber-500/20 text-amber-700 dark:text-amber-400 border-amber-500/30`}>
                          {t('badge_background')}
                        </span>
                      )}

                      {step.badge === 'join' && (
                        <span className={`${hmiCycleStepBadge} bg-teal-500/20 text-teal-700 dark:text-teal-400 border-teal-500/30`}>
                          {t('badge_join')}
                        </span>
                      )}

                      {stepModeActive && step.sbsPause && (
                        <span className={`${hmiCycleStepBadge} bg-teal-600/15 text-teal-800 dark:text-teal-300 border-teal-500/25`}>
                          {t('step_sbs_checkpoint')}
                        </span>
                      )}

                      {stepModeActive && !step.sbsPause && (
                        <span className={`${hmiCycleStepBadge} bg-slate-500/10 text-slate-500 dark:text-slate-400 border-slate-400/20`}>
                          {t('step_sbs_auto')}
                        </span>
                      )}

                      {isCurrent && (
                        <span className={`${hmiCycleStepBadge} bg-teal-500 text-white border-teal-600 animate-pulse normal-case tracking-normal`}>
                          {t('cycle_step_running')}
                        </span>
                      )}
                    </div>

                    {step.note && (
                      <p className={`${hmiCycleStepNote} italic`}>
                        • {step.note}
                      </p>
                    )}
                  </div>
                </div>

                <div className="flex items-center gap-2 shrink-0 ml-3">
                  {step.type === 'delay' && step.delayKey && (
                    step.delayEditable === false ||
                    (step.delayKey && LOCKED_DELAY_KEYS.has(step.delayKey)) ? (
                      <div
                        className="flex items-center gap-1.5 rounded-lg border border-slate-200 dark:border-slate-700 bg-slate-100 dark:bg-slate-800/80 px-3 py-2 shadow-2xs"
                        title={t('cycle_step_fixed_delay_hint')}
                      >
                        <span className="font-mono font-bold text-sm text-slate-600 dark:text-slate-300 sm:text-base">
                          {Math.max(150, delayMs)}
                        </span>
                        <span className="text-sm font-mono text-slate-400 dark:text-slate-500 select-none">
                          ms
                        </span>
                      </div>
                    ) : (
                    <div className="flex items-center gap-1 bg-white dark:bg-slate-800 border border-slate-300 dark:border-slate-700 hover:border-teal-500 dark:hover:border-teal-500 rounded-lg p-1 shadow-2xs transition">
                      <button
                        type="button"
                        onClick={() => handleUpdateDelay(step.delayKey, Math.max(0, delayMs - 25))}
                        title="Restar 25ms"
                        className="flex h-8 w-8 items-center justify-center rounded hover:bg-slate-100 dark:hover:bg-slate-700 text-slate-500 hover:text-slate-900 dark:text-slate-400 dark:hover:text-white transition active:scale-95 cursor-pointer sm:h-9 sm:w-9"
                      >
                        <Minus className="h-4 w-4" />
                      </button>

                      <div className="flex items-center gap-1 px-1">
                        <input
                          id={`input-delay-step-${step.id}`}
                          type="number"
                          min={0}
                          step={10}
                          value={Number.isFinite(delayMs) ? delayMs : ''}
                          onChange={(e) => handleUpdateDelay(step.delayKey, Number(e.target.value))}
                          className="w-20 sm:w-24 bg-transparent text-center font-mono font-bold text-base text-teal-600 dark:text-teal-400 focus:outline-hidden focus:ring-1 focus:ring-teal-500 rounded sm:text-lg"
                        />
                        <span className="text-sm font-mono text-slate-400 dark:text-slate-500 select-none">
                          ms
                        </span>
                      </div>

                      <button
                        type="button"
                        onClick={() => handleUpdateDelay(step.delayKey, delayMs + 25)}
                        title="Sumar 25ms"
                        className="flex h-8 w-8 items-center justify-center rounded hover:bg-slate-100 dark:hover:bg-slate-700 text-slate-500 hover:text-slate-900 dark:text-slate-400 dark:hover:text-white transition active:scale-95 cursor-pointer sm:h-9 sm:w-9"
                      >
                        <Plus className="h-4 w-4" />
                      </button>
                    </div>
                    )
                  )}
                </div>
              </div>
            );
          })}
        </div>
      </div>

      <div className={`${hmiPanelCls} space-y-3 p-3 sm:p-4`}>
        <div>
          <div className="mb-1.5 flex items-center gap-1.5">
            <Droplets className="h-3.5 w-3.5 text-sky-600 dark:text-sky-400" />
            <h3 className={hmiSectionTitleMb}>
              {t('refill_helpers_title')}
            </h3>
          </div>
          <div className={hmiCfgGrid}>
            <label className="min-w-0">
              <span className={cfgLabel}>{t('cfg_refill_mm')}</span>
              <input
                type="number"
                min={1}
                step={0.5}
                value={config.refillMm ?? 55}
                onChange={(e) => updateConfigField('refillMm', Number(e.target.value))}
                className={hmiModuleCompactFieldInput}
              />
            </label>
            <label className="min-w-0">
              <span className={cfgLabel}>{t('cfg_refill_asda')}</span>
              <input
                type="number"
                step={1}
                value={config.refillAsdaMm ?? -300}
                onChange={(e) => updateConfigField('refillAsdaMm', Number(e.target.value))}
                className={hmiModuleCompactFieldInput}
              />
            </label>
          </div>
        </div>

        <div className="border-t border-slate-100 dark:border-slate-800 pt-2.5">
          <div className="mb-1.5 flex items-center gap-1.5">
            <Boxes className="h-3.5 w-3.5 text-teal-600 dark:text-teal-400" />
            <h3 className={hmiSectionTitleMb}>
              {t('material_handling_title')}
            </h3>
          </div>
          <div className={hmiCfgGrid}>
            <div className="min-w-0">
              <span className={cfgLabel}>{t('cfg_feed_sides')}</span>
              <div className="flex items-center gap-1">
                {(['L', 'R', 'LR'] as const).map((side) => (
                  <button
                    key={side}
                    type="button"
                    onClick={() => handleFeedSides(side)}
                    className={`${hmiModuleBtnChip} ${
                      config.feedSides === side ? hmiModuleBtnChipOn : hmiModuleBtnChipOff
                    }`}
                  >
                    {side === 'LR' ? t('cfg_feed_sides_both') : side}
                  </button>
                ))}
              </div>
            </div>
            <div className="min-w-0">
              <span className={cfgLabel}>{t('cfg_pf_trigger')}</span>
              <div className="flex items-center gap-1">
                {([true, false] as const).map((on) => (
                  <button
                    key={on ? 'on' : 'off'}
                    type="button"
                    onClick={() => updateConfigField('pfTriggerEnabled', on)}
                    className={`${hmiModuleBtnChip} ${
                      (config.pfTriggerEnabled !== false) === on
                        ? hmiModuleBtnChipOn
                        : hmiModuleBtnChipOff
                    }`}
                  >
                    {on ? t('cfg_pf_trigger_on') : t('cfg_pf_trigger_off')}
                  </button>
                ))}
              </div>
            </div>
            <label className="min-w-0">
              <span className={cfgLabel}>{t('cfg_deposito_batch_size')}</span>
              <input
                type="number"
                min={1}
                value={config.depositBatchSize}
                onChange={(e) => updateConfigField('depositBatchSize', Number(e.target.value))}
                className={hmiModuleCompactFieldInput}
              />
            </label>
            <label className="min-w-0">
              <span className={cfgLabel}>{t('cfg_deposito_extra')}</span>
              <input
                type="number"
                step={0.1}
                value={config.depositExtraMm}
                onChange={(e) => updateConfigField('depositExtraMm', Number(e.target.value))}
                className={hmiModuleCompactFieldInput}
              />
            </label>
            <label className="min-w-0">
              <span className={cfgLabel}>{t('cfg_deposito_stack_gap')}</span>
              <input
                type="number"
                step={0.1}
                min={0}
                value={config.depositStackGapMm ?? 20}
                onChange={(e) => updateConfigField('depositStackGapMm', Number(e.target.value))}
                className={hmiModuleCompactFieldInput}
              />
            </label>
            <label className="min-w-0">
              <span className={cfgLabel}>{t('cfg_deposito_max_travel')}</span>
              <input
                type="number"
                step={1}
                min={1}
                value={config.depositMaxTravelMm ?? 1500}
                onChange={(e) => updateConfigField('depositMaxTravelMm', Number(e.target.value))}
                className={hmiModuleCompactFieldInput}
              />
            </label>
            <label className="min-w-0">
              <span className={cfgLabel}>{t('cfg_gripper_clearance')}</span>
              <input
                type="number"
                step={0.1}
                min={0}
                value={config.gripperClearanceMm ?? 0}
                onChange={(e) => updateConfigField('gripperClearanceMm', Number(e.target.value))}
                className={hmiModuleCompactFieldInput}
              />
            </label>
            <label className="min-w-0">
              <span className={cfgLabel}>{t('cfg_cut_offset')}</span>
              <input
                type="number"
                step={0.1}
                value={config.cutOffsetMm ?? 0}
                onChange={(e) => updateConfigField('cutOffsetMm', Number(e.target.value))}
                className={hmiModuleCompactFieldInput}
              />
            </label>
            <label className="min-w-0">
              <span className={cfgLabel}>{t('cfg_wip_blower_inicio_offset')}</span>
              <input
                type="number"
                step={0.1}
                value={config.wipBlowerInicioOffsetMm ?? 0}
                onChange={(e) =>
                  updateConfigField('wipBlowerInicioOffsetMm', Number(e.target.value))
                }
                className={hmiModuleCompactFieldInput}
              />
            </label>
          </div>
        </div>

        <div className="border-t border-slate-100 dark:border-slate-800 pt-2.5">
          <div className="mb-1.5 flex items-center gap-1.5">
            <ShieldAlert className="h-3.5 w-3.5 text-teal-600 dark:text-teal-400" />
            <h3 className={hmiSectionTitleMb}>
              {t('timeouts_title')}
            </h3>
          </div>
          <div className={hmiCfgGrid}>
            <label className="min-w-0">
              <span className={cfgLabel}>{t('cfg_timeout_piece')}</span>
              <input
                type="number"
                min={3}
                value={config.pieceWatchTimeoutS ?? 20}
                onChange={(e) => updateConfigField('pieceWatchTimeoutS', Number(e.target.value))}
                className={hmiModuleCompactFieldInput}
              />
            </label>
            <label className="min-w-0">
              <span className={cfgLabel}>{t('cfg_timeout_motion')}</span>
              <input
                type="number"
                min={1}
                value={config.motionWaitTimeoutS}
                onChange={(e) => updateConfigField('motionWaitTimeoutS', Number(e.target.value))}
                className={hmiModuleCompactFieldInput}
              />
            </label>
            <label className="min-w-0">
              <span className={cfgLabel}>{t('cfg_timeout_feed')}</span>
              <input
                type="number"
                min={1}
                value={config.feedWaitTimeoutS}
                onChange={(e) => updateConfigField('feedWaitTimeoutS', Number(e.target.value))}
                className={hmiModuleCompactFieldInput}
              />
            </label>
            <label className="min-w-0">
              <span className={cfgLabel}>{t('cfg_timeout_pf_ready')}</span>
              <input
                type="number"
                min={1}
                value={config.pfReadyTimeoutS}
                onChange={(e) => updateConfigField('pfReadyTimeoutS', Number(e.target.value))}
                className={hmiModuleCompactFieldInput}
              />
            </label>
          </div>
        </div>
      </div>

      <div className={hmiModuleBtnGroup}>
        <button
          id="btn-save-cycle-config"
          type="button"
          onClick={handleSave}
          className={hmiModuleBtnPrimary}
        >
          <Save className={hmiModuleIconBtn} />
          <span>{t('btn_save')}</span>
        </button>

        <button
          id="btn-reload-cycle-config"
          type="button"
          onClick={handleReload}
          className={hmiModuleBtnSecondary}
        >
          <RotateCcw className={hmiModuleIconBtn} />
          <span>{t('btn_reload')}</span>
        </button>
      </div>
    </div>
  );
};
