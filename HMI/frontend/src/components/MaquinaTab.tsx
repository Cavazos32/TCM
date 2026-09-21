import React, { useState, useEffect } from 'react';
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
  PowerOff,
  Zap,
  Package,
  Activity,
} from 'lucide-react';
import { MachineState, LogEntry, MotionState, PlcState, PreFeederState } from '../types';
import { LogTerminal } from './LogTerminal';
import { useApp } from '../context/AppContext';

type FaultModuleKind = 'motion' | 'plc' | 'prefeeder' | 'other';

function faultModuleKind(module?: string): FaultModuleKind {
  const m = (module || '').toLowerCase();
  if (m.includes('motion')) return 'motion';
  if (m.includes('plc')) return 'plc';
  if (m.includes('pre') || m.includes('feeder')) return 'prefeeder';
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
  t: (key: 'node_disconnected' | 'module_status_ok' | 'state_error' | 'state_ready' | 'module_status_busy') => string
): string {
  if (!connected) return t('node_disconnected');
  const raw = (statusText || '').trim();
  if (hasError) {
    return raw || t('state_error');
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
  // Quitar "Comando …" y opcodes técnicos
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
  onRefill?: () => void;
  onRefillConfirm?: (ok: boolean) => void;
  onRefillRetry?: () => void;
  onGotoCycle?: () => void;
  onMotionStop?: () => void;
  onMotionReset?: () => void;
  onMotionSearchHome?: () => void;
  onPlcReset?: () => void;
  onPlcAllOff?: () => void;
  onPfStart?: () => void;
  onPfStop?: () => void;
  onPfReset?: () => void;
  onPfJogL?: () => void;
  onPfJogR?: () => void;
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
  onRefill,
  onRefillConfirm,
  onRefillRetry,
  onGotoCycle,
  onMotionStop,
  onMotionReset,
  onMotionSearchHome,
  onPlcReset,
  onPlcAllOff,
  onPfStart,
  onPfStop,
  onPfReset,
  onPfJogL,
  onPfJogR,
  onBusy,
  onMaterialist,
  showLogs = true,
  logs,
  onClearLogs,
}) => {
  const { t } = useApp();
  const [offsetInput, setOffsetInput] = useState(String(machineState.offsetMm ?? 0));

  useEffect(() => {
    setOffsetInput(String(machineState.offsetMm ?? 0));
  }, [machineState.offsetMm]);

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
  const etaSec =
    machineState.cycleActive && stepProgress > 0 && stepProgress < 100
      ? Math.round((elapsedSec / stepProgress) * (100 - stepProgress))
      : 0;

  const hasFault = !!(machineState.errorActive || machineState.fault);
  const modKind = faultModuleKind(machineState.faultModule);
  const modulePanelError =
    !!motionState.hasError || !!plcState.hasError || !!preFeederState.hasError;
  const faultLabel =
    machineState.fault ||
    (machineState.faultCode
      ? `${machineState.faultCode}${
          machineState.faultDescription
            ? `: ${machineState.faultModule || '—'}, ${machineState.faultDescription}`
            : ''
        }`
      : '');
  const generalStatus = hasFault
    ? faultLabel || t('state_error')
    : machineState.isPaused
      ? t('state_paused')
      : machineState.isRunning
        ? t('state_producing')
        : machineState.statusText || t('state_ready');

  const btnBase =
    'flex items-center justify-center gap-1.5 rounded-lg border px-3 py-1.5 text-xs font-bold transition shadow-2xs active:scale-95';

  const moduleCard = (
    _kind: FaultModuleKind,
    title: string,
    statusText: string | undefined,
    connected: boolean,
    hasError: boolean,
    actions: React.ReactNode
  ) => {
    // Solo el estado propio del módulo: no heredar EXXX/rojo de máquina.
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
          {operatorModuleStatus(statusText, showError, connected, t)}
        </p>
        <div className="flex flex-wrap items-center gap-1.5">{actions}</div>
      </div>
    );
  };

  return (
    <div className="space-y-4">
      <div className="rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 px-4 py-2.5 text-slate-800 dark:text-slate-200 shadow-2xs flex flex-wrap items-center justify-between gap-3 transition-colors">
        <div className="flex items-center gap-2.5 min-w-0">
          <div
            className={`h-2.5 w-2.5 shrink-0 rounded-full ${
              hasFault
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
              hasFault
                ? 'text-red-700 dark:text-red-300'
                : 'text-slate-900 dark:text-white'
            }`}
            title={hasFault ? generalStatus : undefined}
          >
            {hasFault ? t('state_error') : generalStatus}
          </span>
          {hasFault && faultLabel ? (
            <span className="rounded border border-red-300 dark:border-red-800 bg-red-50 dark:bg-red-950/50 px-2 py-0.5 font-mono text-[11px] font-semibold text-red-700 dark:text-red-300 truncate max-w-[min(100%,36rem)]">
              {faultLabel}
            </span>
          ) : null}
          {machineState.cycleCompleted && (
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
                    onChange={(e) => setOffsetInput(e.target.value)}
                    onBlur={() => {
                      const n = parseFloat(offsetInput);
                      if (!isNaN(n)) onCutOffsetSave(n);
                    }}
                    disabled={machineState.isRunning}
                    className="w-full rounded-md border border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 pl-3 pr-10 py-1.5 text-xs font-mono font-medium text-slate-900 dark:text-slate-100 shadow-2xs focus:border-teal-500 focus:outline-none disabled:opacity-50"
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
                {machineState.cycleActive && elapsedSec > 0 && (
                  <span className="text-[10px] font-mono text-slate-500 dark:text-slate-400">
                    {Math.floor(elapsedSec / 60)}:{String(elapsedSec % 60).padStart(2, '0')}
                    {etaSec > 0 ? ` · ~${Math.floor(etaSec / 60)}:${String(etaSec % 60).padStart(2, '0')}` : ''}
                  </span>
                )}
              </div>
              <div className="h-2.5 w-full overflow-hidden rounded-full bg-slate-200 dark:bg-slate-700 p-0.5">
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
            </div>

            {machineState.refillAwaitingConfirm && onRefillConfirm && (
              <div className="flex flex-wrap items-center gap-3 rounded-lg border border-sky-300 dark:border-sky-700 bg-sky-50 dark:bg-sky-950/50 px-4 py-3">
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
                    id="btn-refill-confirm-retry"
                    type="button"
                    onClick={onRefillRetry}
                    className="flex items-center gap-1.5 rounded-lg bg-amber-500 hover:bg-amber-600 px-3.5 py-2 text-xs font-bold text-white shadow-2xs active:scale-95"
                  >
                    <RotateCcw className="h-3.5 w-3.5" />
                    {t('btn_refill_confirm_retry')}
                  </button>
                )}
                <button
                  id="btn-refill-confirm-yes"
                  type="button"
                  onClick={() => onRefillConfirm(true)}
                  className="flex items-center gap-1.5 rounded-lg bg-emerald-600 hover:bg-emerald-700 px-3.5 py-2 text-xs font-bold text-white shadow-2xs active:scale-95"
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
                  className="flex items-center gap-1.5 rounded-lg border border-slate-300 dark:border-slate-600 bg-white dark:bg-slate-800 px-3.5 py-2 text-xs font-bold text-slate-700 dark:text-slate-200 shadow-2xs active:scale-95"
                >
                  <X className="h-3.5 w-3.5" />
                  {t('btn_refill_confirm_no')}
                </button>
              </div>
            )}
          </div>

          <div className="w-full lg:w-40 shrink-0 flex flex-col gap-2 border-t lg:border-t-0 lg:border-l border-slate-100 dark:border-slate-800 pt-4 lg:pt-0 lg:pl-4">
            <button
              id="btn-start-maquina"
              onClick={onStart}
              disabled={machineState.isRunning || machineState.refillActive}
              className={`flex w-full items-center justify-center gap-1.5 rounded-lg px-3 py-2.5 text-xs font-bold tracking-wide transition-all shadow-2xs ${
                machineState.isRunning || machineState.refillActive
                  ? 'bg-slate-100 dark:bg-slate-800 text-slate-400 cursor-not-allowed border border-slate-200 dark:border-slate-700'
                  : 'bg-emerald-600 hover:bg-emerald-700 text-white active:scale-[0.98]'
              }`}
            >
              <Play className="h-3.5 w-3.5 fill-current" />
              <span>{t('btn_start')}</span>
            </button>

            <button
              id="btn-pause-maquina"
              onClick={onPause}
              disabled={!machineState.pauseEnabled}
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
              className="flex w-full items-center justify-center gap-1.5 rounded-lg border border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 hover:bg-slate-50 px-3 py-2.5 text-xs font-semibold text-slate-700 dark:text-slate-200 transition shadow-2xs"
            >
              <RotateCcw className="h-3.5 w-3.5 text-amber-600" />
              <span>{t('btn_reset_cycle')}</span>
            </button>

            <button
              id="btn-reanudar-maquina"
              onClick={onResume}
              disabled={!resumeEnabled || machineState.refillAwaitingConfirm}
              className="group flex w-full items-center justify-center gap-1.5 rounded-lg border border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 hover:bg-slate-50 px-3 py-2.5 text-xs font-semibold text-slate-700 dark:text-slate-200 transition shadow-2xs disabled:opacity-40 disabled:cursor-not-allowed"
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

            <div
              id="ind-coming-soon"
              className="flex w-full items-center justify-center rounded-lg border border-dashed border-slate-300 dark:border-slate-700 bg-slate-50/80 dark:bg-slate-800/40 px-3 py-2.5 text-[11px] font-semibold text-slate-400 dark:text-slate-500 select-none"
            >
              {t('coming_soon')}
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

        <div className="mt-4 grid grid-cols-1 md:grid-cols-3 gap-3">
          {moduleCard(
            'motion',
            t('tab_motion'),
            motionState.statusText,
            motionState.connection.connected,
            !!motionState.hasError,
            <>
              <button
                id="btn-main-motion-stop"
                type="button"
                onClick={onMotionStop}
                disabled={!onMotionStop}
                className={`${btnBase} border-red-200 dark:border-red-900/60 bg-red-50 dark:bg-red-950/40 text-red-700 dark:text-red-300 hover:bg-red-100 disabled:opacity-40`}
              >
                <Square className="h-3.5 w-3.5 fill-current" />
                <span>{t('btn_stop')}</span>
              </button>
              <button
                id="btn-main-motion-reset"
                type="button"
                onClick={onMotionReset}
                disabled={!onMotionReset}
                className={`${btnBase} border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 text-slate-700 dark:text-slate-200 hover:bg-slate-50 disabled:opacity-40`}
              >
                <RotateCcw className="h-3.5 w-3.5 text-amber-600" />
                <span>{t('btn_reset')}</span>
              </button>
              <button
                id="btn-main-motion-search-home"
                type="button"
                onClick={onMotionSearchHome}
                disabled={!onMotionSearchHome || motionState.isMoving}
                className={`${btnBase} border-emerald-200 dark:border-emerald-900/60 bg-emerald-50 dark:bg-emerald-950/40 text-emerald-800 dark:text-emerald-300 hover:bg-emerald-100 disabled:opacity-40`}
              >
                <Home className="h-3.5 w-3.5" />
                <span>{t('btn_search_home')}</span>
              </button>
            </>
          )}

          {moduleCard(
            'plc',
            t('tab_plc'),
            plcState.statusText,
            plcState.connection.connected,
            !!plcState.hasError,
            <>
              <button
                id="btn-main-plc-reset"
                type="button"
                onClick={onPlcReset}
                disabled={!onPlcReset}
                className={`${btnBase} border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 text-slate-700 dark:text-slate-200 hover:bg-slate-50 disabled:opacity-40`}
              >
                <RotateCcw className="h-3.5 w-3.5 text-amber-600" />
                <span>{t('btn_reset')}</span>
              </button>
              <button
                id="btn-main-plc-all-off"
                type="button"
                onClick={onPlcAllOff}
                disabled={!onPlcAllOff}
                className={`${btnBase} border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 text-slate-700 dark:text-slate-200 hover:bg-slate-50 disabled:opacity-40`}
              >
                <PowerOff className="h-3.5 w-3.5 text-slate-500" />
                <span>{t('btn_all_off')}</span>
              </button>
            </>
          )}

          {moduleCard(
            'prefeeder',
            t('tab_prefeeder'),
            preFeederState.statusText,
            preFeederState.connection.connected,
            !!preFeederState.hasError,
            <>
              <button
                id="btn-main-pf-start"
                type="button"
                onClick={onPfStart}
                disabled={!onPfStart}
                className={`${btnBase} border-emerald-300 dark:border-emerald-800 bg-emerald-50 dark:bg-emerald-950/40 text-emerald-800 dark:text-emerald-200 hover:bg-emerald-100 disabled:opacity-40`}
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
                disabled={!onPfReset}
                className={`${btnBase} border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 text-slate-700 dark:text-slate-200 hover:bg-slate-50 disabled:opacity-40`}
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
                disabled={!onBusy || machineState.cycleActive}
                className={`${btnBase} ${
                  machineState.cycleBusy
                    ? 'border-emerald-400 bg-emerald-100 dark:bg-emerald-950/50 text-emerald-900 dark:text-emerald-200'
                    : 'border-emerald-200 dark:border-emerald-900/60 bg-emerald-50 dark:bg-emerald-950/40 text-emerald-800 dark:text-emerald-300 hover:bg-emerald-100'
                } disabled:opacity-40`}
              >
                <Activity className="h-3.5 w-3.5" />
                <span>{t('btn_busy_cycle')}</span>
              </button>
              <button
                id="btn-main-pf-materialist"
                type="button"
                onClick={onMaterialist}
                disabled={!onMaterialist || machineState.cycleActive}
                className={`${btnBase} ${
                  machineState.cycleMaterialist
                    ? 'border-amber-400 bg-amber-100 dark:bg-amber-950/50 text-amber-900 dark:text-amber-200'
                    : 'border-amber-200 dark:border-amber-900/60 bg-amber-50 dark:bg-amber-950/40 text-amber-800 dark:text-amber-300 hover:bg-amber-100'
                } disabled:opacity-40`}
              >
                <Package className="h-3.5 w-3.5" />
                <span>{t('btn_materialist_cycle')}</span>
              </button>
            </>
          )}
        </div>
      </div>

      {showLogs && (
        <LogTerminal title={t('logs_title')} logs={logs} onClear={onClearLogs} filterModule="ALL" />
      )}
    </div>
  );
};
