#pragma once

#include <Arduino.h>

// Opcodes TCP TCM ↔ Maestro (contrato externo, sin copiar implementación legacy).
enum PfCmd : uint8_t {
  PF_CMD_START = 0x2A,
  PF_CMD_STOP = 0x2B,
  PF_CMD_RESET = 0x2C,
  PF_CMD_MATERIALIST = 0x3F,
  PF_CMD_TRIGGER_R = 0x4C,
  PF_CMD_TRIGGER_L = 0x51,
};

enum PfErrorByte : uint8_t {
  PF_ERR_BUFFER_FULL_R = 0x2D,
  PF_ERR_BUFFER_MAX_R = 0x2E,
  PF_ERR_TENSION_R = 0x2F,
  PF_ERR_CILINDRO_R = 0x30,
  PF_ERR_MANGUERA_R = 0x31,
  PF_ERR_HOLGURA_R = 0x32,
  PF_ERR_BUFFER_FULL_L = 0x33,
  PF_ERR_BUFFER_MAX_L = 0x34,
  PF_ERR_TENSION_L = 0x35,
  PF_ERR_CILINDRO_L = 0x36,
  PF_ERR_MANGUERA_L = 0x37,
  PF_ERR_HOLGURA_L = 0x38,
  PF_ERR_NOT_INITIALIZED = 0x7D,
};

enum PfState : uint8_t {
  PF_ST_INIT = 0x39,
  PF_ST_IDLE = 0x3A,
  PF_ST_BUSY = 0x3B,
  PF_ST_ERROR = 0x3C,
  PF_ST_STOP = 0x3D,
  PF_ST_RETURN = 0x3E,
};

static inline bool pfIsCmdByte(uint8_t b) {
  return b == PF_CMD_START || b == PF_CMD_STOP || b == PF_CMD_RESET
      || b == PF_CMD_MATERIALIST || b == PF_CMD_TRIGGER_R || b == PF_CMD_TRIGGER_L;
}

static inline bool pfIsStateByte(uint8_t b) {
  return b >= PF_ST_INIT && b <= PF_ST_RETURN;
}

static inline bool pfIsErrorByte(uint8_t b) {
  return (b >= PF_ERR_BUFFER_FULL_R && b <= PF_ERR_HOLGURA_R)
      || (b >= PF_ERR_BUFFER_FULL_L && b <= PF_ERR_HOLGURA_L)
      || b == PF_ERR_NOT_INITIALIZED;
}

static inline bool pfTcpIsCmdByte(uint8_t b) { return pfIsCmdByte(b); }
static inline bool pfTcpIsStateByte(uint8_t b) { return pfIsStateByte(b); }
static inline bool pfTcpIsErrorByte(uint8_t b) { return pfIsErrorByte(b); }

static inline const char* pfTcpErrorName(uint8_t byteCode) {
  switch (byteCode) {
    case PF_ERR_BUFFER_FULL_R: return "BufferFR";
    case PF_ERR_BUFFER_MAX_R: return "BufferMR";
    case PF_ERR_TENSION_R: return "TensionerR";
    case PF_ERR_CILINDRO_R: return "CilindroR";
    case PF_ERR_MANGUERA_R: return "MangueraR";
    case PF_ERR_HOLGURA_R: return "reserved";
    case PF_ERR_BUFFER_FULL_L: return "BufferFL";
    case PF_ERR_BUFFER_MAX_L: return "BufferML";
    case PF_ERR_TENSION_L: return "TensionerL";
    case PF_ERR_CILINDRO_L: return "CilindroL";
    case PF_ERR_MANGUERA_L: return "MangueraL";
    case PF_ERR_HOLGURA_L: return "reserved";
    case PF_ERR_NOT_INITIALIZED: return "NotInitialized";
    default: return "unknown";
  }
}

static inline const char* pfStateName(uint8_t b) {
  switch (b) {
    case PF_ST_INIT: return "InitState";
    case PF_ST_IDLE: return "IdleState";
    case PF_ST_BUSY: return "BusyState";
    case PF_ST_ERROR: return "ErrorState";
    case PF_ST_STOP: return "StoprState";
    case PF_ST_RETURN: return "ReturnState";
    default: return "unknown";
  }
}

static inline const char* pfTcpStateName(uint8_t b) { return pfStateName(b); }

static inline const char* pfErrorName(uint8_t b) {
  switch (b) {
    case PF_ERR_BUFFER_FULL_R: return "BufferFR";
    case PF_ERR_BUFFER_MAX_R: return "BufferMR";
    case PF_ERR_TENSION_R: return "TensionerR";
    case PF_ERR_CILINDRO_R: return "CilindroR";
    case PF_ERR_MANGUERA_R: return "MangueraR";
    case PF_ERR_BUFFER_FULL_L: return "BufferFL";
    case PF_ERR_BUFFER_MAX_L: return "BufferML";
    case PF_ERR_TENSION_L: return "TensionerL";
    case PF_ERR_CILINDRO_L: return "CilindroL";
    case PF_ERR_MANGUERA_L: return "MangueraL";
    case PF_ERR_NOT_INITIALIZED: return "NotInitialized";
    default: return "unknown";
  }
}
