import React, { useState, useEffect, useCallback } from 'react';
import { REFILL_LONG_FEED_MM, TabType } from './types';
import { Header } from './components/Header';
import { Navigation } from './components/Navigation';
import { MaquinaTab } from './components/MaquinaTab';
import { CycleTab } from './components/CycleTab';
import { MotionTab } from './components/MotionTab';
import { PlcTab } from './components/PlcTab';
import { PreFeederTab } from './components/PreFeederTab';
import { AndonTab } from './components/AndonTab';
import { SettingsDrawer } from './components/SettingsDrawer';
import { ParametrosPasswordModal } from './components/ParametrosPasswordModal';
import { AppProvider, useApp } from './context/AppContext';
import { useHmiState } from './hooks/useHmiState';

type ViewMode = 'maquina' | 'parametros';

function AppMain() {
  const { isSettingsOpen, setIsSettingsOpen, showLogs, debugMode, disableDebugMode } = useApp();
  const logsVisible = debugMode && showLogs;
  const [viewMode, setViewMode] = useState<ViewMode>('maquina');
  const [currentTab, setCurrentTab] = useState<TabType>('maquina');
  const [showParamPassword, setShowParamPassword] = useState(false);
  const [reconnecting, setReconnecting] = useState(false);
  const hmi = useHmiState();
  const { view } = hmi;
  const {
    onTabChange,
    reconnectNetwork,
    setCycleStepByStep,
    stop,
    start,
    resume,
    motionStop,
    pfStop,
  } = hmi;

  const handleReconnectNetwork = useCallback(async () => {
    setReconnecting(true);
    try {
      await reconnectNetwork();
    } finally {
      setReconnecting(false);
    }
  }, [reconnectNetwork]);

  const enterParametrosView = useCallback(
    (initialTab: TabType = 'cycle') => {
      setViewMode('parametros');
      setCurrentTab(initialTab);
      onTabChange(initialTab);
    },
    [onTabChange]
  );

  const handleTabChange = useCallback(
    (tab: TabType) => {
      if (tab === 'maquina') {
        setViewMode('maquina');
        setCurrentTab('maquina');
        onTabChange('maquina');
        return;
      }
      if (!debugMode) return;
      setViewMode('parametros');
      setCurrentTab(tab);
      onTabChange(tab);
    },
    [onTabChange, debugMode]
  );

  const handleOpenParametros = useCallback(() => {
    if (debugMode) {
      enterParametrosView(currentTab === 'maquina' ? 'cycle' : currentTab);
    } else {
      setShowParamPassword(true);
    }
  }, [debugMode, enterParametrosView, currentTab]);

  const handleBackToMaquina = useCallback(() => {
    setViewMode('maquina');
    setCurrentTab('maquina');
    onTabChange('maquina');
  }, [onTabChange]);

  const handleDebugModeDisable = useCallback(() => {
    if (view.machineState.stepByStep) {
      void setCycleStepByStep(false);
    }
    setViewMode('maquina');
    setCurrentTab('maquina');
    onTabChange('maquina');
  }, [onTabChange, setCycleStepByStep, view.machineState.stepByStep]);

  const handleExitParametros = useCallback(() => {
    disableDebugMode();
    handleDebugModeDisable();
  }, [disableDebugMode, handleDebugModeDisable]);

  useEffect(() => {
    if (!debugMode) {
      setViewMode('maquina');
      if (currentTab !== 'maquina') {
        setCurrentTab('maquina');
        onTabChange('maquina');
      }
    }
  }, [debugMode, currentTab, onTabChange]);

  useEffect(() => {
    const handleKeyDown = (e: KeyboardEvent) => {
      if (
        ['INPUT', 'SELECT', 'TEXTAREA'].includes(
          (e.target as HTMLElement)?.tagName || ''
        )
      ) {
        return;
      }

      if (e.key === '1') handleTabChange('maquina');
      else if (debugMode && e.key === '2') handleTabChange('cycle');
      else if (debugMode && e.key === '3') handleTabChange('motion');
      else if (debugMode && e.key === '4') handleTabChange('plc');
      else if (debugMode && e.key === '5') handleTabChange('prefeeder');
      else if (debugMode && e.key === '6') handleTabChange('andon');
      else if (e.code === 'Space') {
        e.preventDefault();
        if (viewMode === 'maquina') {
          if (view.machineState.isRunning) stop();
          else if (
            !view.machineState.errorActive &&
            !view.machineState.fault &&
            !view.machineState.workBlocked &&
            !view.machineState.refillActive &&
            !view.machineState.purgeHandsWarning &&
            !view.machineState.asdaMoveWarning
          ) {
            if (view.resumeEnabled) resume();
            else start();
          }
        }
      } else if (e.key === 'Escape') {
        if (isSettingsOpen) setIsSettingsOpen(false);
        else if (showParamPassword) setShowParamPassword(false);
        else if (viewMode === 'parametros') handleBackToMaquina();
        else {
          stop();
          motionStop();
          pfStop();
        }
      }
    };

    window.addEventListener('keydown', handleKeyDown);
    return () => window.removeEventListener('keydown', handleKeyDown);
  }, [
    viewMode,
    view.machineState.isRunning,
    view.machineState.errorActive,
    view.machineState.fault,
    view.machineState.workBlocked,
    view.machineState.refillActive,
    view.machineState.purgeHandsWarning,
    view.machineState.asdaMoveWarning,
    view.resumeEnabled,
    isSettingsOpen,
    showParamPassword,
    setIsSettingsOpen,
    handleTabChange,
    handleBackToMaquina,
    debugMode,
    stop,
    start,
    resume,
    motionStop,
    pfStop,
  ]);

  const showMaquina = viewMode === 'maquina';
  const showParametros = viewMode === 'parametros' && debugMode;

  return (
    <div className="flex h-screen flex-col overflow-hidden bg-slate-50 dark:bg-slate-950 text-slate-900 dark:text-slate-100 font-sans antialiased selection:bg-slate-800 dark:selection:bg-slate-200 selection:text-white dark:selection:text-slate-900 transition-colors">
      <Header
        onOpenSettings={() => setIsSettingsOpen(true)}
        onOpenParametros={handleOpenParametros}
        parametrosActive={showParametros}
      />

      {showParametros && (
        <Navigation
          currentTab={currentTab}
          onSelectTab={handleTabChange}
          motionConn={view.motionState.connection}
          plcConn={view.plcState.connection}
          preFeederConn={view.preFeederState.connection}
          andonConn={view.andonConn}
          hasErrors={{
            motion:
              view.motionState.hasError ||
              (!!view.machineState.errorActive &&
                (view.machineState.faultModule || '').toLowerCase().includes('motion')),
            plc:
              view.plcState.hasError ||
              (!!view.machineState.errorActive &&
                (view.machineState.faultModule || '').toLowerCase().includes('plc')),
            prefeeder:
              view.preFeederState.hasError ||
              (!!view.machineState.errorActive &&
                /pre-?feeder|\bpf\b/.test(
                  (view.machineState.faultModule || '').toLowerCase()
                )),
          }}
          onBackToMaquina={handleBackToMaquina}
          onExitParametros={handleExitParametros}
        />
      )}

      <main className="flex min-h-0 flex-1 flex-col px-2 py-2 sm:px-3 w-full overflow-hidden">
        {showMaquina && (
          <div className="min-h-0 flex-1">
          <MaquinaTab
            machineState={view.machineState}
            motionState={view.motionState}
            plcState={view.plcState}
            preFeederState={view.preFeederState}
            models={view.models}
            selectedModelIndex={view.selectedModelIndex}
            feedSides={view.cycleConfig?.feedSides === 'L' || view.cycleConfig?.feedSides === 'R' || view.cycleConfig?.feedSides === 'LR' ? view.cycleConfig.feedSides : 'LR'}
            resumeEnabled={view.resumeEnabled}
            onModelSelect={hmi.selectModel}
            onTargetPiecesChange={hmi.setTargetQty}
            onCutOffsetSave={(mm) => {
              void hmi.setCutOffset(mm);
            }}
            onStart={hmi.start}
            onStop={hmi.stop}
            onResume={hmi.resume}
            onPause={hmi.pauseCycle}
            onReset={hmi.resetCycleCmd}
            onMachineHome={hmi.machineHomeCmd}
            onRefill={() => {
              void hmi.startRefill();
            }}
            onRefillConfirm={(ok) => {
              void hmi.confirmRefill(ok);
            }}
            onRefillRetry={() => {
              void hmi.retryRefill();
            }}
            onRefillLongFeed={() => {
              void hmi.retryRefill(REFILL_LONG_FEED_MM);
            }}
            onRecoveryReview={(ok) => {
              void hmi.confirmRecoveryReview(ok);
            }}
            onPfStart={hmi.pfStart}
            onPfStop={hmi.pfStop}
            onPfReset={hmi.pfReset}
            onPfRefill={hmi.pfRefill}
            onMaterialist={hmi.toggleCycleMaterialist}
            purgeHandsWarning={view.machineState.purgeHandsWarning}
            asdaMoveWarning={view.machineState.asdaMoveWarning}
            showLogs={logsVisible}
            logs={logsVisible ? hmi.filterLogs('ALL') : []}
            onClearLogs={() => hmi.clearLogs('all')}
          />
          </div>
        )}

        {showParametros && currentTab === 'cycle' && (
          <CycleTab
            machineState={view.machineState}
            cycleConfig={view.cycleConfig}
            cycleStep={view.cycleStep}
            cycleActive={view.cycleActive}
            cycleFlow={view.cycleFlow}
            onSaveConfig={hmi.saveCycleConfig}
            onReloadConfig={hmi.reloadCycleConfig}
            onPause={hmi.pauseCycle}
            onReset={hmi.resetCycleCmd}
            onMaterialist={hmi.toggleCycleMaterialist}
            onSetStepByStep={hmi.setCycleStepByStep}
            onStart={hmi.start}
            onResume={hmi.resume}
            resumeEnabled={view.resumeEnabled}
            onRefill={() => {
              void hmi.startRefill();
            }}
            onRefillConfirm={(ok) => {
              void hmi.confirmRefill(ok);
            }}
            onRefillRetry={() => {
              void hmi.retryRefill();
            }}
            onRefillLongFeed={() => {
              void hmi.retryRefill(REFILL_LONG_FEED_MM);
            }}
            onRecoveryReview={(ok) => {
              void hmi.confirmRecoveryReview(ok);
            }}
          />
        )}

        {showParametros && currentTab === 'motion' && (
          <MotionTab
            motionState={view.motionState}
            onUpdateTargetPos={(pos) => hmi.setMmRpm(pos, view.motionState.rpm)}
            onUpdateRpm={(rpm) => hmi.setMmRpm(view.motionState.targetPositionMm, rpm)}
            onUpdateOffsetL={() => {}}
            onUpdateOffsetR={() => {}}
            onMover={hmi.motionMove}
            onStop={hmi.motionStop}
            onServoOn={hmi.motionServoOn}
            onServoOff={hmi.motionServoOff}
            onSearchHome={hmi.motionSearchHome}
            onMoveToZero={hmi.motionMoveZero}
            onResetErrors={hmi.motionReset}
            onSetZeroR={hmi.encSetZeroR}
            onSetZeroL={hmi.encSetZeroL}
            onTestCanL={hmi.feedL}
            onTestCanR={hmi.feedR}
            onSaveFeedOffset={(l, r) => hmi.saveFeedOffset(l, r)}
            onReloadFeedOffset={() => {
              void hmi.reloadFeedOffset();
            }}
            safetyExhaust={view.machineState.safetyExhaust}
            onReconnectNetwork={handleReconnectNetwork}
            reconnecting={reconnecting}
            showLogs={logsVisible}
            logs={logsVisible ? hmi.filterLogs('MOTION') : []}
            onClearLogs={() => hmi.clearLogs('motion')}
          />
        )}

        {showParametros && currentTab === 'plc' && (
          <PlcTab
            plcState={view.plcState}
            onToggleValve={hmi.toggleValve}
            valveBusy={hmi.valveBusy}
            onBlowerSecChange={hmi.setBlowerSec}
            onResetPlc={hmi.plcReset}
            onAllOff={hmi.plcAllOff}
            onReconnectNetwork={handleReconnectNetwork}
            reconnecting={reconnecting}
            showLogs={logsVisible}
            logs={logsVisible ? hmi.filterLogs('PLC') : []}
            onClearLogs={() => hmi.clearLogs('plc')}
          />
        )}

        {showParametros && currentTab === 'prefeeder' && (
          <PreFeederTab
            preFeederState={view.preFeederState}
            cycleMaterialist={view.machineState.cycleMaterialist}
            cycleBusy={view.machineState.cycleBusy}
            onStart={hmi.pfStart}
            onStop={hmi.pfStop}
            onReset={hmi.pfReset}
            onMaterialist={hmi.pfMaterialist}
            onBusy={hmi.toggleCycleBusy}
            onTriggerR={hmi.pfTriggerR}
            onTriggerL={hmi.pfTriggerL}
            onReconnectNetwork={handleReconnectNetwork}
            reconnecting={reconnecting}
            showLogs={logsVisible}
            logs={logsVisible ? hmi.filterLogs('PREFEEDER') : []}
            onClearLogs={() => hmi.clearLogs('prefeeder')}
          />
        )}

        {showParametros && currentTab === 'andon' && (
          <AndonTab
            andonState={view.andonState}
            machineByte={view.andonState.machineByte ?? view.machineState.machineByte}
            machineName={view.machineState.machineName}
            buzzerMute={view.andonBuzzerMute}
            onBuzzerMute={hmi.setAndonBuzzerMute}
            onSetOut={hmi.andonSetOut}
            onAllOff={hmi.andonAllOff}
            onResumeAuto={hmi.andonResumeAuto}
            onMachineState={hmi.andonMachineState}
            onReconnectNetwork={handleReconnectNetwork}
            reconnecting={reconnecting}
            showLogs={logsVisible}
            logs={logsVisible ? hmi.filterLogs('ANDON') : []}
            onClearLogs={() => hmi.clearLogs('andon')}
          />
        )}
      </main>

      <SettingsDrawer
        isOpen={isSettingsOpen}
        onClose={() => setIsSettingsOpen(false)}
        connected={view.connected}
      />

      <ParametrosPasswordModal
        isOpen={showParamPassword}
        onClose={() => setShowParamPassword(false)}
        onUnlocked={() => {
          setShowParamPassword(false);
          enterParametrosView('cycle');
        }}
      />
    </div>
  );
}

export default function App() {
  return (
    <AppProvider>
      <AppMain />
    </AppProvider>
  );
}
