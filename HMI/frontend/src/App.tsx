import React, { useState, useEffect, useCallback } from 'react';
import { TabType } from './types';
import { Header } from './components/Header';
import { Navigation } from './components/Navigation';
import { MaquinaTab } from './components/MaquinaTab';
import { CycleTab } from './components/CycleTab';
import { MotionTab } from './components/MotionTab';
import { PlcTab } from './components/PlcTab';
import { PreFeederTab } from './components/PreFeederTab';
import { SettingsDrawer } from './components/SettingsDrawer';
import { AppProvider, useApp } from './context/AppContext';
import { useHmiState } from './hooks/useHmiState';

function AppMain() {
  const { isSettingsOpen, setIsSettingsOpen, showLogs } = useApp();
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
      setCurrentTab(tab);
      hmi.onTabChange(tab);
    },
    [hmi]
  );

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
      else if (e.key === '2') handleTabChange('cycle');
      else if (e.key === '3') handleTabChange('motion');
      else if (e.key === '4') handleTabChange('plc');
      else if (e.key === '5') handleTabChange('prefeeder');
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
  }, [currentTab, view.machineState.isRunning, isSettingsOpen, setIsSettingsOpen, hmi, handleTabChange]);

  return (
    <div className="min-h-screen bg-slate-50 dark:bg-slate-950 text-slate-900 dark:text-slate-100 font-sans flex flex-col antialiased selection:bg-slate-800 dark:selection:bg-slate-200 selection:text-white dark:selection:text-slate-900 transition-colors">
      <Header machineState={view.machineState} onOpenSettings={() => setIsSettingsOpen(true)} />

      <Navigation
        currentTab={currentTab}
        onSelectTab={handleTabChange}
        motionConn={view.motionState.connection}
        plcConn={view.plcState.connection}
        preFeederConn={view.preFeederState.connection}
        hasErrors={{
          motion: view.motionState.hasError,
        }}
      />

      <main className="flex-1 px-4 py-3.5 sm:px-6 max-w-7xl mx-auto w-full">
        {currentTab === 'maquina' && (
          <MaquinaTab
            machineState={view.machineState}
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
            onGotoCycle={() => handleTabChange('cycle')}
            onToggleTrialMode={hmi.setCycleTrialMode}
            showLogs={showLogs}
            logs={showLogs ? hmi.filterLogs('ALL') : []}
            onClearLogs={() => hmi.clearLogs('all')}
          />
        )}

        {currentTab === 'cycle' && (
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
            onToggleTrialMode={hmi.setCycleTrialMode}
            onResume={hmi.resume}
            resumeEnabled={view.resumeEnabled}
          />
        )}

        {currentTab === 'motion' && (
          <MotionTab
            motionState={view.motionState}
            onUpdateTargetPos={(pos) => hmi.setMmRpm(pos, view.motionState.rpm)}
            onUpdateRpm={(rpm) => hmi.setMmRpm(view.motionState.targetPositionMm, rpm)}
            onUpdateOffsetL={() => {}}
            onUpdateOffsetR={() => {}}
            onMover={hmi.motionMove}
            onStop={hmi.motionStop}
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
            showLogs={showLogs}
            logs={showLogs ? hmi.filterLogs('MOTION') : []}
            onClearLogs={() => hmi.clearLogs('motion')}
          />
        )}

        {currentTab === 'plc' && (
          <PlcTab
            plcState={view.plcState}
            onToggleValve={hmi.toggleValve}
            onResetPlc={hmi.plcReset}
            onAllOff={hmi.plcAllOff}
            showLogs={showLogs}
            logs={showLogs ? hmi.filterLogs('PLC') : []}
            onClearLogs={() => hmi.clearLogs('plc')}
          />
        )}

        {currentTab === 'prefeeder' && (
          <PreFeederTab
            preFeederState={view.preFeederState}
            onStart={hmi.pfStart}
            onStop={hmi.pfStop}
            onReset={hmi.pfReset}
            onMaterialist={hmi.pfMaterialist}
            onTriggerR={hmi.pfTriggerR}
            onTriggerL={hmi.pfTriggerL}
            showLogs={showLogs}
            logs={showLogs ? hmi.filterLogs('PREFEEDER') : []}
            onClearLogs={() => hmi.clearLogs('prefeeder')}
          />
        )}
      </main>

      <SettingsDrawer
        isOpen={isSettingsOpen}
        onClose={() => setIsSettingsOpen(false)}
        motionConn={view.motionState.connection}
        plcConn={view.plcState.connection}
        preFeederConn={view.preFeederState.connection}
        connected={view.connected}
        onReconnectNetwork={handleReconnectNetwork}
        reconnecting={reconnecting}
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
