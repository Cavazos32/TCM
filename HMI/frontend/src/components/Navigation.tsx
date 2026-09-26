import React from 'react';
import { LayoutDashboard, Repeat, Move, CircuitBoard, Layers, Lightbulb } from 'lucide-react';
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
}

export const Navigation: React.FC<NavigationProps> = ({
  currentTab,
  onSelectTab,
  motionConn,
  plcConn,
  preFeederConn,
  andonConn,
  hasErrors,
}) => {
  const { t, debugMode } = useApp();

  const allTabs: {
    id: TabType;
    label: string;
    icon: React.ReactNode;
    isConnected?: boolean;
    hasError?: boolean;
    debugOnly?: boolean;
  }[] = [
    {
      id: 'maquina',
      label: t('tab_maquina'),
      icon: <LayoutDashboard className="h-4 w-4" />,
    },
    {
      id: 'cycle',
      label: t('tab_cycle'),
      icon: <Repeat className="h-4 w-4" />,
      debugOnly: true,
    },
    {
      id: 'motion',
      label: t('tab_motion'),
      icon: <Move className="h-4 w-4" />,
      isConnected: motionConn.connected,
      hasError: hasErrors?.motion,
    },
    {
      id: 'plc',
      label: t('tab_plc'),
      icon: <CircuitBoard className="h-4 w-4" />,
      isConnected: plcConn.connected,
      hasError: hasErrors?.plc,
    },
    {
      id: 'prefeeder',
      label: t('tab_prefeeder'),
      icon: <Layers className="h-4 w-4" />,
      isConnected: preFeederConn.connected,
      hasError: hasErrors?.prefeeder,
    },
    {
      id: 'andon',
      label: t('tab_andon'),
      icon: <Lightbulb className="h-4 w-4" />,
      isConnected: andonConn.connected,
      debugOnly: true,
    },
  ];

  const tabs = allTabs.filter((tab) => debugMode || !tab.debugOnly);

  return (
    <nav className="flex items-center gap-2 border-b border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 px-4 py-2 sm:px-6 shadow-2xs transition-colors">
      <div className="flex flex-wrap items-center gap-2 max-w-7xl mx-auto w-full">
        {tabs.map((tab) => {
          const isActive = currentTab === tab.id;
          const interactive = debugMode || tab.id === 'maquina';
          const problem = !!tab.hasError;
          const ledTitle = tab.hasError
            ? t('state_error')
            : tab.isConnected
              ? t('node_connected')
              : t('node_disconnected');
          const ledClass = `h-2 w-2 rounded-full ${
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

          if (!interactive) {
            return (
              <div
                key={tab.id}
                id={`nav-status-${tab.id}`}
                role="status"
                title={ledTitle}
                className={`flex items-center gap-2 rounded-lg px-3.5 py-2 text-xs sm:text-sm font-medium ${
                  problem
                    ? 'bg-red-50 dark:bg-red-950/40 text-red-800 dark:text-red-200 border border-red-300 dark:border-red-800'
                    : 'bg-white dark:bg-slate-800/80 text-slate-700 dark:text-slate-300 border border-slate-200/80 dark:border-slate-700'
                }`}
              >
                <span className={problem ? 'text-red-600 dark:text-red-400' : 'text-slate-500 dark:text-slate-400'}>
                  {tab.icon}
                </span>
                <span className="font-semibold">{tab.label}</span>
                {led}
              </div>
            );
          }

          return (
            <button
              key={tab.id}
              id={`nav-tab-${tab.id}`}
              onClick={() => onSelectTab(tab.id)}
              className={`group relative flex items-center gap-2 rounded-lg px-3.5 py-2 text-xs sm:text-sm font-medium transition-all duration-150 cursor-pointer ${
                isActive
                  ? 'bg-slate-900 dark:bg-slate-100 text-white dark:text-slate-900 shadow-2xs border border-slate-900 dark:border-white font-bold'
                  : problem
                    ? 'bg-red-50 dark:bg-red-950/40 text-red-800 dark:text-red-200 hover:bg-red-100 dark:hover:bg-red-950/60 border border-red-300 dark:border-red-800'
                    : 'bg-white dark:bg-slate-800/80 text-slate-700 dark:text-slate-300 hover:bg-slate-100 dark:hover:bg-slate-700 hover:text-slate-900 dark:hover:text-white border border-slate-200/80 dark:border-slate-700'
              }`}
            >
              <span className={isActive ? 'text-white dark:text-slate-900' : problem ? 'text-red-600 dark:text-red-400' : 'text-slate-500 dark:text-slate-400 group-hover:text-slate-900 dark:group-hover:text-white'}>
                {tab.icon}
              </span>
              <span className="font-semibold">{tab.label}</span>
              {led}
            </button>
          );
        })}
      </div>
    </nav>
  );
};
