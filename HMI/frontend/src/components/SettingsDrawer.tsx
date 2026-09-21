import React, { useState } from 'react';
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
  Bug,
  VolumeX,
  Volume2,
  Lock,
  Unlock,
  LogOut,
} from 'lucide-react';
import { useApp } from '../context/AppContext';
import { ConnectionState } from '../types';

interface SettingsDrawerProps {
  isOpen: boolean;
  onClose: () => void;
  motionConn: ConnectionState;
  plcConn: ConnectionState;
  preFeederConn: ConnectionState;
  andonConn: ConnectionState;
  andonBuzzerMute: boolean;
  onAndonBuzzerMute: (mute: boolean) => void;
  ignorePrefeeder?: boolean;
  onIgnorePrefeeder?: (on: boolean) => void;
  /** IO Safety air (Motion): safetyExhaust true = trip; UI ON = OK. */
  safetyExhaust?: boolean;
  connected?: boolean;
  onReconnectNetwork?: () => void;
  reconnecting?: boolean;
  onDebugModeDisable?: () => void;
}

export const SettingsDrawer: React.FC<SettingsDrawerProps> = ({
  isOpen,
  onClose,
  motionConn,
  plcConn,
  preFeederConn,
  andonConn,
  andonBuzzerMute,
  onAndonBuzzerMute,
  ignorePrefeeder = false,
  onIgnorePrefeeder,
  safetyExhaust = false,
  connected = true,
  onReconnectNetwork,
  reconnecting = false,
  onDebugModeDisable,
}) => {
  const {
    language,
    setLanguage,
    isDarkMode,
    setIsDarkMode,
    showLogs,
    setShowLogs,
    debugMode,
    enableDebugMode,
    disableDebugMode,
    t,
  } = useApp();

  const [showPasswordPrompt, setShowPasswordPrompt] = useState(false);
  const [password, setPassword] = useState('');
  const [passwordError, setPasswordError] = useState<'invalid' | 'server' | null>(null);
  const [unlocking, setUnlocking] = useState(false);

  if (!isOpen) return null;

  const nodes = [
    { label: 'Motion ASDA B3', conn: motionConn },
    { label: 'PLC Válvulas', conn: plcConn },
    { label: 'PreFeeder Feed', conn: preFeederConn },
    { label: 'Andon Torre', conn: andonConn },
  ];

  const handleEnterDebug = () => {
    setShowPasswordPrompt(true);
    setPassword('');
    setPasswordError(null);
  };

  const handleExitDebug = () => {
    disableDebugMode();
    onDebugModeDisable?.();
    setShowPasswordPrompt(false);
    setPassword('');
    setPasswordError(null);
  };

  const handleUnlock = async () => {
    setUnlocking(true);
    setPasswordError(null);
    try {
      const result = await enableDebugMode(password);
      if (result === 'ok') {
        setShowPasswordPrompt(false);
        setPassword('');
      } else {
        setPasswordError(result);
      }
    } finally {
      setUnlocking(false);
    }
  };

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
              <Bug className="h-4 w-4 text-orange-500" />
              <h3 className="text-xs font-bold uppercase tracking-wider text-slate-700 dark:text-slate-300">
                {t('debug_title')}
              </h3>
            </div>
            <p className="text-[11px] text-slate-500 dark:text-slate-400 leading-relaxed">
              {t('debug_desc')}
            </p>

            {!debugMode ? (
              <button
                type="button"
                id="btn-debug-mode"
                onClick={handleEnterDebug}
                className="flex w-full items-center justify-between rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900/50 p-3.5 text-left transition shadow-2xs"
              >
                <div className="flex items-center gap-2.5">
                  <Lock className="h-4 w-4 text-slate-500" />
                  <div>
                    <div className="text-xs font-bold">{t('debug_mode_title')}</div>
                    <div className="text-[10px] text-slate-500 dark:text-slate-400">
                      {t('debug_mode_off')}
                    </div>
                  </div>
                </div>
              </button>
            ) : (
              <div className="rounded-xl border border-orange-500 bg-orange-50 dark:bg-orange-950/30 ring-2 ring-orange-500/20 p-3.5 space-y-3">
                <div className="flex items-center justify-between">
                  <div className="flex items-center gap-2.5">
                    <Unlock className="h-4 w-4 text-orange-600" />
                    <div>
                      <div className="text-xs font-bold">{t('debug_mode_title')}</div>
                      <div className="text-[10px] text-slate-500 dark:text-slate-400">
                        {t('debug_mode_on')}
                      </div>
                    </div>
                  </div>
                  <Check className="h-4 w-4 text-orange-600" />
                </div>
                <button
                  type="button"
                  id="btn-debug-exit"
                  onClick={handleExitDebug}
                  className="flex w-full items-center justify-center gap-2 rounded-lg border border-orange-300 dark:border-orange-800 bg-white dark:bg-slate-900 px-3 py-2.5 text-xs font-bold text-orange-800 dark:text-orange-200 hover:bg-orange-100 dark:hover:bg-orange-950/50 transition"
                >
                  <LogOut className="h-3.5 w-3.5" />
                  {t('debug_mode_exit')}
                </button>
              </div>
            )}

            {showPasswordPrompt && !debugMode && (
              <div className="rounded-xl border border-orange-200 dark:border-orange-900/50 bg-orange-50/80 dark:bg-orange-950/20 p-3.5 space-y-3">
                <div>
                  <div className="text-xs font-bold text-slate-800 dark:text-slate-100">
                    {t('debug_mode_password_title')}
                  </div>
                  <p className="text-[10px] text-slate-500 dark:text-slate-400 mt-0.5">
                    {t('debug_mode_password_hint')}
                  </p>
                </div>
                <input
                  type="password"
                  id="input-debug-password"
                  autoFocus
                  value={password}
                  onChange={(e) => {
                    setPassword(e.target.value);
                    setPasswordError(null);
                  }}
                  onKeyDown={(e) => {
                    if (e.key === 'Enter') void handleUnlock();
                    if (e.key === 'Escape') {
                      setShowPasswordPrompt(false);
                      setPassword('');
                      setPasswordError(null);
                    }
                  }}
                  placeholder={t('debug_mode_password_placeholder')}
                  className={`w-full rounded-lg border px-3 py-2 text-xs font-mono bg-white dark:bg-slate-900 ${
                    passwordError
                      ? 'border-red-400 focus:ring-red-400'
                      : 'border-slate-300 dark:border-slate-700'
                  }`}
                />
                {passwordError && (
                  <p className="text-[10px] font-semibold text-red-600 dark:text-red-400">
                    {passwordError === 'server'
                      ? t('debug_mode_server_error')
                      : t('debug_mode_wrong_password')}
                  </p>
                )}
                <div className="flex gap-2">
                  <button
                    type="button"
                    onClick={() => {
                      setShowPasswordPrompt(false);
                      setPassword('');
                      setPasswordError(null);
                    }}
                    className="flex-1 rounded-lg border border-slate-300 dark:border-slate-700 px-3 py-2 text-xs font-bold text-slate-600 dark:text-slate-300"
                  >
                    {t('debug_mode_cancel')}
                  </button>
                  <button
                    type="button"
                    id="btn-debug-unlock"
                    disabled={unlocking || !password}
                    onClick={() => void handleUnlock()}
                    className="flex-1 rounded-lg bg-orange-600 hover:bg-orange-700 disabled:opacity-40 px-3 py-2 text-xs font-bold text-white"
                  >
                    {t('debug_mode_unlock')}
                  </button>
                </div>
              </div>
            )}

            {debugMode && (
              <>
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

                {onIgnorePrefeeder && (
                  <button
                    type="button"
                    id="btn-ignore-prefeeder"
                    onClick={() => onIgnorePrefeeder(!ignorePrefeeder)}
                    className={`flex w-full items-center justify-between rounded-xl border p-3.5 text-left transition shadow-2xs ${
                      ignorePrefeeder
                        ? 'border-violet-500 bg-violet-50 dark:bg-violet-950/30 ring-2 ring-violet-500/20'
                        : 'border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900/50'
                    }`}
                  >
                    <div className="flex items-center gap-2.5">
                      <Network className="h-4 w-4 text-violet-600" />
                      <div>
                        <div className="text-xs font-bold">{t('ignore_prefeeder_title')}</div>
                        <div className="text-[10px] text-slate-500 dark:text-slate-400">
                          {ignorePrefeeder
                            ? t('ignore_prefeeder_on')
                            : t('ignore_prefeeder_off')}
                        </div>
                      </div>
                    </div>
                    {ignorePrefeeder && <Check className="h-4 w-4 text-violet-600" />}
                  </button>
                )}

                <button
                  type="button"
                  onClick={() => onAndonBuzzerMute(!andonBuzzerMute)}
                  className={`flex w-full items-center justify-between rounded-xl border p-3.5 text-left transition shadow-2xs ${
                    andonBuzzerMute
                      ? 'border-amber-500 bg-amber-50 dark:bg-amber-950/30 ring-2 ring-amber-500/20'
                      : 'border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900/50'
                  }`}
                >
                  <div className="flex items-center gap-2.5">
                    {andonBuzzerMute ? (
                      <VolumeX className="h-4 w-4 text-amber-600" />
                    ) : (
                      <Volume2 className="h-4 w-4 text-slate-500" />
                    )}
                    <div>
                      <div className="text-xs font-bold">{t('andon_buzzer_mute_title')}</div>
                      <div className="text-[10px] text-slate-500 dark:text-slate-400">
                        {andonBuzzerMute ? t('andon_buzzer_muted') : t('andon_buzzer_on')}
                      </div>
                    </div>
                  </div>
                  {andonBuzzerMute && <Check className="h-4 w-4 text-amber-600" />}
                </button>

                {(() => {
                  const safetyOk = !safetyExhaust;
                  return (
                    <div
                      id="dbg-safety-exhaust"
                      title={t('safety_exhaust_hint')}
                      className={`flex w-full items-center justify-between rounded-xl border p-3.5 text-left shadow-2xs select-none ${
                        safetyOk
                          ? 'border-emerald-500 bg-emerald-50 dark:bg-emerald-950/30 ring-2 ring-emerald-500/20'
                          : 'border-red-500 bg-red-50 dark:bg-red-950/40 ring-2 ring-red-500/25 animate-pulse'
                      }`}
                    >
                      <div className="flex items-center gap-2.5">
                        <span
                          className={`h-2.5 w-2.5 rounded-full ${
                            safetyOk ? 'bg-emerald-500' : 'bg-red-500'
                          }`}
                        />
                        <div>
                          <div className="text-xs font-bold">{t('safety_exhaust')}</div>
                          <div className="text-[10px] text-slate-500 dark:text-slate-400">
                            {safetyOk
                              ? t('safety_exhaust_on_desc')
                              : t('safety_exhaust_off_desc')}
                          </div>
                        </div>
                      </div>
                      <span
                        className={`text-[10px] font-bold ${
                          safetyOk
                            ? 'text-emerald-700 dark:text-emerald-300'
                            : 'text-red-700 dark:text-red-300'
                        }`}
                      >
                        {safetyOk ? t('safety_exhaust_on') : t('safety_exhaust_off')}
                      </span>
                    </div>
                  );
                })()}
              </>
            )}
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
