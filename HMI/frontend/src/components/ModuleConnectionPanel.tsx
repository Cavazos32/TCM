import React from 'react';
import { Network, RefreshCw } from 'lucide-react';
import { ConnectionState } from '../types';
import { useApp } from '../context/AppContext';

interface ModuleConnectionPanelProps {
  label: string;
  conn: ConnectionState;
  onReconnectAll?: () => void;
  reconnecting?: boolean;
  showReconnect?: boolean;
}

export const ModuleConnectionPanel: React.FC<ModuleConnectionPanelProps> = ({
  label,
  conn,
  onReconnectAll,
  reconnecting = false,
  showReconnect = false,
}) => {
  const { t } = useApp();

  return (
    <div className="rounded-xl border border-slate-200 dark:border-slate-800 bg-slate-50 dark:bg-slate-800/60 p-3.5 space-y-2.5">
      <div className="flex items-center gap-2">
        <Network className="h-4 w-4 text-sky-600 dark:text-sky-400" />
        <h3 className="text-xs font-bold uppercase tracking-wider text-slate-700 dark:text-slate-300">
          {t('network_nodes_title')}
        </h3>
      </div>
      <div className="flex items-center justify-between rounded-lg border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 p-2.5 font-mono text-xs">
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
      {showReconnect && onReconnectAll && (
        <button
          type="button"
          onClick={onReconnectAll}
          disabled={reconnecting}
          title={t('reconnect_all_title')}
          className="flex w-full items-center justify-center gap-2 rounded-lg border border-sky-300 dark:border-sky-700 bg-sky-50 dark:bg-sky-950/40 px-3 py-2 text-xs font-bold text-sky-800 dark:text-sky-200 transition hover:bg-sky-100 dark:hover:bg-sky-900/50 disabled:opacity-50"
        >
          <RefreshCw className={`h-3.5 w-3.5 ${reconnecting ? 'animate-spin' : ''}`} />
          {reconnecting ? t('reconnecting') : t('reconnect_all')}
        </button>
      )}
    </div>
  );
};
