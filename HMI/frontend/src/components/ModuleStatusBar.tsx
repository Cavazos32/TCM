import React from 'react';
import { ConnectionState } from '../types';
import { useApp } from '../context/AppContext';
import {
  hmiStatusBar,
  hmiStatusChip,
  hmiStatusDivider,
  hmiStatusHeadline,
  hmiStatusLed,
  hmiStatusMeta,
  hmiStatusMetaLabel,
} from '../styles/hmiUi';

export type ModuleStatusTone =
  | 'ready'
  | 'error'
  | 'warning'
  | 'busy'
  | 'materialist'
  | 'moving'
  | 'disconnected';

interface ModuleStatusMetric {
  label: string;
  value: React.ReactNode;
  tone?: 'default' | 'success' | 'violet' | 'error';
}

interface ModuleStatusBarProps {
  headline: string;
  tone: ModuleStatusTone;
  connection: ConnectionState;
  headlineError?: boolean;
  metric?: ModuleStatusMetric;
  extra?: React.ReactNode;
}

const toneLedClass: Record<ModuleStatusTone, string> = {
  ready: 'bg-emerald-500',
  error: 'bg-red-500',
  warning: 'bg-amber-500',
  busy: 'bg-emerald-500 animate-pulse',
  materialist: 'bg-violet-500',
  moving: 'bg-amber-500 animate-ping',
  disconnected: 'bg-red-500',
};

const metricToneClass: Record<NonNullable<ModuleStatusMetric['tone']>, string> = {
  default:
    'bg-slate-100 dark:bg-slate-800 text-slate-700 dark:text-slate-300 border-slate-200 dark:border-slate-700',
  success:
    'bg-emerald-50 dark:bg-emerald-950/40 text-emerald-800 dark:text-emerald-300 border-emerald-200 dark:border-emerald-800',
  violet:
    'bg-violet-50 dark:bg-violet-950/40 text-violet-800 dark:text-violet-200 border-violet-200 dark:border-violet-800',
  error:
    'bg-red-50 dark:bg-red-950/40 text-red-800 dark:text-red-300 border-red-200 dark:border-red-900/60',
};

export const ModuleStatusBar: React.FC<ModuleStatusBarProps> = ({
  headline,
  tone,
  connection,
  headlineError = false,
  metric,
  extra,
}) => {
  const { t } = useApp();
  const connected = connection.connected;
  const ledTone = !connected ? 'disconnected' : tone;

  return (
    <div className={hmiStatusBar}>
      <div className="flex min-w-0 flex-1 flex-wrap items-center gap-x-4 gap-y-2">
        <div className="flex min-w-0 items-center gap-3">
          <span className={`${hmiStatusLed} ${toneLedClass[ledTone]}`} />
          <span
            className={`${hmiStatusHeadline} ${
              headlineError
                ? 'text-red-700 dark:text-red-300'
                : 'text-slate-900 dark:text-white'
            }`}
          >
            {headline}
          </span>
        </div>

        <span className={hmiStatusDivider}>|</span>

        <div className={hmiStatusMeta}>
          <span className={hmiStatusMetaLabel}>{t('link_label')}:</span>
          <span
            className={`font-bold ${
              connected
                ? 'text-emerald-700 dark:text-emerald-400'
                : 'text-red-600 dark:text-red-400'
            }`}
          >
            {connected ? t('node_connected') : t('node_disconnected')}
          </span>
          <span className="text-slate-400 dark:text-slate-500">
            ({connection.ip}:{connection.port})
          </span>
        </div>

        {extra}
      </div>

      {metric && (
        <div className={`${hmiStatusMeta} shrink-0`}>
          <span className={hmiStatusMetaLabel}>{metric.label}:</span>
          <span
            className={`${hmiStatusChip} ${
              metricToneClass[metric.tone ?? 'default']
            }`}
          >
            {metric.value}
          </span>
        </div>
      )}
    </div>
  );
};
