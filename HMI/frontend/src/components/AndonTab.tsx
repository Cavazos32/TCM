import React from 'react';
import {
  Lightbulb,
  Volume2,
  PowerOff,
  RotateCcw,
  Circle,
} from 'lucide-react';
import { AndonState, LogEntry } from '../types';
import { LogTerminal } from './LogTerminal';
import { useApp } from '../context/AppContext';

interface AndonTabProps {
  andonState: AndonState;
  onSetOut: (out: 'green' | 'yellow' | 'red' | 'buzzer', on: boolean) => void;
  onAllOff: () => void;
  onResumeAuto: () => void;
  onMachineState: (byte: number) => void;
  showLogs?: boolean;
  logs: LogEntry[];
  onClearLogs: () => void;
}

const OUTPUTS: {
  id: 'green' | 'yellow' | 'red' | 'buzzer';
  labelKey: 'andon_out_green' | 'andon_out_yellow' | 'andon_out_red' | 'andon_out_buzzer';
  dotClass: string;
  activeClass: string;
}[] = [
  {
    id: 'green',
    labelKey: 'andon_out_green',
    dotClass: 'bg-emerald-500',
    activeClass: 'border-emerald-500 bg-emerald-50 dark:bg-emerald-950/30',
  },
  {
    id: 'yellow',
    labelKey: 'andon_out_yellow',
    dotClass: 'bg-amber-400',
    activeClass: 'border-amber-500 bg-amber-50 dark:bg-amber-950/30',
  },
  {
    id: 'red',
    labelKey: 'andon_out_red',
    dotClass: 'bg-red-500',
    activeClass: 'border-red-500 bg-red-50 dark:bg-red-950/30',
  },
  {
    id: 'buzzer',
    labelKey: 'andon_out_buzzer',
    dotClass: 'bg-slate-500',
    activeClass: 'border-slate-600 bg-slate-100 dark:bg-slate-800/80',
  },
];

const PRESETS: {
  byte: number;
  labelKey:
    | 'andon_preset_idle'
    | 'andon_preset_busy'
    | 'andon_preset_pause'
    | 'andon_preset_stop'
    | 'andon_preset_error'
    | 'andon_preset_finish'
    | 'andon_preset_materialist';
  hint: string;
}[] = [
  { byte: 0x44, labelKey: 'andon_preset_idle', hint: 'Green' },
  { byte: 0x45, labelKey: 'andon_preset_busy', hint: 'Green' },
  { byte: 0x48, labelKey: 'andon_preset_pause', hint: 'Yellow' },
  { byte: 0x42, labelKey: 'andon_preset_stop', hint: 'Red' },
  { byte: 0x46, labelKey: 'andon_preset_error', hint: 'Red + Buzzer' },
  { byte: 0x47, labelKey: 'andon_preset_finish', hint: 'R→Y→G + Buzzer' },
  { byte: 0x49, labelKey: 'andon_preset_materialist', hint: 'Yellow + Buzzer' },
];

export const AndonTab: React.FC<AndonTabProps> = ({
  andonState,
  onSetOut,
  onAllOff,
  onResumeAuto,
  onMachineState,
  showLogs = true,
  logs,
  onClearLogs,
}) => {
  const { t } = useApp();
  const s = andonState;

  const activeCount = [s.green, s.yellow, s.red, s.buzzer].filter(Boolean).length;

  return (
    <div className="space-y-4">
      <div className="rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 px-4 py-2.5 shadow-2xs flex flex-wrap items-center justify-between gap-3">
        <div className="flex items-center gap-3">
          <span
            className={`h-2.5 w-2.5 rounded-full ${
              s.connection.connected ? 'bg-emerald-500 animate-pulse' : 'bg-red-500'
            }`}
          />
          <span className="text-sm font-semibold text-slate-900 dark:text-white">
            {s.manual ? t('andon_mode_manual') : t('andon_mode_auto')}
          </span>
          <span className="text-slate-300 dark:text-slate-700">|</span>
          <span className="font-mono text-xs text-slate-600 dark:text-slate-400">
            {s.connection.ip}:{s.connection.port}
          </span>
        </div>
        <div className="flex items-center gap-2 text-xs font-mono">
          <span className="text-slate-500">{t('andon_active_outputs')}:</span>
          <span className="font-bold text-slate-900 dark:text-white">
            {activeCount} / 4
          </span>
        </div>
      </div>

      <div className="rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 p-4 sm:p-5 shadow-2xs">
        <div className="flex flex-wrap items-center justify-between gap-3 border-b border-slate-100 dark:border-slate-800 pb-3">
          <div className="flex items-center gap-2">
            <Lightbulb className="h-4 w-4 text-amber-500" />
            <h2 className="text-xs font-bold uppercase tracking-wider text-slate-800 dark:text-slate-200">
              {t('andon_manual_title')}
            </h2>
          </div>
          <div className="flex items-center gap-2">
            <button
              type="button"
              onClick={onResumeAuto}
              className="flex items-center gap-1.5 rounded-lg border border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 px-3 py-1.5 text-xs font-bold text-slate-700 dark:text-slate-200 hover:bg-slate-50 dark:hover:bg-slate-700"
            >
              <RotateCcw className="h-3.5 w-3.5" />
              {t('andon_resume_auto')}
            </button>
            <button
              type="button"
              onClick={onAllOff}
              className="flex items-center gap-1.5 rounded-lg border border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 px-3 py-1.5 text-xs font-bold text-slate-700 dark:text-slate-200 hover:bg-slate-50 dark:hover:bg-slate-700"
            >
              <PowerOff className="h-3.5 w-3.5" />
              {t('andon_all_off')}
            </button>
          </div>
        </div>

        <div className="mt-4 grid gap-3 sm:grid-cols-2 lg:grid-cols-4">
          {OUTPUTS.map(({ id, labelKey, dotClass, activeClass }) => {
            const active = s[id];
            return (
              <div
                key={id}
                className={`rounded-xl border p-4 transition ${active ? activeClass : 'border-slate-200 dark:border-slate-800'}`}
              >
                <div className="flex items-center gap-2 mb-3">
                  <span className={`h-3 w-3 rounded-full ${active ? dotClass : 'bg-slate-300 dark:bg-slate-600'}`} />
                  <span className="text-sm font-bold text-slate-800 dark:text-slate-100">
                    {t(labelKey)}
                  </span>
                  {id === 'buzzer' && (
                    <Volume2 className="h-3.5 w-3.5 text-slate-400 ml-auto" />
                  )}
                </div>
                <div className="grid grid-cols-2 gap-2">
                  <button
                    type="button"
                    id={`btn-andon-${id}-on`}
                    onClick={() => onSetOut(id, true)}
                    className={`rounded-lg border px-2 py-2 text-xs font-bold transition ${
                      active
                        ? 'border-emerald-600 bg-emerald-600 text-white'
                        : 'border-slate-300 dark:border-slate-600 hover:bg-slate-50 dark:hover:bg-slate-800'
                    }`}
                  >
                    ON
                  </button>
                  <button
                    type="button"
                    id={`btn-andon-${id}-off`}
                    onClick={() => onSetOut(id, false)}
                    className={`rounded-lg border px-2 py-2 text-xs font-bold transition ${
                      !active
                        ? 'border-slate-700 bg-slate-800 text-white dark:border-slate-500 dark:bg-slate-600'
                        : 'border-slate-300 dark:border-slate-600 hover:bg-slate-50 dark:hover:bg-slate-800'
                    }`}
                  >
                    OFF
                  </button>
                </div>
              </div>
            );
          })}
        </div>
      </div>

      <div className="rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 p-4 sm:p-5 shadow-2xs">
        <div className="flex items-center gap-2 border-b border-slate-100 dark:border-slate-800 pb-3 mb-3">
          <Circle className="h-4 w-4 text-indigo-500" />
          <h2 className="text-xs font-bold uppercase tracking-wider text-slate-800 dark:text-slate-200">
            {t('andon_presets_title')}
          </h2>
          <span className="text-[10px] text-slate-500 dark:text-slate-400 ml-1">
            {t('andon_presets_desc')}
          </span>
        </div>
        <div className="grid gap-2 sm:grid-cols-2 lg:grid-cols-3">
          {PRESETS.map(({ byte, labelKey, hint }) => (
            <button
              key={byte}
              type="button"
              id={`btn-andon-preset-${byte}`}
              onClick={() => onMachineState(byte)}
              className="flex items-center justify-between rounded-lg border border-slate-200 dark:border-slate-700 bg-slate-50 dark:bg-slate-800/60 px-3 py-2.5 text-left hover:bg-slate-100 dark:hover:bg-slate-800 transition"
            >
              <span className="text-xs font-bold text-slate-800 dark:text-slate-200">
                {t(labelKey)}
              </span>
              <span className="font-mono text-[10px] text-slate-500">
                0x{byte.toString(16).toUpperCase().padStart(2, '0')} · {hint}
              </span>
            </button>
          ))}
        </div>
      </div>

      {showLogs && (
        <LogTerminal
          title={t('logs_title')}
          logs={logs}
          onClear={onClearLogs}
          filterModule="ANDON"
        />
      )}
    </div>
  );
};
