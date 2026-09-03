"""Modulo PLC — cliente TCP, protocolo válvulas/sensores PLCA."""

from __future__ import annotations

from typing import Any, Callable, Optional

from tcp_link import ModuleTcpClient

# --- Protocolo PLC (bytes 0x19–0x23, exclusivos de PLCA) ---
CMD_CUTTER_R = 0x19
CMD_CUTTER_L = 0x1A
CMD_GRIPPER = 0x1B
CMD_HOLDER = 0x1C
CMD_ENCODER = 0x1D
CMD_RESET = 0x1E
TX_CUTTER_ERR = 0x1F
TX_GRIPPER_ERR = 0x20
TX_HOLDER_ERR = 0x21
TX_ENCODER_ERR = 0x22
CMD_BLOWER = 0x23

# --- Estatus PLC (bytes 0x24–0x29, esclavo → maestro) ---
TX_PLC_INIT = 0x24
TX_PLC_IDLE = 0x25
TX_PLC_BUSY = 0x26
TX_PLC_ERROR = 0x27
TX_PLC_STOP = 0x28
TX_PLC_RETURN = 0x29

ERROR_LABELS = {
    TX_CUTTER_ERR: "Error cutter (0x01F)",
    TX_GRIPPER_ERR: "Error gripper (0x020)",
    TX_HOLDER_ERR: "Error holder (0x021)",
    TX_ENCODER_ERR: "Error encoder (0x022)",
}

STATE_LABELS = {
    TX_PLC_INIT: "Init (0x024)",
    TX_PLC_IDLE: "Idle (0x025)",
    TX_PLC_BUSY: "Busy (0x026)",
    TX_PLC_ERROR: "Error (0x027)",
    TX_PLC_STOP: "Stop (0x028)",
    TX_PLC_RETURN: "ReturnStop (0x029)",
}

PLC_VALVE_OUT_NAMES = {
    CMD_CUTTER_R: "CUTTER_R",
    CMD_CUTTER_L: "CUTTER_L",
    CMD_GRIPPER: "GRIPPERS",
    CMD_HOLDER: "HOLDER",
    CMD_ENCODER: "ENCODER",
    CMD_BLOWER: "BLOWER",
}

PLC_VALVE_LABELS = {
    CMD_CUTTER_R: "Cutter R",
    CMD_CUTTER_L: "Cutter L",
    CMD_GRIPPER: "Gripper",
    CMD_HOLDER: "Holder",
    CMD_ENCODER: "Encoder",
    CMD_BLOWER: "Blower",
}

DEFAULT_HOST = "10.10.32.50"
DEFAULT_PORT = 8766


class PlcClient(ModuleTcpClient):
    """Cliente TCP al esclavo PLCA. Maneja enlace y reconexion en segundo plano."""

    def __init__(
        self,
        on_message: Optional[Callable[[dict], None]] = None,
        on_connection: Optional[Callable[[bool], None]] = None,
    ) -> None:
        super().__init__(DEFAULT_HOST, DEFAULT_PORT, on_message, on_connection)

    def _send_probe(self) -> bool:
        return self.send_command(command="poll")

    def cmd_byte(self, byte_code: int, **extra: Any) -> bool:
        return self.send_command(byte=byte_code, **extra)

    def cmd_valve(self, byte_code: int, on: bool = True) -> bool:
        return self.cmd_byte(byte_code, on=on)

    def cmd_set_out(self, out_name: str, on: bool = True) -> bool:
        return self.send_command(
            command="setOut",
            out=out_name,
            value="on" if on else "off",
        )

    def cmd_valve_by_byte(self, byte_code: int, on: bool = True) -> bool:
        out_name = PLC_VALVE_OUT_NAMES.get(byte_code)
        if out_name:
            return self.cmd_set_out(out_name, on)
        return self.cmd_valve(byte_code, on)

    def cmd_cutter_r(self, on: bool = True) -> bool:
        return self.cmd_valve(CMD_CUTTER_R, on)

    def cmd_cutter_l(self, on: bool = True) -> bool:
        return self.cmd_valve(CMD_CUTTER_L, on)

    def cmd_gripper(self, on: bool = True) -> bool:
        return self.cmd_valve(CMD_GRIPPER, on)

    def cmd_holder(self, on: bool = True) -> bool:
        return self.cmd_valve(CMD_HOLDER, on)

    def cmd_encoder(self, on: bool = True) -> bool:
        return self.cmd_valve(CMD_ENCODER, on)

    def cmd_blower(self, on: bool = True) -> bool:
        return self.cmd_valve(CMD_BLOWER, on)

    def cmd_reset(self) -> bool:
        return self.cmd_byte(CMD_RESET, on=True)

    def cmd_all_off(self) -> bool:
        return self.send_command(command="allOff")

    def cmd_poll(self) -> bool:
        return self.send_command(command="poll")

    def cmd_status(self) -> bool:
        return self.cmd_byte(TX_PLC_IDLE)
