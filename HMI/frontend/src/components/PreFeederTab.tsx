import React from 'react';
import {
  Play,
  Square,
  RotateCcw,
  Layers,
  Activity,
} from 'lucide-react';
import { PreFeederState, LogEntry, PreFeederSensor } from '../types';
import { LogTerminal } from './LogTerminal';
import { ModuleStatusBar } from './ModuleStatusBar';
import { useApp } from '../context/AppContext';
import {
  hmiBadge,
  hmiModuleBtnDark,
  hmiModuleBtnDangerSoft,
  hmiModuleBtnGroup,
  hmiModuleBtnSecondary,
  hmiModuleBtnSuccess,
  hmiModuleIconBtn,
  hmiModuleIconSection,
  hmiModuleBadge,
  hmiModuleSectionTitle,
  hmiPanelCls,
  hmiPanelHeader,
  hmiPanelPadding,
} from '../styles/hmiUi';

interface PreFeederTabProps {
  preFeederState: PreFeederState;
  cycleMaterialist?: boolean;
  cycleBusy?: boolean;
  onStart: () => void;
  onStop: () => void;
  onReset: () => void;
  onMaterialist: () => void;
  onBusy?: () => void;
  onTriggerR: () => void;
  onTriggerL: () => void;
  showLogs?: boolean;
  logs: LogEntry[];
  onClearLogs: () => void;
}

export const PreFeederTab: React.FC<PreFeederTabProps> = ({
  preFeederState,
  cycleMaterialist = false,
  cycleBusy = false,
  onStart,
  onStop,
  onReset,
  onBusy,
  showLogs = true,
  logs,
  onClearLogs,
}) => {
  const { t } = useApp();

  const renderSensorRow = (sensor: PreFeederSensor) => {
    const alarm = sensor.status === 'error' || sensor.active;
    const ok = !alarm && sensor.status === 'ok';
    const warn = !alarm && sensor.status === 'warning';
    return (
      <div
        key={sensor.id}
        className={`flex items-center justify-between rounded-lg px-4 py-4 border shadow-2xs sm:px-5 sm:py-5 ${
          alarm
            ? 'bg-red-50/50 dark:bg-red-950/30 border-red-200 dark:border-red-800'
            : warn
              ? 'bg-amber-50/50 dark:bg-amber-950/20 border-amber-200 dark:border-amber-800'
              : 'bg-slate-50 dark:bg-slate-800/60 border-slate-200 dark:border-slate-700'
        }`}
      >
        <div className="flex items-center gap-3">
          <span
            className={`h-3.5 w-3.5 rounded-full transition-all shrink-0 ${
              alarm
                ? 'bg-red-500 ring-2 ring-red-200 dark:ring-red-900'
                : ok
                  ? 'bg-emerald-500'
                  : warn
                    ? 'bg-amber-500'
                    : 'bg-slate-300 dark:bg-slate-600'
            }`}
          />
          <span className="text-xl font-semibold text-slate-800 dark:text-slate-200 sm:text-2xl">
            {sensor.name}
          </span>
        </div>

        <div className="flex items-center gap-2 font-mono text-lg font-bold sm:text-xl">
          <span
            className={`font-bold transition ${
              alarm
                ? 'text-red-700 dark:text-red-400'
                : ok
                  ? 'text-emerald-700 dark:text-emerald-400'
                  : warn
                    ? 'text-amber-700 dark:text-amber-400'
                    : 'text-slate-400 dark:text-slate-500'
            }`}
          >
            {alarm ? t('active') : ok ? 'OK' : warn ? t('sensor_inactive') : '—'}
          </span>
        </div>
      </div>
    );
  };

  const connected = preFeederState.connection.connected;
  const hasError = !!preFeederState.hasError && connected;
  const inMaterialist = cycleMaterialist || !!preFeederState.idleMode;
  const pfMode = inMaterialist
    ? 'materialist'
    : cycleBusy || preFeederState.isRunning
      ? 'busy'
      : null;
  const pfHeadline = !connected
    ? t('node_disconnected')
    : hasError
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

  const statusTone = !connected
    ? 'disconnected'
    : hasError
      ? 'error'
      : pfMode === 'materialist'
        ? 'materialist'
        : pfMode === 'busy'
          ? 'busy'
          : 'ready';

  return (
    <div className="space-y-4">
      <ModuleStatusBar
        headline={pfHeadline}
        tone={statusTone}
        connection={preFeederState.connection}
        headlineError={hasError}
        metric={{
          label: t('feed_status'),
          value: pfFeedLabel,
          tone:
            pfMode === 'materialist'
              ? 'violet'
              : pfMode === 'busy'
                ? 'success'
                : 'default',
        }}
      />

      <div className={hmiModuleBtnGroup}>
        <button
          id="btn-start-prefeeder"
          type="button"
          onClick={onStart}
          disabled={preFeederState.isRunning}
          className={preFeederState.isRunning ? `${hmiModuleBtnSecondary} opacity-40` : hmiModuleBtnDark}
        >
          <Play className={`${hmiModuleIconBtn} fill-current`} />
          <span>{t('btn_start')}</span>
        </button>

        <button
          id="btn-stop-prefeeder"
          type="button"
          onClick={onStop}
          className={hmiModuleBtnDangerSoft}
        >
          <Square className={`${hmiModuleIconBtn} fill-current`} />
          <span>{t('btn_stop')}</span>
        </button>

        <button
          id="btn-reset-prefeeder"
          type="button"
          onClick={onReset}
          className={`group ${hmiModuleBtnSecondary}`}
        >
          <RotateCcw className={`${hmiModuleIconBtn} text-amber-600 dark:text-amber-400 group-hover:rotate-45 transition-transform`} />
          <span>{t('btn_reset')}</span>
        </button>

        <button
          id="btn-busy-prefeeder"
          type="button"
          onClick={onBusy}
          disabled={!onBusy}
          className={cycleBusy ? hmiModuleBtnSuccess : hmiModuleBtnSecondary}
        >
          <Activity className={hmiModuleIconBtn} />
          <span>{t('btn_busy_cycle')}</span>
        </button>
      </div>

      <div className="grid grid-cols-1 gap-4 md:grid-cols-2">
        <div className={`${hmiPanelCls} ${hmiPanelPadding}`}>
          <div className={hmiPanelHeader}>
            <div className="flex items-center gap-2">
              <Layers className={`${hmiModuleIconSection} text-slate-600 dark:text-slate-400`} />
              <h2 className={`${hmiModuleSectionTitle} text-slate-800 dark:text-slate-200`}>
                {t('side_l')}
              </h2>
            </div>
            <span className={hmiModuleBadge}>{t('left_channel')}</span>
          </div>

          <div className="space-y-2.5">
            {preFeederState.sensorsL.map((s) => renderSensorRow(s))}
          </div>
        </div>

        <div className={`${hmiPanelCls} ${hmiPanelPadding}`}>
          <div className={hmiPanelHeader}>
            <div className="flex items-center gap-2">
              <Layers className={`${hmiModuleIconSection} text-slate-600 dark:text-slate-400`} />
              <h2 className={`${hmiModuleSectionTitle} text-slate-800 dark:text-slate-200`}>
                {t('side_r')}
              </h2>
            </div>
            <span className={hmiModuleBadge}>{t('right_channel')}</span>
          </div>

          <div className="space-y-2.5">
            {preFeederState.sensorsR.map((s) => renderSensorRow(s))}
          </div>
        </div>
      </div>

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
