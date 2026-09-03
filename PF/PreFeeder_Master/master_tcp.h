#pragma once

#include <Arduino.h>
#include "../PF_LR/Status_Mode.h"

// Protocolo TCP maestro (HMI) — módulo PreFeeder (tabla 0x2A–0x3F).
enum PfMasterTcp : uint8_t {
  PF_CMD_START        = 0x2A,  // maestro → esclavo: Start()
  PF_CMD_STOP         = 0x2B,  // maestro → esclavo: Stop()
  PF_CMD_RESET        = 0x2C,  // maestro → esclavo: ResetPF()
  PF_TX_BUFFER_FULL_R = 0x2D,  // esclavo → maestro: BufferFR()
  PF_TX_BUFFER_MAX_R  = 0x2E,  // esclavo → maestro: BufferMR()
  PF_TX_TENSION_R     = 0x2F,  // esclavo → maestro: TensionerR()
  PF_TX_CILINDRO_R    = 0x30,  // esclavo → maestro: CilindroR()
  PF_TX_MANGUERA_R    = 0x31,  // esclavo → maestro: MangueraR()
  PF_TX_HOLGURA_R     = 0x32,  // esclavo → maestro: HolguraR()
  PF_TX_BUFFER_FULL_L = 0x33,  // esclavo → maestro: BufferFL()
  PF_TX_BUFFER_MAX_L  = 0x34,  // esclavo → maestro: BufferML()
  PF_TX_TENSION_L     = 0x35,  // esclavo → maestro: TensionerL()
  PF_TX_CILINDRO_L    = 0x36,  // esclavo → maestro: CilindroL()
  PF_TX_MANGUERA_L    = 0x37,  // esclavo → maestro: MangueraL()
  PF_TX_HOLGURA_L     = 0x38,  // esclavo → maestro: HolguraL()
  PF_TX_INIT          = 0x39,  // esclavo → maestro: InitState()
  PF_TX_IDLE          = 0x3A,  // esclavo → maestro: IdleState()
  PF_TX_BUSY          = 0x3B,  // esclavo → maestro: BusyState()
  PF_TX_ERROR         = 0x3C,  // esclavo → maestro: ErrorState()
  PF_TX_STOP          = 0x3D,  // esclavo → maestro: StoprState()
  PF_TX_RETURN        = 0x3E,  // esclavo → maestro: ReturnState()
  PF_CMD_MATERIALIST  = 0x3F,  // maestro → esclavo: Materialist()
};

static inline bool pfTcpIsCmdByte(uint8_t b)
{
  return b == PF_CMD_START || b == PF_CMD_STOP || b == PF_CMD_RESET
      || b == PF_CMD_MATERIALIST;
}

static inline bool pfTcpIsErrorByte(uint8_t b)
{
  return (b >= PF_TX_BUFFER_FULL_R && b <= PF_TX_HOLGURA_R)
      || (b >= PF_TX_BUFFER_FULL_L && b <= PF_TX_HOLGURA_L);
}

static inline bool pfTcpIsStateByte(uint8_t b)
{
  return b >= PF_TX_INIT && b <= PF_TX_RETURN;
}

static inline uint8_t pfTcpErrorByteFromId(PfErrorId id, char side)
{
  // Orden tabla: buffer full, buffer max, tension, cilindro, manguera, holgura
  static const int8_t offById[8] = { -1, 1, 2, 3, 4, 0, 5, -1 };
  if (id >= 8 || offById[id] < 0) return 0;
  return (side == 'R')
           ? (uint8_t)(PF_TX_BUFFER_FULL_R + (uint8_t)offById[id])
           : (uint8_t)(PF_TX_BUFFER_FULL_L + (uint8_t)offById[id]);
}

static inline uint8_t pfTcpErrorByteFromWire(uint8_t wireCode, char side)
{
  return pfTcpErrorByteFromId(pfErrorIdFromWireCode(wireCode), side);
}

static inline const char* pfTcpErrorName(uint8_t byteCode)
{
  switch (byteCode) {
    case PF_TX_BUFFER_FULL_R: return "BufferFR";
    case PF_TX_BUFFER_MAX_R:  return "BufferMR";
    case PF_TX_TENSION_R:     return "TensionerR";
    case PF_TX_CILINDRO_R:    return "CilindroR";
    case PF_TX_MANGUERA_R:    return "MangueraR";
    case PF_TX_HOLGURA_R:     return "HolguraR";
    case PF_TX_BUFFER_FULL_L: return "BufferFL";
    case PF_TX_BUFFER_MAX_L:  return "BufferML";
    case PF_TX_TENSION_L:     return "TensionerL";
    case PF_TX_CILINDRO_L:    return "CilindroL";
    case PF_TX_MANGUERA_L:    return "MangueraL";
    case PF_TX_HOLGURA_L:     return "HolguraL";
    default:                  return "unknown";
  }
}

static inline const char* pfTcpStateName(uint8_t byteCode)
{
  switch (byteCode) {
    case PF_TX_INIT:   return "InitState";
    case PF_TX_IDLE:   return "IdleState";
    case PF_TX_BUSY:   return "BusyState";
    case PF_TX_ERROR:  return "ErrorState";
    case PF_TX_STOP:   return "StoprState";
    case PF_TX_RETURN: return "ReturnState";
    default:           return "unknown";
  }
}
