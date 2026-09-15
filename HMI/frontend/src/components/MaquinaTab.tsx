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
} from 'lucide-react';
import { MachineState, LogEntry } from '../types';
import { LogTerminal } from './LogTerminal';
import { useApp } from '../context/AppContext';

interface MaquinaTabProps {
  machineState: MachineState;
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
  onGotoCycle?: () => void;
  onToggleTrialMode?: (on: boolean) => void;
  showLogs?: boolean;
  logs: LogEntry[];
  onClearLogs: () => void;
}

export const MaquinaTab: React.FC<MaquinaTabProps> = ({
  machineState,
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
  onGotoCycle,
  onToggleTrialMode,
  showLogs = true,
  logs,
  onClearLogs,
}) => {
  const { t, debugMode } = useApp();
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

  return (
    <div className="space-y-4">
      <div className="rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 px-4 py-2.5 text-slate-800 dark:text-slate-200 shadow-2xs flex flex-wrap items-center justify-between gap-3 transition-colors">
        <div className="flex items-center gap-2.5 min-w-0">
          <div
            className={`h-2.5 w-2.5 shrink-0 rounded-full ${
              machineState.fault
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
            className={`text-sm font-semibold tracking-tight ${
              machineState.fault
                ? 'text-red-700 dark:text-red-300'
                : 'text-slate-900 dark:text-white'
            }`}
          >
            {machineState.fault || machineState.statusText || t('state_ready')}
          </span>
          {machineState.faultClass ? (
            <span className="rounded border border-red-300 dark:border-red-800 bg-red-50 dark:bg-red-950/50 px-1.5 py-0.5 font-mono text-[10px] font-semibold text-red-700 dark:text-red-300">
              {machineState.faultClass}
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
              {machineState.mm.toFixed(1)} mm @ {machineState.rpm} RPM
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

        <div className="mt-4 grid grid-cols-1 md:grid-cols-3 gap-4 items-start">
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

        <div className="mt-4 rounded-lg bg-slate-50 dark:bg-slate-800/60 p-3 border border-slate-200/80 dark:border-slate-800">
          <div className="mb-2 flex items-center justify-between text-xs font-mono">
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

        <div className="mt-4 flex flex-wrap items-center gap-2 pt-1">
          {/* Safety exhaust — MOT_ERR_EXHAUST 0x4F (indicador IO, no comando) */}
          {debugMode && (
            <button
              id="btn-safety-exhaust"
              type="button"
              disabled
              title={t('safety_exhaust_hint')}
              className={`flex items-center justify-center gap-1.5 rounded-lg border px-3.5 py-2 text-xs font-bold transition shadow-2xs cursor-default ${
                machineState.safetyExhaust
                  ? 'border-red-400 dark:border-red-700 bg-red-100 dark:bg-red-950/60 text-red-800 dark:text-red-200 animate-pulse'
                  : 'border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 text-slate-600 dark:text-slate-300 opacity-80'
              }`}
            >
              <AlertTriangle
                className={`h-3.5 w-3.5 ${
                  machineState.safetyExhaust
                    ? 'text-red-600 dark:text-red-300'
                    : 'text-slate-400'
                }`}
              />
              <span>{t('safety_exhaust')}</span>
              <span className="font-mono text-[10px] opacity-80">0x4F</span>
            </button>
          )}

          {debugMode && onToggleTrialMode && (
            <button
              id="btn-trial-mode-maquina"
              type="button"
              onClick={() => onToggleTrialMode(!machineState.trialMode)}
              disabled={machineState.isRunning}
              className={`flex items-center justify-center gap-1.5 rounded-lg border px-3.5 py-2 text-xs font-bold transition shadow-2xs disabled:opacity-40 ${
                machineState.trialMode
                  ? 'border-sky-400 bg-sky-100 dark:bg-sky-950/50 text-sky-800 dark:text-sky-200'
                  : 'border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 text-slate-700 dark:text-slate-200'
              }`}
            >
              <span>{t('trial_mode')}</span>
            </button>
          )}

          <button
            id="btn-start-maquina"
            onClick={onStart}
            disabled={machineState.isRunning}
            className={`flex items-center justify-center gap-1.5 rounded-lg px-4 py-2 text-xs font-bold tracking-wide transition-all shadow-2xs ${
              machineState.isRunning
                ? 'bg-slate-100 dark:bg-slate-800 text-slate-400 cursor-not-allowed border border-slate-200 dark:border-slate-700'
                : 'bg-emerald-600 hover:bg-emerald-700 text-white active:scale-95'
            }`}
          >
            <Play className="h-3.5 w-3.5 fill-current" />
            <span>{t('btn_start')}</span>
            <span className="font-mono text-[10px] opacity-80">0x040</span>
          </button>

          <button
            id="btn-stop-maquina"
            onClick={onStop}
            className="flex items-center justify-center gap-1.5 rounded-lg border border-red-200 dark:border-red-900/60 bg-red-50 dark:bg-red-950/40 hover:bg-red-100 px-4 py-2 text-xs font-bold text-red-700 dark:text-red-300 transition active:scale-95 shadow-2xs"
          >
            <Square className="h-3.5 w-3.5 fill-current text-red-600" />
            <span>{t('btn_stop')}</span>
            <span className="font-mono text-[10px] opacity-80">0x041</span>
          </button>

          <button
            id="btn-pause-maquina"
            onClick={onPause}
            disabled={!machineState.pauseEnabled}
            className="flex items-center justify-center gap-1.5 rounded-lg border border-amber-200 dark:border-amber-800 bg-amber-50 dark:bg-amber-950/40 hover:bg-amber-100 px-3.5 py-2 text-xs font-semibold text-amber-800 dark:text-amber-200 transition shadow-2xs disabled:opacity-40 disabled:cursor-not-allowed"
          >
            <Pause className="h-3.5 w-3.5" />
            <span>{t('btn_pause')}</span>
          </button>

          <button
            id="btn-reanudar-maquina"
            onClick={onResume}
            disabled={!resumeEnabled}
            className="group flex items-center justify-center gap-1.5 rounded-lg border border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 hover:bg-slate-50 px-3.5 py-2 text-xs font-semibold text-slate-700 dark:text-slate-200 transition shadow-2xs disabled:opacity-40 disabled:cursor-not-allowed"
          >
            <RotateCcw className="h-3.5 w-3.5 group-hover:rotate-45 transition-transform" />
            <span>
              {machineState.stepByStep && machineState.isPaused
                ? t('btn_next_step')
                : t('btn_resume')}
            </span>
            <span className="font-mono text-[10px]">0x014</span>
          </button>

          <button
            id="btn-reset-maquina"
            onClick={onReset}
            className="flex items-center justify-center gap-1.5 rounded-lg border border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 hover:bg-slate-50 px-3.5 py-2 text-xs font-semibold text-slate-700 dark:text-slate-200 transition shadow-2xs"
          >
            <RotateCcw className="h-3.5 w-3.5 text-amber-600" />
            <span>{t('btn_reset_cycle')}</span>
            <span className="font-mono text-[10px]">0x042</span>
          </button>
        </div>
      </div>

      {showLogs && (
        <LogTerminal title={t('logs_title')} logs={logs} onClear={onClearLogs} filterModule="ALL" />
      )}
    </div>
  );
};
