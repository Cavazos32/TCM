#pragma once

#include <Arduino.h>

// ====================== Protocolo TCP maestro (HMI) — módulo PLC ======================
// Bytes 0x19–0x23 comandos/eventos · 0x24–0x29 estatus (esclavo→maestro).
enum PlcTcpCmd : uint8_t {
  PLC_CMD_CUTTER_R    = 0x19,  // maestro → esclavo: válvula cutter R
  PLC_CMD_CUTTER_L    = 0x1A,  // maestro → esclavo: válvula cutter L
  PLC_CMD_GRIPPER     = 0x1B,  // maestro → esclavo: válvula gripper
  PLC_CMD_HOLDER      = 0x1C,  // maestro → esclavo: válvula holder
  PLC_CMD_ENCODER     = 0x1D,  // maestro → esclavo: válvula encoder (fgtray)
  PLC_CMD_RESET       = 0x1E,  // maestro → esclavo: reset error PLC
  PLC_TX_CUTTER_ERR   = 0x1F,  // esclavo → maestro: error sensor cutter
  PLC_TX_GRIPPER_ERR  = 0x20,  // esclavo → maestro: error sensor gripper
  PLC_TX_HOLDER_ERR   = 0x21,  // esclavo → maestro: error sensor holder
  PLC_TX_ENCODER_ERR  = 0x22,  // esclavo → maestro: error sensor encoder/tray
  PLC_CMD_BLOWER      = 0x23,  // maestro → esclavo: válvula blower
  // 0x24–0x29 — estatus general del módulo PLC
  PLC_TX_INIT         = 0x24,  // esclavo → maestro: InitState
  PLC_TX_IDLE         = 0x25,  // esclavo → maestro: IdleState
  PLC_TX_BUSY         = 0x26,  // esclavo → maestro: BusyState
  PLC_TX_ERROR        = 0x27,  // esclavo → maestro: ErrorState (general; ver 0x1F–0x22)
  PLC_TX_STOP         = 0x28,  // esclavo → maestro: StoprState
  PLC_TX_RETURN       = 0x29,  // esclavo → maestro: ReturnState
};
