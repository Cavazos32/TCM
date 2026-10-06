import React, { useState, useEffect } from 'react';
import {
  Move,
  Activity,
  RotateCcw,
  Square,
  Compass,
  Home,
  Power,
  PowerOff,
  Crosshair,
} from 'lucide-react';
import { MotionState, LogEntry } from '../types';
import { LogTerminal } from './LogTerminal';
import { ModuleStatusBar } from './ModuleStatusBar';
import { useApp } from '../context/AppContext';
import {
  hmiModuleBadge,
  hmiModuleBtnBase,
  hmiModuleBtnDangerSoft,
  hmiModuleBtnDark,
  hmiModuleBtnGroup,
  hmiModuleBtnSecondary,
  hmiModuleBtnSuccessSoft,
  hmiModuleCenterSectionTitle,
  hmiModuleCompactFieldInput,
  hmiModuleFieldLabel,
  hmiModuleIconSection,
  hmiModuleSectionTitle,
  hmiPanelCls,
  hmiPanelHeader,
  hmiPanelPadding,
  hmiSubPanelHeader,
} from '../styles/hmiUi';

interface MotionTabProps {
  motionState: MotionState;
  onUpdateTargetPos: (pos: number) => void;
  onUpdateRpm: (rpm: number) => void;
  onUpdateOffsetL?: (offset: number) => void;
  onUpdateOffsetR?: (offset: number) => void;
  onMover: (mm: number, rpm: number) => void;
  onStop: () => void;
  onServoOn: () => void;
  onServoOff: () => void;
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
  onServoOn,
  onServoOff,
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
  // Posición ASDA: internamente negativa; en UI se muestra y edita como magnitud positiva.
  const toDisplayMm = (internalMm: number) => -internalMm;
  const toInternalMm = (displayMm: number) => -displayMm;

  const [inputPos, setInputPos] = useState<string>(
    toDisplayMm(motionState.targetPositionMm).toString()
  );
  const [inputRpm, setInputRpm] = useState<string>(motionState.rpm.toString());
  const [inputOffsetL, setInputOffsetL] = useState<string>((motionState.offsetL ?? 0).toString());
  const [inputOffsetR, setInputOffsetR] = useState<string>((motionState.offsetR ?? 0).toString());

  useEffect(() => {
    setInputPos(toDisplayMm(motionState.targetPositionMm).toString());
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
      onUpdateTargetPos(toInternalMm(num));
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
    const nextDisplay = (parseFloat(inputPos) || 0) + delta;
    setInputPos(nextDisplay.toString());
    onUpdateTargetPos(toInternalMm(nextDisplay));
  };

  const handleMove = () => {
    const displayMm = parseFloat(inputPos);
    const rpm = parseInt(inputRpm, 10);
    onMover(
      Number.isFinite(displayMm)
        ? toInternalMm(displayMm)
        : motionState.targetPositionMm,
      Number.isFinite(rpm) ? rpm : motionState.rpm
    );
  };

  // laserR/L true = Active (sensor ON / material presente) → indicador verde;
  // false = Inactive (sin material) → ámbar.
  const laserChip = (
    id: string,
    label: string,
    active: boolean
  ) => (
    <div
      key={id}
      className={`flex items-center justify-between gap-3 rounded-lg border px-4 py-4 sm:px-5 sm:py-4 ${
        active
          ? 'border-emerald-300 dark:border-emerald-800 bg-emerald-50 dark:bg-emerald-950/40'
          : 'border-amber-300 dark:border-amber-800 bg-amber-50 dark:bg-amber-950/40'
      }`}
    >
      <div className="flex items-center gap-2 min-w-0">
        <span
          className={`h-3 w-3 rounded-full shrink-0 ${
            active ? 'bg-emerald-500' : 'bg-amber-500 animate-pulse'
          }`}
        />
        <span className="truncate text-lg font-semibold text-slate-800 dark:text-slate-200 sm:text-xl">
          {label}
        </span>
      </div>
      <div className="flex items-center gap-1.5 shrink-0">
        <span
          className={`text-base font-bold font-mono sm:text-lg ${
            active
              ? 'text-emerald-700 dark:text-emerald-300'
              : 'text-amber-800 dark:text-amber-200'
          }`}
        >
          {active ? t('sensor_active') : t('sensor_inactive')}
        </span>
      </div>
    </div>
  );

  const connected = motionState.connection.connected;
  const hasError = !!motionState.hasError && connected;
  const statusTone = !connected
    ? 'disconnected'
    : hasError
      ? 'error'
      : motionState.isMoving
        ? 'moving'
        : 'ready';

  return (
    <div className="space-y-4">
      <ModuleStatusBar
        headline={
          motionState.statusText ||
          (motionState.isMoving
            ? t('motor_moving')
            : hasError
              ? t('state_error')
              : t('state_ready'))
        }
        tone={statusTone}
        connection={motionState.connection}
        headlineError={hasError}
        metric={{
          label: t('current_position'),
          value: `${toDisplayMm(motionState.currentPositionMm).toFixed(2)} mm`,
        }}
      />

      {/* 2 columnas: ASDA+láseres | OM+Feeder */}
      <div className="grid grid-cols-1 gap-4 lg:grid-cols-12 lg:items-start">
        {/* —— Izquierda —— */}
        <div className="lg:col-span-7 space-y-4">
          <div className={`${hmiPanelCls} ${hmiPanelPadding}`}>
            <div className={hmiPanelHeader}>
              <div className="flex items-center gap-2">
                <Move className={`${hmiModuleIconSection} text-slate-600 dark:text-slate-400`} />
                <h2 className={hmiModuleCenterSectionTitle}>{t('linear_actuator_title')}</h2>
              </div>
              <span className={hmiModuleBadge}>{t('motion_badge_manual')}</span>
            </div>

            <div className="mt-3.5 space-y-3">
              <div className="flex items-center justify-between gap-3">
                <label
                  htmlFor="input-pos-mm"
                  className={`${hmiModuleFieldLabel} min-w-[90px]`}
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
                    className={`${hmiModuleCompactFieldInput} font-mono`}
                  />
                  <div className="flex items-center gap-1 shrink-0">
                    <button
                      onClick={() => applyNudge(-10)}
                      className={`${hmiModuleBtnBase} px-4 font-mono font-semibold`}
                      title="-10 mm"
                    >
                      -10
                    </button>
                    <button
                      onClick={() => applyNudge(10)}
                      className={`${hmiModuleBtnBase} px-4 font-mono font-semibold`}
                      title="+10 mm"
                    >
                      +10
                    </button>
                  </div>
                </div>
              </div>

              <div className="flex items-center justify-between gap-3">
                <label
                  htmlFor="input-rpm"
                  className={`${hmiModuleFieldLabel} min-w-[90px]`}
                >
                  {t('speed_label')}
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
                    className={`${hmiModuleCompactFieldInput} font-mono`}
                  />
                  <div className="flex items-center gap-1 shrink-0">
                    {[600, 1200, 2400].map((preset) => (
                      <button
                        key={preset}
                        onClick={() => {
                          setInputRpm(preset.toString());
                          onUpdateRpm(preset);
                        }}
                        className={`${hmiModuleBtnBase} px-4 font-mono ${
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

            {/* Comandos: movimiento / servo / home */}
            <div className="mt-4 pt-3 border-t border-slate-100 dark:border-slate-800 space-y-2">
              <div className={`${hmiModuleBtnGroup} !gap-2`}>
                <button
                  id="btn-mover-motion"
                  onClick={handleMove}
                  disabled={motionState.isMoving}
                  className={hmiModuleBtnDark}
                >
                  <span>{t('btn_move')}</span>
                </button>
                <button
                  id="btn-stop-motion"
                  onClick={onStop}
                  className={hmiModuleBtnDangerSoft}
                >
                  <Square className="h-5 w-5 fill-current" />
                  <span>{t('btn_stop')}</span>
                </button>
                <button
                  id="btn-servo-on-motion"
                  onClick={onServoOn}
                  disabled={motionState.isMoving}
                  className={hmiModuleBtnSuccessSoft}
                >
                  <Power className="h-5 w-5" />
                  <span>{t('btn_servo_on')}</span>
                </button>
                <button
                  id="btn-servo-off-motion"
                  onClick={onServoOff}
                  className={hmiModuleBtnSecondary}
                >
                  <PowerOff className="h-5 w-5" />
                  <span>{t('btn_servo_off')}</span>
                </button>
              </div>
              <div className={`${hmiModuleBtnGroup} !gap-2`}>
                <button
                  id="btn-search-home-motion"
                  onClick={onSearchHome}
                  disabled={motionState.isMoving}
                  className={hmiModuleBtnSuccessSoft}
                >
                  <Home className="h-5 w-5" />
                  <span>{t('btn_search_home')}</span>
                </button>
                <button
                  id="btn-move0-motion"
                  onClick={onMoveToZero}
                  className={hmiModuleBtnSecondary}
                >
                  <span>{t('btn_move_to_zero')}</span>
                </button>
                <button
                  id="btn-reset-errores-motion"
                  onClick={onResetErrors}
                  className={hmiModuleBtnSecondary}
                >
                  <RotateCcw className="h-5 w-5 text-amber-600 dark:text-amber-400" />
                  <span>{t('btn_reset_errors')}</span>
                </button>
              </div>
            </div>
          </div>

          {/* Láseres bajo ASDA (misma columna) */}
          <div className={`${hmiPanelCls} ${hmiPanelPadding}`}>
            <div className={hmiSubPanelHeader}>
              <div className="flex items-center gap-2">
                <Crosshair className={`${hmiModuleIconSection} text-slate-600 dark:text-slate-400`} />
                <h2 className={`${hmiModuleSectionTitle} text-slate-800 dark:text-slate-200`}>
                  {t('laser_sensors')}
                </h2>
              </div>
              <span className={hmiModuleBadge}>{t('laser_sensors_badge')}</span>
            </div>
            <div className="grid grid-cols-1 sm:grid-cols-2 gap-2.5">
              {laserChip('laser-r', t('laser_r'), motionState.laserR)}
              {laserChip('laser-l', t('laser_l'), motionState.laserL)}
            </div>
          </div>
        </div>

        {/* —— Derecha —— */}
        <div className="lg:col-span-5 space-y-4">
          <div className={`${hmiPanelCls} ${hmiPanelPadding}`}>
            <div className={hmiSubPanelHeader}>
              <div className="flex items-center gap-2">
                <Compass className={`${hmiModuleIconSection} text-slate-600 dark:text-slate-400`} />
                <h2 className={`${hmiModuleSectionTitle} text-slate-800 dark:text-slate-200`}>
                  {t('external_measure_title')}
                </h2>
              </div>
              <span className={hmiModuleBadge}>{t('dual_readings')}</span>
            </div>

            <div className="grid grid-cols-2 gap-3">
              <div className="rounded-lg bg-slate-50 dark:bg-slate-800/60 p-3 border border-slate-200 dark:border-slate-700">
                <div className="text-base font-semibold text-slate-500 dark:text-slate-400 sm:text-lg">
                  {t('reading_r')}
                </div>
                <div className="mt-1 font-mono text-lg font-bold text-slate-900 dark:text-white sm:text-xl">
                  {motionState.encoderR !== null ? `${motionState.encoderR.toFixed(2)} mm` : '—'}
                </div>
                <div className="mt-2">
                  <button
                    id="btn-set0-r"
                    onClick={onSetZeroR}
                    className={`${hmiModuleBtnSecondary} w-full`}
                  >
                    <span>{t('btn_set0_r')}</span>
                  </button>
                </div>
              </div>

              <div className="rounded-lg bg-slate-50 dark:bg-slate-800/60 p-3 border border-slate-200 dark:border-slate-700">
                <div className="text-base font-semibold text-slate-500 dark:text-slate-400 sm:text-lg">
                  {t('reading_l')}
                </div>
                <div className="mt-1 font-mono text-lg font-bold text-slate-900 dark:text-white sm:text-xl">
                  {motionState.encoderL !== null ? `${motionState.encoderL.toFixed(2)} mm` : '—'}
                </div>
                <div className="mt-2">
                  <button
                    id="btn-set0-l"
                    onClick={onSetZeroL}
                    className={`${hmiModuleBtnSecondary} w-full`}
                  >
                    <span>{t('btn_set0_l')}</span>
                  </button>
                </div>
              </div>
            </div>
          </div>

          <div className={`${hmiPanelCls} ${hmiPanelPadding}`}>
            <div className={hmiSubPanelHeader}>
              <div className="flex items-center gap-2">
                <Activity className={`${hmiModuleIconSection} text-slate-600 dark:text-slate-400`} />
                <h2 className={`${hmiModuleSectionTitle} text-slate-800 dark:text-slate-200`}>
                  {t('material_feed_title')}
                </h2>
              </div>
              <span className={hmiModuleBadge}>{t('can_diagnostics')}</span>
            </div>

            <div className="grid grid-cols-2 gap-3">
              <div className="flex flex-col gap-2 rounded-lg bg-slate-50 dark:bg-slate-800/60 p-3 border border-slate-200 dark:border-slate-700">
                <button
                  id="btn-test-can-r"
                  onClick={onTestCanR}
                  className={`${hmiModuleBtnSecondary} w-full ${
                    motionState.feederCanRTesting
                      ? 'bg-slate-100 dark:bg-slate-800 border-slate-400 text-slate-900 dark:text-white animate-pulse'
                      : 'border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 hover:bg-slate-50 dark:hover:bg-slate-700 text-slate-700 dark:text-slate-200'
                  }`}
                >
                  <span>{t('btn_feed_r')}</span>
                </button>
                <div className="space-y-1">
                  <label htmlFor="input-offset-can-r" className={`${hmiModuleFieldLabel} flex items-center justify-between`}>
                    <span>{t('offset_r')}</span>
                    <span className="text-slate-400 font-mono">mm</span>
                  </label>
                  <input
                    id="input-offset-can-r"
                    type="number"
                    step="0.1"
                    value={inputOffsetR}
                    onChange={(e) => handleOffsetRChange(e.target.value)}
                    placeholder="0.0"
                    className={hmiModuleCompactFieldInput}
                  />
                </div>
              </div>

              <div className="flex flex-col gap-2 rounded-lg bg-slate-50 dark:bg-slate-800/60 p-3 border border-slate-200 dark:border-slate-700">
                <button
                  id="btn-test-can-l"
                  onClick={onTestCanL}
                  className={`${hmiModuleBtnSecondary} w-full ${
                    motionState.feederCanLTesting
                      ? 'bg-slate-100 dark:bg-slate-800 border-slate-400 text-slate-900 dark:text-white animate-pulse'
                      : 'border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 hover:bg-slate-50 dark:hover:bg-slate-700 text-slate-700 dark:text-slate-200'
                  }`}
                >
                  <span>{t('btn_feed_l')}</span>
                </button>
                <div className="space-y-1">
                  <label htmlFor="input-offset-can-l" className={`${hmiModuleFieldLabel} flex items-center justify-between`}>
                    <span>{t('offset_l')}</span>
                    <span className="text-slate-400 font-mono">mm</span>
                  </label>
                  <input
                    id="input-offset-can-l"
                    type="number"
                    step="0.1"
                    value={inputOffsetL}
                    onChange={(e) => handleOffsetLChange(e.target.value)}
                    placeholder="0.0"
                    className={hmiModuleCompactFieldInput}
                  />
                </div>
              </div>
            </div>

            <div className={`${hmiModuleBtnGroup} mt-3 !gap-2`}>
              <button
                type="button"
                id="btn-feed-offset-save"
                onClick={handleSaveOffsets}
                className={hmiModuleBtnDark}
              >
                {t('btn_save_offset')}
              </button>
              <button
                type="button"
                id="btn-feed-offset-reload"
                onClick={onReloadFeedOffset}
                className={hmiModuleBtnSecondary}
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

