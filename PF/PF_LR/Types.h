#pragma once

#include <stdint.h>

// Enums internos del firmware. Catálogo PF-xxx y estados operativos: Status_Mode.h

enum Motor2Phase : uint8_t {
  M2_PHASE_IDLE = 0,
  M2_PHASE_TRIGGER_FEED   // disparo TCP → alimenta Tfeed s
};

enum SystemFault : uint8_t {
  FAULT_NONE = 0,
  FAULT_ENDSTOP,
  FAULT_TENSION_TIMEOUT,
  FAULT_CYLINDER_OPEN,
  FAULT_HOSE_ABSENT,
  FAULT_BUFFER_TIMEOUT,
  FAULT_HOLGURA_TIMEOUT,
  FAULT_OPERATOR_STOP   // Detener en UI: enclavado hasta Reset
};

enum AutoState : uint8_t {
  AUTO_OFF,
  AUTO_HOME_HOLD,      // Buffer Full activo
  AUTO_SERVO_LEAD,     // Buffer Full inactivo: servo ON, DeReeler espera
  AUTO_CW,
  AUTO_ENDSTOP_FAULT,
  AUTO_TENSION_FAULT,
  AUTO_CYLINDER_FAULT,
  AUTO_HOSE_FAULT,
  AUTO_BUFFER_FAULT,
  AUTO_HOLGURA_FAULT,
  AUTO_OPERATOR_STOP
};
