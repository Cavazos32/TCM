import React from 'react';
import {
  X,
  Sun,
  Moon,
  Globe,
  Sliders,
  Network,
  ShieldCheck,
  Check,
  Terminal,
  RefreshCw,
} from 'lucide-react';
import { useApp } from '../context/AppContext';
import { ConnectionState } from '../types';

interface SettingsDrawerProps {
  isOpen: boolean;
  onClose: () => void;
  motionConn: ConnectionState;
  plcConn: ConnectionState;
  preFeederConn: ConnectionState;
  connected?: boolean;
  onReconnectNetwork?: () => void;
  reconnecting?: boolean;
}

export const SettingsDrawer: React.FC<SettingsDrawerProps> = ({
  isOpen,
  onClose,
  motionConn,
  plcConn,
  preFeederConn,
  connected = true,
  onReconnectNetwork,
  reconnecting = false,
}) => {
  const { language, setLanguage, isDarkMode, setIsDarkMode, showLogs, setShowLogs, t } = useApp();

  if (!isOpen) return null;

  const nodes = [
    { label: 'Motion ASDA B3', conn: motionConn },
    { label: 'PLC Válvulas', conn: plcConn },
    { label: 'PreFeeder Feed', conn: preFeederConn },
  ];

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
                {t('settings_desc')}
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

          <hr className="border-slate-200 dark:border-slate-800" />

          <div className="space-y-3">
            <div className="flex items-center gap-2">
              <Globe className="h-4 w-4 text-sky-500" />
              <h3 className="text-xs font-bold uppercase tracking-wider text-slate-700 dark:text-slate-300">
                {t('language_title')}
              </h3>
            </div>
            <div className="grid grid-cols-2 gap-3">
              <button
                onClick={() => setLanguage('es')}
                className={`rounded-xl border p-3 text-xs font-bold ${language === 'es' ? 'border-slate-900 dark:border-emerald-500 bg-slate-50 dark:bg-slate-800' : 'border-slate-200 dark:border-slate-800'}`}
              >
                Español
              </button>
              <button
                onClick={() => setLanguage('en')}
                className={`rounded-xl border p-3 text-xs font-bold ${language === 'en' ? 'border-slate-900 dark:border-emerald-500 bg-slate-50 dark:bg-slate-800' : 'border-slate-200 dark:border-slate-800'}`}
              >
                English
              </button>
            </div>
          </div>

          <hr className="border-slate-200 dark:border-slate-800" />

          <div className="space-y-3">
            <div className="flex items-center gap-2">
              <Terminal className="h-4 w-4 text-teal-500" />
              <h3 className="text-xs font-bold uppercase tracking-wider text-slate-700 dark:text-slate-300">
                {t('logs_visibility_title')}
              </h3>
            </div>
            <p className="text-[11px] text-slate-500 dark:text-slate-400 leading-relaxed">
              {t('logs_visibility_desc')}
            </p>
            <button
              type="button"
              onClick={() => setShowLogs(!showLogs)}
              className={`flex w-full items-center justify-between rounded-xl border p-3.5 text-left transition shadow-2xs ${
                showLogs
                  ? 'border-teal-500 bg-teal-50 dark:bg-teal-950/30 ring-2 ring-teal-500/20'
                  : 'border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900/50'
              }`}
            >
              <span className="text-xs font-bold">
                {showLogs ? t('logs_visible') : t('logs_hidden')}
              </span>
              {showLogs && <Check className="h-4 w-4 text-teal-600" />}
            </button>
          </div>

          <hr className="border-slate-200 dark:border-slate-800" />

          <div className="space-y-3">
            <div className="flex items-center gap-2">
              <Network className="h-4 w-4 text-indigo-500" />
              <h3 className="text-xs font-bold uppercase tracking-wider text-slate-700 dark:text-slate-300">
                {t('network_nodes_title')}
              </h3>
            </div>
            <div className="space-y-2 font-mono text-xs">
              {nodes.map(({ label, conn }) => (
                <div
                  key={label}
                  className="flex items-center justify-between rounded-lg border border-slate-200 dark:border-slate-800 bg-slate-50 dark:bg-slate-800/60 p-2.5"
                >
                  <div className="flex items-center gap-2">
                    <span
                      className={`h-2.5 w-2.5 rounded-full ${conn.connected ? 'bg-emerald-500' : 'bg-red-500'}`}
                    />
                    <div>
                      <div className="font-sans font-semibold">{label}</div>
                      <div className="text-[10px] text-slate-500">
                        {conn.ip}:{conn.port}
                      </div>
                    </div>
                  </div>
                  <span className="text-[10px] font-bold uppercase">
                    {conn.connected ? t('node_connected') : t('node_disconnected')}
                  </span>
                </div>
              ))}
            </div>
            <button
              type="button"
              onClick={onReconnectNetwork}
              disabled={!onReconnectNetwork || reconnecting}
              title={t('reconnect_all_title')}
              className="flex w-full items-center justify-center gap-2 rounded-xl border border-indigo-300 dark:border-indigo-700 bg-indigo-50 dark:bg-indigo-950/40 px-4 py-2.5 text-xs font-bold text-indigo-800 dark:text-indigo-200 transition hover:bg-indigo-100 dark:hover:bg-indigo-900/50 disabled:opacity-50"
            >
              <RefreshCw className={`h-4 w-4 ${reconnecting ? 'animate-spin' : ''}`} />
              {reconnecting ? t('reconnecting') : t('reconnect_all')}
            </button>
          </div>

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
