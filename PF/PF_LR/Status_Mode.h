#pragma once

#include "Types.h"
#include <stdint.h>

// Catálogo unificado PreFeeder: códigos PF-xxx, fases auto y estado operativo.
// Wire code legacy: base L=20 / R=30 + PfErrorId (1..7) → 21–27 / 31–37.

constexpr uint8_t PF_WIRE_BASE_L = 20;
constexpr uint8_t PF_WIRE_BASE_R = 30;

enum PfErrorId : uint8_t {
  PF_ERR_NONE = 0,
  PF_ERR_ENDSTOP = 1,    // PF-001
  PF_ERR_TENSION = 2,    // PF-002
  PF_ERR_CYLINDER = 3,   // PF-003
  PF_ERR_HOSE = 4,       // PF-004
  PF_ERR_BUFFER = 5,     // PF-005
  PF_ERR_HOLGURA = 6,    // PF-006
  PF_ERR_OPERATOR = 7    // PF-007
};

enum PfMachineState : uint8_t {
  PF_MS_OFF = 0,
  PF_MS_IDLE,
  PF_MS_START,
  PF_MS_PRODUCTION,
  PF_MS_MATERIALIST,
  PF_MS_IN_PROCESS,
  PF_MS_ERROR,
  PF_MS_STOP
};

struct PfErrorEntry {
  PfErrorId   id;
  SystemFault fault;
  AutoState   autoPhase;
  const char* tag;
  const char* slug;
  const char* desc;
  uint8_t     level;  // 1=aviso, 2=PAUSE, 3=safety
};

static const PfErrorEntry PF_ERROR_TABLE[] = {
  { PF_ERR_ENDSTOP,   FAULT_ENDSTOP,          AUTO_ENDSTOP_FAULT,   "PF-001", "endstop",          "Buffer Max",           3 },
  { PF_ERR_TENSION,   FAULT_TENSION_TIMEOUT,  AUTO_TENSION_FAULT,   "PF-002", "tension_timeout",  "Tension timeout",      2 },
  { PF_ERR_CYLINDER,  FAULT_CYLINDER_OPEN,    AUTO_CYLINDER_FAULT,  "PF-003", "cylinder_open",    "Cilindro abierto",     2 },
  { PF_ERR_HOSE,      FAULT_HOSE_ABSENT,      AUTO_HOSE_FAULT,      "PF-004", "hose_absent",      "Manguera ausente",     2 },
  { PF_ERR_BUFFER,    FAULT_BUFFER_TIMEOUT,   AUTO_BUFFER_FAULT,    "PF-005", "buffer_timeout",   "Buffer sin relleno",   2 },
  { PF_ERR_HOLGURA,   FAULT_HOLGURA_TIMEOUT,  AUTO_HOLGURA_FAULT,   "PF-006", "holgura_timeout",  "Sin holgura",          2 },
  { PF_ERR_OPERATOR,  FAULT_OPERATOR_STOP,    AUTO_OPERATOR_STOP,   "PF-007", "operator_stop",    "Parada operador",      2 },
};

static const char* const PF_AUTO_PHASE_NAMES[] = {
  "off", "home_hold", "servo_lead", "cw",
  "endstop_fault", "tension_fault", "cylinder_fault", "hose_fault",
  "buffer_fault", "holgura_fault", "operator_stop"
};

static const char* const PF_MACHINE_STATE_NAMES[] = {
  "off", "idle", "start", "production", "materialist", "in_process", "error", "stop"
};

static inline const PfErrorEntry* pfErrorFindByFault(SystemFault fault)
{
  for (uint8_t i = 0; i < sizeof(PF_ERROR_TABLE) / sizeof(PF_ERROR_TABLE[0]); i++)
    if (PF_ERROR_TABLE[i].fault == fault)
      return &PF_ERROR_TABLE[i];
  return nullptr;
}

static inline const PfErrorEntry* pfErrorFindById(PfErrorId id)
{
  for (uint8_t i = 0; i < sizeof(PF_ERROR_TABLE) / sizeof(PF_ERROR_TABLE[0]); i++)
    if (PF_ERROR_TABLE[i].id == id)
      return &PF_ERROR_TABLE[i];
  return nullptr;
}

static inline PfErrorId pfErrorIdFromFault(SystemFault fault)
{
  const PfErrorEntry* e = pfErrorFindByFault(fault);
  return e ? e->id : PF_ERR_NONE;
}

static inline PfErrorId pfErrorIdFromWireCode(uint8_t code)
{
  if (code > PF_WIRE_BASE_L && code <= PF_WIRE_BASE_L + 7)
    return (PfErrorId)(code - PF_WIRE_BASE_L);
  if (code > PF_WIRE_BASE_R && code <= PF_WIRE_BASE_R + 7)
    return (PfErrorId)(code - PF_WIRE_BASE_R);
  return PF_ERR_NONE;
}

static inline uint8_t pfErrorWireCodeFromId(PfErrorId id, uint8_t wireBase)
{
  return id ? (uint8_t)(wireBase + id) : 0;
}

static inline uint8_t pfErrorWireCodeFromFault(SystemFault fault, uint8_t wireBase)
{
  return pfErrorWireCodeFromId(pfErrorIdFromFault(fault), wireBase);
}

static inline const char* pfErrorTag(PfErrorId id)
{
  const PfErrorEntry* e = pfErrorFindById(id);
  return e ? e->tag : "PF-000";
}

static inline const char* pfErrorTagFromFault(SystemFault fault)
{
  const PfErrorEntry* e = pfErrorFindByFault(fault);
  return e ? e->tag : "PF-000";
}

static inline const char* pfErrorTagFromWireCode(uint8_t code)
{
  return pfErrorTag(pfErrorIdFromWireCode(code));
}

static inline const char* pfErrorSlugFromFault(SystemFault fault)
{
  const PfErrorEntry* e = pfErrorFindByFault(fault);
  return e ? e->slug : "none";
}

static inline const char* pfErrorDescFromFault(SystemFault fault)
{
  const PfErrorEntry* e = pfErrorFindByFault(fault);
  return e ? e->desc : "none";
}

static inline uint8_t pfErrorLevelFromFault(SystemFault fault)
{
  const PfErrorEntry* e = pfErrorFindByFault(fault);
  return e ? e->level : 0;
}

static inline AutoState pfAutoPhaseFromFault(SystemFault fault)
{
  const PfErrorEntry* e = pfErrorFindByFault(fault);
  return e ? e->autoPhase : AUTO_OFF;
}

static inline const char* pfAutoPhaseName(AutoState phase)
{
  const uint8_t i = (uint8_t)phase;
  if (i < sizeof(PF_AUTO_PHASE_NAMES) / sizeof(PF_AUTO_PHASE_NAMES[0]))
    return PF_AUTO_PHASE_NAMES[i];
  return "off";
}

static inline PfMachineState pfMachineStateResolve(bool materialist, bool inProcess,
                                                   bool autoEnabled, SystemFault fault)
{
  if (fault == FAULT_OPERATOR_STOP)
    return PF_MS_STOP;
  if (fault != FAULT_NONE)
    return PF_MS_ERROR;
  if (materialist)
    return PF_MS_MATERIALIST;
  // In process manda sobre el quieto (antes autoEnabled → start tapaba in_process).
  if (inProcess)
    return PF_MS_IN_PROCESS;
  // Quieto: sensores bloqueados hasta Iniciar / In process.
  (void)autoEnabled;
  return PF_MS_IDLE;
}

static inline const char* pfMachineStateName(PfMachineState state)
{
  const uint8_t i = (uint8_t)state;
  if (i < sizeof(PF_MACHINE_STATE_NAMES) / sizeof(PF_MACHINE_STATE_NAMES[0]))
    return PF_MACHINE_STATE_NAMES[i];
  return "off";
}
