"""Modulo Motion — cliente TCP, protocolo del esclavo Motion."""

from __future__ import annotations

import json
import urllib.parse
import urllib.request
from typing import Any, Callable, Optional

from tcp_link import ModuleTcpClient

# --- ASDA (bytes 0x01–0x08, maestro → esclavo / eventos) ---
CMD_HOME = 0x01
CMD_STOP = 0x02
CMD_OFF = 0x03
CMD_ON = 0x04
CMD_MOVE = 0x05
TX_REACHED = 0x06
CMD_MOVE_ZERO = 0x07
CMD_STATUS = 0x08

# --- Estatus general del modulo Motion (0x09–0x0E, esclavo → maestro) ---
TX_INIT = 0x09
TX_IDLE = 0x0A
TX_BUSY = 0x0B
TX_ERROR = 0x0C
TX_STOP_STATE = 0x0D
TX_RETURN = 0x0E
CMD_RESUME = 0x0E

# --- Encoder (bytes) ---
CMD_ENC_MEASURE_R = 0x0F
CMD_ENC_SET0_R = 0x10
TX_ENC_ERROR = 0x11  # detalle; coexiste con TX_ERROR (0x012)
CMD_ENC_MEASURE_L = 0x17
CMD_ENC_SET0_L = 0x18

# --- Feeder (bytes) ---
CMD_FEED_R = 0x12
CMD_FEED_L = 0x13
TX_LENGTH_OK_L = 0x14
TX_LENGTH_NG_L = 0x15
TX_LENGTH_OK_R = 0x4A
TX_LENGTH_NG_R = 0x4B
# Alias legacy (lado L)
TX_LENGTH_OK = TX_LENGTH_OK_L
TX_LENGTH_NG = TX_LENGTH_NG_L
CMD_MOT_RESET_ERR = 0x16

STATE_LABELS = {
    TX_INIT: "Motion — Init (0x009)",
    TX_IDLE: "Motion — Idle (0x010)",
    TX_BUSY: "Motion — Busy (0x011)",
    TX_ERROR: "Motion — Error (0x012)",
    TX_STOP_STATE: "Motion — Stop (0x013)",
    TX_RETURN: "Motion — ReturnStop (0x014)",
}

MOTION_STATE_BYTES = frozenset(
    {TX_INIT, TX_IDLE, TX_BUSY, TX_ERROR, TX_STOP_STATE, TX_RETURN}
)

DEFAULT_HOST = "10.10.32.20"
DEFAULT_PORT = 8767
MOTION_HTTP_PORT = 80

FEED_OFFSET_MM_MIN = -50.0
FEED_OFFSET_MM_MAX = 50.0


def clamp_feed_offset_mm(value: float) -> float:
    return max(FEED_OFFSET_MM_MIN, min(FEED_OFFSET_MM_MAX, float(value)))


def motion_http_get_feed_offset(
    host: str = DEFAULT_HOST,
    port: int = MOTION_HTTP_PORT,
    timeout: float = 3.0,
) -> dict[str, Any]:
    url = f"http://{host}:{port}/getFeedOffset"
    with urllib.request.urlopen(url, timeout=timeout) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    if not isinstance(data, dict):
        raise ValueError("Respuesta getFeedOffset invalida")
    return data


def motion_http_set_feed_offset(
    offset_l: float,
    offset_r: float,
    host: str = DEFAULT_HOST,
    port: int = MOTION_HTTP_PORT,
    timeout: float = 3.0,
) -> dict[str, Any]:
    offset_l = clamp_feed_offset_mm(offset_l)
    offset_r = clamp_feed_offset_mm(offset_r)
    q = urllib.parse.urlencode(
        {"offsetMm": f"{offset_l:.2f}", "offsetMmB": f"{offset_r:.2f}"}
    )
    url = f"http://{host}:{port}/setFeedOffset?{q}"
    with urllib.request.urlopen(url, timeout=timeout) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    if not isinstance(data, dict):
        raise ValueError("Respuesta setFeedOffset invalida")
    return data


class MotionClient(ModuleTcpClient):
    """Cliente TCP al esclavo Motion. Maneja enlace y reconexion en segundo plano."""

    def __init__(
        self,
        on_message: Optional[Callable[[dict], None]] = None,
        on_connection: Optional[Callable[[bool], None]] = None,
    ) -> None:
        super().__init__(DEFAULT_HOST, DEFAULT_PORT, on_message, on_connection)

    def _send_probe(self) -> bool:
        return self.send_command(byte=CMD_STATUS)

    def cmd_byte(self, byte_code: int, **extra: Any) -> bool:
        return self.send_command(byte=byte_code, **extra)

    def cmd_on(self) -> bool:
        return self.cmd_byte(CMD_ON)

    def cmd_stop(self) -> bool:
        return self.cmd_byte(CMD_STOP)

    def cmd_home(self, direction: str = "F") -> bool:
        return self.cmd_byte(CMD_HOME, direction=direction)

    def cmd_move_mm(self, mm: float, rpm: float) -> bool:
        mm = abs(float(mm))
        return self.cmd_byte(CMD_MOVE, mm=mm, speedRpm=float(rpm))

    def cmd_move_zero(self, rpm: float) -> bool:
        return self.cmd_byte(CMD_MOVE_ZERO, speedRpm=float(rpm))

    def cmd_status(self) -> bool:
        return self.cmd_byte(CMD_STATUS)

    def cmd_resume(self) -> bool:
        return self.cmd_byte(CMD_RESUME)

    def cmd_reset_error(self) -> bool:
        return self.cmd_byte(CMD_MOT_RESET_ERR)

    def cmd_enc_measure_r(self) -> bool:
        return self.cmd_byte(CMD_ENC_MEASURE_R)

    def cmd_enc_measure_l(self) -> bool:
        return self.cmd_byte(CMD_ENC_MEASURE_L)

    def cmd_enc_set0_r(self) -> bool:
        return self.cmd_byte(CMD_ENC_SET0_R)

    def cmd_enc_set0_l(self) -> bool:
        return self.cmd_byte(CMD_ENC_SET0_L)

    def cmd_enc_poll_both(self) -> bool:
        ok = self.cmd_enc_measure_r()
        return self.cmd_enc_measure_l() and ok

    def cmd_feed_r(self) -> bool:
        return self.cmd_byte(CMD_FEED_R)

    def cmd_feed_l(self) -> bool:
        return self.cmd_byte(CMD_FEED_L)

    def cmd_reset_errors(self) -> bool:
        return self.cmd_byte(CMD_MOT_RESET_ERR)
