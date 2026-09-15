import React, { createContext, useContext, useState, useEffect } from 'react';
import { translations, Language, TranslationKey } from '../i18n/translations';
import { unlockDebugMode as apiUnlockDebug } from '../api/hmiApi';

interface AppContextType {
  language: Language;
  setLanguage: (lang: Language) => void;
  isDarkMode: boolean;
  setIsDarkMode: (dark: boolean) => void;
  toggleDarkMode: () => void;
  showLogs: boolean;
  setShowLogs: (show: boolean) => void;
  debugMode: boolean;
  enableDebugMode: (password: string) => Promise<'ok' | 'invalid' | 'server'>;
  disableDebugMode: () => void;
  isSettingsOpen: boolean;
  setIsSettingsOpen: (open: boolean) => void;
  t: (key: TranslationKey, params?: Record<string, string | number>) => string;
}

const AppContext = createContext<AppContextType | undefined>(undefined);

const DEBUG_MODE_KEY = 'tcm_hmi_debug_mode';

export const AppProvider: React.FC<{ children: React.ReactNode }> = ({ children }) => {
  const [language, setLanguageState] = useState<Language>(() => {
    try {
      const saved = localStorage.getItem('tcm_hmi_lang');
      if (saved === 'en' || saved === 'es') return saved;
    } catch {
      // fallback
    }
    return 'es';
  });

  const [isDarkMode, setIsDarkModeState] = useState<boolean>(() => {
    try {
      const saved = localStorage.getItem('tcm_hmi_theme');
      if (saved === 'dark') return true;
      if (saved === 'light') return false;
      return window.matchMedia('(prefers-color-scheme: dark)').matches;
    } catch {
      return false;
    }
  });

  const [isSettingsOpen, setIsSettingsOpen] = useState<boolean>(false);

  const [showLogs, setShowLogsState] = useState<boolean>(() => {
    try {
      // Solo aplica si ya hay sesión debug; producción nunca muestra logs
      if (sessionStorage.getItem(DEBUG_MODE_KEY) !== 'true') return false;
      const saved = localStorage.getItem('tcm_hmi_show_logs');
      if (saved === 'true') return true;
    } catch {
      // fallback
    }
    return false;
  });

  const [debugMode, setDebugModeState] = useState<boolean>(() => {
    try {
      return sessionStorage.getItem(DEBUG_MODE_KEY) === 'true';
    } catch {
      return false;
    }
  });

  useEffect(() => {
    try {
      localStorage.setItem('tcm_hmi_lang', language);
    } catch {
      // ignore
    }
  }, [language]);

  useEffect(() => {
    try {
      localStorage.setItem('tcm_hmi_theme', isDarkMode ? 'dark' : 'light');
    } catch {
      // ignore
    }
    if (isDarkMode) {
      document.documentElement.classList.add('dark');
    } else {
      document.documentElement.classList.remove('dark');
    }
  }, [isDarkMode]);

  useEffect(() => {
    try {
      localStorage.setItem('tcm_hmi_show_logs', showLogs ? 'true' : 'false');
    } catch {
      // ignore
    }
  }, [showLogs]);

  useEffect(() => {
    try {
      if (debugMode) sessionStorage.setItem(DEBUG_MODE_KEY, 'true');
      else {
        sessionStorage.removeItem(DEBUG_MODE_KEY);
        setShowLogsState(false);
      }
    } catch {
      // ignore
    }
  }, [debugMode]);

  const setLanguage = (lang: Language) => {
    setLanguageState(lang);
  };

  const setIsDarkMode = (dark: boolean) => {
    setIsDarkModeState(dark);
  };

  const toggleDarkMode = () => {
    setIsDarkModeState((prev) => !prev);
  };

  const setShowLogs = (show: boolean) => {
    // Logs solo en debug mode
    setShowLogsState(debugMode ? show : false);
  };

  const enableDebugMode = async (
    password: string
  ): Promise<'ok' | 'invalid' | 'server'> => {
    try {
      const res = await apiUnlockDebug(password);
      if (res.error === 'server_unavailable') return 'server';
      if (res.ok) {
        setDebugModeState(true);
        return 'ok';
      }
      return 'invalid';
    } catch {
      return 'server';
    }
  };

  const disableDebugMode = () => {
    setShowLogsState(false);
    setDebugModeState(false);
  };

  const t = (key: TranslationKey, params?: Record<string, string | number>): string => {
    let text = translations[language][key] || translations['es'][key] || key;
    if (params) {
      Object.entries(params).forEach(([k, v]) => {
        text = text.replace(new RegExp(`\\{${k}\\}`, 'g'), String(v));
      });
    }
    return text;
  };

  return (
    <AppContext.Provider
      value={{
        language,
        setLanguage,
        isDarkMode,
        setIsDarkMode,
        toggleDarkMode,
        showLogs,
        setShowLogs,
        debugMode,
        enableDebugMode,
        disableDebugMode,
        isSettingsOpen,
        setIsSettingsOpen,
        t,
      }}
    >
      {children}
    </AppContext.Provider>
  );
};

export const useApp = (): AppContextType => {
  const context = useContext(AppContext);
  if (!context) {
    throw new Error('useApp must be used within an AppProvider');
  }
  return context;
};
