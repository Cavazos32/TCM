import React from 'react';
import {
  X,
  Sun,
  Moon,
  Sliders,
  ShieldCheck,
  Check,
  Terminal,
} from 'lucide-react';
import { useApp } from '../context/AppContext';

interface SettingsDrawerProps {
  isOpen: boolean;
  onClose: () => void;
  connected?: boolean;
}

export const SettingsDrawer: React.FC<SettingsDrawerProps> = ({
  isOpen,
  onClose,
  connected = true,
}) => {
  const {
    isDarkMode,
    setIsDarkMode,
    showLogs,
    setShowLogs,
    debugMode,
    t,
  } = useApp();

  if (!isOpen) return null;

  return (
    <div className="fixed inset-0 z-50 flex justify-end animate-fade-in">
      <div
        className="fixed inset-0 bg-slate-900/50 backdrop-blur-xs transition-opacity"
        onClick={onClose}
      />

      <div className="relative z-50 flex h-full w-full max-w-md flex-col bg-white dark:bg-slate-900 text-slate-900 dark:text-slate-100 shadow-2xl border-l border-slate-200 dark:border-slate-800 transition-transform duration-200 overflow-y-auto">
        <div className="sticky top-0 z-10 flex items-center justify-between border-b border-slate-200 dark:border-slate-800 bg-white/95 dark:bg-slate-900/95 px-5 py-4 backdrop-blur-xs">
          <div className="flex items-center gap-2.5">
            <div className="flex h-9 w-9 items-center justify-center rounded-lg bg-slate-100 dark:bg-slate-800 text-slate-800 dark:text-slate-200 border border-slate-200 dark:border-slate-700">
              <Sliders className="h-4 w-4" />
            </div>
            <div>
              <h2 className="text-sm font-bold tracking-tight text-slate-900 dark:text-white">
                {t('settings_title')}
              </h2>
              <p className="text-xs text-slate-500 dark:text-slate-400">
                {t('settings_desc_app')}
              </p>
            </div>
          </div>
          <button
            onClick={onClose}
            className="rounded-lg p-1.5 text-slate-400 hover:bg-slate-100 hover:text-slate-600 dark:hover:bg-slate-800 dark:hover:text-slate-200 transition"
            title={t('close')}
          >
            <X className="h-5 w-5" />
          </button>
        </div>

        <div className="p-5 space-y-6">
          <div className="space-y-3">
            <div className="flex items-center gap-2">
              <Sun className="h-4 w-4 text-amber-500" />
              <h3 className="text-xs font-bold uppercase tracking-wider text-slate-700 dark:text-slate-300">
                {t('appearance_title')}
              </h3>
            </div>
            <div className="grid grid-cols-2 gap-3 pt-1">
              <button
                onClick={() => setIsDarkMode(false)}
                className={`flex items-center justify-between rounded-xl border p-3.5 text-left transition shadow-2xs ${
                  !isDarkMode
                    ? 'border-slate-900 bg-slate-50 dark:bg-slate-800/80 ring-2 ring-slate-900/10 dark:ring-white/20'
                    : 'border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900/50'
                }`}
              >
                <div className="flex items-center gap-2.5">
                  <Sun className="h-4 w-4 text-amber-600" />
                  <span className="text-xs font-bold">{t('theme_light')}</span>
                </div>
                {!isDarkMode && <Check className="h-4 w-4" />}
              </button>
              <button
                onClick={() => setIsDarkMode(true)}
                className={`flex items-center justify-between rounded-xl border p-3.5 text-left transition shadow-2xs ${
                  isDarkMode
                    ? 'border-emerald-500 bg-slate-900 ring-2 ring-emerald-500/20'
                    : 'border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900/50'
                }`}
              >
                <div className="flex items-center gap-2.5">
                  <Moon className="h-4 w-4 text-indigo-300" />
                  <span className="text-xs font-bold">{t('theme_dark')}</span>
                </div>
                {isDarkMode && <Check className="h-4 w-4 text-emerald-400" />}
              </button>
            </div>
          </div>

          {debugMode && (
            <>
              <hr className="border-slate-200 dark:border-slate-800" />
              <div className="space-y-3">
                <div className="flex items-center gap-2">
                  <Terminal className="h-4 w-4 text-teal-600" />
                  <h3 className="text-xs font-bold uppercase tracking-wider text-slate-700 dark:text-slate-300">
                    {t('logs_visibility_title')}
                  </h3>
                </div>
                <button
                  type="button"
                  onClick={() => setShowLogs(!showLogs)}
                  className={`flex w-full items-center justify-between rounded-xl border p-3.5 text-left transition shadow-2xs ${
                    showLogs
                      ? 'border-teal-500 bg-teal-50 dark:bg-teal-950/30 ring-2 ring-teal-500/20'
                      : 'border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900/50'
                  }`}
                >
                  <div className="flex items-center gap-2.5">
                    <Terminal className="h-4 w-4 text-teal-600" />
                    <div>
                      <div className="text-xs font-bold">{t('logs_visibility_title')}</div>
                      <div className="text-[10px] text-slate-500 dark:text-slate-400">
                        {showLogs ? t('logs_visible') : t('logs_hidden')}
                      </div>
                    </div>
                  </div>
                  {showLogs && <Check className="h-4 w-4 text-teal-600" />}
                </button>
              </div>
            </>
          )}

          <hr className="border-slate-200 dark:border-slate-800" />

          <div className="rounded-xl border border-slate-200 dark:border-slate-800 bg-slate-50 dark:bg-slate-800/40 p-3.5 space-y-2">
            <div className="flex items-center gap-2 text-xs font-bold text-slate-700 dark:text-slate-300">
              <ShieldCheck className="h-4 w-4 text-emerald-500" />
              <span>{t('system_info_title')}</span>
            </div>
            <div className="grid grid-cols-2 gap-2 text-xs text-slate-500 font-mono">
              <div>{t('port')}: <span className="font-bold text-slate-800 dark:text-slate-200">:5050</span></div>
              <div>SSE: <span className={`font-bold ${connected ? 'text-emerald-600' : 'text-red-500'}`}>{connected ? 'OK' : '—'}</span></div>
              <div>{t('protocol')}: <span className="font-bold">Flask + TCP</span></div>
            </div>
          </div>
        </div>

        <div className="sticky bottom-0 border-t border-slate-200 dark:border-slate-800 bg-white/95 dark:bg-slate-900/95 p-4">
          <button
            onClick={onClose}
            className="w-full rounded-xl bg-slate-900 dark:bg-slate-100 text-white dark:text-slate-900 py-2.5 text-xs font-bold"
          >
            {t('close')}
          </button>
        </div>
      </div>
    </div>
  );
};
