import React, { useCallback, useState } from 'react';
import { Network, RefreshCw } from 'lucide-react';
import { ConnectionState, HmiModuleKey } from '../types';
import { useApp } from '../context/AppContext';
import {
  hmiModuleBtnGroup,
  hmiModuleBtnSecondary,
  hmiModuleCenterSectionTitle,
  hmiModuleIconSection,
  hmiPanelCls,
  hmiPanelHeader,
  hmiPanelPadding,
  hmiStatusBar,
  hmiStatusChip,
  hmiStatusHeadline,
  hmiStatusLed,
  hmiStatusMeta,
  hmiStatusMetaLabel,
} from '../styles/hmiUi';

interface ModuleRow {
  key: HmiModuleKey;
  labelKey:
    | 'motion_node'
    | 'plc_node'
    | 'prefeeder_node'
    | 'andon_node';
  conn: ConnectionState;
}

interface ModulesConnectionTabProps {
  motionConn: ConnectionState;
  plcConn: ConnectionState;
  preFeederConn: ConnectionState;
  andonConn: ConnectionState;
  onReconnectAll?: () => void | Promise<void>;
  onReconnectModule?: (module: HmiModuleKey) => void | Promise<void>;
  reconnectingAll?: boolean;
}

export const ModulesConnectionTab: React.FC<ModulesConnectionTabProps> = ({
  motionConn,
  plcConn,
  preFeederConn,
  andonConn,
  onReconnectAll,
  onReconnectModule,
  reconnectingAll = false,
}) => {
  const { t } = useApp();
  const [reconnectingModule, setReconnectingModule] = useState<HmiModuleKey | null>(null);

  const modules: ModuleRow[] = [
    { key: 'motion', labelKey: 'motion_node', conn: motionConn },
    { key: 'plc', labelKey: 'plc_node', conn: plcConn },
    { key: 'prefeeder', labelKey: 'prefeeder_node', conn: preFeederConn },
    { key: 'andon', labelKey: 'andon_node', conn: andonConn },
  ];

  const handleReconnectModule = useCallback(
    async (module: HmiModuleKey) => {
      if (!onReconnectModule || reconnectingAll || reconnectingModule) return;
      setReconnectingModule(module);
      try {
        await onReconnectModule(module);
      } finally {
        setReconnectingModule(null);
      }
    },
    [onReconnectModule, reconnectingAll, reconnectingModule]
  );

  const connectedCount = modules.filter((m) => m.conn.connected).length;

  return (
    <div className="space-y-4">
      <div className={`${hmiPanelCls} ${hmiPanelPadding}`}>
        <div className={hmiPanelHeader}>
          <div className="flex items-start gap-2.5">
            <Network className={`${hmiModuleIconSection} mt-0.5 text-sky-600 dark:text-sky-400`} />
            <div>
              <h2 className={hmiModuleCenterSectionTitle}>{t('tab_conexion')}</h2>
              <p className="mt-1 text-base text-slate-600 dark:text-slate-400 sm:text-lg">
                {t('network_nodes_desc')}
              </p>
            </div>
          </div>

          {onReconnectAll && (
            <button
              id="btn-reconnect-all-modules"
              type="button"
              onClick={onReconnectAll}
              disabled={reconnectingAll || reconnectingModule !== null}
              title={t('reconnect_all_title')}
              className={`${hmiModuleBtnSecondary} border-sky-300 bg-sky-50 text-sky-800 hover:bg-sky-100 dark:border-sky-700 dark:bg-sky-950/40 dark:text-sky-200 dark:hover:bg-sky-900/50`}
            >
              <RefreshCw className={`${hmiModuleIconSection} !h-6 !w-6 ${reconnectingAll ? 'animate-spin' : ''}`} />
              {reconnectingAll ? t('reconnecting') : t('reconnect_all')}
            </button>
          )}
        </div>

        <div className={`${hmiStatusBar} mb-4 !min-h-0`}>
          <div className={hmiStatusMeta}>
            <span className={hmiStatusMetaLabel}>{t('network_nodes_title')}:</span>
            <span className={`${hmiStatusHeadline} text-slate-900 dark:text-white`}>
              {connectedCount} / {modules.length}
            </span>
            <span>{t('node_connected').toLowerCase()}</span>
          </div>
        </div>

        <div className="space-y-3">
          {modules.map((module) => {
            const busy =
              reconnectingAll ||
              reconnectingModule === module.key ||
              (reconnectingModule !== null && reconnectingModule !== module.key);

            return (
              <div
                key={module.key}
                className="flex flex-wrap items-center justify-between gap-4 rounded-lg border border-slate-200 bg-slate-50 p-5 dark:border-slate-800 dark:bg-slate-800/60 sm:p-6"
              >
                <div className="flex min-w-0 items-center gap-4">
                  <span
                    className={`${hmiStatusLed} ${
                      module.conn.connected ? 'bg-emerald-500' : 'bg-red-500'
                    }`}
                  />
                  <div className="min-w-0">
                    <div className="text-lg font-bold normal-case tracking-normal text-slate-800 dark:text-slate-200 sm:text-xl">
                      {t(module.labelKey)}
                    </div>
                    <div className={hmiStatusMeta}>
                      {module.conn.ip}:{module.conn.port}
                    </div>
                  </div>
                </div>

                <div className={`${hmiModuleBtnGroup} !gap-3`}>
                  <span
                    className={`${hmiStatusChip} ${
                      module.conn.connected
                        ? 'bg-emerald-50 dark:bg-emerald-950/40 text-emerald-800 dark:text-emerald-300 border-emerald-200 dark:border-emerald-800 uppercase'
                        : 'bg-red-50 dark:bg-red-950/40 text-red-800 dark:text-red-300 border-red-200 dark:border-red-900/60 uppercase'
                    }`}
                  >
                    {module.conn.connected ? t('node_connected') : t('node_disconnected')}
                  </span>

                  {onReconnectModule && (
                    <button
                      id={`btn-reconnect-${module.key}`}
                      type="button"
                      onClick={() => handleReconnectModule(module.key)}
                      disabled={busy}
                      title={t('reconnect_module_title')}
                      className={hmiModuleBtnSecondary}
                    >
                      <RefreshCw
                        className={`${hmiModuleIconSection} !h-6 !w-6 ${
                          reconnectingModule === module.key ? 'animate-spin' : ''
                        }`}
                      />
                      {reconnectingModule === module.key
                        ? t('reconnecting')
                        : t('reconnect_module')}
                    </button>
                  )}
                </div>
              </div>
            );
          })}
        </div>
      </div>
    </div>
  );
};
