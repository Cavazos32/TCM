#pragma once

#include <stdint.h>

// Enums internos del firmware. Catálogo PF-xxx y estados operativos: Status_Mode.h

enum Motor2Phase : uint8_t {
  M2_PHASE_IDLE = 0,
  M2_PHASE_TIMED_FEED     // feeder por tiempo (TCM trigger TFEED)
};

enum Motor2FeedSource : uint8_t {
  M2_FEED_NONE = 0,
  M2_FEED_TCP        // mensaje TCM/Master (opcode 0x4C / 0x51)
};

enum SystemFault : uint8_t {
  FAULT_NONE = 0,
  FAULT_ENDSTOP,
  FAULT_TENSION_TIMEOUT,
  FAULT_CYLINDER_OPEN,
  FAULT_HOSE_ABSENT,
  FAULT_BUFFER_TIMEOUT,
  FAULT_OPERATOR_STOP   // Detener en UI: enclavado hasta Reset
  // FAULT_HOLGURA eliminado del producto
};

enum AutoState : uint8_t {
  AUTO_OFF,
  AUTO_HOME_HOLD,      // Buffer Full activo
  AUTO_SERVO_LEAD,     // Buffer Full inactivo: servo ON, DeReeler espera
  AUTO_CW,
  AUTO_CCW,            // Inversión por tensión (GPIO 23); al terminar → AUTO_CW
  AUTO_ENDSTOP_FAULT,
  AUTO_TENSION_FAULT,
  AUTO_CYLINDER_FAULT,
  AUTO_HOSE_FAULT,
  AUTO_BUFFER_FAULT,
  AUTO_OPERATOR_STOP
};
