import React from 'react';
import {
  Play,
  Square,
  RotateCcw,
  Package,
  Layers,
} from 'lucide-react';
import { PreFeederState, LogEntry, PreFeederSensor } from '../types';
import { LogTerminal } from './LogTerminal';
import { useApp } from '../context/AppContext';

interface PreFeederTabProps {
  preFeederState: PreFeederState;
  onStart: () => void;
  onStop: () => void;
  onReset: () => void;
  onMaterialist: () => void;
  showLogs?: boolean;
  logs: LogEntry[];
  onClearLogs: () => void;
}

export const PreFeederTab: React.FC<PreFeederTabProps> = ({
  preFeederState,
  onStart,
  onStop,
  onReset,
  onMaterialist,
  showLogs = true,
  logs,
  onClearLogs,
}) => {
  const { t } = useApp();

  const renderSensorRow = (sensor: PreFeederSensor) => {
    const alarm = sensor.active;
    return (
      <div
        key={sensor.id}
        className={`flex items-center justify-between rounded-lg px-3.5 py-2.5 border shadow-2xs ${
          alarm
            ? 'bg-red-50/50 dark:bg-red-950/30 border-red-200 dark:border-red-800'
            : 'bg-slate-50 dark:bg-slate-800/60 border-slate-200 dark:border-slate-700'
        }`}
      >
        <div className="flex items-center gap-2.5">
          <span
            className={`h-2.5 w-2.5 rounded-full transition-all ${
              alarm
                ? 'bg-red-500 ring-2 ring-red-200 dark:ring-red-900'
                : sensor.status === 'ok'
                  ? 'bg-emerald-500'
                  : 'bg-slate-300 dark:bg-slate-600'
            }`}
          />
          <span className="text-sm font-medium text-slate-800 dark:text-slate-200">
            {sensor.name}
          </span>
        </div>

        <div className="flex items-center gap-2 font-mono text-xs">
          <span
            className={`font-bold transition ${
              alarm
                ? 'text-red-700 dark:text-red-400'
                : sensor.status === 'ok'
                  ? 'text-emerald-700 dark:text-emerald-400'
                  : 'text-slate-400 dark:text-slate-500'
            }`}
          >
            {alarm ? t('active') : sensor.status === 'ok' ? 'OK' : '—'}
          </span>
        </div>
      </div>
    );
  };

  return (
    <div className="space-y-4">
      {/* Unified Compact Status & Link Bar */}
      <div className="rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 px-4 py-2.5 text-slate-800 dark:text-slate-200 shadow-2xs flex flex-wrap items-center justify-between gap-3 transition-colors">
        <div className="flex items-center gap-3">
          <div className="flex items-center gap-2">
            <span
              className={`h-2.5 w-2.5 rounded-full ${
                !preFeederState.connection.connected
                  ? 'bg-red-500'
                  : preFeederState.isRunning
                    ? 'bg-emerald-500 animate-pulse'
                    : 'bg-emerald-500'
              }`}
            />
            <span className="text-sm font-semibold text-slate-900 dark:text-white tracking-tight">
              {preFeederState.statusText || (preFeederState.isRunning ? t('prefeeder_active_desc') : t('state_ready'))}
            </span>
          </div>

          <span className="text-slate-300 dark:text-slate-700">|</span>

          <div className="flex items-center gap-1.5 font-mono text-xs text-slate-600 dark:text-slate-400">
            <span className="text-slate-400 dark:text-slate-500">{t('link_label')}:</span>
            <span className={`font-semibold ${preFeederState.connection.connected ? 'text-emerald-700 dark:text-emerald-400' : 'text-red-600 dark:text-red-400'}`}>
              {preFeederState.connection.connected ? t('node_connected') : t('node_disconnected')}
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
                preFeederState.isRunning
                  ? 'bg-emerald-50 dark:bg-emerald-950/40 text-emerald-800 dark:text-emerald-300 border-emerald-200 dark:border-emerald-800'
                  : 'bg-slate-100 dark:bg-slate-800 text-slate-600 dark:text-slate-400 border-slate-200 dark:border-slate-700'
              }`}
            >
              {preFeederState.isRunning ? 'FEEDING' : 'IDLE'}
            </span>
          </div>
        </div>
      </div>

      {/* Action Buttons: Start (0x02A), Stop (0x02B), Reset (0x02C), Materialist (0x03F) - Compact & Shortened */}
      <div className="flex flex-wrap items-center gap-2">
        {/* Start (0x02A) */}
        <button
          id="btn-start-prefeeder"
          onClick={onStart}
          disabled={preFeederState.isRunning}
          className={`group flex items-center justify-center gap-1.5 rounded-lg px-3.5 py-2 text-xs font-bold transition active:scale-95 shadow-2xs border ${
            preFeederState.isRunning
              ? 'bg-slate-100 dark:bg-slate-800 text-slate-400 dark:text-slate-600 border-slate-200 dark:border-slate-700 cursor-not-allowed'
              : 'bg-slate-900 dark:bg-slate-100 hover:bg-slate-800 dark:hover:bg-white text-white dark:text-slate-900 border-slate-900 dark:border-white'
          }`}
        >
          <Play className="h-3.5 w-3.5 fill-current" />
          <span>{t('btn_start')}</span>
          <span className="rounded bg-slate-800 dark:bg-slate-200 px-1 py-0.2 font-mono text-[10px] text-slate-300 dark:text-slate-800 border border-slate-700 dark:border-slate-300">
            0x02A
          </span>
        </button>

        {/* Stop (0x02B) */}
        <button
          id="btn-stop-prefeeder"
          onClick={onStop}
          className="group flex items-center justify-center gap-1.5 rounded-lg border border-red-200 dark:border-red-900/60 bg-red-50 dark:bg-red-950/40 hover:bg-red-100 dark:hover:bg-red-950/70 px-3.5 py-2 text-xs font-bold text-red-700 dark:text-red-300 transition active:scale-95 shadow-2xs"
        >
          <Square className="h-3 w-3 fill-current text-red-600 dark:text-red-400" />
          <span>{t('btn_stop')}</span>
          <span className="rounded bg-red-100 dark:bg-red-900/60 px-1 py-0.2 font-mono text-[10px] text-red-800 dark:text-red-200 border border-red-200 dark:border-red-800">
            0x02B
          </span>
        </button>

        {/* Reset (0x02C) */}
        <button
          id="btn-reset-prefeeder"
          onClick={onReset}
          className="group flex items-center justify-center gap-1.5 rounded-lg border border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 hover:bg-slate-50 dark:hover:bg-slate-700 px-3.5 py-2 text-xs font-bold text-slate-700 dark:text-slate-200 transition active:scale-95 shadow-2xs"
        >
          <RotateCcw className="h-3.5 w-3.5 text-amber-600 dark:text-amber-400 group-hover:rotate-45 transition-transform" />
          <span>{t('btn_reset')}</span>
          <span className="rounded bg-slate-100 dark:bg-slate-700 px-1 py-0.2 font-mono text-[10px] text-amber-700 dark:text-amber-300 border border-slate-200 dark:border-slate-600">
            0x02C
          </span>
        </button>

        {/* Materialist (0x03F) */}
        <button
          id="btn-materialist-prefeeder"
          onClick={onMaterialist}
          className="group flex items-center justify-center gap-1.5 rounded-lg border border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 hover:bg-slate-50 dark:hover:bg-slate-700 px-3.5 py-2 text-xs font-bold text-slate-700 dark:text-slate-200 transition active:scale-95 shadow-2xs cursor-pointer"
        >
          <Package className="h-3.5 w-3.5 text-slate-600 dark:text-slate-400" />
          <span>{t('btn_materialist')}</span>
          <span className="rounded bg-slate-100 dark:bg-slate-700 px-1 py-0.2 font-mono text-[10px] text-slate-600 dark:text-slate-300 border border-slate-200 dark:border-slate-600">
            0x03F
          </span>
        </button>
      </div>

      {/* Dual Column Cards: Lado L & Lado R */}
      <div className="grid grid-cols-1 gap-4 md:grid-cols-2">
        {/* Lado L Card */}
        <div className="rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 p-4 shadow-2xs transition-colors">
          <div className="flex items-center justify-between border-b border-slate-100 dark:border-slate-800 pb-2.5">
            <div className="flex items-center gap-2">
              <Layers className="h-4 w-4 text-slate-600 dark:text-slate-400" />
              <h2 className="text-xs font-bold uppercase tracking-wider text-slate-800 dark:text-slate-200">
                {t('side_l')}
              </h2>
            </div>
            <span className="rounded bg-slate-100 dark:bg-slate-800 border border-slate-200 dark:border-slate-700 px-2 py-0.5 font-mono text-[10px] text-slate-600 dark:text-slate-400">
              {t('left_channel')}
            </span>
          </div>

          <div className="mt-3 space-y-1.5">
            {preFeederState.sensorsL.map((s) => renderSensorRow(s))}
          </div>
        </div>

        {/* Lado R Card */}
        <div className="rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 p-4 shadow-2xs transition-colors">
          <div className="flex items-center justify-between border-b border-slate-100 dark:border-slate-800 pb-2.5">
            <div className="flex items-center gap-2">
              <Layers className="h-4 w-4 text-slate-600 dark:text-slate-400" />
              <h2 className="text-xs font-bold uppercase tracking-wider text-slate-800 dark:text-slate-200">
                {t('side_r')}
              </h2>
            </div>
            <span className="rounded bg-slate-100 dark:bg-slate-800 border border-slate-200 dark:border-slate-700 px-2 py-0.5 font-mono text-[10px] text-slate-600 dark:text-slate-400">
              {t('right_channel')}
            </span>
          </div>

          <div className="mt-3 space-y-1.5">
            {preFeederState.sensorsR.map((s) => renderSensorRow(s))}
          </div>
        </div>
      </div>

      {/* Log PreFeeder */}
      {showLogs && (
        <LogTerminal
          title={t('logs_title')}
          logs={logs}
          onClear={onClearLogs}
          filterModule="PREFEEDER"
        />
      )}
    </div>
  );
};

