import React, { useState } from 'react';
import { Lock, RotateCcw, X } from 'lucide-react';
import { useApp } from '../context/AppContext';
import { resetMaintenanceCycleCounter } from '../api/hmiApi';
import {
  hmiAssistHint,
  hmiBtnPrimary,
  hmiBtnSecondary,
  hmiFieldInput,
  hmiFieldLabel,
  hmiIconBtn,
  hmiPanelCls,
} from '../styles/hmiUi';

interface MaintenanceCycleResetModalProps {
  isOpen: boolean;
  onClose: () => void;
}

export const MaintenanceCycleResetModal: React.FC<MaintenanceCycleResetModalProps> = ({
  isOpen,
  onClose,
}) => {
  const { t } = useApp();
  const [employeeId, setEmployeeId] = useState('');
  const [password, setPassword] = useState('');
  const [error, setError] = useState<'invalid_employee' | 'invalid_password' | 'server' | null>(
    null
  );
  const [submitting, setSubmitting] = useState(false);

  if (!isOpen) return null;

  const handleReset = async () => {
    setSubmitting(true);
    setError(null);
    try {
      const result = await resetMaintenanceCycleCounter(employeeId, password);
      if (result.ok) {
        setEmployeeId('');
        setPassword('');
        onClose();
        return;
      }
      if (result.error === 'invalid_employee') {
        setError('invalid_employee');
      } else if (result.error === 'invalid_password') {
        setError('invalid_password');
      } else {
        setError('server');
      }
    } catch {
      setError('server');
    } finally {
      setSubmitting(false);
    }
  };

  const handleClose = () => {
    setEmployeeId('');
    setPassword('');
    setError(null);
    onClose();
  };

  const canSubmit =
    employeeId.length === 5 && password.length > 0 && !submitting;

  return (
    <div className="fixed inset-0 z-[60] flex items-center justify-center p-4 animate-fade-in">
      <div
        className="fixed inset-0 bg-slate-900/50 backdrop-blur-xs"
        onClick={handleClose}
      />
      <div
        role="dialog"
        aria-modal="true"
        aria-labelledby="maintenance-reset-title"
        className={`relative z-10 w-full max-w-md p-4 shadow-2xl sm:p-5 ${hmiPanelCls}`}
      >
        <div className="mb-4 flex items-start justify-between gap-3">
          <div className="flex items-center gap-2.5">
            <div className="flex h-10 w-10 items-center justify-center rounded-lg border border-amber-200 bg-amber-100 text-amber-700 dark:border-amber-800 dark:bg-amber-950/50 dark:text-amber-300">
              <RotateCcw className="h-5 w-5" />
            </div>
            <div>
              <h2
                id="maintenance-reset-title"
                className="text-base font-bold text-slate-900 dark:text-white sm:text-lg"
              >
                {t('maintenance_reset_title')}
              </h2>
              <p className={`mt-0.5 ${hmiAssistHint}`}>{t('maintenance_reset_hint')}</p>
            </div>
          </div>
          <button
            type="button"
            onClick={handleClose}
            className="rounded-lg p-1.5 text-slate-400 hover:bg-slate-100 dark:hover:bg-slate-800"
            title={t('close')}
          >
            <X className="h-5 w-5" />
          </button>
        </div>

        <div className="space-y-3">
          <div>
            <label htmlFor="input-maintenance-employee" className={hmiFieldLabel}>
              {t('maintenance_employee_label')}
            </label>
            <input
              type="text"
              inputMode="numeric"
              pattern="[0-9]*"
              maxLength={5}
              id="input-maintenance-employee"
              autoFocus
              value={employeeId}
              onChange={(e) => {
                setEmployeeId(e.target.value.replace(/\D/g, '').slice(0, 5));
                setError(null);
              }}
              onKeyDown={(e) => {
                if (e.key === 'Enter' && canSubmit) void handleReset();
                if (e.key === 'Escape') handleClose();
              }}
              placeholder={t('maintenance_employee_placeholder')}
              className={`${hmiFieldInput} mt-1.5 font-mono tracking-widest ${
                error === 'invalid_employee'
                  ? 'border-red-400 focus:ring-red-400'
                  : 'focus:border-sky-500 focus:ring-sky-400'
              }`}
            />
          </div>

          <div>
            <label htmlFor="input-maintenance-password" className={hmiFieldLabel}>
              {t('maintenance_password_label')}
            </label>
            <div className="relative mt-1.5">
              <Lock className="pointer-events-none absolute left-3 top-1/2 h-4 w-4 -translate-y-1/2 text-slate-400" />
              <input
                type="password"
                id="input-maintenance-password"
                value={password}
                onChange={(e) => {
                  setPassword(e.target.value);
                  setError(null);
                }}
                onKeyDown={(e) => {
                  if (e.key === 'Enter' && canSubmit) void handleReset();
                  if (e.key === 'Escape') handleClose();
                }}
                placeholder={t('debug_mode_password_placeholder')}
                className={`${hmiFieldInput} pl-9 font-mono ${
                  error === 'invalid_password'
                    ? 'border-red-400 focus:ring-red-400'
                    : 'focus:border-sky-500 focus:ring-sky-400'
                }`}
              />
            </div>
          </div>
        </div>

        {error && (
          <p className="mt-3 text-sm font-semibold text-red-600 dark:text-red-400">
            {error === 'invalid_employee'
              ? t('maintenance_invalid_employee')
              : error === 'invalid_password'
                ? t('debug_mode_wrong_password')
                : t('debug_mode_server_error')}
          </p>
        )}

        <div className="mt-4 flex gap-2">
          <button
            type="button"
            onClick={handleClose}
            className={`${hmiBtnSecondary} flex-1`}
          >
            {t('debug_mode_cancel')}
          </button>
          <button
            type="button"
            id="btn-maintenance-reset-confirm"
            disabled={!canSubmit}
            onClick={() => void handleReset()}
            className={`${hmiBtnPrimary} flex-1 border-amber-600 bg-amber-600 hover:bg-amber-700`}
          >
            {t('maintenance_reset_confirm')}
          </button>
        </div>
      </div>
    </div>
  );
};
