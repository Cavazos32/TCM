import React, { useCallback, useMemo, useState } from 'react';
import {
  FlaskConical,
  Play,
  Square,
  Download,
  Trash2,
  Loader2,
} from 'lucide-react';
import { useApp } from '../context/AppContext';
import type { DebugTrailsState, DebugTrailsSide } from '../types';

interface DebugTrailsTabProps {
  trails: DebugTrailsState;
  onStart: (side: DebugTrailsSide, numTests: number, waitTimeS: number) => Promise<void>;
  onStop: () => Promise<void>;
  onClear: () => Promise<void>;
  onExport: () => void;
}

function seedSide(trails: DebugTrailsState): DebugTrailsSide {
  return trails.side === 'R' || trails.side === 'L' || trails.side === 'Both'
    ? trails.side
    : 'Both';
}

function seedNumTests(trails: DebugTrailsState): number {
  return trails.numTests >= 1 ? trails.numTests : 10;
}

function seedWaitTimeS(trails: DebugTrailsState): number {
  return trails.numTests >= 1 ? Math.max(0, trails.waitTimeS) : 1;
}

export const DebugTrailsTab: React.FC<DebugTrailsTabProps> = ({
  trails,
  onStart,
  onStop,
  onClear,
  onExport,
}) => {
  const { t } = useApp();
  // Preferencias locales: no re-sincronizar desde poll (solo semilla al montar / último start del backend).
  const [side, setSide] = useState<DebugTrailsSide>(() => seedSide(trails));
  const [numTestsText, setNumTestsText] = useState(() => String(seedNumTests(trails)));
  const [waitTimeText, setWaitTimeText] = useState(() => String(seedWaitTimeS(trails)));
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState('');

  const active = trails.active;

  const handleStart = useCallback(async () => {
    setErr('');
    const n = Math.max(1, Math.floor(Number(numTestsText)) || 1);
    const w = Math.max(0, Number(waitTimeText));
    const wait = Number.isFinite(w) ? w : 0;
    setNumTestsText(String(n));
    setWaitTimeText(String(wait));
    setBusy(true);
    try {
      await onStart(side, n, wait);
    } catch (e) {
      setErr(e instanceof Error ? e.message : String(e));
    } finally {
      setBusy(false);
    }
  }, [onStart, side, numTestsText, waitTimeText]);

  const handleStop = useCallback(async () => {
    setBusy(true);
    try {
      await onStop();
    } finally {
      setBusy(false);
    }
  }, [onStop]);

  const handleClear = useCallback(async () => {
    setBusy(true);
    try {
      await onClear();
    } finally {
      setBusy(false);
    }
  }, [onClear]);

  const statusLabel = useMemo(() => {
    if (active) {
      return `${t('trails_running')} · ${trails.phase} · ${trails.currentTest}/${trails.numTests}`;
    }
    if (trails.fault) return trails.fault;
    if (trails.lastOk) return t('trails_completed');
    return t('trails_idle');
  }, [active, trails, t]);

  return (
    <div className="space-y-4">
      <section className="rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 p-4 shadow-2xs">
        <div className="flex items-start gap-3 mb-4">
          <div className="rounded-lg bg-slate-100 dark:bg-slate-800 p-2">
            <FlaskConical className="h-5 w-5 text-slate-700 dark:text-slate-200" />
          </div>
          <div>
            <h2 className="text-sm font-bold tracking-wide text-slate-900 dark:text-slate-100">
              {t('trails_title')}
            </h2>
            <p className="text-xs text-slate-500 dark:text-slate-400 mt-0.5">
              {t('trails_subtitle')}
            </p>
          </div>
        </div>

        <div className="rounded-lg border border-amber-200/80 dark:border-amber-900/50 bg-amber-50/80 dark:bg-amber-950/30 px-3 py-2 text-xs text-amber-900 dark:text-amber-200 mb-4">
          {t('trails_stage1_hint')}
        </div>

        <div className="grid grid-cols-1 sm:grid-cols-3 gap-3 mb-4">
          <label className="block">
            <span className="text-[11px] font-semibold uppercase tracking-wide text-slate-500">
              {t('trails_side')}
            </span>
            <select
              id="trails-side"
              disabled={active || busy}
              value={side}
              onChange={(e) => setSide(e.target.value as DebugTrailsSide)}
              className="mt-1 w-full rounded-lg border border-slate-200 dark:border-slate-700 bg-white dark:bg-slate-800 px-3 py-2 text-sm"
            >
              <option value="R">R</option>
              <option value="L">L</option>
              <option value="Both">Both</option>
            </select>
          </label>

          <label className="block">
            <span className="text-[11px] font-semibold uppercase tracking-wide text-slate-500">
              {t('trails_num_tests')}
            </span>
            <input
              id="trails-num-tests"
              type="number"
              min={1}
              max={9999}
              disabled={active || busy}
              value={numTestsText}
              onChange={(e) => setNumTestsText(e.target.value)}
              onBlur={() => {
                const n = Math.max(1, Math.floor(Number(numTestsText)) || 1);
                setNumTestsText(String(n));
              }}
              className="mt-1 w-full rounded-lg border border-slate-200 dark:border-slate-700 bg-white dark:bg-slate-800 px-3 py-2 text-sm"
            />
          </label>

          <label className="block">
            <span className="text-[11px] font-semibold uppercase tracking-wide text-slate-500">
              {t('trails_wait_time')}
            </span>
            <input
              id="trails-wait-time"
              type="number"
              min={0}
              step={0.1}
              disabled={active || busy}
              value={waitTimeText}
              onChange={(e) => setWaitTimeText(e.target.value)}
              onBlur={() => {
                const w = Math.max(0, Number(waitTimeText));
                setWaitTimeText(String(Number.isFinite(w) ? w : 0));
              }}
              className="mt-1 w-full rounded-lg border border-slate-200 dark:border-slate-700 bg-white dark:bg-slate-800 px-3 py-2 text-sm"
            />
          </label>
        </div>

        <div className="flex flex-wrap items-center gap-2">
          {!active ? (
            <button
              id="btn-trails-start"
              type="button"
              disabled={busy}
              onClick={() => void handleStart()}
              className="inline-flex items-center gap-2 rounded-lg bg-emerald-600 hover:bg-emerald-500 text-white px-4 py-2 text-sm font-semibold disabled:opacity-50"
            >
              {busy ? <Loader2 className="h-4 w-4 animate-spin" /> : <Play className="h-4 w-4" />}
              {t('trails_start')}
            </button>
          ) : (
            <button
              id="btn-trails-stop"
              type="button"
              disabled={busy}
              onClick={() => void handleStop()}
              className="inline-flex items-center gap-2 rounded-lg bg-red-600 hover:bg-red-500 text-white px-4 py-2 text-sm font-semibold disabled:opacity-50"
            >
              {busy ? <Loader2 className="h-4 w-4 animate-spin" /> : <Square className="h-4 w-4" />}
              {t('trails_stop')}
            </button>
          )}

          <button
            id="btn-trails-export"
            type="button"
            disabled={trails.records.length === 0}
            onClick={onExport}
            className="inline-flex items-center gap-2 rounded-lg border border-slate-200 dark:border-slate-700 bg-white dark:bg-slate-800 px-3 py-2 text-sm font-medium disabled:opacity-40"
          >
            <Download className="h-4 w-4" />
            {t('trails_export')}
          </button>

          <button
            id="btn-trails-clear"
            type="button"
            disabled={active || trails.records.length === 0 || busy}
            onClick={() => void handleClear()}
            className="inline-flex items-center gap-2 rounded-lg border border-slate-200 dark:border-slate-700 bg-white dark:bg-slate-800 px-3 py-2 text-sm font-medium disabled:opacity-40"
          >
            <Trash2 className="h-4 w-4" />
            {t('trails_clear')}
          </button>

          <span
            className={`ml-auto text-xs font-medium ${
              active
                ? 'text-emerald-600 dark:text-emerald-400'
                : 'text-slate-500 dark:text-slate-400'
            }`}
          >
            {statusLabel}
          </span>
        </div>

        {err && (
          <p className="mt-3 text-xs text-red-600 dark:text-red-400">{err}</p>
        )}
      </section>

      <section className="rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 p-4 shadow-2xs overflow-hidden">
        <h3 className="text-xs font-bold uppercase tracking-wide text-slate-500 mb-3">
          {t('trails_results')} ({trails.records.length})
        </h3>
        <div className="overflow-x-auto max-h-[28rem]">
          <table className="w-full text-sm">
            <thead className="sticky top-0 bg-slate-50 dark:bg-slate-800 text-[11px] uppercase tracking-wide text-slate-500">
              <tr>
                <th className="text-left px-2 py-2 font-semibold">#</th>
                <th className="text-left px-2 py-2 font-semibold">{t('trails_col_side')}</th>
                <th className="text-left px-2 py-2 font-semibold">{t('trails_col_mm')}</th>
                <th className="text-left px-2 py-2 font-semibold">{t('trails_col_status')}</th>
                <th className="text-left px-2 py-2 font-semibold">{t('trails_col_error')}</th>
                <th className="text-left px-2 py-2 font-semibold">{t('trails_col_time')}</th>
              </tr>
            </thead>
            <tbody>
              {trails.records.length === 0 ? (
                <tr>
                  <td colSpan={6} className="px-2 py-6 text-center text-slate-400 text-xs">
                    {t('trails_no_records')}
                  </td>
                </tr>
              ) : (
                [...trails.records].reverse().map((r, idx) => (
                  <tr
                    key={`${r.testNum}-${r.side}-${r.timestamp}-${idx}`}
                    className="border-t border-slate-100 dark:border-slate-800"
                  >
                    <td className="px-2 py-1.5 font-mono text-xs">{r.testNum}</td>
                    <td className="px-2 py-1.5 font-semibold">{r.side}</td>
                    <td className="px-2 py-1.5 font-mono text-xs">
                      {r.measureMm == null ? '—' : `${r.measureMm.toFixed(2)}`}
                    </td>
                    <td className="px-2 py-1.5">
                      <span
                        className={`inline-block rounded px-1.5 py-0.5 text-[10px] font-bold uppercase ${
                          r.status === 'ok'
                            ? 'bg-emerald-100 text-emerald-800 dark:bg-emerald-900/40 dark:text-emerald-300'
                            : r.status === 'fail'
                              ? 'bg-amber-100 text-amber-900 dark:bg-amber-900/40 dark:text-amber-200'
                              : 'bg-red-100 text-red-800 dark:bg-red-900/40 dark:text-red-300'
                        }`}
                      >
                        {r.status}
                      </span>
                    </td>
                    <td className="px-2 py-1.5 text-xs text-slate-500 max-w-[14rem] truncate" title={r.error}>
                      {r.error || '—'}
                    </td>
                    <td className="px-2 py-1.5 text-[11px] text-slate-400 font-mono whitespace-nowrap">
                      {r.timestamp}
                    </td>
                  </tr>
                ))
              )}
            </tbody>
          </table>
        </div>
      </section>
    </div>
  );
};
