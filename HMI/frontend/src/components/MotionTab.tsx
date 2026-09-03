import React, { useState, useEffect } from 'react';
import {
  Move,
  Activity,
  RotateCcw,
  Square,
  Compass,
  Home,
} from 'lucide-react';
import { MotionState, LogEntry } from '../types';
import { LogTerminal } from './LogTerminal';
import { useApp } from '../context/AppContext';

interface MotionTabProps {
  motionState: MotionState;
  onUpdateTargetPos: (pos: number) => void;
  onUpdateRpm: (rpm: number) => void;
  onUpdateOffsetL?: (offset: number) => void;
  onUpdateOffsetR?: (offset: number) => void;
  onMover: (mm: number, rpm: number) => void;
  onStop: () => void;
  onSearchHome: () => void;
  onMoveToZero: () => void;
  onResetErrors: () => void;
  onSetZeroR: () => void;
  onSetZeroL: () => void;
  onTestCanL: () => void;
  onTestCanR: () => void;
  onSaveFeedOffset: (l: number, r: number) => void;
  onReloadFeedOffset: () => void;
  showLogs?: boolean;
  logs: LogEntry[];
  onClearLogs: () => void;
}

export const MotionTab: React.FC<MotionTabProps> = ({
  motionState,
  onUpdateTargetPos,
  onUpdateRpm,
  onUpdateOffsetL,
  onUpdateOffsetR,
  onMover,
  onStop,
  onSearchHome,
  onMoveToZero,
  onResetErrors,
  onSetZeroR,
  onSetZeroL,
  onTestCanL,
  onTestCanR,
  onSaveFeedOffset,
  onReloadFeedOffset,
  showLogs = true,
  logs,
  onClearLogs,
}) => {
  const { t } = useApp();
  const [inputPos, setInputPos] = useState<string>(motionState.targetPositionMm.toString());
  const [inputRpm, setInputRpm] = useState<string>(motionState.rpm.toString());
  const [inputOffsetL, setInputOffsetL] = useState<string>((motionState.offsetL ?? 0).toString());
  const [inputOffsetR, setInputOffsetR] = useState<string>((motionState.offsetR ?? 0).toString());

  useEffect(() => {
    setInputPos(motionState.targetPositionMm.toString());
    setInputRpm(motionState.rpm.toString());
    setInputOffsetL((motionState.offsetL ?? 0).toString());
    setInputOffsetR((motionState.offsetR ?? 0).toString());
  }, [
    motionState.targetPositionMm,
    motionState.rpm,
    motionState.offsetL,
    motionState.offsetR,
  ]);

  const handlePosChange = (val: string) => {
    setInputPos(val);
    const num = parseFloat(val);
    if (!isNaN(num)) {
      onUpdateTargetPos(num);
    }
  };

  const handleRpmChange = (val: string) => {
    setInputRpm(val);
    const num = parseInt(val, 10);
    if (!isNaN(num)) {
      onUpdateRpm(num);
    }
  };

  const handleOffsetLChange = (val: string) => {
    setInputOffsetL(val);
  };

  const handleOffsetRChange = (val: string) => {
    setInputOffsetR(val);
  };

  const handleSaveOffsets = () => {
    const l = parseFloat(inputOffsetL);
    const r = parseFloat(inputOffsetR);
    if (!isNaN(l)) onUpdateOffsetL?.(l);
    if (!isNaN(r)) onUpdateOffsetR?.(r);
    onSaveFeedOffset(isNaN(l) ? 0 : l, isNaN(r) ? 0 : r);
  };

  const applyNudge = (delta: number) => {
    const next = (parseFloat(inputPos) || 0) + delta;
    setInputPos(next.toString());
    onUpdateTargetPos(next);
  };

  const handleMove = () => {
    const mm = parseFloat(inputPos);
    const rpm = parseInt(inputRpm, 10);
    onMover(
      Number.isFinite(mm) ? mm : motionState.targetPositionMm,
      Number.isFinite(rpm) ? rpm : motionState.rpm
    );
  };

  return (
    <div className="space-y-4">
      {/* Unified Compact Status & Link Bar */}
      <div className="rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 px-4 py-2.5 text-slate-800 dark:text-slate-200 shadow-2xs flex flex-wrap items-center justify-between gap-3 transition-colors">
        <div className="flex items-center gap-3">
          <div className="flex items-center gap-2">
            <span
              className={`h-2.5 w-2.5 rounded-full ${
                !motionState.connection.connected
                  ? 'bg-red-500'
                  : motionState.isMoving
                    ? 'bg-amber-500 animate-ping'
                    : 'bg-emerald-500'
              }`}
            />
            <span className="text-sm font-semibold text-slate-900 dark:text-white tracking-tight">
              {motionState.statusText || (motionState.isMoving ? t('motor_moving') : t('state_ready'))}
            </span>
          </div>

          <span className="text-slate-300 dark:text-slate-700">|</span>

          <div className="flex items-center gap-1.5 font-mono text-xs text-slate-600 dark:text-slate-400">
            <span className="text-slate-400 dark:text-slate-500">{t('link_label')}:</span>
            <span className={`font-semibold ${motionState.connection.connected ? 'text-emerald-700 dark:text-emerald-400' : 'text-red-600 dark:text-red-400'}`}>
              {motionState.connection.connected ? t('node_connected') : t('node_disconnected')}
            </span>
            <span className="text-slate-400 dark:text-slate-500 text-[11px]">
              ({motionState.connection.ip}:{motionState.connection.port})
            </span>
          </div>
        </div>

        <div className="flex items-center gap-3">
          <div className="flex items-center gap-1.5 text-xs font-mono">
            <span className="text-slate-500 dark:text-slate-400">{t('current_position')}:</span>
            <span className="font-bold text-slate-900 dark:text-white bg-slate-100 dark:bg-slate-800 px-2 py-0.5 rounded-md border border-slate-200 dark:border-slate-700">
              {motionState.currentPositionMm.toFixed(2)} mm
            </span>
          </div>
        </div>
      </div>

      {/* Main Grid: ASDA B3 + (OM Encoder & Feeder CAN) */}
      <div className="grid grid-cols-1 gap-4 lg:grid-cols-12">
        {/* ASDA B3 Controller Panel (Left Card) */}
        <div className="lg:col-span-7 rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 p-4 sm:p-5 shadow-2xs flex flex-col justify-between transition-colors">
          <div>
            <div className="flex items-center justify-between border-b border-slate-100 dark:border-slate-800 pb-2.5">
              <div className="flex items-center gap-2">
                <Move className="h-4 w-4 text-slate-600 dark:text-slate-400" />
                <h2 className="text-xs font-bold uppercase tracking-wider text-slate-800 dark:text-slate-200">
                  ASDA B3
                </h2>
              </div>
              <span className="rounded bg-slate-100 dark:bg-slate-800 border border-slate-200 dark:border-slate-700 px-2 py-0.5 font-mono text-[10px] text-slate-600 dark:text-slate-400">
                Servo Drive
              </span>
            </div>

            {/* Inputs Section - Compact Grid */}
            <div className="mt-3.5 space-y-3">
              {/* Posición mm */}
              <div className="flex items-center justify-between gap-3">
                <label
                  htmlFor="input-pos-mm"
                  className="text-xs font-semibold text-slate-700 dark:text-slate-300 min-w-[90px]"
                >
                  {t('position_mm')}
                </label>
                <div className="flex items-center gap-1.5 flex-1 max-w-xs">
                  <input
                    id="input-pos-mm"
                    type="number"
                    step="0.1"
                    value={inputPos}
                    onChange={(e) => handlePosChange(e.target.value)}
                    className="w-full rounded-md border border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 px-2.5 py-1.5 font-mono text-xs font-semibold text-slate-900 dark:text-slate-100 shadow-2xs focus:border-slate-500 focus:outline-none focus:ring-1 focus:ring-slate-400"
                  />
                  <div className="flex items-center gap-1 shrink-0">
                    <button
                      onClick={() => applyNudge(-10)}
                      className="rounded-md bg-slate-100 dark:bg-slate-800 hover:bg-slate-200 dark:hover:bg-slate-700 px-2 py-1 text-xs font-mono font-medium text-slate-700 dark:text-slate-300 border border-slate-200 dark:border-slate-700 shadow-2xs"
                      title="-10 mm"
                    >
                      -10
                    </button>
                    <button
                      onClick={() => applyNudge(10)}
                      className="rounded-md bg-slate-100 dark:bg-slate-800 hover:bg-slate-200 dark:hover:bg-slate-700 px-2 py-1 text-xs font-mono font-medium text-slate-700 dark:text-slate-300 border border-slate-200 dark:border-slate-700 shadow-2xs"
                      title="+10 mm"
                    >
                      +10
                    </button>
                  </div>
                </div>
              </div>

              {/* RPM */}
              <div className="flex items-center justify-between gap-3">
                <label
                  htmlFor="input-rpm"
                  className="text-xs font-semibold text-slate-700 dark:text-slate-300 min-w-[90px]"
                >
                  RPM
                </label>
                <div className="flex items-center gap-1.5 flex-1 max-w-xs">
                  <input
                    id="input-rpm"
                    type="number"
                    step="50"
                    min="0"
                    max="5000"
                    value={inputRpm}
                    onChange={(e) => handleRpmChange(e.target.value)}
                    className="w-full rounded-md border border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 px-2.5 py-1.5 font-mono text-xs font-semibold text-slate-900 dark:text-slate-100 shadow-2xs focus:border-slate-500 focus:outline-none focus:ring-1 focus:ring-slate-400"
                  />
                  <div className="flex items-center gap-1 shrink-0">
                    {[600, 1200, 2400].map((preset) => (
                      <button
                        key={preset}
                        onClick={() => {
                          setInputRpm(preset.toString());
                          onUpdateRpm(preset);
                        }}
                        className={`rounded-md px-2 py-1 text-xs font-mono transition border shadow-2xs ${
                          motionState.rpm === preset
                            ? 'bg-slate-900 dark:bg-slate-100 text-white dark:text-slate-900 border-slate-900 dark:border-white font-semibold'
                            : 'bg-slate-100 dark:bg-slate-800 hover:bg-slate-200 dark:hover:bg-slate-700 text-slate-700 dark:text-slate-300 border-slate-200 dark:border-slate-700 font-medium'
                        }`}
                      >
                        {preset}
                      </button>
                    ))}
                  </div>
                </div>
              </div>
            </div>
          </div>

          {/* Action Buttons - Compact & Shortened */}
          <div className="mt-4 pt-3 border-t border-slate-100 dark:border-slate-800">
            <div className="flex flex-wrap items-center gap-2">
              {/* Mover (0x005) */}
              <button
                id="btn-mover-motion"
                onClick={handleMove}
                disabled={motionState.isMoving}
                className="group flex items-center justify-center gap-1.5 rounded-lg bg-slate-900 dark:bg-slate-100 hover:bg-slate-800 dark:hover:bg-white px-3.5 py-2 text-xs font-bold text-white dark:text-slate-900 transition active:scale-95 shadow-2xs disabled:opacity-50"
              >
                <span>{t('btn_move')}</span>
                <span className="rounded bg-slate-800 dark:bg-slate-200 px-1 py-0.2 font-mono text-[10px] text-slate-300 dark:text-slate-800 border border-slate-700 dark:border-slate-300">
                  0x005
                </span>
              </button>

              {/* Stop (0x002) */}
              <button
                id="btn-stop-motion"
                onClick={onStop}
                className="group flex items-center justify-center gap-1.5 rounded-lg border border-red-200 dark:border-red-900/60 bg-red-50 dark:bg-red-950/40 hover:bg-red-100 dark:hover:bg-red-950/70 px-3.5 py-2 text-xs font-bold text-red-700 dark:text-red-300 transition active:scale-95 shadow-2xs"
              >
                <Square className="h-3 w-3 fill-current" />
                <span>{t('btn_stop')}</span>
                <span className="rounded bg-red-100 dark:bg-red-900/60 px-1 py-0.2 font-mono text-[10px] text-red-800 dark:text-red-200 border border-red-200 dark:border-red-800">
                  0x002
                </span>
              </button>

              {/* Search HOME (0x001) — torque homing HomeASDA() */}
              <button
                id="btn-search-home-motion"
                onClick={onSearchHome}
                disabled={motionState.isMoving}
                className="group flex items-center justify-center gap-1.5 rounded-lg border border-emerald-200 dark:border-emerald-900/60 bg-emerald-50 dark:bg-emerald-950/40 hover:bg-emerald-100 dark:hover:bg-emerald-950/70 px-3.5 py-2 text-xs font-bold text-emerald-800 dark:text-emerald-300 transition active:scale-95 shadow-2xs disabled:opacity-50"
              >
                <Home className="h-3 w-3" />
                <span>{t('btn_search_home')}</span>
                <span className="rounded bg-emerald-100 dark:bg-emerald-900/60 px-1 py-0.2 font-mono text-[10px] text-emerald-800 dark:text-emerald-200 border border-emerald-200 dark:border-emerald-800">
                  0x001
                </span>
              </button>

              {/* Move to 0 (0x007) */}
              <button
                id="btn-move0-motion"
                onClick={onMoveToZero}
                className="group flex items-center justify-center gap-1.5 rounded-lg border border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 hover:bg-slate-50 dark:hover:bg-slate-700 px-3.5 py-2 text-xs font-bold text-slate-700 dark:text-slate-200 transition active:scale-95 shadow-2xs"
              >
                <span>{t('btn_move_to_zero')}</span>
                <span className="rounded bg-slate-100 dark:bg-slate-700 px-1 py-0.2 font-mono text-[10px] text-slate-600 dark:text-slate-300 border border-slate-200 dark:border-slate-600">
                  0x007
                </span>
              </button>

              {/* Reset errores */}
              <button
                id="btn-reset-errores-motion"
                onClick={onResetErrors}
                className="flex items-center justify-center gap-1.5 rounded-lg border border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 hover:bg-slate-50 dark:hover:bg-slate-700 px-3.5 py-2 text-xs font-semibold text-slate-700 dark:text-slate-200 transition active:scale-95 shadow-2xs"
              >
                <RotateCcw className="h-3 w-3 text-amber-600 dark:text-amber-400" />
                <span>{t('btn_reset_errors')}</span>
                <span className="rounded bg-slate-100 dark:bg-slate-700 px-1 py-0.2 font-mono text-[10px] text-amber-700 dark:text-amber-300 border border-slate-200 dark:border-slate-600">
                  0x016
                </span>
              </button>
            </div>
          </div>
        </div>

        {/* Right Column: OM Encoder & Feeder CAN */}
        <div className="lg:col-span-5 space-y-4">
          {/* OM Encoder Card */}
          <div className="rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 p-4 shadow-2xs transition-colors">
            <div className="flex items-center justify-between border-b border-slate-100 dark:border-slate-800 pb-2.5">
              <div className="flex items-center gap-2">
                <Compass className="h-4 w-4 text-slate-600 dark:text-slate-400" />
                <h2 className="text-xs font-bold uppercase tracking-wider text-slate-800 dark:text-slate-200">
                  OM Encoder
                </h2>
              </div>
              <span className="rounded bg-slate-100 dark:bg-slate-800 border border-slate-200 dark:border-slate-700 px-2 py-0.5 font-mono text-[10px] text-slate-600 dark:text-slate-400">
                {t('dual_readings')}
              </span>
            </div>

            {/* Readout Rows - Compact 2-column layout */}
            <div className="mt-3 grid grid-cols-2 gap-2.5">
              {/* Lectura R */}
              <div className="rounded-lg bg-slate-50 dark:bg-slate-800/60 p-2.5 border border-slate-200 dark:border-slate-700">
                <div className="flex items-center justify-between text-xs text-slate-500 dark:text-slate-400">
                  <span className="font-medium">{t('reading_r')}</span>
                </div>
                <div className="mt-1 font-mono text-sm font-bold text-slate-900 dark:text-white">
                  {motionState.encoderR !== null ? `${motionState.encoderR.toFixed(2)} mm` : '—'}
                </div>
                <div className="mt-2">
                  <button
                    id="btn-set0-r"
                    onClick={onSetZeroR}
                    className="w-full flex items-center justify-center gap-1 rounded-md border border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 hover:bg-slate-50 dark:hover:bg-slate-700 px-2 py-1 text-xs font-semibold text-slate-700 dark:text-slate-200 transition active:scale-95 shadow-2xs"
                  >
                    <span>{t('btn_set0_r')}</span>
                    <span className="rounded bg-slate-100 dark:bg-slate-700 px-1 py-0.2 font-mono text-[9px] text-slate-600 dark:text-slate-300 border border-slate-200 dark:border-slate-600">
                      0x010
                    </span>
                  </button>
                </div>
              </div>

              {/* Lectura L */}
              <div className="rounded-lg bg-slate-50 dark:bg-slate-800/60 p-2.5 border border-slate-200 dark:border-slate-700">
                <div className="flex items-center justify-between text-xs text-slate-500 dark:text-slate-400">
                  <span className="font-medium">{t('reading_l')}</span>
                </div>
                <div className="mt-1 font-mono text-sm font-bold text-slate-900 dark:text-white">
                  {motionState.encoderL !== null ? `${motionState.encoderL.toFixed(2)} mm` : '—'}
                </div>
                <div className="mt-2">
                  <button
                    id="btn-set0-l"
                    onClick={onSetZeroL}
                    className="w-full flex items-center justify-center gap-1 rounded-md border border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 hover:bg-slate-50 dark:hover:bg-slate-700 px-2 py-1 text-xs font-semibold text-slate-700 dark:text-slate-200 transition active:scale-95 shadow-2xs"
                  >
                    <span>{t('btn_set0_l')}</span>
                    <span className="rounded bg-slate-100 dark:bg-slate-700 px-1 py-0.2 font-mono text-[9px] text-slate-600 dark:text-slate-300 border border-slate-200 dark:border-slate-600">
                      0x018
                    </span>
                  </button>
                </div>
              </div>
            </div>
          </div>

          {/* Feeder CAN Card */}
          <div className="rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 p-4 shadow-2xs transition-colors">
            <div className="flex items-center justify-between border-b border-slate-100 dark:border-slate-800 pb-2.5">
              <div className="flex items-center gap-2">
                <Activity className="h-4 w-4 text-slate-600 dark:text-slate-400" />
                <h2 className="text-xs font-bold uppercase tracking-wider text-slate-800 dark:text-slate-200">
                  Feeder CAN
                </h2>
              </div>
              <span className="rounded bg-slate-100 dark:bg-slate-800 border border-slate-200 dark:border-slate-700 px-2 py-0.5 font-mono text-[10px] text-slate-600 dark:text-slate-400">
                {t('can_diagnostics')}
              </span>
            </div>

            {/* Test Actions & Respective Offsets */}
            <div className="mt-3 grid grid-cols-2 gap-3">
              {/* Left Side (L) */}
              <div className="flex flex-col gap-2 rounded-lg bg-slate-50 dark:bg-slate-800/60 p-2.5 border border-slate-200 dark:border-slate-700">
                <button
                  id="btn-test-can-l"
                  onClick={onTestCanL}
                  className={`w-full flex items-center justify-center gap-1.5 rounded-lg border px-2.5 py-1.5 text-xs font-semibold transition active:scale-95 shadow-2xs ${
                    motionState.feederCanLTesting
                      ? 'bg-slate-100 dark:bg-slate-800 border-slate-400 text-slate-900 dark:text-white animate-pulse'
                      : 'border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 hover:bg-slate-50 dark:hover:bg-slate-700 text-slate-700 dark:text-slate-200'
                  }`}
                >
                  <span>{t('btn_feed_l')}</span>
                  <span className="rounded bg-slate-100 dark:bg-slate-700 px-1 py-0.2 font-mono text-[10px] text-slate-600 dark:text-slate-300 border border-slate-200 dark:border-slate-600">
                    0x013
                  </span>
                </button>

                <div className="space-y-1">
                  <label htmlFor="input-offset-can-l" className="text-[11px] font-semibold text-slate-600 dark:text-slate-400 flex items-center justify-between">
                    <span>Offset L</span>
                    <span className="text-[10px] text-slate-400 font-mono">mm</span>
                  </label>
                  <input
                    id="input-offset-can-l"
                    type="number"
                    step="0.1"
                    value={inputOffsetL}
                    onChange={(e) => handleOffsetLChange(e.target.value)}
                    placeholder="0.0"
                    className="w-full rounded-md border border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 px-2.5 py-1 font-mono text-xs font-semibold text-slate-900 dark:text-slate-100 shadow-2xs focus:border-slate-500 focus:outline-none focus:ring-1 focus:ring-slate-400"
                  />
                </div>
              </div>

              {/* Right Side (R) */}
              <div className="flex flex-col gap-2 rounded-lg bg-slate-50 dark:bg-slate-800/60 p-2.5 border border-slate-200 dark:border-slate-700">
                <button
                  id="btn-test-can-r"
                  onClick={onTestCanR}
                  className={`w-full flex items-center justify-center gap-1.5 rounded-lg border px-2.5 py-1.5 text-xs font-semibold transition active:scale-95 shadow-2xs ${
                    motionState.feederCanRTesting
                      ? 'bg-slate-100 dark:bg-slate-800 border-slate-400 text-slate-900 dark:text-white animate-pulse'
                      : 'border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 hover:bg-slate-50 dark:hover:bg-slate-700 text-slate-700 dark:text-slate-200'
                  }`}
                >
                  <span>{t('btn_feed_r')}</span>
                  <span className="rounded bg-slate-100 dark:bg-slate-700 px-1 py-0.2 font-mono text-[10px] text-slate-600 dark:text-slate-300 border border-slate-200 dark:border-slate-600">
                    0x012
                  </span>
                </button>

                <div className="space-y-1">
                  <label htmlFor="input-offset-can-r" className="text-[11px] font-semibold text-slate-600 dark:text-slate-400 flex items-center justify-between">
                    <span>Offset R</span>
                    <span className="text-[10px] text-slate-400 font-mono">mm</span>
                  </label>
                  <input
                    id="input-offset-can-r"
                    type="number"
                    step="0.1"
                    value={inputOffsetR}
                    onChange={(e) => handleOffsetRChange(e.target.value)}
                    placeholder="0.0"
                    className="w-full rounded-md border border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 px-2.5 py-1 font-mono text-xs font-semibold text-slate-900 dark:text-slate-100 shadow-2xs focus:border-slate-500 focus:outline-none focus:ring-1 focus:ring-slate-400"
                  />
                </div>
              </div>
            </div>

            <div className="mt-3 flex flex-wrap gap-2">
              <button
                type="button"
                id="btn-feed-offset-save"
                onClick={handleSaveOffsets}
                className="flex items-center gap-1 rounded-lg bg-slate-900 dark:bg-slate-100 text-white dark:text-slate-900 px-3 py-1.5 text-xs font-bold"
              >
                {t('btn_save_offset')}
              </button>
              <button
                type="button"
                id="btn-feed-offset-reload"
                onClick={onReloadFeedOffset}
                className="flex items-center gap-1 rounded-lg border border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 px-3 py-1.5 text-xs font-semibold text-slate-700 dark:text-slate-200"
              >
                {t('btn_reload_offset')}
              </button>
            </div>
          </div>
        </div>
      </div>

      {showLogs && (
        <LogTerminal
          title={t('logs_title')}
          logs={logs}
          onClear={onClearLogs}
          filterModule="MOTION"
        />
      )}
    </div>
  );
};

