import React from 'react';
import { Menu, Cpu, Settings2 } from 'lucide-react';
import { useApp } from '../context/AppContext';

interface HeaderProps {
  onOpenSettings: () => void;
  onOpenParametros: () => void;
  parametrosActive?: boolean;
}

export const Header: React.FC<HeaderProps> = ({
  onOpenSettings,
  onOpenParametros,
  parametrosActive = false,
}) => {
  const { t } = useApp();

  return (
    <header className="border-b border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 text-slate-900 dark:text-slate-100 shadow-2xs transition-colors">
      <div className="mx-auto flex w-full shrink-0 items-center justify-between px-3 py-2 sm:px-4">
        <div className="flex items-center gap-3">
          <button
            id="btn-app-settings-menu"
            onClick={onOpenSettings}
            title={t('open_settings')}
            className="flex h-11 w-11 items-center justify-center rounded-lg border border-slate-300 dark:border-slate-700 bg-slate-50 dark:bg-slate-800 text-slate-700 dark:text-slate-200 hover:bg-slate-100 dark:hover:bg-slate-700/80 hover:text-slate-900 dark:hover:text-white transition active:scale-95 shadow-2xs cursor-pointer"
            aria-label={t('settings')}
          >
            <Menu className="h-6 w-6" />
          </button>

          <div className="flex h-11 w-11 items-center justify-center rounded-lg bg-slate-100 dark:bg-slate-800 border border-slate-200 dark:border-slate-700 text-slate-800 dark:text-slate-200 shadow-2xs">
            <Cpu className="h-5 w-5" />
          </div>

          <div>
            <div className="flex items-center gap-2">
              <h1 className="text-xl font-bold tracking-tight text-slate-900 dark:text-white flex items-center gap-1.5">
                <span>TCM</span>
                <span className="text-slate-500 dark:text-slate-400 font-normal">HMI</span>
              </h1>
              <span className="rounded bg-slate-100 dark:bg-slate-800 border border-slate-200 dark:border-slate-700 px-2 py-0.5 font-mono text-xs font-medium text-slate-600 dark:text-slate-400">
                :5050
              </span>
            </div>
            <p className="text-xs text-slate-500 dark:text-slate-400 font-medium hidden sm:block">
              {t('hmi_subtitle')}
            </p>
          </div>
        </div>

        <button
          id="btn-parametros"
          type="button"
          onClick={onOpenParametros}
          title={t('btn_parametros')}
          className={`flex items-center gap-2 rounded-lg border px-4 py-2.5 text-sm font-bold transition shadow-2xs active:scale-95 cursor-pointer ${
            parametrosActive
              ? 'border-sky-600 bg-sky-700 text-white ring-2 ring-sky-400/40'
              : 'border-sky-300 dark:border-sky-700 bg-sky-50 dark:bg-sky-950/40 text-sky-900 dark:text-sky-100 hover:bg-sky-100 dark:hover:bg-sky-900/50'
          }`}
        >
          <Settings2 className="h-4 w-4" />
          <span>{t('btn_parametros')}</span>
        </button>
      </div>
    </header>
  );
};
