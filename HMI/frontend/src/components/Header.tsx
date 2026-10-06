import React, { useEffect, useState } from 'react';
import { useApp } from '../context/AppContext';

function formatHeaderClock(date: Date): { dateLine: string; timeLine: string } {
  const dateLine = date.toLocaleDateString('es-ES', {
    day: '2-digit',
    month: 'short',
    year: 'numeric',
  });
  const timeLine = date.toLocaleTimeString('es-ES', {
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
    hour12: false,
  });
  return { dateLine, timeLine };
}

export const Header: React.FC = () => {
  const { t } = useApp();
  const [now, setNow] = useState(() => new Date());

  useEffect(() => {
    const id = window.setInterval(() => setNow(new Date()), 1000);
    return () => window.clearInterval(id);
  }, []);

  const { dateLine, timeLine } = formatHeaderClock(now);

  return (
    <header className="border-b border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 text-slate-900 dark:text-slate-100 shadow-2xs transition-colors">
      <div className="mx-auto flex w-full shrink-0 items-center justify-between px-4 py-3 sm:px-6 sm:py-4">
        <h1 className="flex min-w-0 flex-wrap items-baseline gap-x-2 gap-y-0.5 text-2xl font-bold tracking-tight sm:text-3xl">
          <span className="text-blue-600 dark:text-blue-400">{t('header_brand')}</span>
          <span className="font-normal text-slate-400 dark:text-slate-500">|</span>
          <span className="text-slate-900 dark:text-white">{t('header_product')}</span>
        </h1>

        <div className="shrink-0 text-right leading-tight">
          <p className="text-lg font-semibold text-slate-800 dark:text-slate-100 sm:text-xl">
            {dateLine}
          </p>
          <p className="font-mono text-base font-medium text-slate-600 dark:text-slate-300 sm:text-lg">
            {timeLine}
          </p>
        </div>
      </div>
    </header>
  );
};
