#pragma once
// Colas internas PF_LR: Communication → Control / Feeder.
// No cambia el protocolo TCP externo.

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "Config.h"

enum class PfCmdKind : uint8_t {
  Start = 1,
  Stop,
  Reset,
  Halt,
  SetInProcess,
  SetIdleMode,
  Trigger,          // también va a cola Tfeed; Control solo loguea/ACK estado
  Refill,
  SetRefillPulseS,
  SetTriggerFeedS,
  SetAllCfg,
  SetServoPwm,      // UI local
  SetAutoRpm,
  SetMotor1Jog,     // HTTP jog DeReeler (Control aplica)
  Nop
};

struct PfCmd {
  PfCmdKind kind;
  uint16_t  id;
  float     f0;
  float     f1;
  float     f2;
  uint8_t   u0;     // flags: bit0=on, bit1=hold, bit2=saveNvs
  char      which[12]; // refill which / cfg tag
  char      val[96];   // CSV / string payload
};

constexpr UBaseType_t PF_CMD_QUEUE_DEPTH = 24;

inline QueueHandle_t& pfCmdQueue()
{
  static QueueHandle_t q = nullptr;
  return q;
}

inline bool pfCmdQueueInit()
{
  if (pfCmdQueue()) return true;
  pfCmdQueue() = xQueueCreate(PF_CMD_QUEUE_DEPTH, sizeof(PfCmd));
  return pfCmdQueue() != nullptr;
}

inline bool pfCmdEnqueue(const PfCmd& cmd, TickType_t wait = 0)
{
  if (!pfCmdQueue()) return false;
  return xQueueSend(pfCmdQueue(), &cmd, wait) == pdTRUE;
}

inline bool pfCmdDequeue(PfCmd* out, TickType_t wait = 0)
{
  if (!pfCmdQueue() || !out) return false;
  return xQueueReceive(pfCmdQueue(), out, wait) == pdTRUE;
}

inline PfCmd pfCmdMake(PfCmdKind k)
{
  PfCmd c{};
  c.kind = k;
  return c;
}
