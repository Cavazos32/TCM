#pragma once

#include <Arduino.h>
#include <stdint.h>

// =============================================================================
// Andon — opcodes TCP
//
// Torre:
//   • HMI → Andon: ANDON_RX_* (0x40–0x49) estado de máquina (no EXXX).
//   • Presión FRL (GPIO): local — NO se recibe opcode de estado.
//     Cambio en PIN_PRESSURE_FRL → torreta Error + TX 0x50 (E064) a HMI.
//     0x50 es detalle EXXX (Andon → HMI); no hay RX de 0x50.
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

// --- RX desde HMI: estado de máquina (torre) — no EXXX ---
// Columna Andon Excel: Green / Red / N/A / Red+Buzzer / Green+Buzzer / Yellow+Buzzer
enum AndonRx : uint8_t {
  ANDON_RX_INIT        = 0x40,  // InitState()      · Green
  ANDON_RX_START       = 0x41,  // StartCycle()     · N/A
  ANDON_RX_STOP        = 0x42,  // StopCycle()      · Red
  ANDON_RX_RESET       = 0x43,  // ResetCycle()     · N/A
  ANDON_RX_IDLE        = 0x44,  // IdleState()      · Green
  ANDON_RX_BUSY        = 0x45,  // BusyState()      · Green
  ANDON_RX_ERROR       = 0x46,  // ErrorState()     · Red + Buzzer
  ANDON_RX_FINISH      = 0x47,  // LotCompleate()   · Green + Buzzer
  ANDON_RX_RETURN      = 0x48,  // ReturnStop()     · N/A
  ANDON_RX_MATERIALIST = 0x49,  // Materialist()    · Yellow + Buzzer
};

// --- TX Andon → HMI (detalle EXXX; solo salida) ---
enum AndonError : uint8_t {
  ANDON_ERR_PRESSURE = 0x50,  // E064 C1 · PressureError() — pin FRL
};
#define ANDON_TX_PRESSURE ANDON_ERR_PRESSURE

static inline ErrClass andonErrClass(uint8_t b) {
  return (b == ANDON_ERR_PRESSURE) ? ERR_CLASS_C1 : ERR_CLASS_NONE;
}

static inline bool andonIsMachineByte(uint8_t b) {
  return b >= ANDON_RX_INIT && b <= ANDON_RX_MATERIALIST;
}

static inline const char* andonRxName(uint8_t b) {
  switch (b) {
    case ANDON_RX_INIT:        return "InitState";
    case ANDON_RX_START:       return "StartCycle";
    case ANDON_RX_STOP:        return "StopCycle";
    case ANDON_RX_RESET:       return "ResetCycle";
    case ANDON_RX_IDLE:        return "IdleState";
    case ANDON_RX_BUSY:        return "BusyState";
    case ANDON_RX_ERROR:       return "ErrorState";
    case ANDON_RX_FINISH:      return "FinishParts";
    case ANDON_RX_RETURN:      return "ReturnState";
    case ANDON_RX_MATERIALIST: return "Materialist";
    default:                   return "unknown";
  }
}

void andonSetGreen(bool on);
void andonSetYellow(bool on);
void andonSetRed(bool on);
void andonSetBuzzer(bool on);
void andonSetBuzzerMute(bool mute);            // HMI Debug: mute buzzer
void andonTowerAllOff();
void andonApplyMachineByte(uint8_t byteCode);  // ANDON_RX_* desde HMI
void PressureError();                          // TX 0x50 → HMI
bool andonPressureFaultRaw();
void andonServicePressure();                   // pin → torreta Error + PressureError()
