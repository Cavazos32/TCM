"""Modulo PreFeeder — cliente TCP al Master (orquestador L/R)."""

from __future__ import annotations

from typing import Any, Callable, Optional

from tcp_link import ModuleTcpClient

# --- Comandos maestro → PreFeeder Master (0x2A–0x2C, 0x3F, 0x4C, 0x51) ---
CMD_START = 0x2A
CMD_STOP = 0x2B
CMD_RESET = 0x2C
CMD_MATERIALIST = 0x3F
CMD_TRIGGER_R = 0x4C  # TriggerR() — Tfeed lado R
CMD_TRIGGER_L = 0x51  # TriggerL() — Tfeed lado L

# --- Errores por lado (esclavo → maestro, 0x2D–0x38) ---
TX_BUFFER_FULL_R = 0x2D
TX_BUFFER_MAX_R = 0x2E
TX_TENSION_R = 0x2F
TX_CILINDRO_R = 0x30
TX_MANGUERA_R = 0x31
TX_HOLGURA_R = 0x32
TX_BUFFER_FULL_L = 0x33
TX_BUFFER_MAX_L = 0x34
TX_TENSION_L = 0x35
TX_CILINDRO_L = 0x36
TX_MANGUERA_L = 0x37
TX_HOLGURA_L = 0x38

PF_ERROR_BYTES_R = frozenset(
    {
        TX_BUFFER_FULL_R,
        TX_BUFFER_MAX_R,
        TX_TENSION_R,
        TX_CILINDRO_R,
        TX_MANGUERA_R,
        TX_HOLGURA_R,
    }
)
PF_ERROR_BYTES_L = frozenset(
    {
        TX_BUFFER_FULL_L,
        TX_BUFFER_MAX_L,
        TX_TENSION_L,
        TX_CILINDRO_L,
        TX_MANGUERA_L,
        TX_HOLGURA_L,
    }
)
PF_ERROR_BYTES = PF_ERROR_BYTES_R | PF_ERROR_BYTES_L

# --- Estatus general PreFeeder (0x39–0x3E) ---
TX_PF_INIT = 0x39
TX_PF_IDLE = 0x3A
TX_PF_BUSY = 0x3B
TX_PF_ERROR = 0x3C
TX_PF_STOP = 0x3D
TX_PF_RETURN = 0x3E

from error_catalog import ERROR_LABELS  # EXXX: Module, Descripción (E052–E063, E069)

STATE_LABELS = {
    TX_PF_INIT: "Init (0x039)",
    TX_PF_IDLE: "Idle (0x03A)",
    TX_PF_BUSY: "Busy (0x03B)",
    TX_PF_ERROR: "Error (0x03C)",
    TX_PF_STOP: "Stop (0x03D)",
    TX_PF_RETURN: "ReturnStop (0x03E)",
}

PF_STATE_BYTES = frozenset(
    {TX_PF_INIT, TX_PF_IDLE, TX_PF_BUSY, TX_PF_ERROR, TX_PF_STOP, TX_PF_RETURN}
)

DEFAULT_HOST = "10.10.32.100"
DEFAULT_PORT = 8768


class PreFeederClient(ModuleTcpClient):
    """Cliente TCP al PreFeeder Master. Enlace y reconexion en segundo plano."""

    def __init__(
        self,
        on_message: Optional[Callable[[dict], None]] = None,
        on_connection: Optional[Callable[[bool], None]] = None,
    ) -> None:
        super().__init__(DEFAULT_HOST, DEFAULT_PORT, on_message, on_connection)

    def _send_probe(self) -> bool:
        return self.cmd_byte(TX_PF_IDLE)

    def cmd_byte(self, byte_code: int, **extra: Any) -> bool:
        return self.send_command(byte=byte_code, **extra)

    def cmd_start(self) -> bool:
        return self.cmd_byte(CMD_START)

    def cmd_stop(self) -> bool:
        return self.cmd_byte(CMD_STOP)

    def cmd_reset(self) -> bool:
        return self.cmd_byte(CMD_RESET)

    def cmd_materialist(self, on: bool = True) -> bool:
        """Materialista L+R (0x3F). on=False sale del modo."""
        return self.send_command(
            byte=CMD_MATERIALIST, value="1" if on else "0"
        )

    def cmd_in_process(self, on: bool = True) -> bool:
        """In process L+R (misma ruta peer que Master /api/auto?in_process=)."""
        return self.send_command(
            command="setInProcess", value="1" if on else "0"
        )

    def cmd_trigger_r(self) -> bool:
        return self.cmd_byte(CMD_TRIGGER_R)

    def cmd_trigger_l(self) -> bool:
        return self.cmd_byte(CMD_TRIGGER_L)

    def cmd_status(self) -> bool:
        return self.cmd_byte(TX_PF_IDLE)
