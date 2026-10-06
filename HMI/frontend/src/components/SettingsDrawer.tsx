import React, { useState } from 'react';
import {
  X,
  ShieldCheck,
  Check,
  Terminal,
  RotateCcw,
  Gauge,
  Wrench,
  Volume2,
  VolumeX,
} from 'lucide-react';
import { useApp } from '../context/AppContext';
import { MaintenanceCycleResetModal } from './MaintenanceCycleResetModal';
import { hmiAssistHint, hmiBtnSecondary, hmiPanelCls } from '../styles/hmiUi';

interface SettingsDrawerProps {
  isOpen: boolean;
  onClose: () => void;
  connected?: boolean;
  maintenanceCycleCount?: number;
  safetyExhaust?: boolean;
  buzzerMute?: boolean;
  onBuzzerMute?: (mute: boolean) => void;
}

export const SettingsDrawer: React.FC<SettingsDrawerProps> = ({
  isOpen,
  onClose,
  connected = true,
  maintenanceCycleCount = 0,
  safetyExhaust = false,
  buzzerMute = false,
  onBuzzerMute,
}) => {
  const { showLogs, setShowLogs, debugMode, t } = useApp();
  const [showResetModal, setShowResetModal] = useState(false);
  const safetyOk = !safetyExhaust;

  if (!isOpen) return null;

  const panelCls =
    'rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 shadow-2xs';
  const sectionTitle =
    'text-xs font-bold uppercase tracking-wider text-slate-600 dark:text-slate-400 mb-2 shrink-0 sm:text-sm';
  const centerSectionTitle =
    'text-base font-bold uppercase tracking-wider text-slate-800 dark:text-slate-200 shrink-0 sm:text-lg';
  const centerAssistHint = 'text-sm leading-snug text-slate-600 dark:text-slate-400 sm:text-base';
  const iconSection = 'h-7 w-7 shrink-0';

  return (
    <div className="fixed inset-0 z-50 flex justify-end animate-fade-in">
      <div
        className="fixed inset-0 bg-slate-900/50 backdrop-blur-xs transition-opacity"
        onClick={onClose}
      />

      <div
        className={`relative z-50 flex h-full w-full max-w-md flex-col ${panelCls} border-l shadow-2xl overflow-y-auto`}
      >
        <div className="sticky top-0 z-10 flex shrink-0 items-center justify-between border-b border-slate-200 dark:border-slate-800 bg-white/95 px-3 py-3 backdrop-blur-xs dark:bg-slate-900/95 sm:px-4">
          <div className="flex min-w-0 items-center gap-2 border-slate-100 dark:border-slate-800">
            <Wrench className={`${iconSection} text-sky-700 dark:text-sky-400`} />
            <div className="min-w-0">
              <h2 className={`${centerSectionTitle} text-sky-900 dark:text-sky-100`}>
                {t('maintenance_title')}
              </h2>
              <p className="truncate text-sm text-slate-500 dark:text-slate-400 sm:text-base">
                {t('maintenance_desc')}
              </p>
            </div>
          </div>
          <button
            onClick={onClose}
            className="shrink-0 rounded-lg p-1.5 text-slate-400 hover:bg-slate-100 hover:text-slate-600 dark:hover:bg-slate-800 dark:hover:text-slate-200 transition"
            title={t('close')}
          >
            <X className="h-5 w-5" />
          </button>
        </div>

        <div className="flex flex-1 flex-col gap-2.5 p-3 sm:p-4">
          <section className={`shrink-0 p-3 ${panelCls}`}>
            <div className="mb-2 flex items-center gap-2">
              <Gauge className={`${iconSection} text-sky-600 dark:text-sky-400`} />
              <h3 className={sectionTitle.replace(' mb-2', '')}>
                {t('maintenance_cycle_counter_title')}
              </h3>
            </div>

            <div className="rounded-md border border-sky-200 bg-sky-50/80 px-4 py-3 dark:border-sky-800 dark:bg-sky-950/30">
              <p className="text-base font-semibold text-slate-600 dark:text-slate-400">
                {t('maintenance_cycle_counter_label')}
              </p>
              <p
                id="maintenance-cycle-count"
                className="mt-1 font-mono text-2xl font-bold tabular-nums text-sky-800 dark:text-sky-200 sm:text-3xl"
              >
                {maintenanceCycleCount.toLocaleString('es-MX')}
              </p>
            </div>

            <p className={`mt-3 ${centerAssistHint}`}>
              {t('maintenance_cycle_counter_hint')}
            </p>

            <button
              type="button"
              id="btn-maintenance-cycle-reset"
              onClick={() => setShowResetModal(true)}
              className={`${hmiBtnSecondary} mt-3 w-full border-amber-300 bg-amber-50 text-amber-900 hover:bg-amber-100 dark:border-amber-800 dark:bg-amber-950/40 dark:text-amber-100 dark:hover:bg-amber-950/60`}
            >
              <RotateCcw className="h-5 w-5 shrink-0" />
              {t('maintenance_reset_button')}
            </button>
          </section>

          <section className={`shrink-0 p-3 ${panelCls}`}>
            <h3 className={sectionTitle}>{t('safety_exhaust')}</h3>
            <div
              id="dbg-safety-exhaust"
              title={t('safety_exhaust_hint')}
              className={`${hmiPanelCls} p-3 shadow-2xs select-none ${
                safetyOk
                  ? 'border-emerald-500 bg-emerald-50 dark:bg-emerald-950/30 ring-2 ring-emerald-500/20'
                  : 'border-red-500 bg-red-50 dark:bg-red-950/40 ring-2 ring-red-500/25 animate-pulse'
              }`}
            >
              <div className="flex items-center justify-between gap-3">
                <div className="flex items-center gap-2.5">
                  <span
                    className={`h-2.5 w-2.5 rounded-full ${
                      safetyOk ? 'bg-emerald-500' : 'bg-red-500'
                    }`}
                  />
                  <div>
                    <div className="text-sm font-bold sm:text-base">{t('safety_exhaust')}</div>
                    <div className={hmiAssistHint}>
                      {safetyOk
                        ? t('safety_exhaust_on_desc')
                        : t('safety_exhaust_off_desc')}
                    </div>
                  </div>
                </div>
                <span
                  className={`text-sm font-bold sm:text-base ${
                    safetyOk
                      ? 'text-emerald-700 dark:text-emerald-300'
                      : 'text-red-700 dark:text-red-300'
                  }`}
                >
                  {safetyOk ? t('safety_exhaust_on') : t('safety_exhaust_off')}
                </span>
              </div>
            </div>
          </section>

          {onBuzzerMute && (
            <section className={`shrink-0 p-3 ${panelCls}`}>
              <h3 className={sectionTitle}>{t('andon_buzzer_mute_title')}</h3>
              <button
                type="button"
                id="btn-buzzer-mute"
                onClick={() => onBuzzerMute(!buzzerMute)}
                className={`flex w-full items-center justify-between rounded-xl border p-3.5 text-left transition shadow-2xs ${
                  buzzerMute
                    ? 'border-amber-500 bg-amber-50 dark:bg-amber-950/30 ring-2 ring-amber-500/20'
                    : 'border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900/50'
                }`}
              >
                <div className="flex items-center gap-2.5">
                  {buzzerMute ? (
                    <VolumeX className="h-5 w-5 shrink-0 text-amber-600" />
                  ) : (
                    <Volume2 className="h-5 w-5 shrink-0 text-slate-500" />
                  )}
                  <div>
                    <div className="text-sm font-bold sm:text-base">
                      {t('andon_buzzer_mute_title')}
                    </div>
                    <div className="text-sm text-slate-500 dark:text-slate-400">
                      {buzzerMute ? t('andon_buzzer_muted') : t('andon_buzzer_on')}
                    </div>
                  </div>
                </div>
                {buzzerMute && <Check className="h-5 w-5 shrink-0 text-amber-600" />}
              </button>
            </section>
          )}

          {debugMode && (
            <section className={`shrink-0 p-3 ${panelCls}`}>
              <h3 className={sectionTitle}>{t('logs_visibility_title')}</h3>
              <button
                type="button"
                onClick={() => setShowLogs(!showLogs)}
                className={`flex w-full items-center justify-between rounded-md border px-3 py-2.5 text-left transition ${
                  showLogs
                    ? 'border-teal-500 bg-teal-50 dark:bg-teal-950/30 ring-1 ring-teal-500/20'
                    : 'border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900/50'
                }`}
              >
                <div className="flex items-center gap-2.5">
                  <Terminal className="h-5 w-5 shrink-0 text-teal-600" />
                  <div>
                    <div className="text-sm font-bold sm:text-base">{t('logs_visibility_title')}</div>
                    <div className="text-sm text-slate-500 dark:text-slate-400">
                      {showLogs ? t('logs_visible') : t('logs_hidden')}
                    </div>
                  </div>
                </div>
                {showLogs && <Check className="h-5 w-5 shrink-0 text-teal-600" />}
              </button>
            </section>
          )}

          <section className={`shrink-0 p-3 ${panelCls}`}>
            <div className="mb-2 flex items-center gap-2">
              <ShieldCheck className="h-5 w-5 shrink-0 text-emerald-500" />
              <h3 className={sectionTitle.replace(' mb-2', '')}>{t('system_info_title')}</h3>
            </div>
            <div className="grid grid-cols-2 gap-2 text-sm text-slate-500 font-mono sm:text-base">
              <div>
                {t('port')}:{' '}
                <span className="font-bold text-slate-800 dark:text-slate-200">:5050</span>
              </div>
              <div>
                SSE:{' '}
                <span className={`font-bold ${connected ? 'text-emerald-600' : 'text-red-500'}`}>
                  {connected ? 'OK' : '—'}
                </span>
              </div>
              <div className="col-span-2">
                {t('protocol')}: <span className="font-bold">Red local</span>
              </div>
            </div>
          </section>
        </div>

        <div className="sticky bottom-0 shrink-0 border-t border-slate-200 dark:border-slate-800 bg-white/95 p-3 dark:bg-slate-900/95 sm:p-4">
          <button
            onClick={onClose}
            className="w-full rounded-md border border-slate-300 bg-white py-2.5 text-sm font-bold text-slate-700 hover:bg-slate-50 dark:border-slate-700 dark:bg-slate-800 dark:text-slate-200 dark:hover:bg-slate-700 sm:text-base"
          >
            {t('close')}
          </button>
        </div>
      </div>

      <MaintenanceCycleResetModal
        isOpen={showResetModal}
        onClose={() => setShowResetModal(false)}
      />
    </div>
  );
};
