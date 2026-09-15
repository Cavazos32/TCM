import React from 'react';
import {
  RotateCcw,
  PowerOff,
  Power,
  AlertCircle,
  Wind,
} from 'lucide-react';
import { PlcState, LogEntry } from '../types';
import { LogTerminal } from './LogTerminal';
import { useApp } from '../context/AppContext';

interface PlcTabProps {
  plcState: PlcState;
  onToggleValve: (valveId: string) => void;
  valveBusy?: Record<string, boolean>;
  onBlowerSecChange: (sec: number) => void;
  onResetPlc: () => void;
  onAllOff: () => void;
  showLogs?: boolean;
  logs: LogEntry[];
  onClearLogs: () => void;
}

export const PlcTab: React.FC<PlcTabProps> = ({
  plcState,
  onToggleValve,
  valveBusy = {},
  onBlowerSecChange,
  onResetPlc,
  onAllOff,
  showLogs = true,
  logs,
  onClearLogs,
}) => {
  const { t } = useApp();
  const activeValvesCount = plcState.valves.filter((v) => v.active).length;

  return (
    <div className="space-y-4">
      {/* Unified Compact Status & Link Bar */}
      <div className="rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 px-4 py-2.5 text-slate-800 dark:text-slate-200 shadow-2xs flex flex-wrap items-center justify-between gap-3 transition-colors">
        <div className="flex items-center gap-3">
          <div className="flex items-center gap-2">
            <span
              className={`h-2.5 w-2.5 rounded-full ${
                plcState.connection.connected
                  ? 'bg-emerald-500 animate-pulse'
                  : 'bg-red-500'
              }`}
            />
            <span className="text-sm font-semibold text-slate-900 dark:text-white tracking-tight">
              {plcState.statusText || (activeValvesCount > 0
                ? `${activeValvesCount} ${t('valves_active')}`
                : t('state_ready'))}
            </span>
          </div>

          <span className="text-slate-300 dark:text-slate-700">|</span>

          <div className="flex items-center gap-1.5 font-mono text-xs text-slate-600 dark:text-slate-400">
            <span className="text-slate-400 dark:text-slate-500">{t('link_label')}:</span>
            <span className={`font-semibold ${plcState.connection.connected ? 'text-emerald-700 dark:text-emerald-400' : 'text-red-600 dark:text-red-400'}`}>
              {plcState.connection.connected ? t('node_connected') : t('node_disconnected')}
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

      {/* Main Section: VÁLVULAS (MANUAL) with Master Controls inline */}
      <div className="rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 p-4 sm:p-5 shadow-2xs transition-colors">
        <div className="flex flex-wrap items-center justify-between gap-3 border-b border-slate-100 dark:border-slate-800 pb-3">
          <div className="flex items-center gap-2">
            <Wind className="h-4 w-4 text-slate-600 dark:text-slate-400" />
            <h2 className="text-xs font-bold uppercase tracking-wider text-slate-800 dark:text-slate-200">
              {t('valves_manual')}
            </h2>
            <span className="rounded bg-slate-100 dark:bg-slate-800 border border-slate-200 dark:border-slate-700 px-2 py-0.5 font-mono text-[10px] text-slate-600 dark:text-slate-400 ml-1">
              {t('pneumatic_control')}
            </span>
          </div>

          {/* Inline Master Controls */}
          <div className="flex items-center gap-2">
            {/* Reset PLC (0x01E) */}
            <button
              id="btn-reset-plc"
              onClick={onResetPlc}
              className="group flex items-center justify-center gap-1.5 rounded-lg border border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 hover:bg-slate-50 dark:hover:bg-slate-700 px-3 py-1.5 text-xs font-bold text-slate-700 dark:text-slate-200 transition active:scale-95 shadow-2xs"
            >
              <RotateCcw className="h-3.5 w-3.5 text-amber-600 dark:text-amber-400 group-hover:rotate-45 transition-transform" />
              <span>{t('btn_reset_plc')}</span>
              <span className="rounded bg-slate-100 dark:bg-slate-700 px-1 py-0.2 font-mono text-[10px] text-amber-700 dark:text-amber-300 border border-slate-200 dark:border-slate-600">
                0x01E
              </span>
            </button>

            {/* All Off */}
            <button
              id="btn-all-off-plc"
              onClick={onAllOff}
              className="flex items-center justify-center gap-1.5 rounded-lg border border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 hover:bg-slate-50 dark:hover:bg-slate-700 px-3 py-1.5 text-xs font-bold text-slate-700 dark:text-slate-200 transition active:scale-95 shadow-2xs"
            >
              <PowerOff className="h-3.5 w-3.5 text-slate-500 dark:text-slate-400" />
              <span>{t('btn_all_off')}</span>
            </button>
          </div>
        </div>

        {/* Valves Matrix / Table */}
        <div className="mt-3 overflow-hidden rounded-lg border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900">
          {/* Table Header */}
          <div className="grid grid-cols-12 border-b border-slate-200 dark:border-slate-800 bg-slate-50 dark:bg-slate-800/80 px-3.5 py-2 text-xs font-bold uppercase tracking-wider text-slate-500 dark:text-slate-400">
            <div className="col-span-6 sm:col-span-5">{t('function')}</div>
            <div className="col-span-3 sm:col-span-4 text-center">{t('state')}</div>
            <div className="col-span-3 text-right">{t('error')}</div>
          </div>

          {/* Table Rows */}
          <div className="divide-y divide-slate-100 dark:divide-slate-800/70">
            {plcState.valves.map((valve) => {
              const busy = !!valveBusy[valve.id];
              return (
                <div
                  key={valve.id}
                  className={`grid grid-cols-12 items-center px-3.5 py-2 text-xs transition-colors ${
                    valve.active ? 'bg-emerald-50/40 dark:bg-emerald-950/20' : 'hover:bg-slate-50/60 dark:hover:bg-slate-800/40'
                  }`}
                >
                  {/* Función Name */}
                  <div className="col-span-6 sm:col-span-5 flex items-center gap-2 min-w-0">
                    <span
                      className={`h-2 w-2 rounded-full shrink-0 ${
                        valve.active
                          ? 'bg-emerald-500 ring-2 ring-emerald-200 dark:ring-emerald-900'
                          : 'bg-slate-300 dark:bg-slate-600'
                      }`}
                    />
                    <span className="font-semibold text-slate-800 dark:text-slate-200 truncate">{valve.name}</span>
                    {valve.id === 'blower' && (
                      <label className="ml-auto flex items-center gap-1 shrink-0 text-[10px] font-mono text-slate-500 dark:text-slate-400">
                        <span>{t('blower_sec')}</span>
                        <input
                          id="input-blower-sec"
                          type="number"
                          min={0.2}
                          max={300}
                          step={0.5}
                          value={plcState.blowerSec}
                          onChange={(e) => {
                            const v = Number(e.target.value);
                            if (!Number.isFinite(v)) return;
                            onBlowerSecChange(v);
                          }}
                          className="w-14 rounded border border-slate-300 dark:border-slate-600 bg-white dark:bg-slate-800 px-1.5 py-0.5 text-xs font-mono text-slate-800 dark:text-slate-100"
                        />
                        <span>s</span>
                      </label>
                    )}
                  </div>

                  {/* Estado Action Button */}
                  <div className="col-span-3 sm:col-span-4 flex justify-center">
                    <button
                      id={`btn-valve-${valve.id}`}
                      type="button"
                      disabled={busy}
                      onClick={() => onToggleValve(valve.id)}
                      className={`group flex items-center justify-center gap-1.5 rounded-md px-2.5 py-1 text-xs font-mono font-bold transition border shadow-2xs ${
                        busy ? 'opacity-60 cursor-wait' : 'active:scale-95'
                      } ${
                        valve.active
                          ? 'bg-emerald-600 dark:bg-emerald-600 text-white border-emerald-600'
                          : 'bg-white dark:bg-slate-800 text-slate-700 dark:text-slate-200 hover:bg-slate-50 dark:hover:bg-slate-700 hover:text-slate-900 dark:hover:text-white border-slate-300 dark:border-slate-700'
                      }`}
                    >
                      {valve.active ? (
                        <>
                          <span>ON</span>
                          <span
                            className={`rounded px-1 py-0.2 text-[10px] bg-emerald-700 text-white`}
                          >
                            {valve.hexCode}
                          </span>
                        </>
                      ) : (
                        <span>OFF</span>
                      )}
                    </button>
                  </div>

                  {/* Error Column Indicator */}
                  <div className="col-span-3 flex items-center justify-end font-mono text-xs">
                    {valve.hasError ? (
                      <span className="flex items-center gap-1 text-red-700 dark:text-red-400 font-bold bg-red-50 dark:bg-red-950/40 px-1.5 py-0.5 rounded border border-red-200 dark:border-red-900/60 text-[11px]">
                        <AlertCircle className="h-3 w-3 text-red-600 dark:text-red-400" />
                        <span>{t('fault')}</span>
                      </span>
                    ) : (
                      <span className="text-slate-400 dark:text-slate-500 font-bold">—</span>
                    )}
                  </div>
                </div>
              );
            })}
          </div>
        </div>
      </div>

      {showLogs && (
        <LogTerminal
          title={t('logs_title')}
          logs={logs}
          onClear={onClearLogs}
          filterModule="PLC"
        />
      )}
    </div>
  );
};
