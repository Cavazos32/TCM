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
import { ModuleStatusBar } from './ModuleStatusBar';
import { useApp } from '../context/AppContext';
import {
  hmiModuleBadge,
  hmiModuleBtnBase,
  hmiModuleBtnGroup,
  hmiModuleBtnSecondary,
  hmiModuleBtnSuccess,
  hmiModuleCenterSectionTitle,
  hmiModuleIconBtn,
  hmiModuleIconSection,
  hmiPanelCls,
  hmiPanelHeader,
  hmiPanelPadding,
  hmiStatusChip,
  hmiStatusDivider,
  hmiStatusMeta,
  hmiStatusMetaLabel,
} from '../styles/hmiUi';

interface AndonTabProps {
  andonState: AndonState;
  machineByte?: number;
  machineName?: string;
  buzzerMute?: boolean;
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
  hintKey:
    | 'andon_hint_idle'
    | 'andon_hint_busy'
    | 'andon_hint_pause'
    | 'andon_hint_stop'
    | 'andon_hint_error'
    | 'andon_hint_finish'
    | 'andon_hint_materialist';
}[] = [
  { byte: 0x44, labelKey: 'andon_preset_idle', hintKey: 'andon_hint_idle' },
  { byte: 0x45, labelKey: 'andon_preset_busy', hintKey: 'andon_hint_busy' },
  { byte: 0x48, labelKey: 'andon_preset_pause', hintKey: 'andon_hint_pause' },
  { byte: 0x42, labelKey: 'andon_preset_stop', hintKey: 'andon_hint_stop' },
  { byte: 0x46, labelKey: 'andon_preset_error', hintKey: 'andon_hint_error' },
  { byte: 0x47, labelKey: 'andon_preset_finish', hintKey: 'andon_hint_finish' },
  { byte: 0x49, labelKey: 'andon_preset_materialist', hintKey: 'andon_hint_materialist' },
];

export const AndonTab: React.FC<AndonTabProps> = ({
  andonState,
  machineByte,
  machineName,
  buzzerMute = false,
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
  const currentByte = machineByte ?? 0;
  const wantsBuzzer = currentByte === 0x46 || currentByte === 0x47 || currentByte === 0x49;
  const buzzerShown = buzzerMute ? false : s.buzzer;
  const activeCount = [s.green, s.yellow, s.red, buzzerShown].filter(Boolean).length;

  const connected = s.connection.connected;

  return (
    <div className="space-y-4">
      <ModuleStatusBar
        headline={
          !connected
            ? t('andon_no_link')
            : s.manual
              ? t('andon_mode_manual')
              : t('andon_follows_machine')
        }
        tone={connected ? (s.manual ? 'warning' : 'ready') : 'disconnected'}
        connection={s.connection}
        metric={{
          label: t('andon_active_outputs'),
          value: `${activeCount} / 4`,
          tone: activeCount > 0 ? 'success' : 'default',
        }}
        extra={
          <>
            <span className={hmiStatusDivider}>|</span>
            <div className={hmiStatusMeta}>
              <span className={hmiStatusMetaLabel}>{t('andon_current_state')}:</span>
              <strong className="text-slate-800 dark:text-slate-200">
                {machineName || '—'}
                {currentByte
                  ? ` · 0x${currentByte.toString(16).toUpperCase().padStart(2, '0')}`
                  : ''}
              </strong>
            </div>
            {s.pressure && (
              <>
                <span className={hmiStatusDivider}>|</span>
                <span
                  className={`${hmiStatusChip} bg-red-50 dark:bg-red-950/40 text-red-800 dark:text-red-200 border-red-300 dark:border-red-800 uppercase tracking-wider text-xs sm:text-sm`}
                  title={t('andon_pressure_fault_hint')}
                >
                  {t('andon_pressure_fault')}
                </span>
              </>
            )}
            {buzzerMute && (
              <>
                <span className={hmiStatusDivider}>|</span>
                <span
                  className={`${hmiStatusChip} bg-amber-50 dark:bg-amber-950/40 text-amber-800 dark:text-amber-200 border-amber-300 dark:border-amber-800 uppercase tracking-wider text-xs sm:text-sm`}
                  title={t('andon_buzzer_muted_hint')}
                >
                  {t('andon_buzzer_muted')}
                </span>
              </>
            )}
          </>
        }
      />

      <div className={`${hmiPanelCls} ${hmiPanelPadding}`}>
        <div className={hmiPanelHeader}>
          <div className="flex items-center gap-2">
            <Lightbulb className={`${hmiModuleIconSection} text-amber-500`} />
            <h2 className={hmiModuleCenterSectionTitle}>{t('andon_manual_title')}</h2>
          </div>
          <div className={hmiModuleBtnGroup}>
            <button type="button" onClick={onResumeAuto} className={hmiModuleBtnSecondary}>
              <RotateCcw className={hmiModuleIconBtn} />
              {t('andon_resume_auto')}
            </button>
            <button type="button" onClick={onAllOff} className={hmiModuleBtnSecondary}>
              <PowerOff className={hmiModuleIconBtn} />
              {t('andon_all_off')}
            </button>
          </div>
        </div>

        <div className="mt-4 grid gap-3 sm:grid-cols-2 lg:grid-cols-4">
          {OUTPUTS.map(({ id, labelKey, dotClass, activeClass }) => {
            const active = id === 'buzzer' ? buzzerShown : s[id];
            return (
              <div
                key={id}
                className={`rounded-xl border p-5 sm:p-6 transition ${active ? activeClass : 'border-slate-200 dark:border-slate-800'}`}
              >
                <div className="flex items-center gap-3 mb-4">
                  <span className={`h-4 w-4 rounded-full ${active ? dotClass : 'bg-slate-300 dark:bg-slate-600'}`} />
                  <span className="text-lg font-bold text-slate-800 dark:text-slate-100 sm:text-xl">
                    {t(labelKey)}
                  </span>
                  {id === 'buzzer' && (
                    <Volume2
                      className={`h-3.5 w-3.5 ml-auto ${
                        buzzerMute
                          ? 'text-amber-500'
                          : 'text-slate-400'
                      }`}
                    />
                  )}
                  {id === 'buzzer' && buzzerMute && wantsBuzzer && (
                    <span className="text-[10px] font-bold text-amber-700 dark:text-amber-300">
                      {t('andon_buzzer_muted')}
                    </span>
                  )}
                </div>
                <div className="grid grid-cols-2 gap-3">
                  <button
                    type="button"
                    id={`btn-andon-${id}-on`}
                    onClick={() => onSetOut(id, true)}
                    className={`${active ? hmiModuleBtnSuccess : hmiModuleBtnSecondary} w-full`}
                  >
                    ON
                  </button>
                  <button
                    type="button"
                    id={`btn-andon-${id}-off`}
                    onClick={() => onSetOut(id, false)}
                    className={`${
                      !active
                        ? `${hmiModuleBtnBase} border-slate-700 bg-slate-800 text-white dark:border-slate-500 dark:bg-slate-600`
                        : hmiModuleBtnSecondary
                    } w-full`}
                  >
                    OFF
                  </button>
                </div>
              </div>
            );
          })}
        </div>
      </div>

      <div className={`${hmiPanelCls} ${hmiPanelPadding}`}>
        <div className={hmiPanelHeader}>
          <div className="flex flex-wrap items-center gap-2">
            <Circle className={`${hmiModuleIconSection} text-indigo-500`} />
            <h2 className={hmiModuleCenterSectionTitle}>{t('andon_presets_title')}</h2>
            <span className="text-base text-slate-500 dark:text-slate-400 sm:text-lg">
              {t('andon_presets_desc')}
            </span>
          </div>
        </div>
        <div className="grid gap-2 sm:grid-cols-2 lg:grid-cols-3">
          {PRESETS.map(({ byte, labelKey, hintKey }) => (
            <button
              key={byte}
              type="button"
              id={`btn-andon-preset-${byte}`}
              onClick={() => onMachineState(byte)}
              className={`${hmiModuleBtnSecondary} w-full justify-between text-left ${
                currentByte === byte
                  ? 'border-indigo-400 bg-indigo-50 ring-1 ring-indigo-300 dark:border-indigo-600 dark:bg-indigo-950/40 dark:ring-indigo-800'
                  : 'bg-slate-50 dark:bg-slate-800/60'
              }`}
            >
              <span className="text-base font-bold text-slate-800 dark:text-slate-200 sm:text-lg">
                {t(labelKey)}
              </span>
              <span className={`${hmiModuleBadge} ml-2`}>{t(hintKey)}</span>
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
