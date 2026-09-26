import React, { useState, useEffect, useRef } from 'react';
import {
  Play,
  Square,
  RotateCcw,
  Sliders,
  Clock,
  Pause,
  Settings2,
  Layers,
  Droplets,
  Check,
  X,
  Home,
  Zap,
  Package,
  Activity,
  MessageSquare,
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
  onGotoCycle?: () => void;
  onPfStart?: () => void;
  onPfStop?: () => void;
  onPfReset?: () => void;
  onPfRefill?: (side: 'L' | 'R', channel: PfRefillChannel, on: boolean) => void;
  onMaterialist?: () => void;
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
  onRefillLongFeed,
  onRecoveryReview,
  onGotoCycle,
  onPfStart,
  onPfRefill,
  onMaterialist,
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
  const modulePanelError =
    !!preFeederState.hasError ||
    !!plcState.hasError ||
    motionLatched ||
    plcLatched ||
    pfLatched;
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
        : machineState.statusText || t('state_ready');

  const btnBase =
    'flex items-center justify-center gap-1.5 rounded-lg border px-3 py-1.5 text-xs font-bold transition shadow-2xs active:scale-95';
  const btnIdle =
    'border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 text-slate-700 dark:text-slate-200 hover:bg-slate-100 dark:hover:bg-slate-700 active:bg-slate-800 active:text-white active:border-slate-800 dark:active:bg-slate-100 dark:active:text-slate-900';
  const btnOn =
    'border-slate-900 dark:border-white bg-slate-800 dark:bg-slate-100 text-white dark:text-slate-900 ring-2 ring-slate-400/80 ring-offset-1 dark:ring-offset-slate-900';
  const btnNeed =
    'border-amber-400 bg-amber-100 dark:bg-amber-950/60 text-amber-950 dark:text-amber-100 ring-2 ring-amber-300 ring-offset-1 animate-pulse';
  const btnNeedGo =
    'border-emerald-500 bg-emerald-600 hover:bg-emerald-700 text-white ring-2 ring-emerald-300 ring-offset-1 animate-pulse';
  const btnStartIdle =
    'border-emerald-200 dark:border-emerald-800 bg-emerald-50 dark:bg-emerald-950/40 text-emerald-800 dark:text-emerald-200 hover:bg-emerald-100 dark:hover:bg-emerald-900/50';
  const btnStartOn =
    'border-emerald-500 bg-emerald-500 text-white [&_svg]:text-white ring-2 ring-emerald-300 ring-offset-1 dark:ring-offset-slate-900 shadow-md';
  const btnStopIdle =
    'border-red-200 dark:border-red-900/60 bg-red-50 dark:bg-red-950/40 text-red-700 dark:text-red-300 hover:bg-red-100 dark:hover:bg-red-900/40';
  const btnStopOn =
    'border-red-500 bg-red-500 text-white [&_svg]:text-white ring-2 ring-red-300 ring-offset-1 dark:ring-offset-slate-900 shadow-md';
  const btnResetIdle =
    'border-amber-200 dark:border-amber-800 bg-amber-50 dark:bg-amber-950/40 text-amber-800 dark:text-amber-200 hover:bg-amber-100 dark:hover:bg-amber-900/50';
  const btnResetOn =
    'border-amber-500 bg-amber-500 text-white [&_svg]:text-white ring-2 ring-amber-300 ring-offset-1 dark:ring-offset-slate-900 shadow-md';
  const btnPurgeIdle =
    'border-sky-200 dark:border-sky-800 bg-sky-50 dark:bg-sky-950/40 text-sky-800 dark:text-sky-200 hover:bg-sky-100 dark:hover:bg-sky-900/50';
  const btnPurgeOn =
    'border-sky-500 bg-sky-500 text-white [&_svg]:text-white ring-2 ring-sky-300 ring-offset-1 dark:ring-offset-slate-900 shadow-md';
  const btnHomeIdle =
    'border-emerald-200 dark:border-emerald-800 bg-emerald-50 dark:bg-emerald-950/40 text-emerald-800 dark:text-emerald-200 hover:bg-emerald-100 dark:hover:bg-emerald-900/50';
  const btnHomeOn =
    'border-emerald-500 bg-emerald-500 text-white [&_svg]:text-white ring-2 ring-emerald-300 ring-offset-1 dark:ring-offset-slate-900 shadow-md';
  const btnMatIdle =
    'border-violet-200 dark:border-violet-800 bg-violet-50 dark:bg-violet-950/40 text-violet-800 dark:text-violet-200 hover:bg-violet-100 dark:hover:bg-violet-900/50';
  const btnMatOn =
    'border-violet-500 bg-violet-500 text-white [&_svg]:text-white ring-2 ring-violet-300 ring-offset-1 dark:ring-offset-slate-900 shadow-md';

  const pfConnected = preFeederState.connection.connected;
  const pfHasError = !!preFeederState.hasError && pfConnected;
  // Module Controls keeps independent PF diagnostics; machine RESET owns the global EXXX latch.
  const recoveryStage =
    machineState.recoveryPrompt ||
    (machineState.recoveryAfterError || machineState.e050Lot
      ? machineState.refillPrompt ||
        (machineState.refillActive ? 'working' : '')
      : '');
  const showRecoveryTrack = !!recoveryStage;
  const e050Lot = !!machineState.e050Lot;
  const skipCut = !!machineState.refillSkipCut;

  const processStep: string = e050Lot
    ? recoveryStage === 'e050_materialist'
      ? 'ask'
      : recoveryStage === 'e050_materialist_wait' ||
          ((recoveryStage === 'working' ||
            recoveryStage === 'await_feed' ||
            recoveryStage === 'after_feed') &&
            skipCut)
        ? 'empty'
        : recoveryStage === 'review_piece'
          ? 'review'
          : hasFault
            ? 'reset'
            : machineState.e050FinishPiece && machineState.isRunning
              ? 'piece'
              : resumeEnabled || machineState.isPaused
                ? 'resume'
                : ''
    : recoveryStage === 'continue_cycle'
      ? 'continue'
      : recoveryStage === 'review_piece'
        ? 'review'
        : recoveryStage === 'working' ||
            recoveryStage === 'await_feed' ||
            recoveryStage === 'after_feed' ||
            recoveryStage === 'after_cut'
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
  const showRecoveryActions =
    recoveryStage === 'await_feed' ||
    recoveryStage === 'after_feed' ||
    recoveryStage === 'after_cut' ||
    (recoveryStage === 'e050_materialist' &&
      !!machineState.recoveryAwaitingConfirm) ||
    recoveryStage === 'e050_materialist_wait' ||
    recoveryStage === 'review_piece' ||
    recoveryStage === 'continue_cycle';

  const showManualRefill =
    !machineState.recoveryAfterError &&
    !machineState.e050Lot &&
    !!machineState.refillActive &&
    !!machineState.refillPrompt &&
    !!onRefillConfirm;
  const recoveryTitle =
    recoveryStage === 'e050_materialist'
      ? t('e050_materialist_title')
      : recoveryStage === 'e050_materialist_wait'
        ? t('e050_materialist_wait_title')
        : recoveryStage === 'review_piece'
          ? t('recovery_review_title')
          : recoveryStage === 'continue_cycle'
            ? t('recovery_continue_title')
            : recoveryStage === 'after_cut'
              ? t('refill_confirm_title_cut')
              : recoveryStage === 'working'
                ? t('refill_confirm_title_working')
                : recoveryStage === 'await_feed'
                  ? t('refill_confirm_title_await')
                  : t('refill_confirm_title_feed');
  const recoveryHint =
    recoveryStage === 'e050_materialist'
      ? machineState.e050FinishPiece
        ? `${t('e050_materialist_hint')} ${t('lot_recover_hint_e050_finish')}`
        : t('e050_materialist_hint')
      : recoveryStage === 'e050_materialist_wait'
        ? t('e050_materialist_wait_hint')
        : recoveryStage === 'review_piece'
            ? t('recovery_review_hint')
            : recoveryStage === 'continue_cycle'
              ? t('recovery_continue_hint')
              : recoveryStage === 'after_cut'
                ? t('refill_confirm_hint_cut')
                : recoveryStage === 'working'
                  ? t('refill_confirm_hint_working')
                  : recoveryStage === 'await_feed'
                    ? t('refill_confirm_hint_await')
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
  /** Indicaciones PF solo si no hay lote. */
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
    machineState.refillActive;
  // Error activo: no producir. Materialist + JOG permanecen disponibles como antes.
  const pfProdLocked = hasFault;
  const pfInMaterialist = machineState.cycleMaterialist;
  const pfMode = pfInMaterialist
    ? 'materialist'
    : machineState.cycleBusy || preFeederState.isRunning
      ? 'busy'
      : null;
  const pfHeadline = !pfConnected
    ? t('node_disconnected')
    : pfHasError
      ? preFeederState.statusText || t('pf_need_reset_start')
      : pfMode === 'materialist'
        ? t('module_status_materialist')
        : pfMode === 'busy'
          ? t('module_status_busy')
          : preFeederState.statusText || t('state_ready');
  const pfFeedLabel =
    pfMode === 'materialist'
      ? t('status_materialist')
      : pfMode === 'busy'
        ? t('status_in_process')
        : t('status_idle');

  const jogEnabled =
    !!onPfRefill && (pfInMaterialist || !!preFeederState.idleMode);
  const jogLOn = !!preFeederState.refillL?.material;
  const jogROn = !!preFeederState.refillR?.material;

  return (
    <div className="space-y-4">
      <div
        className={`rounded-xl border px-4 py-2.5 shadow-2xs flex flex-wrap items-center justify-between gap-3 transition-colors ${
          hasFault || interlockError
            ? 'border-red-300 dark:border-red-800 bg-red-50/70 dark:bg-red-950/30 text-slate-800 dark:text-slate-200'
            : 'border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 text-slate-800 dark:text-slate-200'
        }`}
      >
        <div className="flex items-center gap-2.5 min-w-0">
          <div
            className={`h-2.5 w-2.5 shrink-0 rounded-full ${
              hasFault || interlockError
                ? 'bg-red-500'
                : machineState.isRunning
                ? 'bg-emerald-500 animate-pulse'
                : machineState.isPaused
                ? 'bg-amber-500'
                : machineState.cycleMaterialist
                ? 'bg-violet-500'
                : 'bg-slate-400'
            }`}
          />
          <span
            className={`text-sm font-semibold tracking-tight min-w-0 truncate ${
              hasFault || interlockError
                ? 'text-red-700 dark:text-red-300'
                : 'text-slate-900 dark:text-white'
            }`}
            title={hasFault || interlockError ? generalStatus : undefined}
          >
            {hasFault
              ? currentFaultUi || t('state_error')
              : interlockError
                ? machineState.statusText || t('err_materialist_start')
                : t('state_ready')}
          </span>
          {hasFault && faultCount >= 1 ? (
            <span className="rounded border border-red-300 dark:border-red-800 bg-red-100 dark:bg-red-950/60 px-2 py-0.5 font-mono text-[11px] font-semibold text-red-700 dark:text-red-300 shrink-0">
              {t(faultCount === 1 ? 'status_error_qty' : 'status_errors_qty', {
                count: faultCount,
              })}
            </span>
          ) : null}
        </div>
      </div>

      <div className="rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 p-4 sm:p-5 shadow-2xs transition-colors">
        <div className="flex items-center justify-between border-b border-slate-100 dark:border-slate-800 pb-2.5">
          <div className="flex items-center gap-2">
            <Sliders className="h-4 w-4 text-slate-600 dark:text-slate-400" />
            <h2 className="text-xs font-bold uppercase tracking-wider text-slate-800 dark:text-slate-200">
              {t('machine_control')}
            </h2>
          </div>
          <div className="flex items-center gap-2">
            {onGotoCycle && (
              <button
                type="button"
                onClick={onGotoCycle}
                className="flex items-center gap-1 rounded bg-slate-100 dark:bg-slate-800 border border-slate-200 dark:border-slate-700 px-2 py-0.5 text-[10px] font-semibold text-slate-600 dark:text-slate-300 hover:bg-slate-200 dark:hover:bg-slate-700 transition"
              >
                <Settings2 className="h-3 w-3" />
                {t('btn_goto_cycle')}
              </button>
            )}
            <span className="rounded bg-slate-100 dark:bg-slate-800 border border-slate-200 dark:border-slate-700 px-2 py-0.5 font-mono text-[10px] text-slate-600 dark:text-slate-400">
              {t('master_control')}
            </span>
          </div>
        </div>

        <div className="mt-4 space-y-4">
            <div className="grid grid-cols-1 md:grid-cols-3 gap-4 items-start">
              <div className="space-y-1.5">
                <label htmlFor="select-modelo" className="text-xs font-semibold text-slate-700 dark:text-slate-300 block">
                  {t('model')}
                </label>
                <div className="relative">
                  <select
                    id="select-modelo"
                    value={selectedModelIndex}
                    onChange={(e) => onModelSelect(Number(e.target.value))}
                    disabled={machineState.isRunning}
                    className="w-full appearance-none rounded-md border border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 px-3 py-1.5 text-xs font-medium text-slate-900 dark:text-slate-100 shadow-2xs focus:border-slate-500 focus:outline-none focus:ring-1 focus:ring-slate-400 disabled:opacity-50"
                  >
                    {models.map((m, i) => (
                      <option key={m.name} value={i}>
                        {m.name}
                      </option>
                    ))}
                  </select>
                </div>
              </div>

              <div className="space-y-1.5">
                <label htmlFor="input-general-offset" className="text-xs font-semibold text-slate-700 dark:text-slate-300 block">
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
                    className="w-full rounded-md border border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 pl-3 pr-10 py-1.5 text-xs font-mono font-medium text-slate-900 dark:text-slate-100 shadow-2xs focus:border-teal-500 focus:outline-none"
                  />
                  <span className="absolute right-3 text-xs font-mono text-slate-400 pointer-events-none">mm</span>
                </div>
              </div>

              <div className="space-y-1.5">
                <label htmlFor="input-target-pieces" className="text-xs font-semibold text-slate-700 dark:text-slate-300">
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
                    className="w-full rounded-md border border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 pl-3 pr-10 py-1.5 text-xs font-mono font-medium text-slate-900 dark:text-slate-100 shadow-2xs focus:border-teal-500 focus:outline-none disabled:opacity-50"
                  />
                  <span className="absolute right-3 text-xs font-mono text-slate-400 pointer-events-none">pz</span>
                </div>
              </div>
            </div>

            <div className="rounded-lg bg-slate-50 dark:bg-slate-800/60 p-3 border border-slate-200/80 dark:border-slate-800">
              <div className="mb-2 flex items-center justify-between text-xs font-mono flex-wrap gap-1">
                <span className="text-slate-700 dark:text-slate-300 flex items-center gap-1.5 font-sans text-xs font-semibold">
                  <Clock className="h-3.5 w-3.5 text-slate-500" />
                  {machineState.cycleActive ? t('current_cycle_progress') : t('total_production_progress')}
                </span>
                <span className="font-bold text-emerald-600 dark:text-emerald-400 text-xs">
                  {stepProgress}%
                </span>
              </div>
              <div className="h-2.5 w-full overflow-hidden rounded-full bg-slate-200 dark:bg-slate-700 p-0.5 mb-2">
                <div
                  className={`h-full rounded-full transition-all duration-300 ${
                    machineState.isRunning
                      ? 'bg-emerald-500'
                      : machineState.isPaused
                      ? 'bg-amber-500'
                      : 'bg-slate-400 dark:bg-slate-500'
                  }`}
                  style={{ width: `${stepProgress}%` }}
                />
              </div>
              <div className="flex flex-wrap items-center gap-x-4 gap-y-1 text-[10px] font-mono text-slate-600 dark:text-slate-400">
                <span>
                  {t('pieces')}:{' '}
                  <strong className="text-slate-800 dark:text-slate-200">
                    {machineState.piecesCount} / {machineState.targetPieces}
                  </strong>
                </span>
                <span>
                  {t('cycle_time_remaining')}:{' '}
                  <strong className="text-teal-700 dark:text-teal-300">
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

            <div className="flex items-stretch gap-2">
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
                  className={`${btnBase} min-w-0 flex-1 ${
                    startDisabled
                      ? 'border-slate-200 dark:border-slate-700 bg-slate-100 dark:bg-slate-800 text-slate-400 cursor-not-allowed'
                      : machineState.isRunning || processStep === 'start'
                        ? `${btnStartOn}${processStep === 'start' && !machineState.isRunning ? ' animate-pulse' : ''}`
                        : btnStartIdle
                  }`}
                >
                  <Play className="h-3.5 w-3.5 fill-current" />
                  <span>{t('btn_start')}</span>
                </button>
                <button
                  id="btn-stop-maquina"
                  onClick={() => {
                    pulseBtn('stop');
                    onStop();
                  }}
                  className={`${btnBase} min-w-0 flex-1 ${
                    btnFlash.stop ? btnStopOn : btnStopIdle
                  }`}
                >
                  <Square className={`h-3.5 w-3.5 fill-current ${btnFlash.stop ? '' : 'text-red-600'}`} />
                  <span>{t('btn_stop')}</span>
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
                  className={`${btnBase} min-w-0 flex-1 ${
                    showMachineResetCoach || btnFlash.reset ? btnResetOn : btnResetIdle
                  }`}
                >
                  <RotateCcw className="h-3.5 w-3.5" />
                  <span>{t('btn_reset_cycle')}</span>
                </button>
                <button
                  id="btn-pause-maquina"
                  onClick={onPause}
                  disabled={!machineState.pauseEnabled || pfProdLocked}
                  className={`${btnBase} min-w-0 flex-1 disabled:opacity-40 disabled:cursor-not-allowed ${
                    machineState.isPaused
                      ? `${btnOn} text-white dark:text-slate-900`
                      : machineState.pauseEnabled
                        ? btnNeed
                        : btnIdle
                  }`}
                >
                  <Pause className="h-3.5 w-3.5" />
                  <span>{t('btn_pause')}</span>
                </button>
                <button
                  id="btn-reanudar-maquina"
                  onClick={onResume}
                  disabled={resumeDisabled}
                  title={
                    showMachineResumeCoach
                      ? t('lot_recover_hint_resume')
                      : showMachineResetCoach
                        ? t('lot_recover_hint_reset')
                        : undefined
                  }
                  className={`group ${btnBase} min-w-0 flex-1 disabled:opacity-40 disabled:cursor-not-allowed ${
                    showMachineResumeCoach ? btnNeedGo : btnIdle
                  }`}
                >
                  <RotateCcw className="h-3.5 w-3.5 group-hover:rotate-45 transition-transform" />
                  <span>
                    {machineState.stepByStep && machineState.isPaused
                      ? t('btn_next_step')
                      : t('btn_resume')}
                  </span>
                </button>

                <div className="w-px shrink-0 self-stretch bg-slate-200 dark:bg-slate-700" />

                <button
                  id="btn-refill-maquina"
                  type="button"
                  onClick={onRefill}
                  disabled={!onRefill}
                  title={t('refill_helpers_subtitle')}
                  className={`${btnBase} min-w-0 flex-1 ${
                    !onRefill
                      ? 'border-slate-200 dark:border-slate-700 bg-slate-100 dark:bg-slate-800 text-slate-400 cursor-not-allowed'
                      : machineState.refillActive
                        ? btnPurgeOn
                        : btnPurgeIdle
                  }`}
                >
                  <Droplets className="h-3.5 w-3.5" />
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
                  disabled={!onMachineHome}
                  title={t('btn_machine_home_hint')}
                  className={`${btnBase} min-w-0 flex-1 ${
                    !onMachineHome
                      ? 'border-slate-200 dark:border-slate-700 bg-slate-100 dark:bg-slate-800 text-slate-400 cursor-not-allowed'
                      : btnFlash.home
                        ? btnHomeOn
                        : btnHomeIdle
                  }`}
                >
                  <Home className="h-3.5 w-3.5" />
                  <span>{t('btn_machine_home')}</span>
                </button>
                <button
                  id="btn-main-pf-materialist"
                  type="button"
                  onClick={onMaterialist}
                  disabled={!onMaterialist}
                  title={
                    machineState.cycleMaterialist
                      ? t('pf_recover_hint_jog')
                      : t('pf_jog_need_materialist')
                  }
                  className={`${btnBase} min-w-0 flex-1 ${
                    !onMaterialist
                      ? 'border-slate-200 dark:border-slate-700 bg-slate-100 dark:bg-slate-800 text-slate-400 cursor-not-allowed'
                      : machineState.cycleMaterialist
                        ? btnMatOn
                        : btnMatIdle
                  }`}
                >
                  <Package className="h-3.5 w-3.5" />
                  <span>{t('btn_materialist_cycle')}</span>
                </button>
            </div>

            <div className="space-y-3">
              <div className="flex items-center gap-2">
                <MessageSquare className="h-3.5 w-3.5 text-slate-500 dark:text-slate-400" />
                <h3 className="text-[10px] font-bold uppercase tracking-wider text-slate-500 dark:text-slate-400">
                  {t('process_assist_title')}
                </h3>
              </div>
            {!showRecoveryActions && !showManualRefill && (
              <p className="text-xs text-slate-500 dark:text-slate-400">
                {t('process_assist_idle')}
              </p>
            )}
            {showRecoveryActions && (
              <div className="flex flex-wrap items-center gap-3 rounded-lg border border-amber-300 dark:border-amber-700 bg-amber-50 dark:bg-amber-950/50 px-4 py-3">
                <div className="min-w-0 flex-1">
                  <p className="text-xs font-bold text-amber-900 dark:text-amber-100">
                    {recoveryTitle}
                  </p>
                  <p className="text-[11px] text-amber-800/80 dark:text-amber-200/80 mt-0.5">
                    {recoveryHint}
                  </p>
                </div>
                {(recoveryStage === 'await_feed' || recoveryStage === 'after_feed') &&
                  onRefillRetry && (
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
                {(recoveryStage === 'await_feed' || recoveryStage === 'after_feed') &&
                  onRefillLongFeed && (
                  <button
                    id="btn-recovery-refill-long"
                    type="button"
                    onClick={onRefillLongFeed}
                    disabled={!machineState.refillAwaitingConfirm}
                    className="flex items-center gap-1.5 rounded-lg bg-sky-600 hover:bg-sky-700 px-3.5 py-2 text-xs font-bold text-white disabled:opacity-40"
                  >
                    <Activity className="h-3.5 w-3.5" />
                    {t('btn_refill_confirm_long')}
                  </button>
                )}
                {recoveryStage === 'after_feed' && onRefillConfirm && (
                  <button
                    id="btn-recovery-next-cut"
                    type="button"
                    onClick={() => onRefillConfirm(true)}
                    disabled={!machineState.refillAwaitingConfirm}
                    className="flex items-center gap-1.5 rounded-lg bg-emerald-600 hover:bg-emerald-700 px-3.5 py-2 text-xs font-bold text-white disabled:opacity-40"
                  >
                    <Check className="h-3.5 w-3.5" />
                    {skipCut
                      ? t('btn_refill_confirm_continue')
                      : t('btn_refill_confirm_next_cut')}
                  </button>
                )}
                {recoveryStage === 'after_cut' && onRefillConfirm && (
                  <button
                    id="btn-recovery-asda-0"
                    type="button"
                    onClick={() => onRefillConfirm(true)}
                    disabled={!machineState.refillAwaitingConfirm}
                    className="flex items-center gap-1.5 rounded-lg bg-emerald-600 hover:bg-emerald-700 px-3.5 py-2 text-xs font-bold text-white disabled:opacity-40"
                  >
                    <Check className="h-3.5 w-3.5" />
                    {t('btn_refill_confirm_yes')}
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
                {(recoveryStage === 'review_piece' ||
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
                      : t('btn_recovery_review_ok')}
                  </button>
                )}
              </div>
            )}
            {showManualRefill && (
              <div className="flex flex-wrap items-center gap-3 rounded-lg border border-sky-300 dark:border-sky-700 bg-sky-50 dark:bg-sky-950/50 px-4 py-3">
                <div className="min-w-0 flex-1">
                  <p className="text-xs font-bold text-sky-900 dark:text-sky-100">
                    {machineState.refillPrompt === 'after_cut'
                      ? t('refill_confirm_title_cut')
                      : machineState.refillPrompt === 'working'
                        ? t('refill_confirm_title_working')
                        : machineState.refillPrompt === 'await_feed'
                          ? t('refill_confirm_title_await')
                          : t('refill_confirm_title_feed')}
                  </p>
                  <p className="text-[11px] text-sky-800/80 dark:text-sky-200/80 mt-0.5">
                    {machineState.refillPrompt === 'after_cut'
                      ? t('refill_confirm_hint_cut')
                      : machineState.refillPrompt === 'working'
                        ? t('refill_confirm_hint_working')
                        : machineState.refillPrompt === 'await_feed'
                          ? t('refill_confirm_hint_await')
                          : t('refill_confirm_hint_feed')}
                  </p>
                </div>
                {(machineState.refillPrompt === 'after_feed' ||
                  machineState.refillPrompt === 'await_feed') &&
                  onRefillRetry && (
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
                {(machineState.refillPrompt === 'after_feed' ||
                  machineState.refillPrompt === 'await_feed') &&
                  onRefillLongFeed && (
                  <button
                    id="btn-refill-confirm-long"
                    type="button"
                    onClick={onRefillLongFeed}
                    disabled={!machineState.refillAwaitingConfirm}
                    className="flex items-center gap-1.5 rounded-lg bg-sky-600 hover:bg-sky-700 px-3.5 py-2 text-xs font-bold text-white shadow-2xs active:scale-95 disabled:opacity-40 disabled:cursor-not-allowed disabled:active:scale-100"
                  >
                    <Activity className="h-3.5 w-3.5" />
                    {t('btn_refill_confirm_long')}
                  </button>
                )}
                {machineState.refillPrompt !== 'working' &&
                  machineState.refillPrompt !== 'await_feed' && (
                  <button
                    id="btn-refill-confirm-yes"
                    type="button"
                    onClick={() => onRefillConfirm(true)}
                    disabled={!machineState.refillAwaitingConfirm}
                    className="flex items-center gap-1.5 rounded-lg bg-emerald-600 hover:bg-emerald-700 px-3.5 py-2 text-xs font-bold text-white shadow-2xs active:scale-95 disabled:opacity-40 disabled:cursor-not-allowed disabled:active:scale-100"
                  >
                    <Check className="h-3.5 w-3.5" />
                    {machineState.refillPrompt === 'after_cut'
                      ? t('btn_refill_confirm_yes')
                      : t('btn_refill_confirm_next_cut')}
                  </button>
                )}
                {machineState.refillPrompt !== 'working' && (
                  <button
                    id="btn-refill-confirm-no"
                    type="button"
                    onClick={() => onRefillConfirm(false)}
                    disabled={!machineState.refillAwaitingConfirm}
                    className="flex items-center gap-1.5 rounded-lg border border-slate-300 dark:border-slate-600 bg-white dark:bg-slate-800 px-3.5 py-2 text-xs font-bold text-slate-700 dark:text-slate-200 shadow-2xs active:scale-95 disabled:opacity-40 disabled:cursor-not-allowed disabled:active:scale-100"
                  >
                    <X className="h-3.5 w-3.5" />
                    {t('btn_refill_confirm_no')}
                  </button>
                )}
              </div>
            )}
            </div>
          </div>
        </div>

      <div className="rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 p-4 sm:p-5 shadow-2xs transition-colors">
        <div className="flex items-center justify-between border-b border-slate-100 dark:border-slate-800 pb-2.5 gap-2 flex-wrap">
          <div className="flex items-center gap-2">
            <Layers
              className={`h-4 w-4 ${
                modulePanelError
                  ? 'text-red-600 dark:text-red-400'
                  : 'text-slate-500 dark:text-slate-400'
              }`}
            />
            <h2 className="text-xs font-bold uppercase tracking-wider text-slate-800 dark:text-slate-200">
              {t('tab_prefeeder')}
            </h2>
          </div>
        </div>

        {hasFault && modKind === 'other' && machineState.fault ? (
          <p className="mt-3 text-xs text-red-700 dark:text-red-300">
            {machineState.fault} — {t('module_recovery_use_machine')}
          </p>
        ) : null}
        {showPfCoach ? (
          <p className="mt-3 text-xs text-amber-800 dark:text-amber-200">
            {resumeEnabled
              ? t('pf_recover_hint_jog_resume')
              : t('pf_recover_hint_jog')}
          </p>
        ) : null}

        <div className="mt-4 grid gap-3 grid-cols-1">
          <div
            className={`rounded-lg border p-3 transition-colors ${
              pfHasError || pfLatched
                ? 'border-red-300 dark:border-red-800 bg-red-50/60 dark:bg-red-950/30'
                : 'border-slate-200 dark:border-slate-800 bg-slate-50/80 dark:bg-slate-800/40'
            }`}
          >
            <div className="rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 px-4 py-2.5 text-slate-800 dark:text-slate-200 shadow-2xs flex flex-wrap items-center justify-between gap-3 transition-colors">
              <div className="flex items-center gap-3 min-w-0">
                <div className="flex items-center gap-2 min-w-0">
                  <span
                    className={`h-2.5 w-2.5 rounded-full shrink-0 ${
                      !pfConnected || pfHasError || pfLatched
                        ? 'bg-red-500'
                        : pfMode === 'materialist'
                          ? 'bg-violet-500'
                          : pfMode === 'busy'
                            ? 'bg-emerald-500 animate-pulse'
                            : 'bg-emerald-500'
                    }`}
                  />
                  <span
                    className={`text-sm font-semibold tracking-tight truncate ${
                      pfHasError || pfLatched
                        ? 'text-red-700 dark:text-red-300'
                        : 'text-slate-900 dark:text-white'
                    }`}
                  >
                    {pfHeadline}
                  </span>
                </div>

                <span className="text-slate-300 dark:text-slate-700 hidden sm:inline">|</span>

                <div className="flex items-center gap-1.5 font-mono text-xs text-slate-600 dark:text-slate-400">
                  <span className="text-slate-400 dark:text-slate-500">{t('link_label')}:</span>
                  <span className={`font-semibold ${pfConnected ? 'text-emerald-700 dark:text-emerald-400' : 'text-red-600 dark:text-red-400'}`}>
                    {pfConnected ? t('node_connected') : t('node_disconnected')}
                  </span>
                </div>
              </div>

              <div className="flex items-center gap-3">
                <div className="flex items-center gap-1.5 text-xs font-mono">
                  <span className="text-slate-500 dark:text-slate-400">{t('feed_status')}:</span>
                  <span
                    className={`font-bold px-2 py-0.5 rounded-md border text-xs ${
                      pfMode === 'materialist'
                        ? 'bg-violet-50 dark:bg-violet-950/40 text-violet-800 dark:text-violet-200 border-violet-200 dark:border-violet-800'
                        : pfMode === 'busy'
                          ? 'bg-emerald-50 dark:bg-emerald-950/40 text-emerald-800 dark:text-emerald-300 border-emerald-200 dark:border-emerald-800'
                          : 'bg-slate-100 dark:bg-slate-800 text-slate-600 dark:text-slate-400 border-slate-200 dark:border-slate-700'
                    }`}
                  >
                    {pfFeedLabel}
                  </span>
                </div>
              </div>
            </div>

            <div className="mt-3 flex flex-wrap items-center gap-1.5">
              <button
                id="btn-main-pf-start"
                type="button"
                onClick={onPfStart}
                disabled={!onPfStart || !pfConnected}
                className={`${btnBase} disabled:opacity-40 ${
                  preFeederState.isRunning ? btnOn : btnIdle
                }`}
              >
                <Play className="h-3.5 w-3.5 fill-current" />
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
                className={`${btnBase} disabled:opacity-40 ${
                  jogLOn ? btnMatOn : btnMatIdle
                }`}
              >
                <Zap className="h-3.5 w-3.5" />
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
                className={`${btnBase} disabled:opacity-40 ${
                  jogROn ? btnMatOn : btnMatIdle
                }`}
              >
                <Zap className="h-3.5 w-3.5" />
                <span>{t('btn_jog')} R</span>
              </button>
            </div>
          </div>
        </div>
      </div>

      {showLogs && (
        <LogTerminal title={t('logs_title')} logs={logs} onClear={onClearLogs} filterModule="ALL" />
      )}
    </div>
  );
};
