import React from 'react';
import {
  RotateCcw,
  PowerOff,
  AlertCircle,
  Wind,
} from 'lucide-react';
import { PlcState, LogEntry } from '../types';
import { LogTerminal } from './LogTerminal';
import { ModuleStatusBar } from './ModuleStatusBar';
import { useApp } from '../context/AppContext';
import {
  hmiAlertText,
  hmiBadge,
  hmiModuleBtnGroup,
  hmiModuleBtnSecondary,
  hmiModuleBtnSuccess,
  hmiModuleBtnValveExtra,
  hmiModuleCenterSectionTitle,
  hmiModuleIconBtn,
  hmiModuleIconSection,
  hmiPanelCls,
  hmiPanelHeader,
  hmiPanelPadding,
} from '../styles/hmiUi';

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
  const connected = plcState.connection.connected;
  const hasError = !!plcState.hasError && connected;
  const manualsLocked = hasError;
  const isCutterValve = (id: string) => id === 'cutter-r' || id === 'cutter-l';

  const statusTone = !connected
    ? 'disconnected'
    : hasError
      ? 'error'
      : activeValvesCount > 0
        ? 'busy'
        : 'ready';

  return (
    <div className="space-y-4">
      <ModuleStatusBar
        headline={
          plcState.statusText ||
          (hasError
            ? t('state_error')
            : activeValvesCount > 0
              ? `${activeValvesCount} ${t('valves_active')}`
              : t('state_ready'))
        }
        tone={statusTone}
        connection={plcState.connection}
        headlineError={hasError}
        metric={{
          label: t('valves_status'),
          value: `${activeValvesCount} / ${plcState.valves.length} ON`,
          tone: activeValvesCount > 0 ? 'success' : 'default',
        }}
      />

      <div className={`${hmiPanelCls} ${hmiPanelPadding}`}>
        <div className={hmiPanelHeader}>
          <div className="flex items-center gap-2">
            <Wind className={`${hmiModuleIconSection} text-slate-600 dark:text-slate-400`} />
            <h2 className={hmiModuleCenterSectionTitle}>{t('valves_manual')}</h2>
            <span className={hmiBadge}>{t('pneumatic_control')}</span>
          </div>

          <div className={hmiModuleBtnGroup}>
            <button
              id="btn-reset-plc"
              type="button"
              onClick={onResetPlc}
              className={`group ${hmiModuleBtnSecondary}`}
            >
              <RotateCcw className={`${hmiModuleIconBtn} text-amber-600 dark:text-amber-400 group-hover:rotate-45 transition-transform`} />
              <span>{t('btn_reset_plc')}</span>
            </button>

            <button
              id="btn-all-off-plc"
              type="button"
              onClick={onAllOff}
              disabled={manualsLocked || !connected}
              title={manualsLocked ? t('plc_manuals_locked') : undefined}
              className={hmiModuleBtnSecondary}
            >
              <PowerOff className={hmiModuleIconBtn} />
              <span>{t('btn_all_off')}</span>
            </button>
          </div>
        </div>

        {manualsLocked && (
          <p className={`mb-3 ${hmiAlertText}`}>{t('plc_manuals_locked')}</p>
        )}

        <div className="overflow-hidden rounded-lg border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900">
          <div className="grid grid-cols-12 border-b border-slate-200 dark:border-slate-800 bg-slate-50 dark:bg-slate-800/80 px-4 py-3 text-base font-bold uppercase tracking-wider text-slate-500 dark:text-slate-400 sm:px-5 sm:py-3.5 sm:text-lg">
            <div className="col-span-6 sm:col-span-5">{t('function')}</div>
            <div className="col-span-3 sm:col-span-4 text-center">{t('state')}</div>
            <div className="col-span-3 text-right">{t('error')}</div>
          </div>

          <div className="divide-y divide-slate-100 dark:divide-slate-800/70">
            {plcState.valves.map((valve) => {
              const busy = !!valveBusy[valve.id];
              return (
                <div
                  key={valve.id}
                  className={`grid grid-cols-12 items-center px-4 py-4 text-lg transition-colors sm:px-5 sm:py-5 sm:text-xl ${
                    valve.active ? 'bg-emerald-50/40 dark:bg-emerald-950/20' : 'hover:bg-slate-50/60 dark:hover:bg-slate-800/40'
                  }`}
                >
                  <div className="col-span-6 sm:col-span-5 flex items-center gap-2 min-w-0">
                    <span
                      className={`h-3.5 w-3.5 rounded-full shrink-0 ${
                        valve.active
                          ? 'bg-emerald-500 ring-2 ring-emerald-200 dark:ring-emerald-900'
                          : 'bg-slate-300 dark:bg-slate-600'
                      }`}
                    />
                    <span className="text-xl font-semibold text-slate-800 dark:text-slate-200 truncate sm:text-2xl">
                      {valve.name}
                    </span>
                    {valve.id === 'blower' && (
                      <label className="ml-auto flex items-center gap-1.5 shrink-0 text-xs font-mono text-slate-500 dark:text-slate-400 sm:text-sm">
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
                          className="w-16 rounded border border-slate-300 dark:border-slate-600 bg-white dark:bg-slate-800 px-2 py-1 text-sm font-mono text-slate-800 dark:text-slate-100 sm:text-base"
                        />
                        <span>s</span>
                      </label>
                    )}
                  </div>

                  <div className="col-span-3 sm:col-span-4 flex justify-center">
                    <button
                      id={`btn-valve-${valve.id}`}
                      type="button"
                      disabled={
                        busy ||
                        !connected ||
                        (manualsLocked && !isCutterValve(valve.id))
                      }
                      title={
                        manualsLocked && !isCutterValve(valve.id)
                          ? t('plc_manuals_locked')
                          : undefined
                      }
                      onClick={() => onToggleValve(valve.id)}
                      className={`${valve.active ? hmiModuleBtnSuccess : hmiModuleBtnSecondary} ${hmiModuleBtnValveExtra} ${
                        busy || (manualsLocked && !isCutterValve(valve.id))
                          ? 'opacity-60 cursor-not-allowed'
                          : ''
                      }`}
                    >
                      {valve.active ? 'ON' : 'OFF'}
                    </button>
                  </div>

                  <div className="col-span-3 flex items-center justify-end font-mono text-lg sm:text-xl">
                    {valve.hasError ? (
                      <span className="flex items-center gap-2 text-red-700 dark:text-red-400 font-bold bg-red-50 dark:bg-red-950/40 px-3 py-1.5 rounded-md border border-red-200 dark:border-red-900/60 text-base sm:px-3.5 sm:py-2 sm:text-lg">
                        <AlertCircle className="h-5 w-5 shrink-0 text-red-600 dark:text-red-400 sm:h-6 sm:w-6" />
                        <span>{t('fault')}</span>
                      </span>
                    ) : (
                      <span className="text-slate-400 dark:text-slate-500 font-bold text-xl sm:text-2xl">—</span>
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
