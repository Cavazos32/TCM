#pragma once

#include <Arduino.h>

// Maestro organiza el bus: comandos globales vs volátiles (por lado).
// Globales → broadcast L+R vía peerDoCmd() en PF_LR.ino (misma lógica que /api/auto local).
//   enable=1 → start | enable=0 → stop | reset=1 → reset
//   idle_mode → setIdleMode | in_process → setInProcess
// Volátiles → un solo esclavo (trigger, settings, etc.) vía side=L|R.

enum class PfCmdScope : uint8_t
{
  Unknown = 0,
  Global  = 1,
  Side    = 2,
};

static inline PfCmdScope pfMasterCmdScope(const String& cmd)
{
  if (cmd == "start" || cmd == "stop" || cmd == "reset"
      || cmd == "setIdleMode" || cmd == "idleMode"
      || cmd == "setTestMode" || cmd == "testMode"
      || cmd == "materialistaCall" || cmd == "setMaterialistaCall"
      || cmd == "setInProcess" || cmd == "inProcess"
      || cmd == "refillMaterial" || cmd == "refillDereeler"
      || cmd == "refillServo" || cmd == "refillFeeder"
      || cmd == "setBuzzerMute" || cmd == "buzzerMute" || cmd == "buzzerMuted"
      || cmd == "halt" || cmd == "motionStop" || cmd == "hardStop"
      || cmd == "ping")
    return PfCmdScope::Global;

  if (cmd == "trigger"
      || cmd == "setRefillPulseS" || cmd == "refillPulseS"
      || cmd == "setHolguraExtra" || cmd == "setHolguraFault" || cmd == "setHolguraFaultS"
      || cmd == "setSafetyFactor" || cmd == "setDereelerLead" || cmd == "setDereelerLeadMs"
      || cmd == "setTriggerFeed" || cmd == "setTriggerCfg"
      || cmd == "setPieceLength" || cmd == "setPieceLengthMm"
      || cmd == "setFeedSpeed" || cmd == "setFeedSpeedMmS"
      || cmd == "setAllCfg" || cmd == "applyAllCfg")
    return PfCmdScope::Side;

  return PfCmdScope::Unknown;
}

static inline bool pfMasterCmdBypassesPause(const String& cmd)
{
  return cmd == "reset" || cmd == "stop";
}

static inline char pfMasterParseSideArg(const String& s)
{
  if (s.length() != 1) return '-';
  const char c = s.charAt(0);
  if (c == 'L' || c == 'l') return 'L';
  if (c == 'R' || c == 'r') return 'R';
  return '-';
}
