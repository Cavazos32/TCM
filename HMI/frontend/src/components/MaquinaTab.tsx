import React, { useState, useEffect, useRef } from 'react';
import {
  Play,
  Square,
  RotateCcw,
  Sliders,
  Clock,
  Pause,
  Settings2,
  AlertTriangle,
  Droplets,
  Check,
  X,
  Home,
  Zap,
  Package,
  Activity,
} from 'lucide-react';
import { MachineState, LogEntry, MotionState, PfRefillChannel, PlcState, PreFeederState } from '../types';
import { LogTerminal } from './LogTerminal';
import { useApp } from '../context/AppContext';

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

function isFullErrorText(text: string): boolean {
  return /^E\d{3}\b/i.test(text.trim());
}

/** Estado corto para operador; EXXX completo solo en error. */
function operatorModuleStatus(
  statusText: string | undefined,
  hasError: boolean,
  connected: boolean,
  t: (key: 'node_disconnected' | 'module_status_ok' | 'state_error' | 'state_ready' | 'module_status_busy' | 'module_status_materialist' | 'pf_need_reset_start') => string,
  mode?: 'materialist' | 'busy' | null
): string {
  if (!connected) return t('node_disconnected');
  if (mode === 'materialist' && !hasError) {
    return t('module_status_materialist');
  }
  const raw = (statusText || '').trim();
  if (hasError) {
    if (/errorstate/i.test(raw) || /0x03c/i.test(raw)) {
      return t('pf_need_reset_start');
    }
    return raw || t('pf_need_reset_start');
  }
  if (mode === 'busy') {
    return t('module_status_busy');
  }
  const lower = raw.toLowerCase();
  if (!raw || lower.includes(' ok') || /(?:^|\b)ok(?:\b|$)/i.test(raw)) {
    return t('module_status_ok');
  }
  if (lower.includes('ocupado') || lower.includes('busy')) {
    return t('module_status_busy');
  }
  if (lower.includes('idle') || lower.includes('listo') || lower.includes('ready')) {
    return t('state_ready');
  }
  const cleaned = raw
    .replace(/^comando\s+[\w/-]+\s+/i, '')
    .replace(/\s*\(0x[0-9a-fA-F]+\)\s*/g, ' ')
    .replace(/\s+/g, ' ')
    .trim();
  if (!cleaned || /^ok$/i.test(cleaned)) return t('module_status_ok');
  if (isFullErrorText(cleaned)) return cleaned;
  return cleaned;
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
  onMachineHome?: () => void;
  onRefill?: () => void;
  onRefillConfirm?: (ok: boolean) => void;
  onRefillRetry?: () => void;
  onRefillLongFeed?: () => void;
  onRecoveryReview?: (ok: boolean) => void;
  onGotoCycle?: () => void;
  onPfStart?: () => void;
  onPfStop?: () => void;
  onPfReset?: () => void;
  onPfJogL?: () => void;
  onPfJogR?: () => void;
  onPfRefill?: (side: 'L' | 'R', channel: PfRefillChannel, on: boolean) => void;
  onBusy?: () => void;
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
  onPfStop,
  onPfReset,
  onPfJogL,
  onPfJogR,
  onPfRefill,
  onBusy,
  onMaterialist,
  showLogs = true,
  logs,
  onClearLogs,
}) => {
  const { t } = useApp();
  const [offsetInput, setOffsetInput] = useState(String(machineState.offsetMm ?? 0));
  const offsetDirtyRef = useRef(false);
  const offsetSaveTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);

  useEffect(() => {
    if (offsetDirtyRef.current) return;
    setOffsetInput(String(machineState.offsetMm ?? 0));
  }, [machineState.offsetMm]);

  useEffect(() => {
    return () => {
      if (offsetSaveTimerRef.current) clearTimeout(offsetSaveTimerRef.current);
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

  const offsetParsed = parseFloat(offsetInput);
  const linealTotalMm = Math.abs(machineState.mm) + (isNaN(offsetParsed) ? 0 : offsetParsed);

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
  const avgPieceSec = machineState.avgPieceSec ?? 0;
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
  const modulePanelError = !!preFeederState.hasError || !!plcState.hasError;
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
  const interlockError = !hasFault && machineState.statusKind === 'error';
  const generalStatus = hasFault
    ? faultLabel || t('state_error')
    : interlockError
      ? machineState.statusText || t('err_materialist_start')
      : machineState.isPaused
      ? t('state_paused')
      : machineState.isRunning
        ? t('state_producing')
        : machineState.statusText || t('state_ready');

  const btnBase =
    'flex items-center justify-center gap-1.5 rounded-lg border px-3 py-1.5 text-xs font-bold transition shadow-2xs active:scale-95';

  const toDisplayMm = (internalMm: number) => -internalMm;
  const activeValvesCount = plcState.valves.filter((v) => v.active).length;
  const plcConnected = plcState.connection.connected;
  const plcHasError = !!plcState.hasError && plcConnected;
  const pfConnected = preFeederState.connection.connected;
  // Module Controls keeps independent PF diagnostics; machine RESET owns the global EXXX latch.
  const pfResetDisabled = !onPfReset || !pfConnected;
  const recoveryStage =
    machineState.recoveryPrompt ||
    (machineState.recoveryAfterError || machineState.e050Lot
      ? machineState.refillPrompt ||
        (machineState.refillActive ? 'working' : '')
      : '');
  const showRecoveryTrack = !!recoveryStage;
  const e050Lot = !!machineState.e050Lot;
  const skipCut = !!machineState.refillSkipCut;
  const lotHeld =
    machineState.cycleActive ||
    machineState.recoveryAfterError ||
    machineState.isPaused;

  const processStep: string = e050Lot
    ? recoveryStage === 'e050_insufficient' ||
      recoveryStage === 'e050_finish_process'
      ? 'ask'
      : recoveryStage === 'e050_empty_material' ||
          ((recoveryStage === 'working' || recoveryStage === 'after_feed') &&
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

  const recoverySteps: { id: string; label: string }[] = [
    { id: 'reset', label: t('lot_recover_step_reset') },
    { id: 'resume', label: t('lot_recover_step_resume') },
    { id: 'piece', label: t('lot_recover_step_piece') },
    { id: 'review', label: t('lot_recover_step_review') },
    { id: 'purge', label: t('lot_recover_step_purge') },
    { id: 'continue', label: t('lot_recover_step_continue') },
  ];
  const e050Steps: { id: string; label: string }[] = [
    { id: 'ask', label: t('lot_recover_step_ask') },
    { id: 'reset', label: t('lot_recover_step_reset') },
    { id: 'resume', label: t('lot_recover_step_resume') },
    { id: 'piece', label: t('lot_recover_step_piece') },
    { id: 'review', label: t('lot_recover_step_review') },
    { id: 'empty', label: t('lot_recover_step_empty') },
  ];
  const processSteps = e050Lot ? e050Steps : recoverySteps;

  const showManualRefill =
    !machineState.recoveryAfterError &&
    !machineState.e050Lot &&
    !!machineState.refillActive &&
    !!machineState.refillPrompt &&
    !!onRefillConfirm;
  const recoveryHint =
    recoveryStage === 'e050_insufficient'
      ? t('e050_insufficient_hint')
      : recoveryStage === 'e050_finish_process'
        ? t('e050_finish_hint')
        : recoveryStage === 'e050_empty_material'
          ? t('e050_empty_hint')
          : recoveryStage === 'review_piece'
            ? t('recovery_review_hint')
            : recoveryStage === 'continue_cycle'
              ? t('recovery_continue_hint')
              : recoveryStage === 'after_cut'
                ? t('refill_confirm_hint_cut')
                : recoveryStage === 'working'
                  ? t('refill_confirm_hint_working')
                  : skipCut
                    ? t('refill_confirm_hint_feed_nocut')
                    : t('refill_confirm_hint_feed');
  const processHint = e050Lot
    ? processStep === 'ask'
      ? recoveryStage === 'e050_finish_process'
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
      ? recoverReset
        ? t('lot_recover_hint_reset_pf')
        : lotHeld
          ? t('lot_recover_hint_reset')
          : t('lot_recover_hint_reset_idle')
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
    !machineState.isRunning &&
    ((recoverReset && !lotHeld) || machineState.cycleMaterialist);
  const startDisabled =
    hasFault ||
    machineState.isRunning ||
    machineState.refillActive ||
    recoverReset;
  const resumeDisabled =
    hasFault ||
    !resumeEnabled ||
    machineState.refillAwaitingConfirm ||
    machineState.recoveryAwaitingConfirm ||
    machineState.refillActive ||
    recoverReset;
  // Error activo: no producir. Tras Reset, Materialist + JOG deben poder alimentar.
  const pfProdLocked = recoverReset;
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

  const moduleCard = (
    title: string,
    statusText: string | undefined,
    connected: boolean,
    hasError: boolean,
    actions: React.ReactNode,
    mode?: 'materialist' | 'busy' | null
  ) => {
    const showError = hasError && connected;
    return (
      <div
        className={`rounded-lg border p-3 transition-colors ${
          showError
            ? 'border-red-300 dark:border-red-800 bg-red-50/60 dark:bg-red-950/30'
            : 'border-slate-200 dark:border-slate-800 bg-slate-50/80 dark:bg-slate-800/40'
        }`}
      >
        <div className="flex items-center justify-between gap-2 mb-2">
          <div className="flex items-center gap-2 min-w-0">
            <span
              className={`h-2 w-2 shrink-0 rounded-full ${
                showError
                  ? 'bg-red-500'
                  : connected
                    ? 'bg-emerald-500'
                    : 'bg-slate-400'
              }`}
            />
            <h3 className="text-xs font-bold uppercase tracking-wider text-slate-800 dark:text-slate-200 truncate">
              {title}
            </h3>
          </div>
        </div>
        <p
          className={`text-[11px] mb-3 line-clamp-2 min-h-[2rem] ${
            showError
              ? 'text-red-700 dark:text-red-300 font-medium'
              : 'text-slate-600 dark:text-slate-400'
          }`}
        >
          {operatorModuleStatus(statusText, showError, connected, t, mode)}
        </p>
        <div className="flex flex-wrap items-center gap-1.5">{actions}</div>
      </div>
    );
  };

  const pfJogCard = (side: 'L' | 'R') => {
    const refill = (side === 'L' ? preFeederState.refillL : preFeederState.refillR) || {
      material: false,
      dereeler: false,
      servo: false,
      feeder: false,
    };
    const enabled =
      !!onPfRefill &&
      pfInMaterialist &&
      preFeederState.connection.connected &&
      !machineState.cycleActive;
    const channels: { id: PfRefillChannel; label: 'pf_refill_material' | 'pf_refill_dereeler' | 'pf_refill_servo' | 'pf_refill_feeder' }[] = [
      { id: 'material', label: 'pf_refill_material' },
      { id: 'dereeler', label: 'pf_refill_dereeler' },
      { id: 'servo', label: 'pf_refill_servo' },
      { id: 'feeder', label: 'pf_refill_feeder' },
    ];
    return (
      <div
        key={side}
        className="rounded-lg border border-slate-200 dark:border-slate-800 bg-slate-50/80 dark:bg-slate-800/40 p-3 min-w-0"
      >
        <div className="flex items-center justify-between gap-2 mb-2">
          <h3 className="text-xs font-bold uppercase tracking-wider text-slate-800 dark:text-slate-200">
            {side === 'L' ? t('pf_jog_l') : t('pf_jog_r')}
          </h3>
        </div>
        <p className="text-[10px] text-slate-500 dark:text-slate-400 mb-3">
          {enabled ? t('pf_refill_hint') : t('pf_jog_need_materialist')}
        </p>
        <div className="grid grid-cols-2 gap-1.5">
          {channels.map((ch) => {
            const on = refill[ch.id];
            return (
              <button
                key={ch.id}
                id={`btn-main-pf-refill-${side.toLowerCase()}-${ch.id}`}
                type="button"
                onClick={() => onPfRefill?.(side, ch.id, !on)}
                disabled={!enabled}
                className={`${btnBase} ${
                  on
                    ? 'border-amber-400 bg-amber-100 dark:bg-amber-950/50 text-amber-900 dark:text-amber-200'
                    : 'border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 text-slate-700 dark:text-slate-200 hover:bg-slate-50'
                } disabled:opacity-40`}
              >
                <span>{t(ch.label)}</span>
              </button>
            );
          })}
        </div>
      </div>
    );
  };

  return (
    <div className="space-y-4">
      <div className="rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 px-4 py-2.5 text-slate-800 dark:text-slate-200 shadow-2xs flex flex-wrap items-center justify-between gap-3 transition-colors">
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
                : machineState.cycleBusy
                ? 'bg-emerald-500'
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
            {hasFault ? t('state_error') : generalStatus}
          </span>
          {hasFault && faultLabel ? (
            <span className="rounded border border-red-300 dark:border-red-800 bg-red-50 dark:bg-red-950/50 px-2 py-0.5 font-mono text-[11px] font-semibold text-red-700 dark:text-red-300 truncate max-w-[min(100%,36rem)]">
              {faultLabel}
            </span>
          ) : null}
          {!hasFault && machineState.lastFault ? (
            <span
              className="rounded border border-slate-300 dark:border-slate-600 bg-slate-50 dark:bg-slate-800 px-2 py-0.5 font-mono text-[11px] font-semibold text-slate-700 dark:text-slate-200 truncate max-w-[min(100%,36rem)]"
              title={machineState.lastFault}
            >
              {t('lot_recover_last_error')}: {machineState.lastFault}
            </span>
          ) : null}
          {showErrorProcess && (
            <span className="rounded border border-amber-300 dark:border-amber-800 bg-amber-50 dark:bg-amber-950/50 px-2 py-0.5 text-[11px] font-semibold text-amber-900 dark:text-amber-100">
              {processHint}
            </span>
          )}
          {machineState.cycleCompleted && !hasFault && (
            <span className="text-xs font-mono text-emerald-700 dark:text-emerald-300 bg-emerald-50 dark:bg-emerald-950/50 border border-emerald-200 dark:border-emerald-800 px-2 py-0.5 rounded">
              {t('cycle_complete')}
            </span>
          )}
          {machineState.cycleActive && machineState.cycleStep > 0 && (
            <span className="text-xs font-mono text-teal-700 dark:text-teal-300 bg-teal-50 dark:bg-teal-950/50 border border-teal-200 dark:border-teal-800 px-2 py-0.5 rounded">
              {t('cycle_step_status').replace('{step}', String(machineState.cycleStep))}
              {machineState.cycleStepLabel ? ` · ${machineState.cycleStepLabel}` : ''}
            </span>
          )}
        </div>
        <div className="flex items-center gap-4 text-xs font-mono flex-wrap">
          <div>
            <span className="text-slate-500 dark:text-slate-400">{t('mm_rpm_label')}: </span>
            <span className="text-slate-800 dark:text-slate-200 font-semibold">
              {(-machineState.mm).toFixed(1)} mm @ {machineState.rpm} RPM
            </span>
          </div>
          <div className="flex items-center gap-1.5">
            <span className="text-slate-500 dark:text-slate-400">{t('pieces')}: </span>
            <span className="text-emerald-700 dark:text-emerald-300 font-semibold bg-emerald-50 dark:bg-emerald-950/50 border border-emerald-200 dark:border-emerald-800 px-2 py-0.5 rounded text-xs">
              {machineState.piecesCount} / {machineState.targetPieces}
            </span>
            {machineState.cycleActive && (
              <span className="text-[10px] font-mono text-slate-500 dark:text-slate-400">
                (en curso {Math.min(machineState.piecesCount + 1, machineState.targetPieces)})
              </span>
            )}
            <span className="font-bold text-slate-700 dark:text-slate-300">
              ({progressPercentage}%)
            </span>
          </div>
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

        <div className="mt-4 flex flex-col lg:flex-row gap-4 lg:gap-5 items-stretch">
          <div className="flex-1 min-w-0 space-y-4">
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
                <p
                  className="text-[10px] font-mono text-slate-500 dark:text-slate-400"
                  title={t('cfg_cut_offset_hint')}
                >
                  {t('cfg_cut_lineal_total')}: {linealTotalMm.toFixed(1)} mm
                </p>
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
              {(machineState.cycleActive || elapsedSec > 0 || lastPieceSec > 0) && (
                <div className="flex flex-wrap items-center gap-x-3 gap-y-1 text-[10px] font-mono text-slate-600 dark:text-slate-400">
                  <span title={t('cycle_time_hint')}>
                    {t('cycle_time_label')}:{' '}
                    <strong className="text-teal-700 dark:text-teal-300">
                      {fmtSec(elapsedSec)}
                    </strong>
                    {machineState.cycleActive && etaSec > 0 ? ` · ~${fmtSec(etaSec)}` : ''}
                  </span>
                  {lastPieceSec > 0 && (
                    <span>
                      {t('cycle_time_piece')}:{' '}
                      <strong className="text-slate-800 dark:text-slate-200">
                        {lastPieceSec.toFixed(1)}s
                      </strong>
                    </span>
                  )}
                  {avgPieceSec > 0 && (machineState.targetPieces || 0) > 1 && (
                    <span>
                      {t('cycle_time_avg')}:{' '}
                      <strong className="text-slate-800 dark:text-slate-200">
                        {avgPieceSec.toFixed(1)}s
                      </strong>
                    </span>
                  )}
                </div>
              )}
            </div>

            {showErrorProcess && (
              <div className="flex flex-wrap items-center gap-3 rounded-lg border border-amber-300 dark:border-amber-700 bg-amber-50 dark:bg-amber-950/50 px-4 py-3">
                <div className="min-w-0 flex-1">
                  <p className="text-xs font-bold text-amber-950 dark:text-amber-100">
                    {e050Lot
                      ? t('lot_recover_title_e050')
                      : t('lot_recover_title_recovery')}
                  </p>
                  <p className="text-[11px] text-amber-900/80 dark:text-amber-200/80 mt-0.5">
                    {processHint}
                  </p>
                  {(machineState.fault || machineState.lastFault) && (
                    <p className="mt-1 font-mono text-[11px] font-semibold text-red-800 dark:text-red-200">
                      {machineState.fault || machineState.lastFault}
                    </p>
                  )}
                  <p className="mt-1 font-mono text-[10px] text-amber-800 dark:text-amber-200 flex flex-wrap gap-x-1">
                    {processSteps.map((s, i) => (
                      <span key={s.id}>
                        {i > 0 ? ' → ' : ''}
                        <span
                          className={
                            s.id === processStep ? 'font-bold underline' : 'opacity-50'
                          }
                        >
                          {s.label}
                        </span>
                      </span>
                    ))}
                  </p>
                </div>
                {recoveryStage === 'after_feed' && onRefillRetry && (
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
                {recoveryStage === 'after_feed' && onRefillLongFeed && (
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
                {(recoveryStage === 'e050_insufficient' ||
                  recoveryStage === 'e050_finish_process' ||
                  recoveryStage === 'e050_empty_material') &&
                  onRecoveryReview && (
                  <>
                    <button
                      id="btn-recovery-e050-yes"
                      type="button"
                      onClick={() => onRecoveryReview(true)}
                      disabled={!machineState.recoveryAwaitingConfirm}
                      className="flex items-center gap-1.5 rounded-lg bg-emerald-600 hover:bg-emerald-700 px-3.5 py-2 text-xs font-bold text-white disabled:opacity-40"
                    >
                      <Check className="h-3.5 w-3.5" />
                      {t('btn_e050_yes')}
                    </button>
                    <button
                      id="btn-recovery-e050-omit"
                      type="button"
                      onClick={() => onRecoveryReview(false)}
                      disabled={!machineState.recoveryAwaitingConfirm}
                      className="flex items-center gap-1.5 rounded-lg border border-slate-300 dark:border-slate-600 bg-white dark:bg-slate-800 px-3.5 py-2 text-xs font-bold text-slate-700 dark:text-slate-200 disabled:opacity-40"
                    >
                      <X className="h-3.5 w-3.5" />
                      {t('btn_e050_omit')}
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
                        : t('refill_confirm_title_feed')}
                  </p>
                  <p className="text-[11px] text-sky-800/80 dark:text-sky-200/80 mt-0.5">
                    {machineState.refillPrompt === 'after_cut'
                      ? t('refill_confirm_hint_cut')
                      : machineState.refillPrompt === 'working'
                        ? t('refill_confirm_hint_working')
                        : t('refill_confirm_hint_feed')}
                  </p>
                </div>
                {machineState.refillPrompt === 'after_feed' && onRefillRetry && (
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
                {machineState.refillPrompt === 'after_feed' && onRefillLongFeed && (
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
                {machineState.refillPrompt !== 'working' && (
                  <>
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
                  </>
                )}
              </div>
            )}
          </div>

          <div className="w-full lg:w-40 shrink-0 flex flex-col gap-2 border-t lg:border-t-0 lg:border-l border-slate-100 dark:border-slate-800 pt-4 lg:pt-0 lg:pl-4">
            {showErrorProcess && (
              <div className="rounded-md border border-amber-300 dark:border-amber-800 bg-amber-50 dark:bg-amber-950/40 px-2 py-1.5 text-[10px] leading-snug text-amber-900 dark:text-amber-100">
                <p className="font-bold uppercase tracking-wide">
                  {e050Lot
                    ? t('lot_recover_title_e050')
                    : t('lot_recover_title_c2')}
                </p>
                <p className="mt-0.5">{processHint}</p>
                {(machineState.fault || machineState.lastFault) && (
                  <p className="mt-1 font-mono font-semibold text-red-800 dark:text-red-200">
                    {machineState.fault || machineState.lastFault}
                  </p>
                )}
                <p className="mt-1 font-mono text-[10px] text-amber-800 dark:text-amber-200">
                  {processSteps.map((s, i) => (
                    <span key={s.id}>
                      {i > 0 ? ' → ' : ''}
                      <span
                        className={
                          s.id === processStep ? 'font-bold underline' : 'opacity-50'
                        }
                      >
                        {s.label}
                      </span>
                    </span>
                  ))}
                </p>
              </div>
            )}
            {showPfCoach && (
              <div className="rounded-md border border-amber-300 dark:border-amber-800 bg-amber-50 dark:bg-amber-950/40 px-2 py-1.5 text-[10px] leading-snug text-amber-900 dark:text-amber-100">
                <p className="font-bold uppercase tracking-wide">
                  {recoverReset ? t('pf_recover_title') : t('status_materialist')}
                </p>
                <p className="mt-0.5">
                  {recoverReset
                    ? t('pf_recover_hint_reset')
                    : resumeEnabled
                      ? t('pf_recover_hint_jog_resume')
                      : t('pf_recover_hint_jog')}
                </p>
                {recoverReset && (
                  <p className="mt-1 font-mono text-[10px] text-amber-800 dark:text-amber-200">
                    <span className="font-bold underline">{t('pf_recover_step_reset')}</span>
                    {' → '}
                    <span className="opacity-50">{t('pf_recover_step_setup')}</span>
                    {' → '}
                    <span className="opacity-50">
                      {resumeEnabled ? t('pf_recover_step_resume') : t('pf_recover_step_start')}
                    </span>
                  </p>
                )}
              </div>
            )}
            <button
              id="btn-start-maquina"
              onClick={onStart}
              disabled={startDisabled}
              title={
                recoverReset
                  ? t('pf_recover_hint_reset')
                  : showErrorProcess
                    ? processHint
                    : machineState.cycleMaterialist
                      ? t('err_materialist_start')
                      : undefined
              }
              className={`flex w-full items-center justify-center gap-1.5 rounded-lg px-3 py-2.5 text-xs font-bold tracking-wide transition-all shadow-2xs ${
                startDisabled
                  ? 'bg-slate-100 dark:bg-slate-800 text-slate-400 cursor-not-allowed border border-slate-200 dark:border-slate-700'
                  : processStep === 'start'
                    ? 'bg-emerald-600 hover:bg-emerald-700 text-white ring-2 ring-emerald-300 ring-offset-1 animate-pulse'
                    : 'bg-emerald-600 hover:bg-emerald-700 text-white active:scale-[0.98]'
              }`}
            >
              <Play className="h-3.5 w-3.5 fill-current" />
              <span>{t('btn_start')}</span>
            </button>

            <button
              id="btn-pause-maquina"
              onClick={onPause}
              disabled={!machineState.pauseEnabled || pfProdLocked}
              className="flex w-full items-center justify-center gap-1.5 rounded-lg border border-amber-200 dark:border-amber-800 bg-amber-50 dark:bg-amber-950/40 hover:bg-amber-100 px-3 py-2.5 text-xs font-semibold text-amber-800 dark:text-amber-200 transition shadow-2xs disabled:opacity-40 disabled:cursor-not-allowed"
            >
              <Pause className="h-3.5 w-3.5" />
              <span>{t('btn_pause')}</span>
            </button>

            <button
              id="btn-stop-maquina"
              onClick={onStop}
              className="flex w-full items-center justify-center gap-1.5 rounded-lg border border-red-200 dark:border-red-900/60 bg-red-50 dark:bg-red-950/40 hover:bg-red-100 px-3 py-2.5 text-xs font-bold text-red-700 dark:text-red-300 transition active:scale-[0.98] shadow-2xs"
            >
              <Square className="h-3.5 w-3.5 fill-current text-red-600" />
              <span>{t('btn_stop')}</span>
            </button>

            <button
              id="btn-reset-maquina"
              onClick={onReset}
              disabled={machineResetDisabled}
              title={
                recoverReset
                  ? t('pf_recover_hint_reset')
                  : showMachineResetCoach
                    ? t('lot_recover_hint_reset')
                    : undefined
              }
              className={`flex w-full items-center justify-center gap-1.5 rounded-lg border px-3 py-2.5 text-xs font-semibold transition shadow-2xs disabled:opacity-40 disabled:cursor-not-allowed ${
                showMachineResetCoach && !machineResetDisabled
                  ? 'border-amber-400 bg-amber-100 dark:bg-amber-950/60 text-amber-950 dark:text-amber-100 ring-2 ring-amber-300 ring-offset-1 animate-pulse'
                  : 'border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 hover:bg-slate-50 text-slate-700 dark:text-slate-200'
              }`}
            >
              <RotateCcw className="h-3.5 w-3.5 text-amber-600" />
              <span>{t('btn_reset_cycle')}</span>
            </button>

            <button
              id="btn-reanudar-maquina"
              onClick={onResume}
              disabled={resumeDisabled}
              title={
                recoverReset
                  ? t('pf_recover_hint_reset')
                  : showMachineResumeCoach
                    ? t('lot_recover_hint_resume')
                    : showMachineResetCoach
                      ? t('lot_recover_hint_reset')
                      : undefined
              }
              className={`group flex w-full items-center justify-center gap-1.5 rounded-lg border px-3 py-2.5 text-xs font-semibold transition shadow-2xs disabled:opacity-40 disabled:cursor-not-allowed ${
                recoverGoResume || showMachineResumeCoach
                  ? 'border-emerald-400 bg-emerald-100 dark:bg-emerald-950/50 text-emerald-900 dark:text-emerald-100 ring-2 ring-emerald-300 ring-offset-1 animate-pulse'
                  : 'border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 hover:bg-slate-50 text-slate-700 dark:text-slate-200'
              }`}
            >
              <RotateCcw className="h-3.5 w-3.5 group-hover:rotate-45 transition-transform" />
              <span>
                {machineState.stepByStep && machineState.isPaused
                  ? t('btn_next_step')
                  : t('btn_resume')}
              </span>
            </button>

            <div className="border-t border-slate-200 dark:border-slate-700 my-1" />

            {onRefill && (
              <button
                id="btn-refill-maquina"
                type="button"
                onClick={onRefill}
                disabled={machineState.isRunning || machineState.refillActive}
                title={t('refill_helpers_subtitle')}
                className={`flex w-full items-center justify-center gap-1.5 rounded-lg border px-3 py-2.5 text-xs font-bold transition shadow-2xs ${
                  machineState.isRunning || machineState.refillActive
                    ? 'border-slate-200 dark:border-slate-700 bg-slate-100 dark:bg-slate-800 text-slate-400 cursor-not-allowed'
                    : 'border-sky-300 dark:border-sky-800 bg-sky-50 dark:bg-sky-950/40 text-sky-800 dark:text-sky-200 hover:bg-sky-100 dark:hover:bg-sky-900/50 active:scale-[0.98]'
                }`}
              >
                <Droplets className="h-3.5 w-3.5" />
                <span>{t('btn_refill')}</span>
              </button>
            )}

            <button
              id="btn-home-maquina"
              type="button"
              onClick={onMachineHome}
              disabled={!onMachineHome || machineState.isRunning}
              title={t('btn_machine_home_hint')}
              className={`flex w-full items-center justify-center gap-1.5 rounded-lg border px-3 py-2.5 text-xs font-bold transition shadow-2xs ${
                !onMachineHome || machineState.isRunning
                  ? 'border-slate-200 dark:border-slate-700 bg-slate-100 dark:bg-slate-800 text-slate-400 cursor-not-allowed'
                  : 'border-emerald-300 dark:border-emerald-800 bg-emerald-50 dark:bg-emerald-950/40 text-emerald-800 dark:text-emerald-200 hover:bg-emerald-100 dark:hover:bg-emerald-900/50 active:scale-[0.98]'
              }`}
            >
              <Home className="h-3.5 w-3.5" />
              <span>{t('btn_machine_home')}</span>
            </button>

            <button
              id="btn-main-pf-materialist"
              type="button"
              onClick={onMaterialist}
              disabled={!onMaterialist || machineState.cycleActive}
              title={
                machineState.cycleMaterialist
                  ? t('pf_recover_hint_jog')
                  : t('pf_jog_need_materialist')
              }
              className={`flex w-full items-center justify-center gap-1.5 rounded-lg border px-3 py-2.5 text-xs font-bold transition shadow-2xs ${
                !onMaterialist || machineState.cycleActive
                  ? 'border-slate-200 dark:border-slate-700 bg-slate-100 dark:bg-slate-800 text-slate-400 cursor-not-allowed'
                  : machineState.cycleMaterialist
                    ? 'border-violet-400 bg-violet-100 dark:bg-violet-950/50 text-violet-900 dark:text-violet-100 ring-2 ring-violet-300/80 ring-offset-1 dark:ring-offset-slate-900'
                    : 'border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 hover:bg-slate-50 dark:hover:bg-slate-700 text-slate-700 dark:text-slate-200 active:scale-[0.98]'
              }`}
            >
              <Package className="h-3.5 w-3.5" />
              <span>{t('btn_materialist_cycle')}</span>
            </button>
          </div>
        </div>
      </div>

      {/* Estados de módulo — debajo de Control de máquina */}
      <div className="space-y-2">
        <div className="rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 px-4 py-2.5 text-slate-800 dark:text-slate-200 shadow-2xs flex flex-wrap items-center justify-between gap-3 transition-colors">
          <div className="flex items-center gap-3 min-w-0">
            <span className="rounded bg-slate-100 dark:bg-slate-800 border border-slate-200 dark:border-slate-700 px-2 py-0.5 font-mono text-[10px] font-bold uppercase tracking-wider text-slate-600 dark:text-slate-400 shrink-0">
              {t('tab_motion')}
            </span>
            <div className="flex items-center gap-2 min-w-0">
              <span
                className={`h-2.5 w-2.5 rounded-full shrink-0 ${
                  !motionState.connection.connected || motionState.hasError
                    ? 'bg-red-500'
                    : motionState.isMoving
                      ? 'bg-amber-500 animate-ping'
                      : 'bg-emerald-500'
                }`}
              />
              <span className="text-sm font-semibold text-slate-900 dark:text-white tracking-tight truncate">
                {motionState.statusText || (motionState.isMoving ? t('motor_moving') : t('state_ready'))}
              </span>
            </div>

            <span className="text-slate-300 dark:text-slate-700 hidden sm:inline">|</span>

            <div className="flex items-center gap-1.5 font-mono text-xs text-slate-600 dark:text-slate-400">
              <span className="text-slate-400 dark:text-slate-500">{t('link_label')}:</span>
              <span className={`font-semibold ${motionState.connection.connected ? 'text-emerald-700 dark:text-emerald-400' : 'text-red-600 dark:text-red-400'}`}>
                {motionState.connection.connected ? t('node_connected') : t('node_disconnected')}
              </span>
              <span className="text-slate-400 dark:text-slate-500 text-[11px]">
                ({motionState.connection.ip}:{motionState.connection.port})
              </span>
            </div>
          </div>

          <div className="flex items-center gap-3">
            <div className="flex items-center gap-1.5 text-xs font-mono">
              <span className="text-slate-500 dark:text-slate-400">{t('current_position')}:</span>
              <span className="font-bold text-slate-900 dark:text-white bg-slate-100 dark:bg-slate-800 px-2 py-0.5 rounded-md border border-slate-200 dark:border-slate-700">
                {toDisplayMm(motionState.currentPositionMm).toFixed(2)} mm
              </span>
            </div>
          </div>
        </div>

        <div className="rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 px-4 py-2.5 text-slate-800 dark:text-slate-200 shadow-2xs flex flex-wrap items-center justify-between gap-3 transition-colors">
          <div className="flex items-center gap-3 min-w-0">
            <span className="rounded bg-slate-100 dark:bg-slate-800 border border-slate-200 dark:border-slate-700 px-2 py-0.5 font-mono text-[10px] font-bold uppercase tracking-wider text-slate-600 dark:text-slate-400 shrink-0">
              {t('tab_plc')}
            </span>
            <div className="flex items-center gap-2 min-w-0">
              <span
                className={`h-2.5 w-2.5 rounded-full shrink-0 ${
                  !plcConnected || plcHasError
                    ? 'bg-red-500'
                    : 'bg-emerald-500 animate-pulse'
                }`}
              />
              <span
                className={`text-sm font-semibold tracking-tight truncate ${
                  plcHasError
                    ? 'text-red-700 dark:text-red-300'
                    : 'text-slate-900 dark:text-white'
                }`}
              >
                {plcState.statusText || (activeValvesCount > 0
                  ? `${activeValvesCount} ${t('valves_active')}`
                  : t('state_ready'))}
              </span>
            </div>

            <span className="text-slate-300 dark:text-slate-700 hidden sm:inline">|</span>

            <div className="flex items-center gap-1.5 font-mono text-xs text-slate-600 dark:text-slate-400">
              <span className="text-slate-400 dark:text-slate-500">{t('link_label')}:</span>
              <span className={`font-semibold ${plcConnected ? 'text-emerald-700 dark:text-emerald-400' : 'text-red-600 dark:text-red-400'}`}>
                {plcConnected ? t('node_connected') : t('node_disconnected')}
              </span>
              <span className="text-slate-400 dark:text-slate-500 text-[11px]">
                ({plcState.connection.ip}:{plcState.connection.port})
              </span>
            </div>
          </div>

          <div className="flex items-center gap-3">
            <div className="flex items-center gap-1.5 text-xs font-mono">
              <span className="text-slate-500 dark:text-slate-400">{t('valves_status')}:</span>
              <span className="font-bold text-slate-900 dark:text-white bg-slate-100 dark:bg-slate-800 px-2 py-0.5 rounded-md border border-slate-200 dark:border-slate-700">
                {activeValvesCount} / {plcState.valves.length} ON
              </span>
            </div>
          </div>
        </div>

        <div className="rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 px-4 py-2.5 text-slate-800 dark:text-slate-200 shadow-2xs flex flex-wrap items-center justify-between gap-3 transition-colors">
          <div className="flex items-center gap-3 min-w-0">
            <span className="rounded bg-slate-100 dark:bg-slate-800 border border-slate-200 dark:border-slate-700 px-2 py-0.5 font-mono text-[10px] font-bold uppercase tracking-wider text-slate-600 dark:text-slate-400 shrink-0">
              {t('tab_prefeeder')}
            </span>
            <div className="flex items-center gap-2 min-w-0">
              <span
                className={`h-2.5 w-2.5 rounded-full shrink-0 ${
                  !pfConnected
                    ? 'bg-red-500'
                    : pfHasError
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
                  pfHasError
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
              <span className="text-slate-400 dark:text-slate-500 text-[11px]">
                ({preFeederState.connection.ip}:{preFeederState.connection.port})
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
      </div>

      <div className="rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 p-4 sm:p-5 shadow-2xs transition-colors">
        <div className="flex items-center justify-between border-b border-slate-100 dark:border-slate-800 pb-2.5 gap-2 flex-wrap">
          <div className="flex items-center gap-2">
            <AlertTriangle
              className={`h-4 w-4 ${
                modulePanelError
                  ? 'text-red-600 dark:text-red-400'
                  : 'text-slate-500 dark:text-slate-400'
              }`}
            />
            <h2 className="text-xs font-bold uppercase tracking-wider text-slate-800 dark:text-slate-200">
              {t('module_controls')}
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
            {recoverReset
              ? t('pf_recover_hint_reset')
              : resumeEnabled
                ? t('pf_recover_hint_jog_resume')
                : t('pf_recover_hint_jog')}
          </p>
        ) : null}

        <div className="mt-4 grid gap-3 grid-cols-1 lg:grid-cols-3">
          {moduleCard(
            t('tab_prefeeder'),
            preFeederState.statusText,
            preFeederState.connection.connected,
            !!preFeederState.hasError,
            <>
              <button
                id="btn-main-pf-start"
                type="button"
                onClick={onPfStart}
                disabled={!onPfStart || !pfConnected}
                className={`${btnBase} ${
                  recoverGo && !resumeEnabled
                    ? 'border-emerald-400 bg-emerald-100 dark:bg-emerald-950/50 text-emerald-900 dark:text-emerald-100 ring-2 ring-emerald-300 ring-offset-1'
                    : 'border-emerald-300 dark:border-emerald-800 bg-emerald-50 dark:bg-emerald-950/40 text-emerald-800 dark:text-emerald-200 hover:bg-emerald-100'
                } disabled:opacity-40`}
              >
                <Play className="h-3.5 w-3.5 fill-current" />
                <span>{t('btn_start')}</span>
              </button>
              <button
                id="btn-main-pf-stop"
                type="button"
                onClick={onPfStop}
                disabled={!onPfStop}
                className={`${btnBase} border-red-200 dark:border-red-900/60 bg-red-50 dark:bg-red-950/40 text-red-700 dark:text-red-300 hover:bg-red-100 disabled:opacity-40`}
              >
                <Square className="h-3.5 w-3.5 fill-current" />
                <span>{t('btn_stop')}</span>
              </button>
              <button
                id="btn-main-pf-reset"
                type="button"
                onClick={onPfReset}
                disabled={pfResetDisabled}
                title={
                  recoverReset || pfHasError
                    ? t('pf_recover_hint_reset')
                    : undefined
                }
                className={`${btnBase} ${
                  recoverReset || pfHasError
                    ? 'border-amber-400 bg-amber-100 dark:bg-amber-950/60 text-amber-950 dark:text-amber-100 ring-2 ring-amber-300 ring-offset-1 animate-pulse'
                    : 'border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 text-slate-700 dark:text-slate-200 hover:bg-slate-50'
                } disabled:opacity-40`}
              >
                <RotateCcw className="h-3.5 w-3.5 text-amber-600" />
                <span>{t('btn_reset')}</span>
              </button>
              <button
                id="btn-main-pf-jog-l"
                type="button"
                onClick={onPfJogL}
                disabled={!onPfJogL || !preFeederState.connection.connected}
                className={`${btnBase} border-sky-200 dark:border-sky-900/60 bg-sky-50 dark:bg-sky-950/40 text-sky-800 dark:text-sky-300 hover:bg-sky-100 disabled:opacity-40`}
              >
                <Zap className="h-3.5 w-3.5" />
                <span>{t('btn_jog')} L</span>
              </button>
              <button
                id="btn-main-pf-jog-r"
                type="button"
                onClick={onPfJogR}
                disabled={!onPfJogR || !preFeederState.connection.connected}
                className={`${btnBase} border-violet-200 dark:border-violet-900/60 bg-violet-50 dark:bg-violet-950/40 text-violet-800 dark:text-violet-300 hover:bg-violet-100 disabled:opacity-40`}
              >
                <Zap className="h-3.5 w-3.5" />
                <span>{t('btn_jog')} R</span>
              </button>
              <button
                id="btn-main-pf-busy"
                type="button"
                onClick={onBusy}
                disabled={!onBusy || machineState.cycleActive || pfProdLocked}
                className={`${btnBase} ${
                  machineState.cycleBusy
                    ? 'border-emerald-400 bg-emerald-100 dark:bg-emerald-950/50 text-emerald-900 dark:text-emerald-200'
                    : 'border-emerald-200 dark:border-emerald-900/60 bg-emerald-50 dark:bg-emerald-950/40 text-emerald-800 dark:text-emerald-300 hover:bg-emerald-100'
                } disabled:opacity-40`}
              >
                <Activity className="h-3.5 w-3.5" />
                <span>{t('btn_busy_cycle')}</span>
              </button>
            </>,
            pfMode
          )}
          {pfJogCard('L')}
          {pfJogCard('R')}
        </div>
      </div>

      {showLogs && (
        <LogTerminal title={t('logs_title')} logs={logs} onClear={onClearLogs} filterModule="ALL" />
      )}
    </div>
  );
};
