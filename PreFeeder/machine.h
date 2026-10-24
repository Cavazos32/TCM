#pragma once

#include "params.h"
#include <StepperRMT.h>

struct SensorSnapshot {
  bool bufFullRaw = false;
  bool bufFullStable = false;
  bool bufMidRaw = false;
  bool bufHighRaw = false;
  bool bufMaxRaw = false;
  bool tensionRaw = false;
  bool cylinderOpen = false;
  bool hoseAbsent = false;
};

enum NetCommandType : uint8_t {
  NC_NONE = 0,
  NC_START,
  NC_STOP,
  NC_RESET,
  NC_TRIGGER,
  NC_IN_PROCESS,
  NC_IDLE_MODE,
  NC_REFILL,
  NC_REFILL_HOLD,
  NC_SET_TRIGGER_FEED,
  NC_SET_REFILL_PULSE,
  NC_PING,
  NC_HALT,
};

struct NetCommand {
  NetCommandType type = NC_NONE;
  String arg;
  int id = 0;
  String refillChannel;
  bool refillOn = false;
};

enum SystemFault : uint8_t {
  FAULT_NONE = 0,
  FAULT_ENDSTOP,
  FAULT_TENSION,
  FAULT_CYLINDER,
  FAULT_HOSE,
  FAULT_BUFFER_TIMEOUT,
  FAULT_OPERATOR,
};

struct LidarReading {
  bool valid = false;
  uint16_t distCm = 0;
};

struct MachineStatus {
  SystemFault fault = FAULT_NONE;
  bool autoEnabled = false;
  bool idleMode = false;
  bool inProcess = false;
  bool feederActive = false;
  bool bufferActive = false;
  bool triggerActive = false;
  uint32_t feederMsLeft = 0;
  LidarReading lidar;
};

// Tipos de salida (en .h para prototipos auto-generados con machine.ino)
enum OutputMode : uint8_t {
  OUT_IDLE = 0,
  OUT_AUTO,
  OUT_REFILL_MANUAL,
};

struct OutputIntent {
  OutputMode mode = OUT_IDLE;
  float feederRpm = 0.0f;
  float dereelerRpm = 0.0f;
  bool tensionReverse = false;
  bool servoRun = false;
  int16_t servoExtraUs = 0;
};

LidarReading machineLidar();

void machineInit(Params* params);
void machineHandleCommand(const NetCommand& cmd);
bool machinePushCommand(const NetCommand& cmd);
bool machinePopCommand(NetCommand& out);
void machineTick(uint32_t nowMs);
const SensorSnapshot& machineSensors();
MachineStatus machineStatus();
uint8_t machineFaultWireCode();

void pfNetworkInit(Params* params);
void pfNetworkTick(uint32_t nowMs);
