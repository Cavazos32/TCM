import React, { useState, useEffect, useRef } from 'react';
import {
  Play,
  Square,
  RotateCcw,
  Sliders,
  Clock,
  Pause,
  Layers,
  Droplets,
  Check,
  X,
  Home,
  Zap,
  Package,
  MessageSquare,
  Info,
} from 'lucide-react';
import { MachineState, LogEntry, MotionState, PfRefillChannel, PlcState, PreFeederState } from '../types';
import { LogTerminal } from './LogTerminal';
import { useApp } from '../context/AppContext';
import { isGenericErrorText } from '../api/mappers';

type FaultModuleKind = 'motion' | 'plc' | 'prefeeder' | 'other';

function isPrefeederFaultModule(module?: string): boolean {
  const m = (module || '').toLowerCase();
  return (
    m.includes('pre-feeder') ||
    m.includes('prefeeder') ||
    m === 'pf' ||
    m.startsWith('pf-') ||
    m.startsWith('pf ')
  );
}

function faultModuleKind(module?: string): FaultModuleKind {
  const m = (module || '').toLowerCase();
  if (m.includes('motion')) return 'motion';
  if (m.includes('plc')) return 'plc';
  if (isPrefeederFaultModule(module)) return 'prefeeder';
  return 'other';
}

interface MaquinaTabProps {
  machineState: MachineState;
  motionState: MotionState;
  plcState: PlcState;
  preFeederState: PreFeederState;
  models: { name: string; mm?: number; rpm?: number; qty?: number }[];
  selectedModelIndex: number;
  feedSides?: 'L' | 'R' | 'LR';
  resumeEnabled?: boolean;
  onModelSelect: (index: number) => void;
  onTargetPiecesChange: (target: number) => void;
  onCutOffsetSave: (mm: number) => void;
  onStart: () => void;
  onStop: () => void;
  onResume: () => void;
  onPause: () => void;
  onReset: () => void;
  onMachineHome?: () => void | Promise<void>;
  onRefill?: () => void;
  onRefillConfirm?: (ok: boolean) => void;
  onRefillRetry?: () => void;
  onRefillLongFeed?: () => void;
  onRecoveryReview?: (ok: boolean) => void;
  onPfStart?: () => void;
  onPfStop?: () => void;
  onPfReset?: () => void;
  onPfRefill?: (side: 'L' | 'R', channel: PfRefillChannel, on: boolean) => void;
  onMaterialist?: () => void;
  purgeHandsWarning?: boolean;
  asdaMoveWarning?: boolean;
  showLogs?: boolean;
  logs: LogEntry[];
  onClearLogs: () => void;
}

export const MaquinaTab: React.FC<MaquinaTabProps> = ({
  machineState,
  motionState,
  plcState,
  preFeederState,
  models,
  selectedModelIndex,
  feedSides = 'LR',
  resumeEnabled = false,
  onModelSelect,
  onTargetPiecesChange,
  onCutOffsetSave,
  onStart,
  onStop,
  onResume,
  onPause,
  onReset,
  onMachineHome,
  onRefill,
  onRefillConfirm,
  onRefillRetry,
  onRecoveryReview,
  onPfStart,
  onPfRefill,
  onMaterialist,
  purgeHandsWarning = false,
  asdaMoveWarning = false,
  showLogs = true,
  logs,
  onClearLogs,
}) => {
  const { t } = useApp();
  const [offsetInput, setOffsetInput] = useState(String(machineState.offsetMm ?? 0));
  const offsetDirtyRef = useRef(false);
  const offsetSaveTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const [btnFlash, setBtnFlash] = useState({ stop: false, reset: false, home: false });
  const btnFlashTimers = useRef<Partial<Record<'stop' | 'reset', ReturnType<typeof setTimeout>>>>({});

  const pulseBtn = (key: 'stop' | 'reset') => {
    setBtnFlash((prev) => ({ ...prev, [key]: true }));
    if (btnFlashTimers.current[key]) clearTimeout(btnFlashTimers.current[key]);
    btnFlashTimers.current[key] = setTimeout(() => {
      setBtnFlash((prev) => ({ ...prev, [key]: false }));
    }, 1400);
  };

  useEffect(() => {
    if (offsetDirtyRef.current) return;
    setOffsetInput(String(machineState.offsetMm ?? 0));
  }, [machineState.offsetMm]);

  useEffect(() => {
    return () => {
      if (offsetSaveTimerRef.current) clearTimeout(offsetSaveTimerRef.current);
      Object.values(btnFlashTimers.current).forEach((id) => {
        if (id) clearTimeout(id);
      });
    };
  }, []);

  const persistCutOffset = (raw: string) => {
    const n = parseFloat(raw);
    if (isNaN(n)) return;
    offsetDirtyRef.current = false;
    onCutOffsetSave(n);
  };

  const scheduleCutOffsetSave = (raw: string) => {
    offsetDirtyRef.current = true;
    if (offsetSaveTimerRef.current) clearTimeout(offsetSaveTimerRef.current);
    offsetSaveTimerRef.current = setTimeout(() => persistCutOffset(raw), 400);
  };

  const flushCutOffsetSave = (raw: string) => {
    if (offsetSaveTimerRef.current) {
      clearTimeout(offsetSaveTimerRef.current);
      offsetSaveTimerRef.current = null;
    }
    persistCutOffset(raw);
  };

  const target = machineState.targetPieces > 0 ? machineState.targetPieces : 1;
  const progressPercentage = Math.min(
    100,
    Math.max(0, Math.round((machineState.piecesCount / target) * 100))
  );

  const stepProgress =
    machineState.cycleActive
      ? machineState.progress
      : machineState.cycleCompleted
      ? 100
      : progressPercentage;

  const elapsedSec = machineState.cycleTimeSec ?? 0;
  const lastPieceSec = machineState.lastPieceSec ?? 0;
  const etaSec =
    machineState.cycleActive && stepProgress > 0 && stepProgress < 100
      ? Math.round((elapsedSec / stepProgress) * (100 - stepProgress))
      : 0;

  const fmtSec = (sec: number) => {
    const s = Math.max(0, Math.round(sec));
    return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
  };

  const hasFault = !!(
    machineState.errorActive ||
    machineState.fault ||
    machineState.workBlocked
  );
  const modKind = faultModuleKind(machineState.faultModule);
  const motionLatched = hasFault && modKind === 'motion';
  const plcLatched = hasFault && modKind === 'plc';
  const pfLatched = hasFault && modKind === 'prefeeder';
  const faultLabel =
    machineState.fault ||
    (machineState.faultCode
      ? `${machineState.faultCode}${
          machineState.faultDescription
            ? `: ${machineState.faultModule || '—'}, ${machineState.faultDescription}`
            : ''
        }`
      : hasFault
        ? plcState.statusText || machineState.statusText || ''
        : '');
  const faultQueue = machineState.faultQueue || [];
  const rawFaultUi = faultQueue[0]?.ui || faultLabel;
  const currentFaultUi = isGenericErrorText(rawFaultUi) ? '' : rawFaultUi;
  const extraModuleFaults =
    (!motionLatched && motionState.hasError && motionState.connection.connected ? 1 : 0) +
    (!plcLatched && plcState.hasError && plcState.connection.connected ? 1 : 0) +
    (!pfLatched && preFeederState.hasError && preFeederState.connection.connected ? 1 : 0);
  const faultCount =
    faultQueue.length > 0
      ? faultQueue.length
      : hasFault
        ? 1 + extraModuleFaults
        : 0;
  const interlockError = !hasFault && machineState.statusKind === 'error';
  const generalStatus = hasFault
    ? currentFaultUi || t('state_error')
    : interlockError
      ? machineState.statusText || t('err_materialist_start')
      : machineState.isPaused
      ? t('state_paused')
      : machineState.isRunning
        ? t('state_producing')
        : machineState.cycleMaterialist
          ? t('module_status_materialist')
          : machineState.statusText || t('state_ready');

  const statusSubtitle = hasFault || interlockError
    ? ''
    : machineState.isRunning
      ? t('state_producing')
      : machineState.isPaused
        ? t('state_paused')
        : t('machine_standby');

  const btnBase =
    'flex items-center justify-center gap-2 rounded-lg border font-bold transition shadow-2xs active:scale-[0.98] w-full';
  const btnPrimary = 'px-4 py-4 text-base';
  const btnSecondary = 'px-3 py-3 text-sm';
  const btnIdle =
    'border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 text-slate-700 dark:text-slate-200 hover:bg-slate-100 dark:hover:bg-slate-700';
  const btnOn =
    'border-slate-900 dark:border-white bg-slate-800 dark:bg-slate-100 text-white dark:text-slate-900 ring-2 ring-slate-400/80 ring-offset-1 dark:ring-offset-slate-900';
  const btnNeed =
    'border-amber-400 bg-amber-100 dark:bg-amber-950/60 text-amber-950 dark:text-amber-100 ring-2 ring-amber-300 ring-offset-1 animate-pulse';
  const btnNeedGo =
    'border-emerald-500 bg-emerald-600 hover:bg-emerald-700 text-white ring-2 ring-emerald-300 ring-offset-1 animate-pulse';
  const btnStartIdle =
    'border-emerald-300 dark:border-emerald-800 bg-emerald-50 dark:bg-emerald-950/40 text-emerald-800 dark:text-emerald-200 hover:bg-emerald-100 dark:hover:bg-emerald-900/50';
  const btnStartOn =
    'border-emerald-600 bg-emerald-600 text-white [&_svg]:text-white ring-2 ring-emerald-300 ring-offset-1 dark:ring-offset-slate-900 shadow-md';
  const btnStopIdle =
    'border-red-300 dark:border-red-900/60 bg-red-50 dark:bg-red-950/40 text-red-700 dark:text-red-300 hover:bg-red-100 dark:hover:bg-red-900/40';
  const btnStopOn =
    'border-red-600 bg-red-600 text-white [&_svg]:text-white ring-2 ring-red-300 ring-offset-1 dark:ring-offset-slate-900 shadow-md';
  const btnResetIdle =
    'border-sky-300 dark:border-sky-800 bg-sky-50 dark:bg-sky-950/40 text-sky-800 dark:text-sky-200 hover:bg-sky-100 dark:hover:bg-sky-900/50';
  const btnResetOn =
    'border-sky-600 bg-sky-600 text-white [&_svg]:text-white ring-2 ring-sky-300 ring-offset-1 dark:ring-offset-slate-900 shadow-md';
  const btnPauseIdle =
    'border-amber-300 dark:border-amber-800 bg-amber-50 dark:bg-amber-950/40 text-amber-900 dark:text-amber-100 hover:bg-amber-100 dark:hover:bg-amber-900/50';
  const btnPauseOn =
    'border-amber-500 bg-amber-500 text-white [&_svg]:text-white ring-2 ring-amber-300 ring-offset-1 dark:ring-offset-slate-900 shadow-md';
  const btnAuxIdle =
    'border-sky-300 dark:border-sky-800 bg-sky-50 dark:bg-sky-950/40 text-sky-800 dark:text-sky-200 hover:bg-sky-100 dark:hover:bg-sky-900/50';
  const btnAuxOn =
    'border-sky-600 bg-sky-600 text-white [&_svg]:text-white ring-2 ring-sky-300 ring-offset-1 dark:ring-offset-slate-900 shadow-md';

  const pfConnected = preFeederState.connection.connected;
  const pfHasError = !!preFeederState.hasError && pfConnected;
  const recoveryStage =
    machineState.refillActive && machineState.refillPrompt
      ? machineState.refillPrompt
      : machineState.recoveryPrompt ||
        (machineState.recoveryAfterError || machineState.e050Lot
          ? machineState.refillPrompt ||
            (machineState.refillActive ? 'working' : '')
          : '');
  const trayFull = recoveryStage === 'tray_full';
  const showRecoveryTrack = !!recoveryStage && !trayFull;
  const e050Lot = !!machineState.e050Lot;
  const skipCut = !!machineState.refillSkipCut;

  const processStep: string = e050Lot
    ? recoveryStage === 'e050_materialist'
      ? 'ask'
      : recoveryStage === 'e050_finishing'
        ? 'piece'
      : recoveryStage === 'e050_materialist_wait' ||
          ((recoveryStage === 'working' ||
            recoveryStage === 'await_feed' ||
            recoveryStage === 'after_feed') &&
            skipCut)
        ? 'empty'
        : recoveryStage === 'review_piece' || recoveryStage === 'verify_piece'
          ? 'review'
          : hasFault
            ? 'reset'
            : machineState.e050FinishPiece && machineState.isRunning
              ? 'piece'
              : resumeEnabled || machineState.isPaused
                ? 'resume'
                : ''
    : recoveryStage === 'abort_decide'
      ? 'abort'
    : recoveryStage === 'continue_cycle'
      ? 'continue'
      : recoveryStage === 'purge_decide' ||
          recoveryStage === 'pre_purge_decide'
        ? 'purge'
        : recoveryStage === 'review_piece'
        ? 'review'
        : recoveryStage === 'working' ||
            recoveryStage === 'await_feed' ||
            recoveryStage === 'after_feed'
          ? 'purge'
          : hasFault
            ? 'reset'
            : machineState.recoveryAfterError && machineState.isRunning
              ? 'piece'
              : resumeEnabled || machineState.isPaused
                ? 'resume'
                : '';

  const showErrorProcess =
    hasFault ||
    machineState.recoveryAfterError ||
    e050Lot ||
    showRecoveryTrack;
  const showMachineResetCoach = showErrorProcess && processStep === 'reset';
  const showMachineResumeCoach = showErrorProcess && processStep === 'resume';
  const showManualRefill =
    !machineState.recoveryAfterError &&
    !machineState.e050Lot &&
    !!machineState.refillActive &&
    !!machineState.refillPrompt &&
    !!onRefillConfirm;
  const showRecoveryActions =
    !showManualRefill &&
    (recoveryStage === 'abort_decide' ||
      recoveryStage === 'pre_purge_decide' ||
      recoveryStage === 'purge_decide' ||
      recoveryStage === 'await_feed' ||
      recoveryStage === 'after_feed' ||
      (recoveryStage === 'e050_materialist' &&
        !!machineState.recoveryAwaitingConfirm) ||
      recoveryStage === 'e050_finishing' ||
      recoveryStage === 'e050_materialist_wait' ||
      recoveryStage === 'review_piece' ||
      recoveryStage === 'verify_piece' ||
      recoveryStage === 'continue_cycle' ||
      trayFull);
  const recoveryTitle = trayFull
    ? t('tray_full_title')
    : recoveryStage === 'e050_materialist'
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
          : recoveryStage === 'continue_cycle'
            ? t('recovery_continue_title')
            : recoveryStage === 'purge_decide'
              ? t('recovery_purge_title')
              : recoveryStage === 'working'
                ? t('refill_confirm_title_working')
                : recoveryStage === 'await_feed'
                  ? t('refill_confirm_title_await')
                  : t('refill_confirm_title_feed');
  const recoveryHint = trayFull
    ? t('tray_full_hint').replace('{n}', String(machineState.trayPieces || ''))
    : recoveryStage === 'e050_materialist'
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
            : recoveryStage === 'continue_cycle'
              ? t('recovery_continue_hint')
              : recoveryStage === 'purge_decide'
                ? t('recovery_purge_hint')
                : recoveryStage === 'working'
                  ? t('refill_confirm_hint_working')
                  : recoveryStage === 'await_feed'
                    ? skipCut
                      ? t('refill_confirm_hint_await_nocut')
                      : t('refill_confirm_hint_await')
                    : skipCut
                      ? t('refill_confirm_hint_feed_nocut')
                      : t('refill_confirm_hint_feed');

  const processHint = e050Lot
    ? processStep === 'ask'
      ? machineState.e050FinishPiece
        ? t('lot_recover_hint_e050_finish')
        : t('lot_recover_hint_e050_ask')
      : processStep === 'reset'
        ? t('lot_recover_hint_e050_reset')
        : processStep === 'resume'
          ? t('lot_recover_hint_e050_resume')
          : processStep === 'piece'
            ? t('lot_recover_hint_piece')
            : processStep === 'review'
              ? t('recovery_review_hint')
              : processStep === 'empty'
                ? recoveryHint
                : t('lot_recover_hint_e050_reset')
    : processStep === 'abort'
      ? t('recovery_abort_hint')
    : processStep === 'reset'
      ? t('lot_recover_hint_reset')
      : processStep === 'resume'
        ? t('lot_recover_hint_resume')
        : processStep === 'piece'
          ? t('lot_recover_hint_piece')
          : processStep === 'review'
            ? t('recovery_review_hint')
            : processStep === 'continue'
              ? t('recovery_continue_hint')
              : processStep === 'purge'
                ? recoveryHint
                : t('lot_recover_hint_reset');

  const showPfCoach =
    !machineState.isRunning && machineState.cycleMaterialist;
  const startDisabled =
    hasFault ||
    machineState.isRunning ||
    machineState.refillActive;
  const resumeDisabled =
    hasFault ||
    !resumeEnabled ||
    machineState.refillAwaitingConfirm ||
    machineState.recoveryAwaitingConfirm ||
    machineState.refillActive ||
    machineState.purgeBusy;
  const pfProdLocked = hasFault;
  const pfInMaterialist =
    machineState.cycleMaterialist || !!preFeederState.idleMode;

  const jogEnabled =
    !!onPfRefill && (pfInMaterialist || !!preFeederState.idleMode);
  const jogLOn = !!preFeederState.refillL?.material;
  const jogROn = !!preFeederState.refillR?.material;

  const hasIndicationsContent =
    purgeHandsWarning ||
    asdaMoveWarning ||
    showPfCoach ||
    (hasFault && modKind === 'other' && !!machineState.fault) ||
    showRecoveryActions ||
    showManualRefill;

  const sectionTitle =
    'text-xs font-bold uppercase tracking-wider text-slate-600 dark:text-slate-400 mb-3';

  return (
    <div className="space-y-4">
      <div className="grid grid-cols-1 xl:grid-cols-12 gap-4 items-start">
        {/* PROCESO — columna izquierda */}
        <section className="xl:col-span-2 rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 p-4 shadow-2xs">
          <h2 className={sectionTitle}>{t('section_proceso')}</h2>
          <div className="flex flex-col gap-2.5">
            <button
              id="btn-start-maquina"
              onClick={onStart}
              disabled={startDisabled}
              title={
                showErrorProcess
                  ? processHint
                  : machineState.cycleMaterialist
                    ? t('err_materialist_start')
                    : undefined
              }
              className={`${btnBase} ${btnPrimary} ${
                startDisabled
                  ? 'border-slate-200 dark:border-slate-700 bg-slate-100 dark:bg-slate-800 text-slate-400 cursor-not-allowed'
                  : machineState.isRunning || processStep === 'start'
                    ? `${btnStartOn}${processStep === 'start' && !machineState.isRunning ? ' animate-pulse' : ''}`
                    : btnStartIdle
              }`}
            >
              <Play className="h-5 w-5 fill-current" />
              <span>{t('btn_start')}</span>
            </button>
            <button
              id="btn-stop-maquina"
              onClick={() => {
                pulseBtn('stop');
                onStop();
              }}
              className={`${btnBase} ${btnPrimary} ${
                btnFlash.stop ? btnStopOn : btnStopIdle
              }`}
            >
              <Square className={`h-5 w-5 fill-current ${btnFlash.stop ? '' : 'text-red-600'}`} />
              <span>{t('btn_stop')}</span>
            </button>
            <button
              id="btn-pause-maquina"
              onClick={onPause}
              disabled={!machineState.pauseEnabled || pfProdLocked || machineState.purgeBusy}
              title={
                machineState.asdaMoveWarning
                  ? t('asda_move_busy_locked')
                  : machineState.purgeBusy
                    ? t('purge_busy_locked')
                    : undefined
              }
              className={`${btnBase} ${btnSecondary} disabled:opacity-40 disabled:cursor-not-allowed ${
                machineState.isPaused
                  ? btnPauseOn
                  : machineState.pauseEnabled
                    ? btnNeed
                    : btnPauseIdle
              }`}
            >
              <Pause className="h-4 w-4" />
              <span>{t('btn_pause')}</span>
            </button>
            <button
              id="btn-reanudar-maquina"
              onClick={onResume}
              disabled={resumeDisabled}
              title={
                machineState.asdaMoveWarning
                  ? t('asda_move_busy_locked')
                  : machineState.purgeBusy
                    ? t('purge_busy_locked')
                    : showMachineResumeCoach
                      ? t('lot_recover_hint_resume')
                      : showMachineResetCoach
                        ? t('lot_recover_hint_reset')
                        : undefined
              }
              className={`group ${btnBase} ${btnSecondary} disabled:opacity-40 disabled:cursor-not-allowed ${
                showMachineResumeCoach ? btnNeedGo : btnStartIdle
              }`}
            >
              <Play className="h-4 w-4 fill-current" />
              <span>
                {machineState.stepByStep && machineState.isPaused
                  ? t('btn_next_step')
                  : t('btn_resume')}
              </span>
            </button>
            <button
              id="btn-reset-maquina"
              onClick={() => {
                pulseBtn('reset');
                onReset();
              }}
              title={
                showMachineResetCoach
                  ? t('lot_recover_hint_reset')
                  : undefined
              }
              className={`${btnBase} ${btnSecondary} ${
                showMachineResetCoach || btnFlash.reset ? btnResetOn : btnResetIdle
              }`}
            >
              <RotateCcw className="h-4 w-4" />
              <span>{t('btn_reset_cycle')}</span>
            </button>
          </div>
        </section>

        {/* LOTE / PRODUCCIÓN — columna central */}
        <section className="xl:col-span-7 rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 p-4 sm:p-5 shadow-2xs">
          <div className="flex items-center gap-2.5 mb-4">
            <Sliders className="h-5 w-5 text-slate-600 dark:text-slate-400" />
            <h2 className="text-sm font-bold uppercase tracking-wider text-slate-800 dark:text-slate-200">
              {t('section_lote')}
            </h2>
          </div>

          {/* Banner de estado dentro de Lote */}
          <div
            className={`rounded-lg border px-4 py-3 mb-5 flex flex-wrap items-center justify-between gap-3 transition-colors ${
              hasFault || interlockError
                ? 'border-red-300 dark:border-red-800 bg-red-50/80 dark:bg-red-950/40'
                : machineState.isRunning
                  ? 'border-emerald-300 dark:border-emerald-800 bg-emerald-50/80 dark:bg-emerald-950/40'
                  : machineState.isPaused
                    ? 'border-amber-300 dark:border-amber-800 bg-amber-50/80 dark:bg-amber-950/40'
                    : 'border-emerald-200 dark:border-emerald-900/60 bg-emerald-50/60 dark:bg-emerald-950/30'
            }`}
          >
            <div className="flex items-center gap-3 min-w-0">
              <div
                className={`h-3.5 w-3.5 shrink-0 rounded-full ${
                  hasFault || interlockError
                    ? 'bg-red-500'
                    : machineState.isRunning
                    ? 'bg-emerald-500 animate-pulse'
                    : machineState.isPaused
                    ? 'bg-amber-500'
                    : 'bg-emerald-500'
                }`}
              />
              <span
                className={`text-base font-bold tracking-tight uppercase min-w-0 truncate ${
                  hasFault || interlockError
                    ? 'text-red-700 dark:text-red-300'
                    : machineState.isRunning
                      ? 'text-emerald-800 dark:text-emerald-200'
                      : machineState.isPaused
                        ? 'text-amber-800 dark:text-amber-200'
                        : 'text-emerald-800 dark:text-emerald-200'
                }`}
                title={hasFault || interlockError ? generalStatus : undefined}
              >
                {hasFault
                  ? currentFaultUi || t('state_error')
                  : interlockError
                    ? machineState.statusText || t('err_materialist_start')
                    : machineState.isRunning
                      ? t('state_producing')
                      : machineState.isPaused
                        ? t('state_paused')
                        : t('state_ready')}
              </span>
              {hasFault && faultCount >= 1 ? (
                <span className="rounded border border-red-300 dark:border-red-800 bg-red-100 dark:bg-red-950/60 px-2 py-0.5 font-mono text-xs font-semibold text-red-700 dark:text-red-300 shrink-0">
                  {t(faultCount === 1 ? 'status_error_qty' : 'status_errors_qty', {
                    count: faultCount,
                  })}
                </span>
              ) : null}
            </div>
            {statusSubtitle && (
              <span className="text-sm text-slate-600 dark:text-slate-400 font-medium">
                {statusSubtitle}
              </span>
            )}
          </div>

          <div className="grid grid-cols-1 md:grid-cols-3 gap-4 mb-5">
            <div className="space-y-2">
              <label htmlFor="select-modelo" className="text-sm font-semibold text-slate-700 dark:text-slate-300 block">
                {t('model')}
              </label>
              <select
                id="select-modelo"
                value={selectedModelIndex}
                onChange={(e) => onModelSelect(Number(e.target.value))}
                disabled={machineState.isRunning}
                className="w-full appearance-none rounded-lg border border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 px-3.5 py-2.5 text-sm font-medium text-slate-900 dark:text-slate-100 shadow-2xs focus:border-slate-500 focus:outline-none focus:ring-1 focus:ring-slate-400 disabled:opacity-50"
              >
                {models.map((m, i) => (
                  <option key={m.name} value={i}>
                    {m.name}
                  </option>
                ))}
              </select>
            </div>

            <div className="space-y-2">
              <label htmlFor="input-general-offset" className="text-sm font-semibold text-slate-700 dark:text-slate-300 block">
                {t('cfg_cut_offset')}
              </label>
              <div className="relative flex items-center gap-1">
                <input
                  id="input-general-offset"
                  type="number"
                  step={0.1}
                  value={offsetInput}
                  onChange={(e) => {
                    setOffsetInput(e.target.value);
                    scheduleCutOffsetSave(e.target.value);
                  }}
                  onBlur={() => flushCutOffsetSave(offsetInput)}
                  onKeyDown={(e) => {
                    if (e.key === 'Enter') {
                      e.currentTarget.blur();
                    }
                  }}
                  className="w-full rounded-lg border border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 pl-3.5 pr-12 py-2.5 text-sm font-mono font-medium text-slate-900 dark:text-slate-100 shadow-2xs focus:border-sky-500 focus:outline-none"
                />
                <span className="absolute right-3.5 text-sm font-mono text-slate-400 pointer-events-none">mm</span>
              </div>
            </div>

            <div className="space-y-2">
              <label htmlFor="input-target-pieces" className="text-sm font-semibold text-slate-700 dark:text-slate-300">
                {t('target_pieces')}
              </label>
              <div className="relative flex items-center">
                <input
                  id="input-target-pieces"
                  type="number"
                  min={1}
                  step={1}
                  value={machineState.targetPieces || ''}
                  onChange={(e) => onTargetPiecesChange(Math.max(1, parseInt(e.target.value, 10) || 1))}
                  disabled={machineState.isRunning}
                  className="w-full rounded-lg border border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 pl-3.5 pr-12 py-2.5 text-sm font-mono font-medium text-slate-900 dark:text-slate-100 shadow-2xs focus:border-sky-500 focus:outline-none disabled:opacity-50"
                />
                <span className="absolute right-3.5 text-sm font-mono text-slate-400 pointer-events-none">pz</span>
              </div>
              {feedSides === 'LR' && (machineState.targetPieces || 0) >= 1 && (
                <p className="text-xs leading-tight text-slate-400 dark:text-slate-500 pl-0.5">
                  {(machineState.targetPieces || 0) % 2 === 0
                    ? t('target_pieces_mirror', {
                        n: Math.floor((machineState.targetPieces || 0) / 2),
                      })
                    : t('target_pieces_mirror_even')}
                </p>
              )}
            </div>
          </div>

          <div className="rounded-lg bg-slate-50 dark:bg-slate-800/60 p-4 border border-slate-200/80 dark:border-slate-800">
            <div className="mb-2.5 flex items-center justify-between text-sm font-mono flex-wrap gap-1">
              <span className="text-slate-700 dark:text-slate-300 flex items-center gap-2 font-sans text-sm font-semibold">
                <Clock className="h-4 w-4 text-slate-500" />
                {machineState.cycleActive ? t('current_cycle_progress') : t('total_production_progress')}
              </span>
              <span className="font-bold text-emerald-600 dark:text-emerald-400 text-sm">
                {stepProgress}%
              </span>
            </div>
            <div className="h-3.5 w-full overflow-hidden rounded-full bg-slate-200 dark:bg-slate-700 p-0.5 mb-2.5">
              <div
                className={`h-full rounded-full transition-all duration-300 ${
                  machineState.isRunning
                    ? 'bg-emerald-500'
                    : machineState.isPaused
                    ? 'bg-amber-500'
                    : 'bg-sky-500 dark:bg-sky-600'
                }`}
                style={{ width: `${stepProgress}%` }}
              />
            </div>
            <div className="flex flex-wrap items-center gap-x-5 gap-y-1 text-xs font-mono text-slate-600 dark:text-slate-400">
              <span>
                {t('pieces')}:{' '}
                <strong className="text-slate-800 dark:text-slate-200">
                  {machineState.piecesCount} / {machineState.targetPieces}
                </strong>
              </span>
              <span>
                {t('cycle_time_remaining')}:{' '}
                <strong className="text-sky-700 dark:text-sky-300">
                  {machineState.cycleActive && etaSec > 0 ? `~${fmtSec(etaSec)}` : '—'}
                </strong>
              </span>
              {(machineState.cycleActive || elapsedSec > 0) && (
                <span title={t('cycle_time_hint')}>
                  {t('cycle_time_label')}:{' '}
                  <strong className="text-slate-800 dark:text-slate-200">
                    {fmtSec(elapsedSec)}
                  </strong>
                </span>
              )}
              {lastPieceSec > 0 && (
                <span>
                  {t('cycle_time_piece')}:{' '}
                  <strong className="text-slate-800 dark:text-slate-200">
                    {lastPieceSec.toFixed(1)}s
                  </strong>
                </span>
              )}
            </div>
          </div>
        </section>

        {/* OPERACIÓN / MANUAL + ALIMENTADOR — columna derecha */}
        <div className="xl:col-span-3 flex flex-col gap-4">
          <section className="rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 p-4 shadow-2xs">
            <h2 className={sectionTitle}>{t('section_operacion_manual')}</h2>
            <div className="flex flex-col gap-2.5">
              <button
                id="btn-refill-maquina"
                type="button"
                onClick={onRefill}
                disabled={!onRefill}
                title={t('refill_helpers_subtitle')}
                className={`${btnBase} ${btnSecondary} ${
                  !onRefill
                    ? 'border-slate-200 dark:border-slate-700 bg-slate-100 dark:bg-slate-800 text-slate-400 cursor-not-allowed'
                    : machineState.refillActive
                      ? btnAuxOn
                      : btnAuxIdle
                }`}
              >
                <Droplets className="h-4 w-4" />
                <span>{t('btn_refill')}</span>
              </button>
              <button
                id="btn-home-maquina"
                type="button"
                onClick={() => {
                  if (!onMachineHome || btnFlash.home) return;
                  setBtnFlash((prev) => ({ ...prev, home: true }));
                  const started = Date.now();
                  void Promise.resolve(onMachineHome()).finally(() => {
                    const wait = Math.max(0, 800 - (Date.now() - started));
                    window.setTimeout(() => {
                      setBtnFlash((prev) => ({ ...prev, home: false }));
                    }, wait);
                  });
                }}
                disabled={!onMachineHome || machineState.purgeBusy}
                title={
                  machineState.asdaMoveWarning
                    ? t('asda_move_busy_locked')
                    : machineState.purgeBusy
                      ? t('purge_busy_locked')
                      : t('btn_machine_home_hint')
                }
                className={`${btnBase} ${btnSecondary} ${
                  !onMachineHome || machineState.purgeBusy
                    ? 'border-slate-200 dark:border-slate-700 bg-slate-100 dark:bg-slate-800 text-slate-400 cursor-not-allowed'
                    : btnFlash.home
                      ? btnAuxOn
                      : btnAuxIdle
                }`}
              >
                <Home className="h-4 w-4" />
                <span>{t('btn_machine_home')}</span>
              </button>
              <button
                id="btn-main-pf-materialist"
                type="button"
                onClick={onMaterialist}
                disabled={!onMaterialist || machineState.purgeBusy}
                title={
                  machineState.asdaMoveWarning
                    ? t('asda_move_busy_locked')
                    : machineState.purgeBusy
                      ? t('purge_busy_locked')
                      : machineState.cycleMaterialist
                        ? t('pf_recover_hint_jog')
                        : t('pf_jog_need_materialist')
                }
                className={`${btnBase} ${btnSecondary} ${
                  !onMaterialist || machineState.purgeBusy
                    ? 'border-slate-200 dark:border-slate-700 bg-slate-100 dark:bg-slate-800 text-slate-400 cursor-not-allowed'
                    : machineState.cycleMaterialist
                      ? btnAuxOn
                      : btnAuxIdle
                }`}
              >
                <Package className="h-4 w-4" />
                <span>{t('btn_materialist_cycle')}</span>
              </button>
            </div>
          </section>

          <section className="rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 p-4 shadow-2xs">
            <div className="flex items-center gap-2 mb-3">
              <Layers className="h-4 w-4 text-slate-500 dark:text-slate-400" />
              <h2 className="text-xs font-bold uppercase tracking-wider text-slate-600 dark:text-slate-400">
                {t('section_alimentador')}
              </h2>
            </div>
            <div className="flex flex-col gap-2.5">
              <button
                id="btn-main-pf-start"
                type="button"
                onClick={onPfStart}
                disabled={!onPfStart || !pfConnected}
                className={`${btnBase} ${btnSecondary} disabled:opacity-40 ${
                  preFeederState.isRunning ? btnOn : btnAuxIdle
                }`}
              >
                <Play className="h-4 w-4 fill-current" />
                <span>{t('btn_start')}</span>
              </button>
              <button
                id="btn-main-pf-jog-l"
                type="button"
                onClick={() => onPfRefill?.('L', 'material', !jogLOn)}
                disabled={!jogEnabled}
                title={
                  pfInMaterialist
                    ? t('pf_refill_material')
                    : t('pf_jog_need_materialist')
                }
                className={`${btnBase} ${btnSecondary} disabled:opacity-40 ${
                  jogLOn ? btnAuxOn : btnAuxIdle
                }`}
              >
                <Zap className="h-4 w-4" />
                <span>{t('btn_jog')} L</span>
              </button>
              <button
                id="btn-main-pf-jog-r"
                type="button"
                onClick={() => onPfRefill?.('R', 'material', !jogROn)}
                disabled={!jogEnabled}
                title={
                  pfInMaterialist
                    ? t('pf_refill_material')
                    : t('pf_jog_need_materialist')
                }
                className={`${btnBase} ${btnSecondary} disabled:opacity-40 ${
                  jogROn ? btnAuxOn : btnAuxIdle
                }`}
              >
                <Zap className="h-4 w-4" />
                <span>{t('btn_jog')} R</span>
              </button>
            </div>
          </section>
        </div>
      </div>

      {/* INDICACIONES — bloque único abajo */}
      <section className="rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 p-4 sm:p-5 shadow-2xs">
        <div className="flex items-center gap-2 mb-3">
          <MessageSquare className="h-4 w-4 text-slate-500 dark:text-slate-400" />
          <h2 className="text-xs font-bold uppercase tracking-wider text-slate-600 dark:text-slate-400">
            {t('process_assist_title')}
          </h2>
        </div>

        <div
          className={`rounded-lg border p-4 sm:p-5 min-h-[5rem] ${
            purgeHandsWarning || asdaMoveWarning
              ? 'border-red-400 dark:border-red-700 bg-red-50 dark:bg-red-950/40'
              : hasIndicationsContent
                ? 'border-sky-200 dark:border-sky-800 bg-sky-50/50 dark:bg-sky-950/20'
                : 'border-slate-200 dark:border-slate-800 bg-slate-50/80 dark:bg-slate-800/40'
          }`}
        >
          {purgeHandsWarning && (
            <div
              id="purge-hands-warning"
              role="alert"
              aria-live="assertive"
              className="mb-3 text-center text-base sm:text-lg font-black uppercase tracking-wide text-red-700 dark:text-red-200 animate-pulse"
            >
              {t('purge_hands_warning')}
            </div>
          )}
          {asdaMoveWarning && !purgeHandsWarning && (
            <div
              id="asda-move-warning"
              role="alert"
              aria-live="assertive"
              className="mb-3 text-center text-base sm:text-lg font-black uppercase tracking-wide text-red-700 dark:text-red-200 animate-pulse"
            >
              {t('asda_move_warning')}
            </div>
          )}

          {!hasIndicationsContent && (
            <div className="flex flex-col items-center justify-center text-center py-2 gap-2">
              <Info className="h-8 w-8 text-sky-400 dark:text-sky-500" />
              <p className="text-base font-semibold text-slate-700 dark:text-slate-200">
                {t('process_assist_idle')}
              </p>
              <p className="text-sm text-slate-500 dark:text-slate-400">
                {t('indications_idle_sub')}
              </p>
            </div>
          )}

          {hasFault && modKind === 'other' && machineState.fault && (
            <p className="text-sm text-red-700 dark:text-red-300 mb-3">
              {machineState.fault} — {t('module_recovery_use_machine')}
            </p>
          )}

          {showPfCoach && (
            <p className="text-sm text-amber-800 dark:text-amber-200 mb-3">
              {resumeEnabled
                ? t('pf_recover_hint_jog_resume')
                : t('pf_recover_hint_jog')}
            </p>
          )}

          {showRecoveryActions && (
            <div className="flex flex-wrap items-center gap-3 rounded-lg border border-amber-300 dark:border-amber-700 bg-amber-50 dark:bg-amber-950/50 px-4 py-3.5">
              <div className="min-w-0 flex-1">
                <p className="text-sm font-bold text-amber-900 dark:text-amber-100">
                  {recoveryTitle}
                </p>
                <p className="text-xs text-amber-800/80 dark:text-amber-200/80 mt-0.5">
                  {recoveryHint}
                </p>
              </div>
              {recoveryStage === 'await_feed' && onRefillRetry && (
                <button
                  id="btn-recovery-refill-retry"
                  type="button"
                  onClick={onRefillRetry}
                  disabled={!machineState.refillAwaitingConfirm}
                  className="flex items-center gap-1.5 rounded-lg bg-amber-500 hover:bg-amber-600 px-3.5 py-2 text-xs font-bold text-white disabled:opacity-40"
                >
                  <RotateCcw className="h-3.5 w-3.5" />
                  {t('btn_refill_confirm_retry')}
                </button>
              )}
              {(recoveryStage === 'after_feed' || recoveryStage === 'await_feed') &&
                onRefillConfirm && (
                <button
                  id="btn-recovery-next-cut"
                  type="button"
                  onClick={() => onRefillConfirm(true)}
                  disabled={!machineState.refillAwaitingConfirm}
                  className="flex items-center gap-1.5 rounded-lg bg-emerald-600 hover:bg-emerald-700 px-3.5 py-2 text-xs font-bold text-white disabled:opacity-40"
                >
                  <Check className="h-3.5 w-3.5" />
                  {t('btn_refill_confirm_next_cut')}
                </button>
              )}
              {recoveryStage === 'e050_materialist' &&
                machineState.recoveryAwaitingConfirm &&
                onRecoveryReview && (
                <>
                  <button
                    id="btn-recovery-e050-no"
                    type="button"
                    onClick={() => onRecoveryReview(false)}
                    disabled={!machineState.recoveryAwaitingConfirm}
                    className="flex items-center gap-1.5 rounded-lg border border-slate-300 dark:border-slate-600 bg-white dark:bg-slate-800 px-3.5 py-2 text-xs font-bold text-slate-700 dark:text-slate-200 disabled:opacity-40"
                  >
                    <X className="h-3.5 w-3.5" />
                    {t('btn_e050_no_materialist')}
                  </button>
                  <button
                    id="btn-recovery-e050-yes"
                    type="button"
                    onClick={() => onRecoveryReview(true)}
                    disabled={!machineState.recoveryAwaitingConfirm}
                    className="flex items-center gap-1.5 rounded-lg bg-emerald-600 hover:bg-emerald-700 px-3.5 py-2 text-xs font-bold text-white disabled:opacity-40"
                  >
                    <Check className="h-3.5 w-3.5" />
                    {t('btn_e050_yes_materialist')}
                  </button>
                </>
              )}
              {recoveryStage === 'abort_decide' && onRecoveryReview && (
                <>
                  <button
                    id="btn-recovery-abort-cycle"
                    type="button"
                    onClick={() => onRecoveryReview(false)}
                    disabled={!machineState.recoveryAwaitingConfirm}
                    className="flex items-center gap-1.5 rounded-lg bg-red-600 hover:bg-red-700 px-3.5 py-2 text-xs font-bold text-white disabled:opacity-40"
                  >
                    <X className="h-3.5 w-3.5" />
                    {t('btn_recovery_abort')}
                  </button>
                  <button
                    id="btn-recovery-abort-continue"
                    type="button"
                    onClick={() => onRecoveryReview(true)}
                    disabled={!machineState.recoveryAwaitingConfirm}
                    className="flex items-center gap-1.5 rounded-lg bg-emerald-600 hover:bg-emerald-700 px-3.5 py-2 text-xs font-bold text-white disabled:opacity-40"
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
                    id="btn-recovery-purge-yes"
                    type="button"
                    onClick={() => onRecoveryReview(true)}
                    disabled={!machineState.recoveryAwaitingConfirm}
                    className="flex items-center gap-1.5 rounded-lg bg-emerald-600 hover:bg-emerald-700 px-3.5 py-2 text-xs font-bold text-white disabled:opacity-40"
                  >
                    <Check className="h-3.5 w-3.5" />
                    {t('btn_recovery_purge_yes')}
                  </button>
                  <button
                    id="btn-recovery-purge-no"
                    type="button"
                    onClick={() => onRecoveryReview(false)}
                    disabled={!machineState.recoveryAwaitingConfirm}
                    className="flex items-center gap-1.5 rounded-lg border border-slate-300 dark:border-slate-600 bg-white dark:bg-slate-800 px-3.5 py-2 text-xs font-bold text-slate-700 dark:text-slate-200 disabled:opacity-40"
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
                  id="btn-recovery-review-ok"
                  type="button"
                  onClick={() => onRecoveryReview(true)}
                  disabled={!machineState.recoveryAwaitingConfirm}
                  className="flex items-center gap-1.5 rounded-lg bg-emerald-600 hover:bg-emerald-700 px-3.5 py-2 text-xs font-bold text-white disabled:opacity-40"
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
                  id="btn-recovery-tray-emptied"
                  type="button"
                  onClick={() => onRecoveryReview(true)}
                  disabled={!machineState.recoveryAwaitingConfirm}
                  className="flex items-center gap-1.5 rounded-lg bg-emerald-600 hover:bg-emerald-700 px-3.5 py-2 text-xs font-bold text-white disabled:opacity-40"
                >
                  <Check className="h-3.5 w-3.5" />
                  {t('btn_tray_emptied')}
                </button>
              )}
            </div>
          )}

          {showManualRefill && (
            <div className="flex flex-wrap items-center gap-3 rounded-lg border border-sky-300 dark:border-sky-700 bg-sky-50 dark:bg-sky-950/50 px-4 py-3">
              <div className="min-w-0 flex-1">
                <p className="text-xs font-bold text-sky-900 dark:text-sky-100">
                  {machineState.refillPrompt === 'working'
                    ? t('refill_confirm_title_working')
                    : machineState.refillPrompt === 'await_feed'
                      ? t('refill_confirm_title_await')
                      : t('refill_confirm_title_feed')}
                </p>
                <p className="text-[11px] text-sky-800/80 dark:text-sky-200/80 mt-0.5">
                  {machineState.refillPrompt === 'working'
                    ? t('refill_confirm_hint_working')
                    : machineState.refillPrompt === 'await_feed'
                      ? t('refill_confirm_hint_await')
                      : t('refill_confirm_hint_feed')}
                </p>
              </div>
              {machineState.refillPrompt === 'await_feed' && onRefillRetry && (
                <button
                  id="btn-refill-confirm-retry"
                  type="button"
                  onClick={onRefillRetry}
                  disabled={!machineState.refillAwaitingConfirm}
                  className="flex items-center gap-1.5 rounded-lg bg-amber-500 hover:bg-amber-600 px-3.5 py-2 text-xs font-bold text-white shadow-2xs active:scale-95 disabled:opacity-40 disabled:cursor-not-allowed disabled:active:scale-100"
                >
                  <RotateCcw className="h-3.5 w-3.5" />
                  {t('btn_refill_confirm_retry')}
                </button>
              )}
              {machineState.refillPrompt !== 'working' && (
                <button
                  id="btn-refill-confirm-yes"
                  type="button"
                  onClick={() => onRefillConfirm!(true)}
                  disabled={!machineState.refillAwaitingConfirm}
                  className="flex items-center gap-1.5 rounded-lg bg-emerald-600 hover:bg-emerald-700 px-3.5 py-2 text-xs font-bold text-white shadow-2xs active:scale-95 disabled:opacity-40 disabled:cursor-not-allowed disabled:active:scale-100"
                >
                  <Check className="h-3.5 w-3.5" />
                  {t('btn_refill_confirm_next_cut')}
                </button>
              )}
            </div>
          )}
        </div>
      </section>

      {showLogs && (
        <LogTerminal title={t('logs_title')} logs={logs} onClear={onClearLogs} filterModule="ALL" />
      )}
    </div>
  );
};
