#pragma once

#include <Arduino.h>
#include <stdint.h>
#include "PF_LR/Status_Mode.h"

// =============================================================================
// PreFeeder — opcodes TCP (comandos, errores, estados)
// Ref: Doc/opcode_reference_list.md
// Usado por PreFeeder_Master y PF_LR.
// =============================================================================

// --- Comandos (maestro → esclavo) ---
enum PfCmd : uint8_t {
  PF_CMD_START       = 0x2A,  // Start()
  PF_CMD_STOP        = 0x2B,  // Stop()
  PF_CMD_RESET       = 0x2C,  // ResetPF()
  PF_CMD_MATERIALIST = 0x3F,  // Materialist()
  PF_CMD_TRIGGER_R   = 0x4C,  // TriggerR() — Tfeed lado R
  PF_CMD_TRIGGER_L   = 0x51,  // TriggerL() — Tfeed lado L
};

// --- Errores (esclavo → maestro) ---
enum PfErrorByte : uint8_t {
  PF_ERR_BUFFER_FULL_R = 0x2D,  // BufferFR()
  PF_ERR_BUFFER_MAX_R  = 0x2E,  // BufferMR()
  PF_ERR_TENSION_R     = 0x2F,  // TensionerR()
  PF_ERR_CILINDRO_R    = 0x30,  // CilindroR()
  PF_ERR_MANGUERA_R    = 0x31,  // MangueraR()
  PF_ERR_HOLGURA_R     = 0x32,  // HolguraR()
  PF_ERR_BUFFER_FULL_L = 0x33,  // BufferFL()
  PF_ERR_BUFFER_MAX_L  = 0x34,  // BufferML()
  PF_ERR_TENSION_L     = 0x35,  // TensionerL()
  PF_ERR_CILINDRO_L    = 0x36,  // CilindroL()
  PF_ERR_MANGUERA_L    = 0x37,  // MangueraL()
  PF_ERR_HOLGURA_L     = 0x38,  // HolguraL()
};

// --- Estados (esclavo ↔ maestro) ---
enum PfState : uint8_t {
  PF_ST_INIT   = 0x39,  // InitState
  PF_ST_IDLE   = 0x3A,  // IdleState
  PF_ST_BUSY   = 0x3B,  // BusyState
  PF_ST_ERROR  = 0x3C,  // ErrorState
  PF_ST_STOP   = 0x3D,  // StoprState
  PF_ST_RETURN = 0x3E,  // ReturnState
};

static inline bool pfTcpIsCmdByte(uint8_t b) {
  return b == PF_CMD_START || b == PF_CMD_STOP || b == PF_CMD_RESET
      || b == PF_CMD_MATERIALIST
      || b == PF_CMD_TRIGGER_R || b == PF_CMD_TRIGGER_L;
}

static inline bool pfIsStateByte(uint8_t b) {
  return b >= PF_ST_INIT && b <= PF_ST_RETURN;
}

static inline bool pfIsErrorByte(uint8_t b) {
  return (b >= PF_ERR_BUFFER_FULL_R && b <= PF_ERR_HOLGURA_R)
      || (b >= PF_ERR_BUFFER_FULL_L && b <= PF_ERR_HOLGURA_L);
}

static inline bool pfTcpIsErrorByte(uint8_t b) { return pfIsErrorByte(b); }
static inline bool pfTcpIsStateByte(uint8_t b) { return pfIsStateByte(b); }

static inline uint8_t pfTcpErrorByteFromId(PfErrorId id, char side) {
  // Orden wire: buffer full, buffer max, tension, cilindro, manguera, holgura
  static const int8_t offById[8] = { -1, 1, 2, 3, 4, 0, 5, -1 };
  if (id >= 8 || offById[id] < 0) return 0;
  return (side == 'R')
           ? (uint8_t)(PF_ERR_BUFFER_FULL_R + (uint8_t)offById[id])
           : (uint8_t)(PF_ERR_BUFFER_FULL_L + (uint8_t)offById[id]);
}

static inline uint8_t pfTcpErrorByteFromWire(uint8_t wireCode, char side) {
  return pfTcpErrorByteFromId(pfErrorIdFromWireCode(wireCode), side);
}

static inline const char* pfTcpErrorName(uint8_t byteCode) {
  switch (byteCode) {
    case PF_ERR_BUFFER_FULL_R: return "BufferFR";
    case PF_ERR_BUFFER_MAX_R:  return "BufferMR";
    case PF_ERR_TENSION_R:     return "TensionerR";
    case PF_ERR_CILINDRO_R:    return "CilindroR";
    case PF_ERR_MANGUERA_R:    return "MangueraR";
    case PF_ERR_HOLGURA_R:     return "HolguraR";
    case PF_ERR_BUFFER_FULL_L: return "BufferFL";
    case PF_ERR_BUFFER_MAX_L:  return "BufferML";
    case PF_ERR_TENSION_L:     return "TensionerL";
    case PF_ERR_CILINDRO_L:    return "CilindroL";
    case PF_ERR_MANGUERA_L:    return "MangueraL";
    case PF_ERR_HOLGURA_L:     return "HolguraL";
    default:                   return "unknown";
  }
}

static inline const char* pfStateName(uint8_t b) {
  switch (b) {
    case PF_ST_INIT:   return "InitState";
    case PF_ST_IDLE:   return "IdleState";
    case PF_ST_BUSY:   return "BusyState";
    case PF_ST_ERROR:  return "ErrorState";
    case PF_ST_STOP:   return "StoprState";
    case PF_ST_RETURN: return "ReturnState";
    default:           return "unknown";
  }
}

static inline const char* pfTcpStateName(uint8_t byteCode) {
  return pfStateName(byteCode);
}
