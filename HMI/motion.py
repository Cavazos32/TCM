"""Modulo Motion — cliente TCP, protocolo del esclavo Motion."""

from __future__ import annotations

import json
import urllib.error
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
# Lado lógico R = OM físico izquierdo (Motion GPIO 18/19/21)
# Lado lógico L = OM físico derecho   (Motion GPIO 22/23/25)
CMD_ENC_MEASURE_R = 0x0F
CMD_ENC_SET0_R = 0x10
TX_ENC_ERROR = 0x11  # E001 detalle; coexiste con TX_ERROR (0x0C estado)
TX_ENC_ERROR_L = 0x78  # E046
CMD_ENC_MEASURE_L = 0x17
CMD_ENC_SET0_L = 0x18

# --- Feeder (bytes) ---
CMD_FEED_R = 0x12
CMD_FEED_L = 0x13
TX_LENGTH_OK_L = 0x14
TX_LENGTH_NG_L = 0x15  # E002
TX_LENGTH_OK_R = 0x4A
TX_LENGTH_NG_R = 0x4B  # E003
# Alias legacy (lado L)
TX_LENGTH_OK = TX_LENGTH_OK_L
TX_LENGTH_NG = TX_LENGTH_NG_L
CMD_MOT_RESET_ERR = 0x16

# --- Errores detalle Motion (EXXX → solo HMI) ---
TX_LASER_R = 0x4D
TX_LASER_L = 0x4E
TX_EXHAUST = 0x4F
MOTION_DETAIL_ERROR_BYTES = frozenset(
    {
        TX_ENC_ERROR,
        TX_LENGTH_NG_L,
        TX_LENGTH_NG_R,
        TX_LASER_R,
        TX_LASER_L,
        TX_EXHAUST,
        0x59,
        0x5A,
        0x5B,
        0x5C,
        0x5D,
        0x5E,
        0x60,
        0x61,
        0x62,
        0x63,
        0x64,
        0x65,
        0x66,
        0x67,
        0x68,
        0x69,
        TX_ENC_ERROR_L,
    }
)

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


def motion_http_stage2_status(
    host: str = DEFAULT_HOST,
    port: int = MOTION_HTTP_PORT,
    timeout: float = 3.0,
) -> dict[str, Any]:
    url = f"http://{host}:{port}/api/stage2/status"
    with urllib.request.urlopen(url, timeout=timeout) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    if not isinstance(data, dict):
        raise ValueError("Respuesta stage2/status invalida")
    return data


def motion_http_asda_status(
    host: str = DEFAULT_HOST,
    port: int = MOTION_HTTP_PORT,
    timeout: float = 3.0,
) -> dict[str, Any]:
    """GET /api/status — ok=true solo si Modbus ASDA responde (P5.007 + pos).

    Con drive sin alimentación Motion suele devolver HTTP 503 y ok=false.
    """
    url = f"http://{host}:{port}/api/status"
    try:
        with urllib.request.urlopen(url, timeout=timeout) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        raw = exc.read().decode("utf-8", errors="replace")
        try:
            data = json.loads(raw) if raw else {"ok": False}
        except json.JSONDecodeError:
            data = {"ok": False, "error": raw or f"HTTP {exc.code}"}
    if not isinstance(data, dict):
        raise ValueError("Respuesta /api/status invalida")
    return data


def motion_http_asda_position_mm(
    host: str = DEFAULT_HOST,
    port: int = MOTION_HTTP_PORT,
    timeout: float = 0.4,
) -> float | None:
    """GET /api/status → positionMm (live). None si no hay dato usable."""
    try:
        data = motion_http_asda_status(host=host, port=port, timeout=timeout)
    except Exception:  # noqa: BLE001 — sondeo best-effort durante HOME
        return None
    raw = data.get("positionMm")
    if raw is None:
        return None
    try:
        return float(raw)
    except (TypeError, ValueError):
        return None


def motion_http_stage2_start(
    piece_mm: float | None = None,
    sides: str = "LR",
    host: str = DEFAULT_HOST,
    port: int = MOTION_HTTP_PORT,
    timeout: float = 5.0,
    *,
    target_abs_mm: float | None = None,
) -> dict[str, Any]:
    """Inicia Stage2 en Motion (FSM local).

    - target_abs_mm: carrera ABS del lineal (contrato CYCLE). Preferido.
    - piece_mm: longitud de pieza L (test Motion HTML); target = L−55.
    Compat: si solo hay abs, también envía pieceMm=abs+55 por si firmware
    aún no entiende targetAbsMm.
    """
    payload: dict[str, Any] = {"sides": str(sides or "LR")}
    if target_abs_mm is not None:
        abs_mm = abs(float(target_abs_mm))
        payload["targetAbsMm"] = abs_mm
        # Compat firmware previo: pieceMm = abs + 55 → Motion target = abs
        payload["pieceMm"] = abs_mm + 55.0
    elif piece_mm is not None:
        payload["pieceMm"] = float(piece_mm)
    else:
        raise ValueError("stage2/start requiere target_abs_mm o piece_mm")
    body = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        f"http://{host}:{port}/api/stage2/start",
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = ""
        try:
            raw = exc.read().decode("utf-8", errors="replace")
            parsed = json.loads(raw) if raw else {}
            if isinstance(parsed, dict):
                detail = str(parsed.get("error") or raw or "").strip()
            else:
                detail = raw.strip()
        except Exception:  # noqa: BLE001
            detail = ""
        msg = f"HTTP Error {exc.code}: {exc.reason}"
        if detail:
            msg = f"{msg} — {detail}"
        raise RuntimeError(msg) from exc
    if not isinstance(data, dict):
        raise ValueError("Respuesta stage2/start invalida")
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
        # Keepalive de enlace (tcp_link), no GetStatus/Modbus (regla C1).
        return self.send_command(command="ping")

    def cmd_byte(self, byte_code: int, **extra: Any) -> bool:
        return self.send_command(byte=byte_code, **extra)

    def cmd_on(self) -> bool:
        return self.cmd_byte(CMD_ON)

    def cmd_off(self) -> bool:
        return self.cmd_byte(CMD_OFF)

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

    def cmd_feed_r(self, *, skip_validate: bool = False) -> bool:
        if skip_validate:
            return self.cmd_byte(CMD_FEED_R, skipValidate=True)
        return self.cmd_byte(CMD_FEED_R)

    def cmd_feed_l(self, *, skip_validate: bool = False) -> bool:
        if skip_validate:
            return self.cmd_byte(CMD_FEED_L, skipValidate=True)
        return self.cmd_byte(CMD_FEED_L)

    def cmd_reset_errors(self) -> bool:
        return self.cmd_byte(CMD_MOT_RESET_ERR)
