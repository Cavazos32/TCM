"""Catálogo oficial de errores EXXX (Doc/TCM - D.xlsx · Error list).

EXXX = identidad del fallo (detalle + texto UI) y su opcode.
Los ErrorState de módulo (0x0C / 0x27 / 0x3C / 0x46) NO son EXXX: informan
estado a otros módulos sin detalle.

UI: \"EXXX: Module, Descripción\"
Clases: C1 stop inmediato · C2 pausar · C3 terminar paso seguro
"""

from __future__ import annotations

from typing import Any, Optional

# Legacy classification kept only as catalog metadata for compatibility.
# It must not select machine recovery behavior.

# code, module, byte, enum, description, legacy_class
_LEGACY = "legacy"
_ROWS: tuple[tuple[str, str, int, str, str, str], ...] = (
    # --- Motion (detalle) ---
    ("E001", "Motion", 0x11, "MOT_ERR_ENCODER_R", "Encoder R; no cambio de valor", _LEGACY,
    ("E002", "Motion", 0x15, "MOT_ERR_LENGTH_NG_L", "Encoder L; longitud fuera de tolerancia", _LEGACY,
    ("E003", "Motion", 0x4B, "MOT_ERR_LENGTH_NG_R", "Encoder R; longitud fuera de tolerancia", _LEGACY,
    ("E004", "Motion", 0x4D, "MOT_ERR_LASER_R", "Sensor R; no detecto material", _LEGACY,
    ("E005", "Motion", 0x4E, "MOT_ERR_LASER_L", "Sensor L; no detecto material", _LEGACY,
    ("E006", "Motion", 0x4F, "MOT_ERR_EXHAUST", "Safety exhaust", _LEGACY,
    ("E015", "Motion", 0x59, "MOT_ERR_ACTUATOR_TARGET", "Actuador lineal no llego al destino", _LEGACY,
    ("E016", "Motion", 0x5A, "MOT_ERR_ASDA_MODBUS", "Fallo de comunicacion Modbus con ASDA", _LEGACY,
    ("E017", "Motion", 0x5B, "MOT_ERR_ACTUATOR_HOME", "Actuador lineal no realizo homing correctamente", _LEGACY,
    ("E018", "Motion", 0x5C, "MOT_ERR_ACTUATOR_OUT_ZONE", "Actuador lineal fuera de zona segura", _LEGACY,
    ("E019", "Motion", 0x5D, "MOT_ERR_ACTUATOR_ABORTED", "Movimiento actuador lineal abortado / fallido", _LEGACY,
    ("E020", "Motion", 0x5E, "MOT_ERR_RESET_WHILE_MOVING", "No se puede resetear error en movimiento", _LEGACY,
    ("E022", "Motion", 0x60, "MOT_ERR_ACTUATOR_NO_RESP", "Actuador lineal no responde", _LEGACY,
    ("E023", "Motion", 0x61, "MOT_ERR_FEED_CAN_NO_RESP", "Driver de servo feed no responde por CAN", _LEGACY,
    ("E024", "Motion", 0x62, "MOT_ERR_FEED_L_NEG_TARGET", "FEED: target L + offset negativo", _LEGACY,
    ("E025", "Motion", 0x63, "MOT_ERR_FEED_R_NEG_TARGET", "FEED: target R + offset negativo", _LEGACY,
    ("E026", "Motion", 0x64, "MOT_ERR_FEED_L_NO_FB", "Sin feedback de posicion Feeder 6064 L", _LEGACY,
    ("E027", "Motion", 0x65, "MOT_ERR_FEED_R_NO_FB", "Sin feedback de posicion Feeder 6064 R", _LEGACY,
    ("E028", "Motion", 0x66, "MOT_ERR_ENCODER_NO_PULSES", "Encoder sin incremento tras comando feed", _LEGACY,
    ("E029", "Motion", 0x67, "MOT_ERR_TOLERANCE_WINDOW", "Lectura fuera de ventana de tolerancia", _LEGACY,
    ("E030", "Motion", 0x68, "MOT_ERR_FEED_DIR_CW", "FEED: sentido horario", _LEGACY,
    ("E031", "Motion", 0x69, "MOT_ERR_FEED_TIMEOUT", "FEED: timeout", _LEGACY,
    ("E046", "Motion", 0x78, "MOT_ERR_ENCODER_L", "Encoder L; no cambio de valor", _LEGACY,
    # --- Cycle (HMI) ---
    ("E008", "Cycle", 0x52, "CYC_ERR_SERVO_IDLE_TIMEOUT", "Timeout esperando Idle/Reached de servo", _LEGACY,
    ("E009", "Cycle", 0x53, "CYC_ERR_LENGTH_OK_TIMEOUT", "Timeout esperando LengthOK", _LEGACY,
    ("E010", "Cycle", 0x54, "CYC_ERR_HOME_FAILED", "Comando HOME fallo", _LEGACY,
    ("E011", "Cycle", 0x55, "CYC_ERR_FEED_START_FAILED", "Feed L+R fallo al iniciar", _LEGACY,
    ("E012", "Cycle", 0x56, "CYC_ERR_MOVE_ABS_FAILED", "Move ABS fallo", _LEGACY,
    ("E013", "Cycle", 0x57, "CYC_ERR_DROP_CMD_FAILED", "Comando de deposito fallo", _LEGACY,
    ("E014", "Cycle", 0x58, "CYC_ERR_FEED_INCOMPLETE", "Alimentacion incompleta", _LEGACY,
    # --- PLC ---
    ("E047", "PLC", 0x1F, "PLC_ERR_CUTTER", "Cutter error", _LEGACY,
    # GripperE: falla mecánica del gripper y/o presión de aire baja·nula
    ("E048", "PLC", 0x20, "PLC_ERR_GRIPPER", "Gripper / presion de aire baja o nula", _LEGACY,
    ("E049", "PLC", 0x21, "PLC_ERR_HOLDER", "Holder error", _LEGACY,
    # EncoderE (bandeja FG): también aire baja·nula, presencia de manguera o falla de cilindro
    # E050 C1 fuera de lote. Con pieza/lote activo: Pause + pregunta (normas § E050).
    ("E050", "PLC", 0x22, "PLC_ERR_ENCODER", "Encoder / aire, manguera o cilindro", _LEGACY,
    # --- PreFeeder ---
    ("E052", "Pre-Feeder", 0x2D, "PF_ERR_BUFFER_FULL_R", "Buffer sin relleno", _LEGACY,
    ("E053", "Pre-Feeder", 0x2E, "PF_ERR_BUFFER_MAX_R", "Buffer Max (endstop)", _LEGACY,
    ("E054", "Pre-Feeder", 0x2F, "PF_ERR_TENSION_R", "Tension timeout", _LEGACY,
    ("E055", "Pre-Feeder", 0x30, "PF_ERR_CILINDRO_R", "Cilindro abierto", _LEGACY,
    ("E056", "Pre-Feeder", 0x31, "PF_ERR_MANGUERA_R", "Manguera ausente", _LEGACY,
    ("E057", "Pre-Feeder", 0x32, "PF_ERR_HOLGURA_R", "Sin holgura", _LEGACY,
    ("E058", "Pre-Feeder", 0x33, "PF_ERR_BUFFER_FULL_L", "Buffer sin relleno", _LEGACY,
    ("E059", "Pre-Feeder", 0x34, "PF_ERR_BUFFER_MAX_L", "Buffer Max (endstop)", _LEGACY,
    ("E060", "Pre-Feeder", 0x35, "PF_ERR_TENSION_L", "Tension timeout", _LEGACY,
    ("E061", "Pre-Feeder", 0x36, "PF_ERR_CILINDRO_L", "Cilindro abierto", _LEGACY,
    ("E062", "Pre-Feeder", 0x37, "PF_ERR_MANGUERA_L", "Manguera ausente", _LEGACY,
    ("E063", "Pre-Feeder", 0x38, "PF_ERR_HOLGURA_L", "Sin holgura", _LEGACY,
    ("E069", "Pre-Feeder", 0x7D, "PF_ERR_NOT_INITIALIZED", "PreFeeder sin inicializar", _LEGACY,
    # --- Andon ---
    ("E064", "Andon", 0x50, "ANDON_ERR_PRESSURE", "Baja / nula presion", _LEGACY,
    # --- Maquina / Main ---
    ("E065", "Maquina / Main", 0x79, "MAIN_ERR_MOTION_ESP_DISCONNECTED", "Socket caido con Motion ESP (:8767) / Sin conexion TCP", _LEGACY,
    ("E066", "Maquina / Main", 0x7A, "MAIN_ERR_PLC_ESP_DISCONNECTED", "Socket caido con PLC ESP (:8766) / Sin conexion TCP", _LEGACY,
    ("E067", "Maquina / Main", 0x7B, "MAIN_ERR_PF_MASTER_DISCONNECTED", "Socket caido con PF Master (:8768) / Sin conexion TCP", _LEGACY,
    ("E068", "Maquina / Main", 0x7C, "MAIN_ERR_CYCLE_ABORTED", "Ciclo abortado", _LEGACY,
)

BY_CODE: dict[str, dict[str, Any]] = {}
BY_BYTE: dict[int, dict[str, Any]] = {}
BY_ENUM: dict[str, dict[str, Any]] = {}

for _code, _mod, _byte, _enum, _desc, _cls in _ROWS:
    _entry = {
        "code": _code,
        "module": _mod,
        "byte": _byte,
        "enum": _enum,
        "description": _desc,
        "legacy_class": _cls,
    }
    BY_CODE[_code] = _entry
    BY_BYTE[_byte] = _entry
    BY_ENUM[_enum] = _entry

# Slugs legacy de cycle.py → código EXXX
CYCLE_FAULT_TO_CODE: dict[str, str] = {
    "timeout_motion": "E008",
    "timeout_feed": "E009",
    "home_cmd": "E010",
    "feed_cmd": "E011",
    "move_cmd": "E012",
    "deposit_cmd": "E013",
    "clearance_cmd": "E013",
    "wip_blower": "E010",
    "wip_start": "E010",
    "feed_incomplete": "E014",
    "aborted": "E068",
    "cycle_failed": "E068",
    "prefeeder_all_ok": "E069",
    "pf_trigger": "E069",
}


def format_ui(code_or_byte: str | int, *, fallback: Optional[str] = None) -> str:
    """Formato UI: 'EXXX: Module, Descripción'."""
    entry = lookup(code_or_byte)
    if entry is None:
        if fallback:
            return fallback
        if isinstance(code_or_byte, int):
            return f"E???: Unknown, byte 0x{code_or_byte:02X}"
        return f"{code_or_byte}: Unknown"
    return f"{entry['code']}: {entry['module']}, {entry['description']}"


def lookup(code_or_byte: str | int) -> Optional[dict[str, Any]]:
    if isinstance(code_or_byte, int):
        return BY_BYTE.get(code_or_byte)
    s = str(code_or_byte).strip()
    if s in BY_CODE:
        return BY_CODE[s]
    if s in BY_ENUM:
        return BY_ENUM[s]
    if s in CYCLE_FAULT_TO_CODE:
        return BY_CODE.get(CYCLE_FAULT_TO_CODE[s])
    if s.startswith("exception:"):
        return BY_CODE.get("E068")
    return None


def is_detail_error_byte(byte: int) -> bool:
    """True si el byte es un EXXX de detalle (no ErrorState de módulo)."""
    return byte in BY_BYTE


# Etiquetas cortas para paneles (mismo formato UI)
ERROR_LABELS: dict[int, str] = {b: format_ui(b) for b in BY_BYTE}
