import React from 'react';
import {
  Menu,
  Cpu,
  CheckCircle2,
} from 'lucide-react';
import { MachineState } from '../types';
import { useApp } from '../context/AppContext';

interface HeaderProps {
  machineState: MachineState;
  onOpenSettings: () => void;
}

export const Header: React.FC<HeaderProps> = ({
  machineState,
  onOpenSettings,
}) => {
  const { t } = useApp();

  return (
    <header className="border-b border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 text-slate-900 dark:text-slate-100 shadow-2xs transition-colors">
      {/* Top Main Navigation Bar */}
      <div className="mx-auto flex max-w-7xl items-center justify-between px-4 py-2.5 sm:px-6">
        {/* Brand & 3-lines Settings Menu Button */}
        <div className="flex items-center gap-2.5">
          {/* 3 Horizontal Lines (Hamburger Menu) for Settings as requested */}
          <button
            id="btn-app-settings-menu"
            onClick={onOpenSettings}
            title={t('open_settings')}
            className="flex h-9 w-9 items-center justify-center rounded-lg border border-slate-300 dark:border-slate-700 bg-slate-50 dark:bg-slate-800 text-slate-700 dark:text-slate-200 hover:bg-slate-100 dark:hover:bg-slate-700/80 hover:text-slate-900 dark:hover:text-white transition active:scale-95 shadow-2xs cursor-pointer"
            aria-label={t('settings')}
          >
            <Menu className="h-5 w-5" />
          </button>

          <div className="flex h-9 w-9 items-center justify-center rounded-lg bg-slate-100 dark:bg-slate-800 border border-slate-200 dark:border-slate-700 text-slate-800 dark:text-slate-200 shadow-2xs">
            <Cpu className="h-4 w-4" />
          </div>

          <div>
            <div className="flex items-center gap-2">
              <h1 className="text-lg font-bold tracking-tight text-slate-900 dark:text-white flex items-center gap-1">
                <span>TCM</span>
                <span className="text-slate-500 dark:text-slate-400 font-normal">HMI</span>
              </h1>
              <span className="rounded bg-slate-100 dark:bg-slate-800 border border-slate-200 dark:border-slate-700 px-1.5 py-0.2 font-mono text-[10px] font-medium text-slate-600 dark:text-slate-400">
                :5050
              </span>
            </div>
            <p className="text-[11px] text-slate-500 dark:text-slate-400 font-medium hidden sm:block">
              {t('hmi_subtitle')}
            </p>
          </div>
        </div>
      </div>

      {/* Global Status Banner (e.g. "Listo." with visual feedback) */}
      <div className="border-t border-slate-100 dark:border-slate-800 bg-slate-50/80 dark:bg-slate-900/80 px-4 py-1.5 sm:px-6">
        <div className="mx-auto flex max-w-7xl items-center justify-between">
          <div className="flex items-center gap-2.5">
            <div
              className={`flex items-center gap-1.5 rounded-md px-2 py-0.5 text-xs font-semibold uppercase tracking-wider ${
                machineState.isRunning
                  ? 'bg-emerald-100 dark:bg-emerald-950/60 text-emerald-800 dark:text-emerald-300 border border-emerald-200 dark:border-emerald-800'
                  : machineState.isPaused
                  ? 'bg-amber-100 dark:bg-amber-950/60 text-amber-800 dark:text-amber-300 border border-amber-200 dark:border-amber-800'
                  : 'bg-slate-200/80 dark:bg-slate-800 text-slate-700 dark:text-slate-300 border border-slate-300/80 dark:border-slate-700'
              }`}
            >
              {machineState.isRunning ? (
                <span className="relative flex h-2 w-2">
                  <span className="animate-ping absolute inline-flex h-full w-full rounded-full bg-emerald-400 opacity-75"></span>
                  <span className="relative inline-flex rounded-full h-2 w-2 bg-emerald-600"></span>
                </span>
              ) : (
                <CheckCircle2 className="h-3 w-3 text-slate-600 dark:text-slate-400" />
              )}
              <span>
                {machineState.isRunning
                  ? t('state_producing')
                  : machineState.isPaused
                  ? t('state_paused')
                  : t('state_system')}
              </span>
            </div>

            {/* Status text */}
            <span
              className={`text-xs font-medium tracking-wide ${
                machineState.fault
                  ? 'text-red-700 dark:text-red-300'
                  : 'text-slate-800 dark:text-slate-200'
              }`}
            >
              {machineState.fault || machineState.statusText || t('state_ready')}
            </span>
            {machineState.faultClass ? (
              <span className="rounded border border-red-300 dark:border-red-800 bg-red-50 dark:bg-red-950/50 px-1.5 py-0.5 font-mono text-[10px] font-semibold text-red-700 dark:text-red-300">
                {machineState.faultClass}
              </span>
            ) : null}
          </div>
        </div>
      </div>
    </header>
  );
};

