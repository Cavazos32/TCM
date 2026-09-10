# Machine / ciclo TCM — opcodes 0x40–0x49 (fuente Python; HMI no usa .h)
# Andon refleja estos bytes en la torre. Detalle de errores de esclavos ≠ estos bytes.
#
# Flujo:
#   esclavo (error detalle) → HMI → Andon (estado máquina) + esclavos (Stop/Error de módulo)

MACH_INIT = 0x40        # InitState · Green
MACH_START = 0x41       # StartCycle
MACH_STOP = 0x42        # StopCycle · Red
MACH_RESET = 0x43       # ResetCycle
MACH_IDLE = 0x44        # IdleState · Green
MACH_BUSY = 0x45        # BusyState · Green
MACH_ERROR = 0x46       # ErrorState · Red + Buzzer
MACH_FINISH = 0x47      # FinishParts / LotCompleate · Green + Buzzer
MACH_RETURN = 0x48      # ReturnState
MACH_MATERIALIST = 0x49 # Materialist · Yellow + Buzzer

# Andon propio: TX only (pin FRL → torreta Error local + aviso HMI). No se recibe 0x50.
ANDON_ERR_PRESSURE = 0x50  # PressureError — baja/nula presión FRL

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
TX_RETURN = MACH_RETURN
TX_MATERIALIST = MACH_MATERIALIST

STATE_LABELS = {
    TX_INIT: "Máquina — Init (0x040)",
    TX_IDLE: "Máquina — Idle (0x044)",
    TX_BUSY: "Máquina — Busy (0x045)",
    TX_ERROR: "Máquina — Error (0x046)",
    TX_STOP: "Máquina — Stop (0x042)",
    TX_FINISH: "Máquina — FinishParts (0x047)",
    TX_RETURN: "Máquina — ReturnStop (0x048)",
    TX_MATERIALIST: "Máquina — Materialist (0x049)",
}

MACHINE_STATE_BYTES = frozenset(STATE_LABELS.keys())
