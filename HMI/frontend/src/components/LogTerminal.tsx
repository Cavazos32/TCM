import React, { useState, useRef, useEffect } from 'react';
import { Trash2, Copy, Check, ArrowDownCircle, Terminal, Search } from 'lucide-react';
import { LogEntry } from '../types';
import { useApp } from '../context/AppContext';

interface LogTerminalProps {
  title: string;
  logs: LogEntry[];
  onClear: () => void;
  filterModule?: 'MAQUINA' | 'MOTION' | 'PLC' | 'PREFEEDER' | 'ANDON' | 'ALL';
}

export const LogTerminal: React.FC<LogTerminalProps> = React.memo(({
  title,
  logs,
  onClear,
  filterModule = 'ALL',
}) => {
  const { t } = useApp();
  const [filterType, setFilterType] = useState<string>('ALL');
  const [searchQuery, setSearchQuery] = useState<string>('');
  const [copied, setCopied] = useState(false);
  const [autoScroll, setAutoScroll] = useState(true);
  const scrollRef = useRef<HTMLDivElement>(null);

  const filteredLogs = logs.filter((log) => {
    if (filterModule !== 'ALL' && log.module !== filterModule && log.module !== 'SYSTEM') {
      return false;
    }
    if (filterType === 'ERRORS' && log.type !== 'error' && log.type !== 'warn') {
      return false;
    }
    if (filterType === 'CMDS' && log.type !== 'cmd') {
      return false;
    }
    if (searchQuery.trim()) {
      const q = searchQuery.toLowerCase();
      return (
        log.message.toLowerCase().includes(q) ||
        (log.code && log.code.toLowerCase().includes(q)) ||
        log.module.toLowerCase().includes(q)
      );
    }
    return true;
  });

  useEffect(() => {
    if (autoScroll && scrollRef.current) {
      scrollRef.current.scrollTop = scrollRef.current.scrollHeight;
    }
  }, [logs, autoScroll]);

  const handleCopy = () => {
    const text = filteredLogs
      .map((l) => `[${l.timestamp}] [${l.module}] [${l.type.toUpperCase()}] ${l.code ? `(${l.code}) ` : ''}${l.message}`)
      .join('\n');
    navigator.clipboard.writeText(text);
    setCopied(true);
    setTimeout(() => setCopied(false), 2000);
  };

  return (
    <div className="mt-4 flex flex-col rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 shadow-2xs overflow-hidden transition-colors">
      {/* Header of Log section */}
      <div className="flex flex-wrap items-center justify-between gap-2 border-b border-slate-100 dark:border-slate-800 px-3.5 py-2 bg-slate-50/80 dark:bg-slate-800/80">
        <div className="flex items-center gap-2">
          <Terminal className="h-3.5 w-3.5 text-slate-600 dark:text-slate-400" />
          <span className="text-xs font-bold uppercase tracking-wider text-slate-800 dark:text-slate-200">
            {title}
          </span>
          <span className="rounded bg-slate-100 dark:bg-slate-800 border border-slate-200 dark:border-slate-700 px-1.5 py-0.2 text-[10px] font-mono font-medium text-slate-600 dark:text-slate-400">
            {filteredLogs.length} {t('events')}
          </span>
        </div>

        {/* Tools and Filters */}
        <div className="flex items-center gap-1.5">
          <div className="flex items-center rounded-md bg-white dark:bg-slate-800 border border-slate-300 dark:border-slate-700 px-2 py-0.5 text-xs shadow-2xs">
            <Search className="h-3 w-3 text-slate-400 mr-1" />
            <input
              type="text"
              placeholder={t('search_logs')}
              value={searchQuery}
              onChange={(e) => setSearchQuery(e.target.value)}
              className="w-16 sm:w-24 bg-transparent text-xs text-slate-800 dark:text-slate-200 placeholder-slate-400 focus:outline-none"
            />
          </div>

          <div className="flex rounded-md bg-slate-100 dark:bg-slate-800 p-0.5 border border-slate-200 dark:border-slate-700 text-xs">
            <button
              onClick={() => setFilterType('ALL')}
              className={`px-2 py-0.5 rounded transition ${
                filterType === 'ALL'
                  ? 'bg-white dark:bg-slate-700 text-slate-900 dark:text-white font-semibold shadow-2xs'
                  : 'text-slate-600 dark:text-slate-400 hover:text-slate-900 dark:hover:text-white'
              }`}
            >
              {t('all')}
            </button>
            <button
              onClick={() => setFilterType('CMDS')}
              className={`px-2 py-0.5 rounded transition ${
                filterType === 'CMDS'
                  ? 'bg-white dark:bg-slate-700 text-emerald-800 dark:text-emerald-400 font-semibold shadow-2xs'
                  : 'text-slate-600 dark:text-slate-400 hover:text-slate-900 dark:hover:text-white'
              }`}
            >
              {t('cmds')}
            </button>
            <button
              onClick={() => setFilterType('ERRORS')}
              className={`px-2 py-0.5 rounded transition ${
                filterType === 'ERRORS'
                  ? 'bg-white dark:bg-slate-700 text-red-700 dark:text-red-400 font-semibold shadow-2xs'
                  : 'text-slate-600 dark:text-slate-400 hover:text-slate-900 dark:hover:text-white'
              }`}
            >
              {t('errors')}
            </button>
          </div>

          <button
            onClick={() => setAutoScroll(!autoScroll)}
            title={autoScroll ? 'Auto-scroll activo' : 'Auto-scroll pausado'}
            className={`p-1 rounded-md transition border shadow-2xs ${
              autoScroll
                ? 'bg-emerald-50 dark:bg-emerald-950/40 border-emerald-200 dark:border-emerald-800 text-emerald-700 dark:text-emerald-300'
                : 'bg-white dark:bg-slate-800 border-slate-200 dark:border-slate-700 text-slate-400 hover:text-slate-600 dark:hover:text-slate-200'
            }`}
          >
            <ArrowDownCircle className="h-3.5 w-3.5" />
          </button>

          <button
            onClick={handleCopy}
            title="Copiar log al portapapeles"
            className="p-1 rounded-md bg-white dark:bg-slate-800 border border-slate-200 dark:border-slate-700 text-slate-600 dark:text-slate-300 hover:text-slate-900 dark:hover:text-white hover:bg-slate-50 dark:hover:bg-slate-700 transition shadow-2xs"
          >
            {copied ? <Check className="h-3.5 w-3.5 text-emerald-600 dark:text-emerald-400" /> : <Copy className="h-3.5 w-3.5" />}
          </button>

          <button
            id="btn-limpiar-log"
            onClick={onClear}
            className="flex items-center gap-1 rounded-md bg-white dark:bg-slate-800 hover:bg-slate-50 dark:hover:bg-slate-700 px-2 py-1 text-xs font-semibold text-slate-700 dark:text-slate-200 transition border border-slate-300 dark:border-slate-700 shadow-2xs active:scale-95"
          >
            <Trash2 className="h-3 w-3 text-slate-400" />
            <span>{t('clear')}</span>
          </button>
        </div>
      </div>

      {/* Terminal Display Content */}
      <div
        ref={scrollRef}
        className="h-36 sm:h-44 overflow-y-auto p-3 font-mono text-xs leading-relaxed bg-slate-950 text-slate-200 select-text"
      >
        {filteredLogs.length === 0 ? (
          <div className="flex h-full items-center justify-center text-slate-500 italic">
            {t('no_logs')}
          </div>
        ) : (
          filteredLogs.map((log) => {
            const isError = log.type === 'error';
            const isWarn = log.type === 'warn';
            const isCmd = log.type === 'cmd';
            const isRx = log.type === 'rx';

            return (
              <div
                key={log.id}
                className={`py-0.5 px-1.5 rounded flex items-start gap-2 hover:bg-slate-800/60 transition-colors ${
                  isError
                    ? 'text-red-300 bg-red-950/40'
                    : isWarn
                    ? 'text-amber-300 bg-amber-950/40'
                    : isCmd
                    ? 'text-emerald-300'
                    : isRx
                    ? 'text-sky-300'
                    : 'text-slate-300'
                }`}
              >
                <span className="text-slate-400 select-none text-[11px] shrink-0 font-mono">
                  {log.timestamp}
                </span>

                <span
                  className={`text-[10px] px-1.5 py-0.2 rounded font-bold shrink-0 uppercase tracking-wider ${
                    isError
                      ? 'bg-red-900/80 text-red-200 border border-red-700/60'
                      : isWarn
                      ? 'bg-amber-900/80 text-amber-200 border border-amber-700/60'
                      : isCmd
                      ? 'bg-emerald-950 text-emerald-300 border border-emerald-800/60'
                      : isRx
                      ? 'bg-sky-950 text-sky-300 border border-sky-800/60'
                      : 'bg-slate-800 text-slate-300'
                  }`}
                >
                  {log.module}
                </span>

                {log.code && (
                  <span className="rounded bg-slate-800 border border-slate-700 px-1.5 py-0.2 text-[11px] font-mono text-cyan-300 shrink-0 font-semibold">
                    {log.code}
                  </span>
                )}

                <span className="flex-1 break-words">{log.message}</span>
              </div>
            );
          })
        )}
      </div>
    </div>
  );
});

LogTerminal.displayName = 'LogTerminal';

