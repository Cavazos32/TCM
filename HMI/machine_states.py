# Machine / ciclo TCM — opcodes 0x40–0x49 + errores Cycle/Main (EXXX)
# Fuente Python; HMI no usa .h de firmware.
#
# Flujo flip-flop:
#   Set (detalle EXXX del esclavo / ciclo) → HMI latchea → UI + clase C1/C2/C3
#                                         → estado máquina 0x46 a Andon / Stop a esclavos
#   Res (Reset HMI) → limpia latch + reset por módulo
#
# Estados MACH_* informan sin detalle. EXXX = identidad del fallo (solo HMI).

# --- Estados máquina / Andon RX (no EXXX) ---
# Torre: Green / Red / N/A / Red+Buzzer / seq RGB+Buzzer / Yellow / Yellow+Buzzer
MACH_INIT = 0x40        # InitState · Green
MACH_START = 0x41       # StartCycle · N/A (torre no cambia)
MACH_STOP = 0x42        # StopCycle · Red
MACH_RESET = 0x43       # ResetCycle · N/A
MACH_IDLE = 0x44        # IdleState · Green
MACH_BUSY = 0x45        # BusyState · Green (máquina trabajando)
MACH_ERROR = 0x46       # ErrorState · Red + Buzzer (prioridad)
MACH_FINISH = 0x47      # FinishParts / LotCompleate · seq R→Y→G + Buzzer (temporal)
MACH_PAUSE = 0x48       # Pause · Yellow (sin buzzer; distinto de Materialist)
MACH_MATERIALIST = 0x49 # Materialist · Yellow + Buzzer
MACH_RETURN = MACH_PAUSE  # alias histórico ReturnStop

# Andon propio: TX only (E064) — pin FRL → torreta + aviso HMI
ANDON_ERR_PRESSURE = 0x50  # E064 C1 · PressureError

# --- Cycle (HMI) — EXXX E008–E014 ---
CYC_ERR_SERVO_IDLE_TIMEOUT = 0x52  # E008 C1
CYC_ERR_LENGTH_OK_TIMEOUT = 0x53   # E009 C1
CYC_ERR_HOME_FAILED = 0x54         # E010 C1
CYC_ERR_FEED_START_FAILED = 0x55   # E011 C1
CYC_ERR_MOVE_ABS_FAILED = 0x56     # E012 C1
CYC_ERR_DROP_CMD_FAILED = 0x57     # E013 C1
CYC_ERR_FEED_INCOMPLETE = 0x58     # E014 C2

# --- Main / enlace — EXXX E065–E068 ---
MAIN_ERR_MOTION_ESP_DISCONNECTED = 0x79  # E065 C1
MAIN_ERR_PLC_ESP_DISCONNECTED = 0x7A     # E066 C1
MAIN_ERR_PF_MASTER_DISCONNECTED = 0x7B   # E067 C1
MAIN_ERR_CYCLE_ABORTED = 0x7C            # E068 C1

# Aliases usados por cycle.py
CMD_START = MACH_START
CMD_STOP = MACH_STOP
CMD_RESET = MACH_RESET
TX_INIT = MACH_INIT
TX_IDLE = MACH_IDLE
TX_BUSY = MACH_BUSY
TX_ERROR = MACH_ERROR
TX_STOP = MACH_STOP
TX_FINISH = MACH_FINISH
TX_PAUSE = MACH_PAUSE
TX_RETURN = MACH_PAUSE  # alias histórico
TX_MATERIALIST = MACH_MATERIALIST

STATE_LABELS = {
    TX_INIT: "Máquina — Init (0x040)",
    TX_IDLE: "Máquina — Idle (0x044)",
    TX_BUSY: "Máquina — Busy (0x045)",
    TX_ERROR: "Máquina — Error (0x046)",
    TX_STOP: "Máquina — Stop (0x042)",
    TX_FINISH: "Máquina — FinishParts (0x047)",
    TX_PAUSE: "Máquina — Pause (0x048)",
    TX_MATERIALIST: "Máquina — Materialist (0x049)",
}

MACHINE_STATE_BYTES = frozenset(STATE_LABELS.keys())
