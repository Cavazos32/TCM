#pragma once

#include <stdint.h>

// =============================================================================
// PLC — opcodes TCP (válvulas, errores detalle EXXX, estados)
// Ref: Doc/TCM - D.xlsx · Opcodes / Error list · Doc/gpio_list_updated.md
//
// Estados (PLC_ST_*) = aviso de estado; NO son códigos EXXX (E051 eliminado).
// Errores detalle (PLC_ERR_*) = EXXX → solo HMI/Main.
// Clase: C1 stop all · C2 pause · C3 finish step
// =============================================================================

#ifndef ERR_CLASS_DEFINED
#define ERR_CLASS_DEFINED
enum ErrClass : uint8_t {
  ERR_CLASS_NONE = 0,
  ERR_CLASS_C1   = 1,
  ERR_CLASS_C2   = 2,
  ERR_CLASS_C3   = 3,
};
#endif

// --- Comandos válvulas / reset (maestro → esclavo) ---
enum PlcCmd : uint8_t {
  PLC_CMD_CUTTER_R = 0x19,  // CutterR()
  PLC_CMD_CUTTER_L = 0x1A,  // CutterL()
  PLC_CMD_GRIPPER  = 0x1B,  // Gripper()
  PLC_CMD_HOLDER   = 0x1C,  // Holder()
  PLC_CMD_ENCODER  = 0x1D,  // Encoder()
  PLC_CMD_RESET    = 0x1E,  // ResetPLC()
  PLC_CMD_BLOWER   = 0x23,  // BlowerE() / Blower()
};

// --- Errores detalle (esclavo → HMI) ---
enum PlcError : uint8_t {
  PLC_ERR_CUTTER  = 0x1F,  // E047 C1 · CutterE()
  // GripperE: falla gripper y/o presión de aire baja·nula
  PLC_ERR_GRIPPER = 0x20,  // E048 C1 · GripperE()
  PLC_ERR_HOLDER  = 0x21,  // E049 C1 · HolderE()
  // EncoderE (bandeja): también aire baja·nula, manguera ausente o falla cilindro
  PLC_ERR_ENCODER = 0x22,  // E050 C1 · EncoderE()
};

#define PLC_TX_CUTTER_ERR  PLC_ERR_CUTTER
#define PLC_TX_GRIPPER_ERR PLC_ERR_GRIPPER
#define PLC_TX_HOLDER_ERR  PLC_ERR_HOLDER
#define PLC_TX_ENCODER_ERR PLC_ERR_ENCODER

// --- Estados módulo (no EXXX) ---
enum PlcState : uint8_t {
  PLC_ST_INIT   = 0x24,  // InitState
  PLC_ST_IDLE   = 0x25,  // IdleState
  PLC_ST_BUSY   = 0x26,  // BusyState
  PLC_ST_ERROR  = 0x27,  // ErrorState — estado, no E051
  PLC_ST_STOP   = 0x28,  // StoprState
  PLC_ST_RETURN = 0x29,  // ReturnState
};

#define PLC_TX_INIT   PLC_ST_INIT
#define PLC_TX_IDLE   PLC_ST_IDLE
#define PLC_TX_BUSY   PLC_ST_BUSY
#define PLC_TX_ERROR  PLC_ST_ERROR
#define PLC_TX_STOP   PLC_ST_STOP
#define PLC_TX_RETURN PLC_ST_RETURN

static inline bool plcIsCmdByte(uint8_t b) {
  return (b >= PLC_CMD_CUTTER_R && b <= PLC_CMD_RESET) || b == PLC_CMD_BLOWER;
}

static inline bool plcIsState(uint8_t b) {
  return b >= PLC_ST_INIT && b <= PLC_ST_RETURN;
}

static inline bool plcIsError(uint8_t b) {
  return b >= PLC_ERR_CUTTER && b <= PLC_ERR_ENCODER;
}

static inline ErrClass plcErrClass(uint8_t b) {
  return plcIsError(b) ? ERR_CLASS_C1 : ERR_CLASS_NONE;
}

static inline const char* plcStateName(uint8_t b) {
  switch (b) {
    case PLC_ST_INIT:   return "InitState";
    case PLC_ST_IDLE:   return "IdleState";
    case PLC_ST_BUSY:   return "BusyState";
    case PLC_ST_ERROR:  return "ErrorState";
    case PLC_ST_STOP:   return "StoprState";
    case PLC_ST_RETURN: return "ReturnState";
    default:            return "unknown";
  }
}
