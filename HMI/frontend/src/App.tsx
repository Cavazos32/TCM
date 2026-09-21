import React, { useState, useEffect, useCallback } from 'react';
import { TabType } from './types';
import { Header } from './components/Header';
import { Navigation } from './components/Navigation';
import { MaquinaTab } from './components/MaquinaTab';
import { CycleTab } from './components/CycleTab';
import { MotionTab } from './components/MotionTab';
import { PlcTab } from './components/PlcTab';
import { PreFeederTab } from './components/PreFeederTab';
import { AndonTab } from './components/AndonTab';
import { SettingsDrawer } from './components/SettingsDrawer';
import { AppProvider, useApp } from './context/AppContext';
import { useHmiState } from './hooks/useHmiState';

function AppMain() {
  const { isSettingsOpen, setIsSettingsOpen, showLogs, debugMode } = useApp();
  const logsVisible = debugMode && showLogs;
  const [currentTab, setCurrentTab] = useState<TabType>('maquina');
  const [reconnecting, setReconnecting] = useState(false);
  const hmi = useHmiState();
  const { view } = hmi;

  const handleReconnectNetwork = useCallback(async () => {
    setReconnecting(true);
    try {
      await hmi.reconnectNetwork();
    } finally {
      setReconnecting(false);
    }
  }, [hmi]);

  const handleTabChange = useCallback(
    (tab: TabType) => {
      if (!debugMode && tab !== 'maquina') return;
      setCurrentTab(tab);
      hmi.onTabChange(tab);
    },
    [hmi, debugMode]
  );

  const handleDebugModeDisable = useCallback(() => {
    if (view.machineState.trialMode) {
      void hmi.setCycleTrialMode(false);
    }
    if (view.machineState.stepByStep) {
      void hmi.setCycleStepByStep(false);
    }
    if (view.machineState.ignorePrefeeder) {
      void hmi.setCycleIgnorePrefeeder(false);
    }
    if (currentTab !== 'maquina') {
      setCurrentTab('maquina');
      hmi.onTabChange('maquina');
    }
  }, [
    currentTab,
    hmi,
    view.machineState.trialMode,
    view.machineState.stepByStep,
    view.machineState.ignorePrefeeder,
  ]);

  useEffect(() => {
    if (!debugMode && currentTab !== 'maquina') {
      setCurrentTab('maquina');
      hmi.onTabChange('maquina');
    }
  }, [debugMode, currentTab, hmi]);

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
        if (currentTab === 'maquina') {
          if (view.machineState.isRunning) hmi.stop();
          else hmi.start();
        }
      } else if (e.key === 'Escape') {
        if (isSettingsOpen) setIsSettingsOpen(false);
        else {
          hmi.stop();
          hmi.motionStop();
          hmi.pfStop();
        }
      }
    };

    window.addEventListener('keydown', handleKeyDown);
    return () => window.removeEventListener('keydown', handleKeyDown);
  }, [currentTab, view.machineState.isRunning, isSettingsOpen, setIsSettingsOpen, hmi, handleTabChange, debugMode]);

  return (
    <div className="min-h-screen bg-slate-50 dark:bg-slate-950 text-slate-900 dark:text-slate-100 font-sans flex flex-col antialiased selection:bg-slate-800 dark:selection:bg-slate-200 selection:text-white dark:selection:text-slate-900 transition-colors">
      <Header onOpenSettings={() => setIsSettingsOpen(true)} />

      <Navigation
        currentTab={currentTab}
        onSelectTab={handleTabChange}
        motionConn={view.motionState.connection}
        plcConn={view.plcState.connection}
        preFeederConn={view.preFeederState.connection}
        andonConn={view.andonConn}
        hasErrors={{
          motion: view.motionState.hasError,
        }}
      />

      <main className="flex-1 px-4 py-3.5 sm:px-6 max-w-7xl mx-auto w-full">
        {currentTab === 'maquina' && (
          <MaquinaTab
            machineState={view.machineState}
            motionState={view.motionState}
            plcState={view.plcState}
            preFeederState={view.preFeederState}
            models={view.models}
            selectedModelIndex={view.selectedModelIndex}
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
            onRefill={() => {
              void hmi.startRefill();
            }}
            onRefillConfirm={(ok) => {
              void hmi.confirmRefill(ok);
            }}
            onRefillRetry={() => {
              void hmi.retryRefill();
            }}
            onGotoCycle={debugMode ? () => handleTabChange('cycle') : undefined}
            onMotionStop={hmi.motionStop}
            onMotionReset={hmi.motionReset}
            onMotionSearchHome={hmi.motionSearchHome}
            onPlcReset={hmi.plcReset}
            onPlcAllOff={hmi.plcAllOff}
            onPfStart={hmi.pfStart}
            onPfStop={hmi.pfStop}
            onPfReset={hmi.pfReset}
            onPfJogL={hmi.pfTriggerL}
            onPfJogR={hmi.pfTriggerR}
            onBusy={hmi.toggleCycleBusy}
            onMaterialist={hmi.toggleCycleMaterialist}
            showLogs={logsVisible}
            logs={logsVisible ? hmi.filterLogs('ALL') : []}
            onClearLogs={() => hmi.clearLogs('all')}
          />
        )}

        {debugMode && (
          <div className={currentTab === 'cycle' ? undefined : 'hidden'} aria-hidden={currentTab !== 'cycle'}>
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
            />
          </div>
        )}

        {debugMode && currentTab === 'motion' && (
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
            showLogs={logsVisible}
            logs={logsVisible ? hmi.filterLogs('MOTION') : []}
            onClearLogs={() => hmi.clearLogs('motion')}
          />
        )}

        {debugMode && currentTab === 'plc' && (
          <PlcTab
            plcState={view.plcState}
            onToggleValve={hmi.toggleValve}
            valveBusy={hmi.valveBusy}
            onBlowerSecChange={hmi.setBlowerSec}
            onResetPlc={hmi.plcReset}
            onAllOff={hmi.plcAllOff}
            showLogs={logsVisible}
            logs={logsVisible ? hmi.filterLogs('PLC') : []}
            onClearLogs={() => hmi.clearLogs('plc')}
          />
        )}

        {debugMode && currentTab === 'prefeeder' && (
          <PreFeederTab
            preFeederState={view.preFeederState}
            onStart={hmi.pfStart}
            onStop={hmi.pfStop}
            onReset={hmi.pfReset}
            onMaterialist={hmi.pfMaterialist}
            onTriggerR={hmi.pfTriggerR}
            onTriggerL={hmi.pfTriggerL}
            showLogs={logsVisible}
            logs={logsVisible ? hmi.filterLogs('PREFEEDER') : []}
            onClearLogs={() => hmi.clearLogs('prefeeder')}
          />
        )}

        {debugMode && currentTab === 'andon' && (
          <AndonTab
            andonState={view.andonState}
            onSetOut={hmi.andonSetOut}
            onAllOff={hmi.andonAllOff}
            onResumeAuto={hmi.andonResumeAuto}
            onMachineState={hmi.andonMachineState}
            showLogs={logsVisible}
            logs={logsVisible ? hmi.filterLogs('ANDON') : []}
            onClearLogs={() => hmi.clearLogs('andon')}
          />
        )}
      </main>

      <SettingsDrawer
        isOpen={isSettingsOpen}
        onClose={() => setIsSettingsOpen(false)}
        motionConn={view.motionState.connection}
        plcConn={view.plcState.connection}
        preFeederConn={view.preFeederState.connection}
        andonConn={view.andonConn}
        andonBuzzerMute={view.andonBuzzerMute}
        onAndonBuzzerMute={hmi.setAndonBuzzerMute}
        ignorePrefeeder={view.machineState.ignorePrefeeder}
        onIgnorePrefeeder={debugMode ? hmi.setCycleIgnorePrefeeder : undefined}
        safetyExhaust={view.machineState.safetyExhaust}
        connected={view.connected}
        onReconnectNetwork={handleReconnectNetwork}
        reconnecting={reconnecting}
        onDebugModeDisable={handleDebugModeDisable}
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
