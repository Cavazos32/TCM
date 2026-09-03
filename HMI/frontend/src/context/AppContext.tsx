import React, { createContext, useContext, useState, useEffect } from 'react';
import { translations, Language, TranslationKey } from '../i18n/translations';

interface AppContextType {
  language: Language;
  setLanguage: (lang: Language) => void;
  isDarkMode: boolean;
  setIsDarkMode: (dark: boolean) => void;
  toggleDarkMode: () => void;
  showLogs: boolean;
  setShowLogs: (show: boolean) => void;
  isSettingsOpen: boolean;
  setIsSettingsOpen: (open: boolean) => void;
  t: (key: TranslationKey, params?: Record<string, string | number>) => string;
}

const AppContext = createContext<AppContextType | undefined>(undefined);

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
      const saved = localStorage.getItem('tcm_hmi_show_logs');
      if (saved === 'false') return false;
      if (saved === 'true') return true;
    } catch {
      // fallback
    }
    return true;
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
    setShowLogsState(show);
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
