import React, { useState } from 'react';
import { Lock, X } from 'lucide-react';
import { useApp } from '../context/AppContext';

interface ParametrosPasswordModalProps {
  isOpen: boolean;
  onClose: () => void;
  onUnlocked: () => void;
}

export const ParametrosPasswordModal: React.FC<ParametrosPasswordModalProps> = ({
  isOpen,
  onClose,
  onUnlocked,
}) => {
  const { enableDebugMode, t } = useApp();
  const [password, setPassword] = useState('');
  const [passwordError, setPasswordError] = useState<'invalid' | 'server' | null>(null);
  const [unlocking, setUnlocking] = useState(false);

  if (!isOpen) return null;

  const handleUnlock = async () => {
    setUnlocking(true);
    setPasswordError(null);
    try {
      const result = await enableDebugMode(password);
      if (result === 'ok') {
        setPassword('');
        onUnlocked();
      } else {
        setPasswordError(result);
      }
    } finally {
      setUnlocking(false);
    }
  };

  const handleClose = () => {
    setPassword('');
    setPasswordError(null);
    onClose();
  };

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center p-4 animate-fade-in">
      <div
        className="fixed inset-0 bg-slate-900/50 backdrop-blur-xs"
        onClick={handleClose}
      />
      <div
        role="dialog"
        aria-modal="true"
        aria-labelledby="parametros-password-title"
        className="relative z-10 w-full max-w-md rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 p-5 shadow-2xl"
      >
        <div className="flex items-start justify-between gap-3 mb-4">
          <div className="flex items-center gap-2.5">
            <div className="flex h-10 w-10 items-center justify-center rounded-lg bg-sky-100 dark:bg-sky-950/50 text-sky-700 dark:text-sky-300 border border-sky-200 dark:border-sky-800">
              <Lock className="h-5 w-5" />
            </div>
            <div>
              <h2
                id="parametros-password-title"
                className="text-sm font-bold text-slate-900 dark:text-white"
              >
                {t('parametros_password_title')}
              </h2>
              <p className="text-xs text-slate-500 dark:text-slate-400 mt-0.5">
                {t('parametros_password_hint')}
              </p>
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

        <input
          type="password"
          id="input-parametros-password"
          autoFocus
          value={password}
          onChange={(e) => {
            setPassword(e.target.value);
            setPasswordError(null);
          }}
          onKeyDown={(e) => {
            if (e.key === 'Enter') void handleUnlock();
            if (e.key === 'Escape') handleClose();
          }}
          placeholder={t('debug_mode_password_placeholder')}
          className={`w-full rounded-lg border px-3 py-2.5 text-sm font-mono bg-white dark:bg-slate-900 ${
            passwordError
              ? 'border-red-400 focus:ring-red-400'
              : 'border-slate-300 dark:border-slate-700'
          }`}
        />
        {passwordError && (
          <p className="mt-2 text-xs font-semibold text-red-600 dark:text-red-400">
            {passwordError === 'server'
              ? t('debug_mode_server_error')
              : t('debug_mode_wrong_password')}
          </p>
        )}
        <div className="mt-4 flex gap-2">
          <button
            type="button"
            onClick={handleClose}
            className="flex-1 rounded-lg border border-slate-300 dark:border-slate-700 px-3 py-2.5 text-sm font-bold text-slate-600 dark:text-slate-300"
          >
            {t('debug_mode_cancel')}
          </button>
          <button
            type="button"
            id="btn-parametros-unlock"
            disabled={unlocking || !password}
            onClick={() => void handleUnlock()}
            className="flex-1 rounded-lg bg-sky-700 hover:bg-sky-800 disabled:opacity-40 px-3 py-2.5 text-sm font-bold text-white"
          >
            {t('debug_mode_unlock')}
          </button>
        </div>
      </div>
    </div>
  );
};
