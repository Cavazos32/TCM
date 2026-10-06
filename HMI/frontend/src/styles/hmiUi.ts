/** Tokens visuales alineados con la pantalla Máquina (MaquinaTab). */

export const hmiPanelCls =
  'rounded-xl border border-slate-200 dark:border-slate-800 bg-white dark:bg-slate-900 shadow-2xs';

export const hmiSectionTitle =
  'text-xs font-bold uppercase tracking-wider text-slate-600 dark:text-slate-400 shrink-0 sm:text-sm';

export const hmiSectionTitleMb =
  'text-xs font-bold uppercase tracking-wider text-slate-600 dark:text-slate-400 mb-2 shrink-0 sm:text-sm';

export const hmiCenterSectionTitle =
  'text-base font-bold uppercase tracking-wider text-slate-800 dark:text-slate-200 shrink-0 sm:text-lg';

export const hmiFieldLabel =
  'block truncate text-sm font-semibold text-slate-700 dark:text-slate-300 sm:text-base';

export const hmiFieldInput =
  'w-full rounded-md border border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 px-3 py-2.5 text-base font-medium text-slate-900 dark:text-slate-100 shadow-2xs focus:outline-none focus:ring-1 focus:border-sky-500 focus:ring-sky-400 sm:text-lg sm:py-3';

export const hmiCompactFieldInput =
  'w-full rounded-md border border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 px-2.5 py-2 font-mono text-sm font-semibold text-slate-900 dark:text-slate-100 shadow-2xs focus:outline-none focus:ring-1 focus:border-sky-500 focus:ring-sky-400 sm:px-3 sm:py-2.5 sm:text-base';

export const hmiAssistTitle = 'text-base font-bold sm:text-lg';
export const hmiAssistHint = 'text-sm leading-snug text-slate-600 dark:text-slate-400 sm:text-base';

export const hmiStatusBar =
  'rounded-xl border border-slate-200 dark:border-slate-800 bg-gradient-to-r from-white to-slate-50 dark:from-slate-900 dark:to-slate-900/80 px-5 py-4 text-slate-800 dark:text-slate-200 shadow-2xs flex flex-wrap items-center justify-between gap-4 min-h-[5rem] sm:min-h-[5.5rem] sm:px-6 sm:py-5';

export const hmiStatusLed = 'h-4 w-4 shrink-0 rounded-full';
export const hmiStatusHeadline = 'text-xl font-bold tracking-tight sm:text-2xl';
export const hmiStatusMeta =
  'flex flex-wrap items-center gap-x-2 gap-y-1 text-base font-mono text-slate-600 dark:text-slate-400 sm:text-lg';
export const hmiStatusMetaLabel = 'font-sans font-semibold text-slate-500 dark:text-slate-500';
export const hmiStatusChip =
  'inline-flex items-center font-bold rounded-lg border px-3.5 py-1.5 text-base sm:px-4 sm:py-2 sm:text-lg';
export const hmiStatusDivider = 'hidden sm:inline text-slate-300 dark:text-slate-700 select-none';

export const hmiPanelPadding = 'p-4 sm:p-5';
export const hmiPanelHeader =
  'flex flex-wrap items-center justify-between gap-3 border-b border-slate-100 dark:border-slate-800 pb-3 mb-4';
export const hmiSubPanelHeader =
  'flex flex-wrap items-center justify-between gap-3 border-b border-slate-100 dark:border-slate-800 pb-3 mb-3';
export const hmiAlertText =
  'text-sm font-medium text-red-700 dark:text-red-300 sm:text-base';

export const hmiIconSection = 'h-7 w-7 shrink-0';
export const hmiIconBtn = 'h-5 w-5 shrink-0';

export const hmiCfgGrid = 'grid grid-cols-2 sm:grid-cols-3 lg:grid-cols-4 gap-3';

export const hmiBadge =
  'rounded bg-slate-100 dark:bg-slate-800 border border-slate-200 dark:border-slate-700 px-2 py-0.5 font-mono text-xs text-slate-600 dark:text-slate-400 sm:text-sm';

/** Botonera Parámetros — una sola altura (2.75rem) y tipografía en todo el módulo técnico. */
export const hmiBtnBase =
  'inline-flex items-center justify-center gap-2 rounded-md border min-h-[2.75rem] px-3.5 py-2.5 text-sm font-bold sm:text-base shadow-2xs transition active:scale-[0.98] disabled:opacity-40 disabled:cursor-not-allowed whitespace-nowrap shrink-0';

/** @deprecated Usar hmiBtnBase o variantes hmiBtn* */
export const hmiActionBtn = hmiBtnBase;

export const hmiBtnPrimary =
  `${hmiBtnBase} border-teal-600 bg-teal-600 text-white hover:bg-teal-500`;
export const hmiBtnSecondary =
  `${hmiBtnBase} border-slate-300 bg-white text-slate-700 hover:bg-slate-50 dark:border-slate-700 dark:bg-slate-800 dark:text-slate-200 dark:hover:bg-slate-700`;
export const hmiBtnSuccess =
  `${hmiBtnBase} border-emerald-600 bg-emerald-600 text-white hover:bg-emerald-700`;
export const hmiBtnDanger =
  `${hmiBtnBase} border-red-600 bg-red-600 text-white hover:bg-red-700`;
export const hmiBtnDangerSoft =
  `${hmiBtnBase} border-red-200 bg-red-50 text-red-700 hover:bg-red-100 dark:border-red-900/60 dark:bg-red-950/40 dark:text-red-300 dark:hover:bg-red-950/70`;
export const hmiBtnWarning =
  `${hmiBtnBase} border-amber-500 bg-amber-500 text-white hover:bg-amber-600`;
export const hmiBtnTealSoft =
  `${hmiBtnBase} border-teal-300 bg-teal-50 text-teal-700 hover:bg-teal-100 dark:border-teal-800 dark:bg-teal-950/40 dark:text-teal-300 dark:hover:bg-teal-900/60`;
export const hmiBtnDark =
  `${hmiBtnBase} border-slate-900 bg-slate-900 text-white hover:bg-slate-800 dark:border-white dark:bg-slate-100 dark:text-slate-900 dark:hover:bg-white`;
export const hmiBtnSuccessSoft =
  `${hmiBtnBase} border-emerald-200 bg-emerald-50 text-emerald-800 hover:bg-emerald-100 dark:border-emerald-900/60 dark:bg-emerald-950/40 dark:text-emerald-300 dark:hover:bg-emerald-950/70`;

export const hmiBtnGroup = 'flex flex-wrap items-center gap-2';

export const hmiBtnSegWrap =
  'inline-flex items-center rounded-md border border-slate-200 bg-slate-100 p-0.5 dark:border-slate-700 dark:bg-slate-800';
export const hmiBtnSeg =
  'inline-flex min-h-[2.75rem] items-center justify-center rounded-md border border-transparent px-3.5 py-2.5 text-sm font-bold transition sm:text-base';
export const hmiBtnSegActive =
  'border-slate-200 bg-white text-slate-900 shadow-2xs dark:border-slate-600 dark:bg-slate-700 dark:text-white';
export const hmiBtnSegIdle =
  'text-slate-600 hover:text-slate-900 dark:text-slate-400 dark:hover:text-white';

export const hmiBtnChip =
  'inline-flex h-[2.75rem] min-w-[2.75rem] items-center justify-center rounded-md border px-3 text-sm font-mono font-semibold sm:text-base';
export const hmiBtnChipOn =
  'border-slate-900 bg-slate-900 text-white dark:border-white dark:bg-slate-100 dark:text-slate-900';
export const hmiBtnChipOff =
  'border-slate-200 bg-slate-100 text-slate-700 dark:border-slate-700 dark:bg-slate-800 dark:text-slate-300';

export const hmiBtnTable = `${hmiBtnBase} min-w-[5.5rem] px-3 py-2 font-mono`;

/** Pestañas técnicas (Parámetros): botones ampliados, alineados con filas PLC. */
export const hmiModuleBtnBase =
  'inline-flex items-center justify-center gap-2.5 rounded-md border min-h-[3.5rem] px-5 py-3 text-lg font-bold sm:text-xl shadow-2xs transition active:scale-[0.98] disabled:opacity-40 disabled:cursor-not-allowed whitespace-nowrap shrink-0';

export const hmiModuleBtnSecondary =
  `${hmiModuleBtnBase} border-slate-300 bg-white text-slate-700 hover:bg-slate-50 dark:border-slate-700 dark:bg-slate-800 dark:text-slate-200 dark:hover:bg-slate-700`;
export const hmiModuleBtnPrimary =
  `${hmiModuleBtnBase} border-teal-600 bg-teal-600 text-white hover:bg-teal-500`;
export const hmiModuleBtnSuccess =
  `${hmiModuleBtnBase} border-emerald-600 bg-emerald-600 text-white hover:bg-emerald-700`;
export const hmiModuleBtnDanger =
  `${hmiModuleBtnBase} border-red-600 bg-red-600 text-white hover:bg-red-700`;
export const hmiModuleBtnDangerSoft =
  `${hmiModuleBtnBase} border-red-200 bg-red-50 text-red-700 hover:bg-red-100 dark:border-red-900/60 dark:bg-red-950/40 dark:text-red-300 dark:hover:bg-red-950/70`;
export const hmiModuleBtnWarning =
  `${hmiModuleBtnBase} border-amber-500 bg-amber-500 text-white hover:bg-amber-600`;
export const hmiModuleBtnDark =
  `${hmiModuleBtnBase} border-slate-900 bg-slate-900 text-white hover:bg-slate-800 dark:border-white dark:bg-slate-100 dark:text-slate-900 dark:hover:bg-white`;
export const hmiModuleBtnSuccessSoft =
  `${hmiModuleBtnBase} border-emerald-200 bg-emerald-50 text-emerald-800 hover:bg-emerald-100 dark:border-emerald-900/60 dark:bg-emerald-950/40 dark:text-emerald-300 dark:hover:bg-emerald-950/70`;
export const hmiModuleBtnValveExtra = 'min-w-[7rem] font-mono sm:min-w-[7.5rem]';
export const hmiModuleBtnGroup = 'flex flex-wrap items-center gap-3';

export const hmiModuleIconBtn = 'h-6 w-6 shrink-0';
export const hmiModuleIconSection = 'h-8 w-8 shrink-0';

export const hmiModuleCenterSectionTitle =
  'text-lg font-bold uppercase tracking-wider text-slate-800 dark:text-slate-200 shrink-0 sm:text-xl';
export const hmiModuleSectionTitle =
  'text-sm font-bold uppercase tracking-wider text-slate-600 dark:text-slate-400 shrink-0 sm:text-base';
export const hmiModuleBadge =
  'rounded bg-slate-100 dark:bg-slate-800 border border-slate-200 dark:border-slate-700 px-2.5 py-1 font-mono text-sm text-slate-600 dark:text-slate-400 sm:text-base';

export const hmiModuleFieldLabel =
  'block truncate text-base font-semibold text-slate-700 dark:text-slate-300 sm:text-lg';
export const hmiModuleCompactFieldInput =
  'w-full rounded-md border border-slate-300 dark:border-slate-700 bg-white dark:bg-slate-800 px-3 py-2.5 font-mono text-base font-semibold text-slate-900 dark:text-slate-100 shadow-2xs focus:outline-none focus:ring-1 focus:border-sky-500 focus:ring-sky-400 sm:px-3.5 sm:py-3 sm:text-lg';

export const hmiModuleBtnTealSoft =
  `${hmiModuleBtnBase} border-teal-300 bg-teal-50 text-teal-700 hover:bg-teal-100 dark:border-teal-800 dark:bg-teal-950/40 dark:text-teal-300 dark:hover:bg-teal-900/60`;
export const hmiModuleBtnChip =
  `${hmiModuleBtnBase} min-w-[3.5rem] min-h-[3.5rem] px-3 font-mono font-semibold`;
export const hmiModuleBtnChipOn =
  'border-slate-900 bg-slate-900 text-white dark:border-white dark:bg-slate-100 dark:text-slate-900';
export const hmiModuleBtnChipOff =
  'border-slate-200 bg-slate-100 text-slate-700 dark:border-slate-700 dark:bg-slate-800 dark:text-slate-300';
export const hmiModuleBtnSegWrap =
  'inline-flex items-center rounded-md border border-slate-200 bg-slate-100 p-1 dark:border-slate-700 dark:bg-slate-800';
export const hmiModuleBtnSeg =
  `${hmiModuleBtnBase} px-4 border-transparent`;
export const hmiModuleBtnSegActive =
  'border-slate-200 bg-white text-slate-900 shadow-2xs dark:border-slate-600 dark:bg-slate-700 dark:text-white';
export const hmiModuleBtnSegIdle =
  'text-slate-600 hover:text-slate-900 dark:text-slate-400 dark:hover:text-white';

export const hmiBtnToolbarWrap =
  'inline-flex items-center gap-1 rounded-md border border-teal-500/40 bg-teal-500/10 p-1 dark:bg-teal-950/60';

/** Alias de hmiBtnChip para grids de configuración */
export const hmiCfgChip = hmiBtnChip;

/** Lista de pasos del ciclo (CycleTab). */
export const hmiCycleStepRow =
  'group flex items-center justify-between rounded-lg px-4 py-2 transition-all sm:px-5 sm:py-2.5';
export const hmiCycleStepIcon =
  'flex h-9 w-9 shrink-0 items-center justify-center rounded-full text-base font-mono font-bold border sm:h-10 sm:w-10 sm:text-lg';
export const hmiCycleStepTitle = 'text-xl font-semibold sm:text-2xl';
export const hmiCycleStepNote = 'mt-1 text-base text-slate-500 dark:text-slate-400 sm:text-lg';
export const hmiCycleStepBadge =
  'rounded border px-2.5 py-1 font-mono text-sm font-bold uppercase sm:text-base';
export const hmiCycleTimeChip =
  'flex items-center gap-1.5 rounded-lg border px-3.5 py-2 font-mono text-base shadow-2xs sm:text-lg sm:px-4 sm:py-2.5';

/** Panel Indicaciones (MaquinaTab) — banners ocupan el alto disponible. */
export const hmiIndicationShell =
  'flex min-h-0 flex-1 flex-col justify-center gap-4 overflow-y-auto p-4 sm:gap-5 sm:p-6';
export const hmiIndicationBanner =
  'flex w-full flex-1 flex-wrap items-center justify-center gap-4 rounded-xl border px-5 py-5 sm:gap-5 sm:px-8 sm:py-7 min-h-[7rem]';
export const hmiIndicationAlert =
  'flex w-full flex-1 items-center justify-center px-4 py-6 text-center text-2xl font-black uppercase leading-tight tracking-wide sm:px-6 sm:py-8 sm:text-3xl lg:text-4xl';
export const hmiIndicationTitle =
  'text-xl font-bold leading-snug sm:text-2xl lg:text-3xl';
export const hmiIndicationHint =
  'mt-2 text-base leading-snug sm:text-lg lg:text-xl';
export const hmiIndicationIcon = 'h-10 w-10 shrink-0 sm:h-12 sm:w-12';
export const hmiIndicationBtn =
  `${hmiBtnBase} min-h-[3.25rem] shrink-0 px-5 text-base sm:min-h-[3.5rem] sm:px-6 sm:text-lg`;
export const hmiIndicationIdleIcon = 'h-12 w-12 shrink-0 text-sky-500 dark:text-sky-400 sm:h-14 sm:w-14';
export const hmiIndicationIdleTitle =
  'max-w-lg text-lg font-bold leading-snug text-slate-800 dark:text-slate-100 sm:text-xl lg:text-2xl';
export const hmiIndicationIdleHint =
  'max-w-lg text-base text-slate-600 dark:text-slate-300 sm:text-lg';
