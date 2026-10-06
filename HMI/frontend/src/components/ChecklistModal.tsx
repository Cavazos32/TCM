import React, { useState } from 'react';
import {
  ClipboardCheck,
  X,
  AlertTriangle,
  Loader2,
} from 'lucide-react';
import { useApp } from '../context/AppContext';
import type { BackendSnapshot } from '../api/backendTypes';
import { checklistCmd, getState } from '../api/hmiApi';
import { mapChecklistState } from '../api/mappers';
import type { PlcState } from '../types';
import type { ChecklistState, ChecklistStepId } from '../types/checklist';
import { CHECKLIST_PIECE_EXTRA_MM, CHECKLIST_STEP_ORDER } from '../types/checklist';
import { ModuleStatusBar } from './ModuleStatusBar';
import {
  hmiAssistHint,
  hmiFieldInput,
  hmiFieldLabel,
  hmiModuleBtnPrimary,
  hmiModuleBtnSecondary,
  hmiPanelCls,
} from '../styles/hmiUi';

interface ChecklistModalProps {
  isOpen: boolean;
  onClose: () => void;
  checklist: ChecklistState;
  maintenanceCycleCount: number;
  nominalPieceMm: number;
  safetyExhaust: boolean;
  andonPressure: boolean;
  plcState: PlcState;
  onOpenMantenimiento?: () => void;
  onChecklistUpdate?: (state: ChecklistState) => void;
  onCompleted?: () => void;
}

const STEP_TITLE_KEYS: Record<ChecklistStepId, string> = {
  cycle_counter: 'checklist_step_counter',
  air_pressure: 'checklist_step_air',
  pf_holgura: 'checklist_step_holgura',
  asda_roundtrip: 'checklist_step_asda',
  feeder_can_purge: 'checklist_step_feeder',
  piece_measure: 'checklist_step_pieces',
  '': '',
};

const CHECKLIST_ERROR_KEYS: Record<string, string> = {
  invalid_employee: 'checklist_err_invalid_employee',
  counter_over_limit: 'checklist_err_counter_over',
  air_not_ready: 'checklist_err_air_not_ready',
  air_test_pending: 'checklist_err_air_test_pending',
  air_test_failed: 'checklist_err_air_test_failed',
  stage2_pending: 'checklist_err_stage2_pending',
  purge_active: 'checklist_err_purge_active',
  measurements_missing: 'checklist_err_measurements_missing',
  measurements_out_of_tolerance: 'checklist_err_measurements_out',
  checklist_busy: 'checklist_err_busy',
  no_session: 'checklist_err_no_session',
  wrong_step: 'checklist_err_wrong_step',
  machine_in_motion: 'checklist_err_machine_in_motion',
  asda_pending: 'checklist_err_asda_pending',
  worker_failed: 'checklist_err_worker',
};

type StepAction = {
  id: string;
  label: string;
  onClick: () => void;
  disabled?: boolean;
  primary?: boolean;
};

function friendlyChecklistError(code: string | undefined, t: (k: string) => string): string {
  if (!code) return t('checklist_error_generic');
  const key = CHECKLIST_ERROR_KEYS[code];
  return key ? t(key) : t('checklist_error_generic');
}

export const ChecklistModal: React.FC<ChecklistModalProps> = ({
  isOpen,
  onClose,
  checklist,
  maintenanceCycleCount,
  nominalPieceMm,
  safetyExhaust,
  andonPressure,
  plcState,
  onOpenMantenimiento,
  onChecklistUpdate,
  onCompleted,
}) => {
  const { t } = useApp();
  const [employeeId, setEmployeeId] = useState('');
  const [lengthL, setLengthL] = useState('');
  const [lengthR, setLengthR] = useState('');
  const [error, setError] = useState('');
  const [busy, setBusy] = useState(false);

  if (!isOpen) return null;

  const session = checklist.session;
  const stepId = checklist.currentStepId;
  const stepIndex = CHECKLIST_STEP_ORDER.indexOf(stepId);
  const expectedPieceMm = nominalPieceMm + CHECKLIST_PIECE_EXTRA_MM;
  const status = checklist.stepStatus;
  const motionBlocked = !!status.motionBlocked;
  const stepFault = !!(error || checklist.stepError || checklist.workerError);
  const stepReady =
    !!session && !!stepId && !!status.ready && !stepFault && !checklist.workerBusy;

  const applyChecklistResponse = async (res: {
    ok: boolean;
    checklist?: BackendSnapshot['checklist'];
  }) => {
    if (!onChecklistUpdate) return;
    if (res.checklist) {
      onChecklistUpdate(
        mapChecklistState({ checklist: res.checklist } as BackendSnapshot)
      );
      return;
    }
    const snap = await getState();
    onChecklistUpdate(mapChecklistState(snap));
  };

  const run = async (
    fn: () => Promise<{ ok: boolean; error?: string; checklist?: BackendSnapshot['checklist'] }>
  ) => {
    setBusy(true);
    setError('');
    try {
      const res = await fn();
      if (res.ok) {
        await applyChecklistResponse(res);
        const cl = res.checklist;
        if (cl && !cl.required && !cl.session) {
          onCompleted?.();
        }
      } else {
        setError(friendlyChecklistError(res.error, t));
      }
    } catch {
      setError(t('checklist_error_generic'));
    } finally {
      setBusy(false);
    }
  };

  const handleClose = async () => {
    await checklistCmd('close');
    onClose();
  };

  const handleConfirm = () =>
    run(() => checklistCmd('action', { name: 'confirm_step', stepId }));

  const handleFinishMeasure = () =>
    run(async () => {
      const save = await checklistCmd('action', {
        name: 'set_measurements',
        lengthL: parseFloat(lengthL),
        lengthR: parseFloat(lengthR),
      });
      if (!save.ok) return save;
      return checklistCmd('action', { name: 'confirm_step', stepId: 'piece_measure' });
    });

  const getStepActions = (): StepAction[] => {
    if (!session) {
      return [
        {
          id: 'start',
          label: t('checklist_start'),
          primary: true,
          disabled: employeeId.length !== 5 || busy,
          onClick: () => run(() => checklistCmd('start', { employeeId })),
        },
      ];
    }

    switch (stepId) {
      case 'cycle_counter':
        if (status.overLimit && onOpenMantenimiento) {
          return [
            {
              id: 'maintenance',
              label: t('checklist_btn_maintenance'),
              primary: true,
              disabled: busy,
              onClick: onOpenMantenimiento,
            },
          ];
        }
        return [
          {
            id: 'continue',
            label: t('checklist_btn_continue'),
            primary: true,
            disabled: busy || !!status.overLimit || motionBlocked,
            onClick: handleConfirm,
          },
        ];

      case 'air_pressure': {
        const pressuresOk = status.pressuresOk !== false;
        const canContinue = !!status.ready;
        if (canContinue) {
          return [
            {
              id: 'continue',
              label: t('checklist_btn_continue'),
              primary: true,
              disabled: busy || motionBlocked,
              onClick: handleConfirm,
            },
          ];
        }
        return [
          {
            id: 'air-test',
            label: t('checklist_btn_air_on'),
            primary: true,
            disabled: busy || !pressuresOk || motionBlocked,
            onClick: () =>
              run(() => checklistCmd('action', { name: 'air_test_on' })),
          },
        ];
      }

      case 'pf_holgura':
        return [
          {
            id: 'continue',
            label: t('checklist_btn_continue'),
            primary: true,
            disabled: busy || motionBlocked,
            onClick: handleConfirm,
          },
        ];

      case 'asda_roundtrip':
        if (checklist.workerBusy) return [];
        if (checklist.workerError) {
          return [
            {
              id: 'asda-retry',
              label: t('checklist_btn_retry'),
              primary: true,
              disabled: busy || motionBlocked,
              onClick: () =>
                run(() => checklistCmd('action', { name: 'start_asda_roundtrip' })),
            },
          ];
        }
        if (status.asdaOk || status.ready) {
          return [
            {
              id: 'continue',
              label: t('checklist_btn_continue'),
              primary: true,
              disabled: busy || motionBlocked,
              onClick: handleConfirm,
            },
          ];
        }
        return [
          {
            id: 'asda-start',
            label: t('checklist_btn_move_axis'),
            primary: true,
            disabled: busy || motionBlocked,
            onClick: () =>
              run(() => checklistCmd('action', { name: 'start_asda_roundtrip' })),
          },
        ];

      case 'feeder_can_purge':
        if (checklist.workerBusy) return [];
        if (checklist.workerError) {
          return [
            {
              id: 'feeder-retry',
              label: t('checklist_btn_retry'),
              primary: true,
              disabled: busy || motionBlocked,
              onClick: () =>
                run(() =>
                  checklistCmd('action', {
                    name: status.stage2Ok ? 'start_purge' : 'start_feeder_can',
                  })
                ),
            },
          ];
        }
        if (!status.stage2Ok) {
          return [
            {
              id: 'feeder-prep',
              label: t('checklist_btn_prepare_feeder'),
              primary: true,
              disabled: busy || motionBlocked,
              onClick: () =>
                run(() => checklistCmd('action', { name: 'start_feeder_can' })),
            },
          ];
        }
        if (status.stage2Ok && status.purgeIdle) {
          return [
            {
              id: 'continue',
              label: t('checklist_btn_continue'),
              primary: true,
              disabled: busy || motionBlocked,
              onClick: handleConfirm,
            },
          ];
        }
        return [
          {
            id: 'purge',
            label: t('checklist_btn_purge'),
            primary: true,
            disabled: busy || motionBlocked,
            onClick: () =>
              run(() => checklistCmd('action', { name: 'start_purge' })),
          },
        ];

      case 'piece_measure':
        if (!checklist.pieceTestStarted) {
          return [
            {
              id: 'start-pieces',
              label: t('checklist_btn_cut_test_pieces'),
              primary: true,
              disabled: busy || motionBlocked,
              onClick: () =>
                run(() => checklistCmd('action', { name: 'start_piece_test' })),
            },
          ];
        }
        if ((status.piecesDone ?? 0) >= 2) {
          return [
            {
              id: 'finish',
              label: t('checklist_btn_finish'),
              primary: true,
              disabled:
                busy || motionBlocked || !lengthL.trim() || !lengthR.trim(),
              onClick: handleFinishMeasure,
            },
          ];
        }
        return [];

      default:
        if (session && !stepId) {
          return [
            {
              id: 'complete',
              label: t('checklist_btn_finish'),
              primary: true,
              disabled: busy || motionBlocked,
              onClick: () => run(() => checklistCmd('complete')),
            },
          ];
        }
        return [];
    }
  };

  const stepActions = getStepActions();

  const renderStepBody = () => {
    if (!session) return null;

    if (!stepId) {
      return (
        <div className="space-y-3">
          <p className={hmiAssistHint}>{t('checklist_finish_pending')}</p>
        </div>
      );
    }

    switch (stepId) {
      case 'cycle_counter':
        return (
          <div className="space-y-4">
            <p className={hmiAssistHint}>{t('checklist_step_counter_hint')}</p>
            <div className="rounded-lg border border-sky-200 bg-sky-50 px-4 py-3 dark:border-sky-800 dark:bg-sky-950/40">
              <p className="text-sm font-semibold text-slate-600 dark:text-slate-400">
                {t('checklist_counter_label')}
              </p>
              <p className="text-3xl font-bold text-sky-800 dark:text-sky-200">
                {maintenanceCycleCount.toLocaleString('es-MX')}
              </p>
              <p className="mt-1 text-sm text-slate-500">{t('checklist_counter_limit_hint')}</p>
            </div>
            {status.overLimit ? (
              <div className="flex items-start gap-2 rounded-lg border border-amber-300 bg-amber-50 p-3 text-amber-900 dark:border-amber-700 dark:bg-amber-950/40 dark:text-amber-100">
                <AlertTriangle className="h-5 w-5 shrink-0" />
                <p className="text-sm font-medium">{t('checklist_counter_over')}</p>
              </div>
            ) : null}
          </div>
        );

      case 'air_pressure': {
        const plcConnected = plcState.connection.connected;
        const plcHasError = !!plcState.hasError && plcConnected;
        const valveErrors = plcState.valves.filter((v) => v.hasError).length;
        const plcTone = !plcConnected
          ? 'disconnected'
          : plcHasError
            ? 'error'
            : checklist.airTestActive
              ? 'busy'
              : 'ready';
        return (
          <div className="space-y-4">
            <p className={hmiAssistHint}>{t('checklist_step_air_hint')}</p>
            <ModuleStatusBar
              headline={
                plcState.statusText ||
                (plcHasError ? t('state_error') : t('state_ready'))
              }
              tone={plcTone}
              connection={plcState.connection}
              headlineError={plcHasError}
              metric={{
                label: t('valves_status'),
                value:
                  valveErrors > 0
                    ? `${valveErrors} ${t('checklist_air_valve_errors')}`
                    : t('checklist_status_ok'),
                tone: valveErrors > 0 ? 'error' : 'success',
              }}
            />
            <div className="grid grid-cols-1 gap-2 sm:grid-cols-2">
              <div
                className={`rounded-lg border p-3 ${!safetyExhaust ? 'border-emerald-400 bg-emerald-50 dark:bg-emerald-950/30' : 'border-red-400 bg-red-50 dark:bg-red-950/30'}`}
              >
                <span className="text-sm font-bold">{t('checklist_air_escape')}</span>
                <p className="mt-1 text-sm">
                  {!safetyExhaust ? t('checklist_status_ok') : t('checklist_air_escape_bad')}
                </p>
              </div>
              <div
                className={`rounded-lg border p-3 ${!andonPressure ? 'border-emerald-400 bg-emerald-50 dark:bg-emerald-950/30' : 'border-red-400 bg-red-50 dark:bg-red-950/30'}`}
              >
                <span className="text-sm font-bold">{t('checklist_air_plant')}</span>
                <p className="mt-1 text-sm">
                  {!andonPressure ? t('checklist_status_ok') : t('checklist_pressure_low')}
                </p>
              </div>
            </div>
            {busy && checklist.airTestActive && !status.airTestVerified && (
              <p className="text-sm font-medium text-sky-800 dark:text-sky-200">
                {t('checklist_air_test_wait')}
              </p>
            )}
            {status.airTestVerified && status.plcValvesOk !== false && (
              <p className="text-sm font-medium text-emerald-800 dark:text-emerald-200">
                {t('checklist_air_test_ok')}
              </p>
            )}
            {checklist.airTestActive &&
              !status.airTestVerified &&
              !busy &&
              status.plcValvesOk === false && (
                <p className="text-sm font-medium text-red-700 dark:text-red-300">
                  {t('checklist_air_test_fail')}
                </p>
              )}
          </div>
        );
      }

      case 'pf_holgura':
        return (
          <div className="space-y-3">
            <p className={hmiAssistHint}>{t('checklist_step_holgura_hint')}</p>
          </div>
        );

      case 'asda_roundtrip':
        return (
          <div className="space-y-4">
            <p className={hmiAssistHint}>{t('checklist_step_asda_hint')}</p>
            {(checklist.workerBusy || checklist.workerPhase) && (
              <div className="flex items-center gap-2 text-sky-700 dark:text-sky-300">
                <Loader2 className="h-5 w-5 animate-spin" />
                <span>{t('checklist_working')}</span>
              </div>
            )}
            {checklist.workerError && (
              <p className="text-sm font-medium text-red-700 dark:text-red-300">
                {t('checklist_err_worker')}
              </p>
            )}
            {status.asdaOk && !checklist.workerError && (
              <p className="text-sm font-medium text-emerald-800 dark:text-emerald-200">
                {t('checklist_step_ok')}
              </p>
            )}
          </div>
        );

      case 'feeder_can_purge':
        return (
          <div className="space-y-4">
            <p className={hmiAssistHint}>{t('checklist_step_feeder_hint')}</p>
            {(checklist.workerBusy || checklist.workerPhase) && (
              <div className="flex items-center gap-2 text-sky-700 dark:text-sky-300">
                <Loader2 className="h-5 w-5 animate-spin" />
                <span>{t('checklist_working')}</span>
              </div>
            )}
            {status.stage2Ok && !status.purgeIdle && (
              <p className="text-sm font-medium text-sky-800 dark:text-sky-200">
                {t('checklist_purge_running_hint')}
              </p>
            )}
            {checklist.workerError && (
              <p className="text-sm font-medium text-red-700 dark:text-red-300">
                {t('checklist_err_worker')}
              </p>
            )}
          </div>
        );

      case 'piece_measure':
        return (
          <div className="space-y-4">
            <p className={hmiAssistHint}>{t('checklist_step_pieces_hint')}</p>
            {checklist.pieceTestStarted && (
              <>
                <p className="text-base font-semibold">
                  {t('checklist_pieces_done', { count: status.piecesDone ?? 0 })}
                </p>
                {(status.piecesDone ?? 0) >= 2 && (
                  <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
                    <div>
                      <label className={hmiFieldLabel}>{t('checklist_measure_left')}</label>
                      <input
                        type="number"
                        step="0.1"
                        inputMode="decimal"
                        value={lengthL}
                        onChange={(e) => setLengthL(e.target.value)}
                        placeholder={expectedPieceMm.toFixed(0)}
                        className={`${hmiFieldInput} mt-1`}
                      />
                    </div>
                    <div>
                      <label className={hmiFieldLabel}>{t('checklist_measure_right')}</label>
                      <input
                        type="number"
                        step="0.1"
                        inputMode="decimal"
                        value={lengthR}
                        onChange={(e) => setLengthR(e.target.value)}
                        placeholder={expectedPieceMm.toFixed(0)}
                        className={`${hmiFieldInput} mt-1`}
                      />
                    </div>
                  </div>
                )}
                {(status.piecesDone ?? 0) < 2 && (
                  <p className="text-sm text-slate-600 dark:text-slate-400">
                    {t('checklist_pieces_waiting')}
                  </p>
                )}
              </>
            )}
          </div>
        );

      default:
        return null;
    }
  };

  return (
    <div className="fixed inset-0 z-[70] flex items-center justify-center p-4">
      <div className="fixed inset-0 bg-slate-900/60 backdrop-blur-xs" />
      <div
        role="dialog"
        aria-modal="true"
        className={`relative z-10 flex max-h-[92vh] w-full max-w-2xl flex-col overflow-hidden shadow-2xl ${hmiPanelCls} ${
          stepFault
            ? 'ring-4 ring-red-500'
            : stepReady
              ? 'ring-2 ring-emerald-400'
              : checklist.required && !session
                ? 'ring-4 ring-amber-400 animate-pulse'
                : ''
        }`}
      >
        <div className="flex items-center justify-between border-b border-slate-200 px-4 py-3 dark:border-slate-800 sm:px-5">
          <div className="flex items-center gap-2">
            <ClipboardCheck className="h-7 w-7 text-amber-600" />
            <div>
              <h2 className="text-lg font-bold sm:text-xl">{t('checklist_title')}</h2>
              {session && stepId && (
                <p className="text-sm text-slate-500">
                  {t('checklist_progress', { current: stepIndex + 1, total: CHECKLIST_STEP_ORDER.length })}
                </p>
              )}
            </div>
          </div>
          <button
            type="button"
            onClick={handleClose}
            aria-label={t('checklist_close')}
            className="rounded-lg p-2 text-slate-400 hover:bg-slate-100 dark:hover:bg-slate-800"
          >
            <X className="h-5 w-5" />
          </button>
        </div>

        <div className="flex flex-1 flex-col overflow-hidden">
          <div className="flex-1 overflow-y-auto p-4 sm:p-5">
            {!session ? (
              <div className="space-y-4">
                <p className={hmiAssistHint}>{t('checklist_start_hint')}</p>
                <label className={hmiFieldLabel} htmlFor="checklist-employee">
                  {t('maintenance_employee_label')}
                </label>
                <input
                  id="checklist-employee"
                  inputMode="numeric"
                  maxLength={5}
                  value={employeeId}
                  onChange={(e) => setEmployeeId(e.target.value.replace(/\D/g, '').slice(0, 5))}
                  className={hmiFieldInput}
                  placeholder={t('maintenance_employee_placeholder')}
                />
              </div>
            ) : (
              <>
                <h3 className="mb-3 text-base font-bold text-slate-800 dark:text-slate-100 sm:text-lg">
                  {t(STEP_TITLE_KEYS[stepId] as 'checklist_title')}
                </h3>
              {renderStepBody()}
              {motionBlocked && !checklist.workerBusy && (
                <p className="mt-3 text-sm font-medium text-amber-800 dark:text-amber-200">
                  {t('checklist_motion_blocked')}
                </p>
              )}
            </>
          )}
          {(error || checklist.stepError) && (
              <p className="mt-3 rounded-lg border border-red-300 bg-red-50 p-3 text-sm font-medium text-red-800 dark:border-red-800 dark:bg-red-950/40 dark:text-red-200">
                {error || friendlyChecklistError(checklist.stepError, t)}
              </p>
            )}
          {stepReady && !error && (
              <p className="mt-3 rounded-lg border border-emerald-300 bg-emerald-50 p-3 text-sm font-medium text-emerald-800 dark:border-emerald-800 dark:bg-emerald-950/40 dark:text-emerald-200">
                {t('checklist_step_ok')}
              </p>
            )}
          </div>

          {stepActions.length > 0 && (
            <div className="flex shrink-0 flex-col gap-2 border-t border-slate-200 p-4 dark:border-slate-800 sm:flex-row sm:p-5">
              {stepActions.map((action) => (
                <button
                  key={action.id}
                  type="button"
                  disabled={action.disabled}
                  onClick={action.onClick}
                  className={`w-full sm:flex-1 ${
                    action.primary ? hmiModuleBtnPrimary : hmiModuleBtnSecondary
                  }`}
                >
                  {action.label}
                </button>
              ))}
            </div>
          )}
        </div>
      </div>
    </div>
  );
};
