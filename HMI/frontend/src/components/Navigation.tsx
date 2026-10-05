import React from 'react';
import { Repeat, Move, CircuitBoard, Layers, Lightbulb, ArrowLeft, LogOut } from 'lucide-react';
import { TabType, ConnectionState } from '../types';
import { useApp } from '../context/AppContext';

interface NavigationProps {
  currentTab: TabType;
  onSelectTab: (tab: TabType) => void;
  motionConn: ConnectionState;
  plcConn: ConnectionState;
  preFeederConn: ConnectionState;
  andonConn: ConnectionState;
  hasErrors?: {
    motion?: boolean;
    plc?: boolean;
    prefeeder?: boolean;
  };
  onBackToMaquina?: () => void;
  onExitParametros?: () => void;
}

export const Navigation: React.FC<NavigationProps> = ({
  currentTab,
  onSelectTab,
  motionConn,
  plcConn,
  preFeederConn,
  andonConn,
  hasErrors,
  onBackToMaquina,
  onExitParametros,
}) => {
  const { t } = useApp();

  const tabs: {
    id: TabType;
    label: string;
    icon: React.ReactNode;
    isConnected?: boolean;
    hasError?: boolean;
  }[] = [
    {
      id: 'cycle',
      label: t('tab_cycle'),
      icon: <Repeat className="h-5 w-5" />,
    },
    {
      id: 'motion',
      label: t('tab_motion'),
      icon: <Move className="h-5 w-5" />,
      isConnected: motionConn.connected,
      hasError: hasErrors?.motion,
    },
    {
      id: 'plc',
      label: t('tab_plc'),
      icon: <CircuitBoard className="h-5 w-5" />,
      isConnected: plcConn.connected,
      hasError: hasErrors?.plc,
    },
    {
      id: 'prefeeder',
      label: t('tab_prefeeder'),
      icon: <Layers className="h-5 w-5" />,
      isConnected: preFeederConn.connected,
      hasError: hasErrors?.prefeeder,
    },
    {
      id: 'andon',
      label: t('tab_andon'),
      icon: <Lightbulb className="h-5 w-5" />,
      isConnected: andonConn.connected,
    },
  ];

  return (
    <nav className="flex flex-wrap items-center gap-2 border-b border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 px-3 py-2.5 sm:px-5 shadow-2xs transition-colors">
      {onBackToMaquina && (
        <button
          id="btn-back-maquina"
          type="button"
          onClick={onBackToMaquina}
          className="flex items-center gap-2 rounded-lg border border-slate-300 dark:border-slate-700 bg-slate-50 dark:bg-slate-800 px-3 py-2.5 text-sm font-bold text-slate-700 dark:text-slate-200 hover:bg-slate-100 dark:hover:bg-slate-700 transition"
        >
          <ArrowLeft className="h-4 w-4" />
          <span className="hidden sm:inline">{t('btn_back_maquina')}</span>
        </button>
      )}

      <div className="flex flex-wrap items-center gap-2.5 flex-1">
        {tabs.map((tab) => {
          const isActive = currentTab === tab.id;
          const problem = !!tab.hasError;
          const ledTitle = tab.hasError
            ? t('state_error')
            : tab.isConnected
              ? t('node_connected')
              : t('node_disconnected');
          const ledClass = `h-2.5 w-2.5 rounded-full ${
            tab.hasError
              ? 'bg-red-500 animate-ping'
              : tab.isConnected
                ? 'bg-emerald-500'
                : 'bg-red-400'
          }`;
          const led =
            tab.isConnected !== undefined ? (
              <span title={ledTitle} className={ledClass} />
            ) : null;

          return (
            <button
              key={tab.id}
              id={`nav-tab-${tab.id}`}
              onClick={() => onSelectTab(tab.id)}
              className={`group relative flex items-center gap-2 rounded-lg px-4 py-2.5 text-sm font-medium transition-all duration-150 cursor-pointer ${
                isActive
                  ? 'bg-sky-800 dark:bg-sky-600 text-white shadow-2xs border border-sky-800 dark:border-sky-500 font-bold'
                  : problem
                    ? 'bg-red-50 dark:bg-red-950/40 text-red-800 dark:text-red-200 hover:bg-red-100 dark:hover:bg-red-950/60 border border-red-300 dark:border-red-800'
                    : 'bg-white dark:bg-slate-800/80 text-slate-700 dark:text-slate-300 hover:bg-slate-100 dark:hover:bg-slate-700 hover:text-slate-900 dark:hover:text-white border border-slate-200/80 dark:border-slate-700'
              }`}
            >
              <span
                className={
                  isActive
                    ? 'text-white'
                    : problem
                      ? 'text-red-600 dark:text-red-400'
                      : 'text-slate-500 dark:text-slate-400 group-hover:text-slate-900 dark:group-hover:text-white'
                }
              >
                {tab.icon}
              </span>
              <span className="font-semibold">{tab.label}</span>
              {led}
            </button>
          );
        })}
      </div>

      {onExitParametros && (
        <button
          id="btn-exit-parametros"
          type="button"
          onClick={onExitParametros}
          className="flex items-center gap-2 rounded-lg border border-orange-300 dark:border-orange-800 bg-orange-50 dark:bg-orange-950/40 px-3 py-2.5 text-sm font-bold text-orange-800 dark:text-orange-200 hover:bg-orange-100 dark:hover:bg-orange-950/60 transition"
        >
          <LogOut className="h-4 w-4" />
          <span className="hidden sm:inline">{t('debug_mode_exit')}</span>
        </button>
      )}
    </nav>
  );
};
