"""Estado compartido HMI — lógica TCP y estado para Flask."""

from __future__ import annotations

import json
import os
import threading
import time
from collections import deque
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any, Callable

HMI_ROOT = Path(__file__).resolve().parent
PLC_CONFIG_PATH = HMI_ROOT / "config" / "plc_config.json"
APP_CONFIG_PATH = HMI_ROOT / "config" / "app_config.json"
DEFAULT_BLOWER_SEC = 2.0
DEFAULT_ANDON_BUZZER_MUTE = False
# CMD_MOVE (0x05): reintentos de transporte acotados (no loop infinito).
MOTION_MOVE_TX_ATTEMPTS = 3  # 1 envío + 2 reintentos máx.
# Peek corto: si el ACK ya está, se usa. Si no, el ciclo sigue a Reached
# (como HOME). Esperar 6 s aquí congelaba lineal/depósito/despeje 0.4–0.9 s.
MOTION_MOVE_ACK_PEEK_S = 0.02
MOTION_MOVE_ACK_TIMEOUT_S = 6.0  # solo recuperación de transporte / boot
MOTION_MOVE_LINK_WAIT_S = 5.0
# SSE/poll: cola interna 400; al aire solo cola reciente (UI no reparsea 2000).
SSE_LOG_TAIL = 48


@dataclass
class _PendingAsdaAck:
    """Espera ACK ASDA de un byte concreto (anti-duplicado en retry MOVE)."""

    byte: int
    gen: int
    event: threading.Event = field(default_factory=threading.Event)
    ok: bool | None = None
    message: str = ""

from cycle import CycleConfig, CycleRunner, PROGRESS_STEPS
from motion import (
    CMD_ENC_MEASURE_L,
    CMD_ENC_MEASURE_R,
    CMD_ENC_SET0_L,
    CMD_ENC_SET0_R,
    CMD_FEED_L,
    CMD_FEED_R,
    CMD_MOVE,
    CMD_MOVE_ZERO,
    CMD_OFF,
    CMD_ON,
    DEFAULT_HOST,
    DEFAULT_PORT,
    MOTION_DETAIL_ERROR_BYTES,
    MOTION_STATE_BYTES,
    MotionClient,
    TX_BUSY,
    TX_ENC_ERROR,
    TX_ERROR,
    TX_EXHAUST,
    TX_IDLE,
    TX_INIT,
    TX_LASER_L,
    TX_LASER_R,
    TX_LENGTH_NG,
    TX_LENGTH_NG_L,
    TX_LENGTH_NG_R,
    TX_LENGTH_OK,
    TX_LENGTH_OK_L,
    TX_LENGTH_OK_R,
    motion_http_asda_position_mm,
    motion_http_asda_status,
    motion_http_stage2_start,
    motion_http_stage2_status,
    TX_REACHED,
    TX_RETURN,
    TX_STOP_STATE,
    clamp_feed_offset_mm,
    motion_http_get_feed_offset,
    motion_http_set_feed_offset,
)
from plc import (
    CMD_BLOWER,
    CMD_CUTTER_L,
    CMD_CUTTER_R,
    CMD_ENCODER,
    CMD_GRIPPER,
    CMD_HOLDER,
    CMD_RESET,
    DEFAULT_HOST as PLC_HOST,
    DEFAULT_PORT as PLC_PORT,
    ERROR_LABELS,
    PLC_VALVE_LABELS,
    PLC_VALVE_OUT_NAMES,
    PlcClient,
    TX_CUTTER_ERR,
    TX_ENCODER_ERR,
    TX_GRIPPER_ERR,
    TX_HOLDER_ERR,
    TX_PLC_BUSY,
    TX_PLC_ERROR,
    TX_PLC_IDLE,
    TX_PLC_INIT,
    TX_PLC_RETURN,
    TX_PLC_STOP,
)
from tcp_link import CONNECT_TIMEOUT, VERIFY_TIMEOUT_SEC, ModuleTcpClient
from prefeeder import (
    CMD_MATERIALIST,
    CMD_RESET as PF_CMD_RESET,
    CMD_START as PF_CMD_START,
    CMD_STOP as PF_CMD_STOP,
    DEFAULT_HOST as PF_HOST,
    DEFAULT_PORT as PF_PORT,
    ERROR_LABELS as PF_ERROR_LABELS,
    PF_ERROR_BYTES,
    PF_ERROR_BYTES_L,
    PF_ERROR_BYTES_R,
    PreFeederClient,
    TX_BUFFER_FULL_L,
    TX_BUFFER_FULL_R,
    TX_HOLGURA_L,
    TX_HOLGURA_R,
    TX_PF_BUSY,
    TX_PF_ERROR,
    TX_PF_IDLE,
    TX_PF_INIT,
    TX_PF_RETURN,
    TX_PF_STOP,
)
from error_catalog import format_ui, lookup
from error_policy import ErrorPolicy
from andon import AndonClient, DEFAULT_HOST as ANDON_HOST, DEFAULT_PORT as ANDON_PORT
from machine_states import MACH_BUSY, MACH_ERROR, MACH_IDLE, MACH_PAUSE, MACH_RESET
from latency_debug import mark as lat_mark

MODELS_PATH = HMI_ROOT / "config" / "models.json"
MAX_LOG_LINES = 400
# No latchear E065–E067 en microcortes WiFi; solo si el enlace sigue caído.
LINK_DOWN_CONFIRM_SEC = 8.0
# Error latch is cleared only by explicit RESET after source validation.
# Arranque ASDA: el MCU puede estar arriba antes que la fuente del drive.
# Sonda = ack del propio Servo ON (0x04): si el ASDA no está alimentado, el
# write Modbus falla y Motion contesta ok:false. Se reintenta sin tope hasta
# que el drive responde; luego asentar SON y recién entonces Home (0x01).
# Sin POST_ON, ON+Home van casi juntos y a veces el drive solo queda en SON.
# Si el tiempo es muy corto y no entra HOME, incrementar MOTION_BOOT_POST_ON_SEC
# (y en Motion ASDA_SERVO_ON_SETTLE_MS).
MOTION_BOOT_SETTLE_SEC = 2.5
MOTION_BOOT_POST_ON_SEC = 2.5
MOTION_BOOT_RETRY_SEC = 4.0
MOTION_BOOT_ACK_TIMEOUT_SEC = 6.0
MOTION_BOOT_LOG_EVERY_SEC = 30.0
MOTION_BOOT_READY_STATES = frozenset({TX_INIT, TX_IDLE})

PLC_ERROR_BYTES = frozenset(
    {TX_CUTTER_ERR, TX_GRIPPER_ERR, TX_HOLDER_ERR, TX_ENCODER_ERR}
)

STATE_TEXT = {
    0x09: "Inicializando módulo Motion (0x009)",
    TX_IDLE: "Motion en espera (0x010)",
    TX_BUSY: "Motion ocupado (0x011)",
    TX_ERROR: "Motion — ErrorState (0x0C)",
    TX_STOP_STATE: "Motion detenido (0x0D)",
    TX_RETURN: "Motion — retorno tras stop (0x0E)",
}

PLC_STATE_TEXT = {
    TX_PLC_INIT: "Inicializando módulo PLC (0x024)",
    TX_PLC_IDLE: "PLC en espera (0x025)",
    TX_PLC_BUSY: "PLC ocupado — válvulas activas (0x026)",
    TX_PLC_ERROR: "PLC — ErrorState (0x027)",
    TX_PLC_STOP: "PLC detenido (0x028)",
    TX_PLC_RETURN: "PLC — retorno tras stop (0x029)",
}

PF_STATE_TEXT = {
    TX_PF_INIT: "Inicializando PreFeeder (0x039)",
    TX_PF_IDLE: "PreFeeder en espera (0x03A)",
    TX_PF_BUSY: "PreFeeder ocupado (0x03B)",
    # 0x3C no se pinta al técnico; la tarjeta usa el EXXX (ver _pf_paint_detail_status).
    TX_PF_ERROR: "PreFeeder — ErrorState (0x03C)",
    TX_PF_STOP: "PreFeeder detenido (0x03D)",
    TX_PF_RETURN: "PreFeeder — retorno tras stop (0x03E)",
}

PLC_VALVES = (
    ("Cutter R", CMD_CUTTER_R, TX_CUTTER_ERR, "sensorCutter"),
    ("Cutter L", CMD_CUTTER_L, TX_CUTTER_ERR, "sensorCutter"),
    ("Gripper", CMD_GRIPPER, TX_GRIPPER_ERR, "sensorGripper"),
    ("Holder", CMD_HOLDER, TX_HOLDER_ERR, "sensorHolder"),
    ("Encoder", CMD_ENCODER, TX_ENCODER_ERR, "sensorTray"),
    ("Blower", CMD_BLOWER, None, None),
)

PF_ERRORS = (
    ("L", (
        ("Buffer Full L", 0x33),
        ("Buffer Max L", 0x34),
        ("Tensioner L", 0x35),
        ("Cilindro L", 0x36),
        ("Manguera L", 0x37),
        ("Holgura L", 0x38),
    )),
    ("R", (
        ("Buffer Full R", 0x2D),
        ("Buffer Max R", 0x2E),
        ("Tensioner R", 0x2F),
        ("Cilindro R", 0x30),
        ("Manguera R", 0x31),
        ("Holgura R", 0x32),
    )),
)

# Campos del status Master (l/r) → byte de indicador HMI
_PF_SIDE_SENSOR_FIELDS = (
    ("home", 0x33, 0x2D),          # Buffer Full
    ("endstop", 0x34, 0x2E),       # Buffer Max
    ("tension", 0x35, 0x2F),
    ("cylinderOpen", 0x36, 0x30),
    ("hoseAbsent", 0x37, 0x31),
    ("holgura", 0x38, 0x32),
)

# Sensor ON = OK; el mismo opcode EXXX solo aplica con fallo enclavado del Master.
_PF_OK_WHEN_ACTIVE = frozenset(
    {TX_BUFFER_FULL_L, TX_BUFFER_FULL_R, TX_HOLGURA_L, TX_HOLGURA_R}
)

# Tras ErrorState el Master suele ir a Busy (Auto ON), no solo Idle/Return.
_PF_CLEAR_LATCH_STATES = frozenset(
    {TX_PF_INIT, TX_PF_IDLE, TX_PF_BUSY, TX_PF_RETURN}
)



def _ts() -> str:
    # HH:MM:SS.mmm (strftime %f = microsegundos)
    return datetime.now().strftime("%H:%M:%S.%f")[:-3]


def _append_log(log: deque[str], text: str) -> None:
    log.append(f"[{_ts()}] {text}")
    while len(log) > MAX_LOG_LINES:
        log.popleft()


class HmiState:
    """Mantiene estado de la HMI y procesa mensajes TCP (thread-safe)."""

    def __init__(self) -> None:
        self._lock = threading.RLock()
        self._listeners: list[Callable[[], None]] = []
        self._enc_poll_stop = threading.Event()
        self._enc_poll_thread: threading.Thread | None = None

        self._models: list[dict] = []
        self._selected_model_idx = 0
        self._mm = -45.0
        self._rpm = 1200.0

        self._last_state_byte: int | None = None
        self._stopped_pending_resume = False
        self._move_target_mm: float | None = None
        self._move_start_mm: float | None = None
        self._last_position_mm: float | None = None
        self._om_official_l: float | None = None
        self._om_official_r: float | None = None
        self._progress = 0

        self._banner = {"text": "Listo.", "kind": "info"}
        self._main_log: deque[str] = deque(maxlen=MAX_LOG_LINES)
        self._motion_log: deque[str] = deque(maxlen=MAX_LOG_LINES)
        self._plc_log: deque[str] = deque(maxlen=MAX_LOG_LINES)
        self._pf_log: deque[str] = deque(maxlen=MAX_LOG_LINES)

        self._motion = {
            "connected": False,
            "status": {"text": "Listo.", "kind": "info"},
            "asdaPositionMm": None,
            "enc_r": "—",
            "enc_l": "—",
            "feedOffsetMmL": 0.0,
            "feedOffsetMmR": 0.0,
            "laserR": False,
            "laserL": False,
            "safetyExhaust": False,
        }
        self._plc = {
            "connected": False,
            "status": {"text": "Listo.", "kind": "info"},
            "last_state_byte": None,
            "blowerSec": DEFAULT_BLOWER_SEC,
            "valves": {
                str(cmd): {"label": label, "on": None, "error": None}
                for label, cmd, _, _ in PLC_VALVES
            },
        }
        self._load_plc_config()
        self._andon_buzzer_mute = DEFAULT_ANDON_BUZZER_MUTE
        self._andon_mute_resync = False
        self._debug_password = "tcm"
        self._load_app_config()
        self._andon = {
            "connected": False,
            "green": False,
            "yellow": False,
            "red": False,
            "buzzer": False,
            "manual": False,
        }
        self._andon_log: deque[str] = deque(maxlen=MAX_LOG_LINES)
        self._pf = {
            "connected": False,
            "status": {"text": "Listo.", "kind": "info"},
            "last_state_byte": None,
            "errors": {
                str(b): {"label": label, "active": None}
                for _, items in PF_ERRORS
                for label, b in items
            },
            # Fallo enclavado para opcodes con sensor OK-when-active (Buffer/Holgura).
            "fault_active": {
                str(b): False for b in _PF_OK_WHEN_ACTIVE
            },
            # Runtime L/R desde status Master (relleno / Tfeed / helper).
            "sides": {
                "L": {
                    "autoState": None,
                    "triggerActive": None,
                    "idleMode": False,
                    "refillMaterial": False,
                    "refillDereeler": False,
                    "refillServo": False,
                    "refillFeeder": False,
                    "refillPulseS": 1.0,
                },
                "R": {
                    "autoState": None,
                    "triggerActive": None,
                    "idleMode": False,
                    "refillMaterial": False,
                    "refillDereeler": False,
                    "refillServo": False,
                    "refillFeeder": False,
                    "refillPulseS": 1.0,
                },
            },
            # Master errorAny (status). ErrorState 0x3C solo no implica EXXX.
            "error_any": False,
            # UI del Master (EXXX / PF-007). Nunca el texto genérico 0x3C.
            "master_error_ui": "",
            "master_error_side": "",
        }

        self._client = MotionClient(
            on_message=lambda m: self._enqueue("motion", m),
            on_connection=lambda c: self._on_motion_connection(c),
        )
        self._plc_client = PlcClient(
            on_message=lambda m: self._enqueue("plc", m),
            on_connection=lambda c: self._on_plc_connection(c),
        )
        self._pf_client = PreFeederClient(
            on_message=lambda m: self._enqueue("prefeeder", m),
            on_connection=lambda c: self._on_pf_connection(c),
        )
        self._andon_client = AndonClient(
            on_message=lambda m: self._enqueue("andon", m),
            on_connection=lambda c: self._on_andon_connection(c),
        )
        self._msg_queue: list[tuple[str, dict]] = []

        # Publicación SSE/notificaciones fuera del hilo RX TCP
        self._notify_event = threading.Event()
        self._notify_stop = threading.Event()
        self._notify_thread = threading.Thread(
            target=self._notify_worker_loop,
            name="hmi-notify",
            daemon=True,
        )

        # Señales para Cycle: Motion (Idle/Return/Reached) ≠ Feed (LengthOK/NG L+R)
        self._motion_idle_or_reached = threading.Event()
        self._feed_ok_l = threading.Event()
        self._feed_ok_r = threading.Event()
        self._feed_ng_l = threading.Event()
        self._feed_ng_r = threading.Event()
        # Armado por lado: ignora LengthOK/NG stale de operaciones anteriores.
        self._feed_armed_l = False
        self._feed_armed_r = False
        self._feed_gen_l = 0
        self._feed_gen_r = 0
        # Motivo del último LengthNG por lado (láser E004/E005, ventana, etc.)
        self._feed_fault_l = ""
        self._feed_fault_r = ""
        # Stage2: operación única (no Reached ASDA; no Feed L/R)
        self._stage2_armed = False
        self._stage2_gen = 0
        self._stage2_fault = ""
        # ACK ASDA pendiente (CMD_MOVE recovery — no reenviar a ciegas)
        self._pending_asda_ack: _PendingAsdaAck | None = None
        self._asda_ack_gen = 0
        self._last_move_fail_kind = ""  # transport | rejected | ""
        self._pf_materialist = False
        # OFF explícito (Start/Reset): no rearmar el interlock por idleMode residual.
        self._pf_materialist_force_off = False
        # JOG refill: gen por (lado, canal) para invalidar timers al cancelar/relanzar.
        self._pf_refill_timer_gen: dict[tuple[str, str], int] = {}

        self._error_policy = ErrorPolicy()
        self._cycle = CycleRunner(self)
        self._cycle.set_machine_state_hook(self._broadcast_machine_state)
        # Histéresis enlace: gen+1 cancela timer pendiente al recuperar.
        self._link_down_gen = {"motion": 0, "plc": 0, "prefeeder": 0}
        # Prep ASDA tras Motion Init/Idle: Servo ON → Home (una vez por enlace TCP).
        self._motion_boot_prep_done = False
        self._motion_boot_prep_gen = 0
        self._motion_boot_awaiting_on_ack = False
        self._motion_boot_armed = False
        self._motion_boot_worker_launched = False
        self._motion_boot_on_fails = 0
        self._motion_boot_last_wait_log = 0.0

        self._load_models()
        self._started = False

    def start(self) -> None:
        if self._started:
            return
        self._started = True
        self._notify_thread.start()
        self._client.start_background()
        self._plc_client.start_background()
        self._pf_client.start_background()
        if os.environ.get("ANDON_ENABLE", "1") == "1":
            self._andon_client.start_background()
        self._cycle.mark_init_done()

    def stop(self) -> None:
        self._enc_poll_stop.set()
        self._notify_stop.set()
        self._notify_event.set()
        if self._cycle.is_active():
            self._cycle.request_stop()
        self._client.stop_background()
        self._plc_client.stop_background()
        self._pf_client.stop_background()
        if os.environ.get("ANDON_ENABLE", "1") == "1":
            self._andon_client.stop_background()

    def subscribe(self, callback: Callable[[], None]) -> None:
        with self._lock:
            self._listeners.append(callback)

    def unsubscribe(self, callback: Callable[[], None]) -> None:
        with self._lock:
            if callback in self._listeners:
                self._listeners.remove(callback)

    def _notify_worker_loop(self) -> None:
        """Worker persistente: coalesce ráfagas y publica SSE sin bloquear RX."""
        while not self._notify_stop.is_set():
            if not self._notify_event.wait(timeout=0.3):
                continue
            while True:
                self._notify_event.clear()
                time.sleep(0.012)
                if not self._notify_event.is_set():
                    break
            if self._notify_stop.is_set():
                break
            listeners: list[Callable[[], None]]
            with self._lock:
                listeners = list(self._listeners)
            for cb in listeners:
                try:
                    cb()
                except Exception:
                    pass

    def _notify(self) -> None:
        self._notify_event.set()

    def snapshot(self, *, slim: bool = False) -> dict[str, Any]:
        cycle_snap = self._cycle.snapshot()
        if slim:
            # FLOW_STEPS es estático; la UI ya lo tiene del primer snapshot.
            cycle_snap = dict(cycle_snap)
            cycle_snap.pop("flow", None)
        with self._lock:
            # EXXX remains latched until explicit RESET validation.
            progress = self._cycle_live_progress(cycle_snap)
            resume = (
                self._stopped_pending_resume
                or (
                    bool(cycle_snap.get("paused"))
                    and not bool(cycle_snap.get("refillAwaitingConfirm"))
                    and not bool(cycle_snap.get("recoveryAwaitingConfirm"))
                    and not bool(cycle_snap.get("refillActive"))
                )
            )

            def _log_out(dq: deque[str]) -> list[str]:
                lines = list(dq)
                if len(lines) > SSE_LOG_TAIL:
                    return lines[-SSE_LOG_TAIL:]
                return lines

            return {
                "models": self._models,
                "selectedModel": self._selected_model_idx,
                "mm": self._mm,
                "rpm": self._rpm,
                "banner": dict(self._banner),
                "error": self._error_policy.snapshot(),
                "progress": progress,
                "resumeEnabled": resume,
                "cycle": cycle_snap,
                "motionLink": {
                    "connected": self._motion["connected"],
                    "host": DEFAULT_HOST,
                    "port": DEFAULT_PORT,
                },
                "plcLink": {
                    "connected": self._plc["connected"],
                    "host": PLC_HOST,
                    "port": PLC_PORT,
                },
                "pfLink": {
                    "connected": self._pf["connected"],
                    "host": PF_HOST,
                    "port": PF_PORT,
                },
                "andonLink": {
                    "connected": self._andon["connected"],
                    "host": ANDON_HOST,
                    "port": ANDON_PORT,
                },
                "appConfig": {
                    "andonBuzzerMute": self._andon_buzzer_mute,
                },
                "andon": {
                    "connected": self._andon["connected"],
                    "green": self._andon["green"],
                    "yellow": self._andon["yellow"],
                    "red": self._andon["red"],
                    "buzzer": self._andon["buzzer"],
                    "manual": self._andon["manual"],
                },
                "motion": dict(self._motion),
                "plc": {
                    **self._plc,
                    "valves": {k: dict(v) for k, v in self._plc["valves"].items()},
                },
                "prefeeder": {
                    **self._pf,
                    "errors": {k: dict(v) for k, v in self._pf["errors"].items()},
                },
                "logs": {
                    "main": _log_out(self._main_log),
                    "motion": _log_out(self._motion_log),
                    "plc": _log_out(self._plc_log),
                    "prefeeder": _log_out(self._pf_log),
                    "andon": _log_out(self._andon_log),
                },
            }

    def _load_models(self) -> None:
        self._models = []
        if MODELS_PATH.exists():
            try:
                data = json.loads(MODELS_PATH.read_text(encoding="utf-8"))
                self._models = list(data.get("models", []))
            except (json.JSONDecodeError, OSError) as exc:
                _append_log(self._main_log, f"models.json: {exc}")
        if not self._models:
            self._models = [{"name": "Default", "mm": -45.0, "rpm": 1200.0}]
        self._apply_model(0)

    def _apply_model(self, idx: int) -> None:
        if not self._models:
            return
        idx = max(0, min(idx, len(self._models) - 1))
        self._selected_model_idx = idx
        m = self._models[idx]
        self._mm = float(m.get("mm", -45.0))
        self._rpm = float(m.get("rpm", 1200.0))

    def select_model(self, idx: int) -> None:
        with self._lock:
            self._apply_model(idx)
        self._notify()

    def set_mm_rpm(self, mm: float, rpm: float) -> None:
        with self._lock:
            self._mm = mm
            self._rpm = rpm
        self._notify()

    def clear_log(self, target: str) -> None:
        with self._lock:
            if target == "all":
                self._main_log.clear()
                self._motion_log.clear()
                self._plc_log.clear()
                self._pf_log.clear()
                self._andon_log.clear()
            else:
                logs = {
                    "main": self._main_log,
                    "motion": self._motion_log,
                    "plc": self._plc_log,
                    "prefeeder": self._pf_log,
                    "andon": self._andon_log,
                }
                log = logs.get(target)
                if log is not None:
                    log.clear()
        self._notify()

    def start_enc_poll(self) -> None:
        if self._enc_poll_thread and self._enc_poll_thread.is_alive():
            return
        self._enc_poll_stop.clear()
        self._enc_poll_thread = threading.Thread(
            target=self._enc_poll_loop, daemon=True
        )
        self._enc_poll_thread.start()

    def stop_enc_poll(self) -> None:
        self._enc_poll_stop.set()

    def _enc_poll_loop(self) -> None:
        while not self._enc_poll_stop.is_set():
            # Mismo TCP que CMD_MOVE: no sondear encoders durante lote.
            if self._client.connected and not self._cycle.is_active():
                self.cmd_motion("enc_poll")
            time.sleep(0.6)

    # --- CycleHost (orquestación) ---

    def cycle_log(self, text: str) -> None:
        """Log de ciclo. No pisa el banner si hay EXXX activo (UI = error.ui)."""
        with self._lock:
            _append_log(self._main_log, text)
            if not self._error_policy.latch.active:
                self._banner = {"text": text, "kind": "info"}
        self._notify()

    def cycle_notify(self) -> None:
        self._notify()

    def motion_connected(self) -> bool:
        return self._client.connected

    def plc_connected(self) -> bool:
        return self._plc_client.connected

    def pf_connected(self) -> bool:
        return self._pf_client.connected

    def motion_state_byte(self) -> int | None:
        with self._lock:
            return self._last_state_byte

    def asda_position_mm(self) -> float | None:
        """Última posición ASDA conocida (mm magnitud / report Motion). None = sin dato."""
        with self._lock:
            pos = self._last_position_mm
            if pos is not None:
                return float(pos)
            cached = self._motion.get("asdaPositionMm")
            if cached is None:
                return None
            return float(cached)

    def om_official_mm(self, side: str) -> float | None:
        """Último mmOfficial de GetMeasured (caché TCP). None = sin evento."""
        s = str(side).strip().upper()
        with self._lock:
            if s == "L":
                return None if self._om_official_l is None else float(self._om_official_l)
            if s == "R":
                return None if self._om_official_r is None else float(self._om_official_r)
        return None

    def motion_laser_on(self, side: str) -> bool:
        """True si el láser del lado detecta material (caché HMI desde Motion)."""
        s = str(side).strip().upper()
        if s == "L":
            key = "laserL"
        elif s == "R":
            key = "laserR"
        else:
            return False
        with self._lock:
            return bool(self._motion.get(key))

    def refresh_motion_lasers(self) -> bool:
        """GET /api/status: actualiza laserL/R. El ciclo de lote no lo usa (caché TCP)."""
        host = getattr(self._client, "_host", DEFAULT_HOST)
        try:
            data = motion_http_asda_status(host=str(host), timeout=0.35)
        except Exception:
            return False
        if not isinstance(data, dict):
            return False
        if "laserL" not in data and "laserR" not in data:
            return False
        with self._lock:
            if "laserL" in data:
                self._motion["laserL"] = bool(data["laserL"])
            if "laserR" in data:
                self._motion["laserR"] = bool(data["laserR"])
        return True

    def refresh_asda_position_mm(self) -> float | None:
        """Posición ASDA live vía HTTP /api/status; actualiza caché.

        None solo si el sondeo HTTP falla (no devuelve caché stale — el caller
        de WIP Delivery debe poder caer a fallback temporal).
        """
        host = getattr(self._client, "_host", DEFAULT_HOST)
        pos = motion_http_asda_position_mm(host=str(host), timeout=0.35)
        if pos is None:
            return None
        with self._lock:
            self._last_position_mm = float(pos)
            self._motion["asdaPositionMm"] = float(pos)
        return float(pos)

    def plc_blower_sec(self) -> float:
        with self._lock:
            return float(self._plc.get("blowerSec", DEFAULT_BLOWER_SEC))

    def cmd_plc_blower(
        self, on: bool = True, duration_sec: float | None = None
    ) -> bool:
        """Blower ON(durationSec) o OFF. True = comando TCP enviado."""
        use_sec: float | None = None
        if on:
            if duration_sec is None:
                use_sec = self.plc_blower_sec()
            else:
                use_sec = max(0.2, min(300.0, float(duration_sec)))
        ok = bool(self._plc_client.cmd_blower(on, duration_sec=use_sec))
        if ok:
            with self._lock:
                key = str(CMD_BLOWER)
                if key in self._plc["valves"]:
                    self._plc["valves"][key]["on"] = bool(on)
        return ok

    def pf_state_byte(self) -> int | None:
        with self._lock:
            return self._pf.get("last_state_byte")

    def pf_has_fault(self) -> bool:
        """¿Hay fallo PF real (errorAny / EXXX)? ErrorState 0x3C solo no cuenta."""
        with self._lock:
            return self._pf_cached_has_fault()

    def pf_is_ready(self) -> bool:
        """Enlace + estado conocido + sin EXXX del lote (listo para ciclo)."""
        with self._lock:
            if not self._pf.get("connected"):
                return False
            if self._pf.get("last_state_byte") is None:
                return False
            return not self._pf_cached_has_fault()

    def pf_fault_detail(self) -> str:
        """Texto del fallo PF que bloquea el ciclo. Vacío si no hay EXXX del lote."""
        with self._lock:
            text = self._pf_detail_ui_text()
            if text:
                return text
            if self._pf.get("error_any"):
                side = str(self._pf.get("master_error_side") or "?").upper()
                return f"errorAny lado={side}"
            return ""

    def pf_holgura_present(self, side: str) -> bool | None:
        """Holgura L/R desde status Master: True=presente, False=ausente, None=sin dato.

        Polaridad OK-when-active: sensor ON (errors[byte].active) = holgura OK.
        """
        side_u = str(side or "").strip().upper()
        byte = TX_HOLGURA_L if side_u == "L" else TX_HOLGURA_R if side_u == "R" else 0
        if not byte:
            return None
        with self._lock:
            info = self._pf.get("errors", {}).get(str(byte)) or {}
            active = info.get("active")
            if active is None:
                return None
            return bool(active)

    def pf_buffer_full(self, side: str) -> bool | None:
        """Buffer Full (home) L/R: True=lleno, False=no, None=sin dato."""
        side_u = str(side or "").strip().upper()
        byte = (
            TX_BUFFER_FULL_L
            if side_u == "L"
            else TX_BUFFER_FULL_R
            if side_u == "R"
            else 0
        )
        if not byte:
            return None
        with self._lock:
            info = self._pf.get("errors", {}).get(str(byte)) or {}
            active = info.get("active")
            if active is None:
                return None
            return bool(active)

    def pf_trigger_active(self, side: str) -> bool | None:
        """Motor2 Tfeed / helper holgura en curso (Master triggerActive)."""
        side_u = str(side or "").strip().upper()
        if side_u not in ("L", "R"):
            return None
        with self._lock:
            info = (self._pf.get("sides") or {}).get(side_u) or {}
            val = info.get("triggerActive")
            if val is None:
                return None
            return bool(val)

    def pf_auto_filling(self, side: str) -> bool | None:
        """True si autoState está rellenando buffer (cw / servo_lead)."""
        side_u = str(side or "").strip().upper()
        if side_u not in ("L", "R"):
            return None
        with self._lock:
            info = (self._pf.get("sides") or {}).get(side_u) or {}
            st = str(info.get("autoState") or "").strip().lower()
            if not st:
                return None
            return st in ("cw", "servo_lead")

    def pf_request_status(self) -> bool:
        """Sondea Master (byte Idle): responde con status L/R + holgura."""
        return self._manual_pf(self._pf_client.cmd_status)

    def pf_is_materialist(self) -> bool:
        with self._lock:
            return self._pf_materialist_now()

    def _pf_materialist_now(self) -> bool:
        """Materialist HMI o idleMode L/R. Llamar con o sin lock (RLock)."""
        if self._pf_materialist:
            return True
        sides = self._pf.get("sides") or {}
        return bool(
            (sides.get("L") or {}).get("idleMode")
            or (sides.get("R") or {}).get("idleMode")
        )

    def clear_motion_wait_flags(self) -> None:
        self._motion_idle_or_reached.clear()
        self._feed_ok_l.clear()
        self._feed_ok_r.clear()
        self._feed_ng_l.clear()
        self._feed_ng_r.clear()
        self._feed_armed_l = False
        self._feed_armed_r = False
        self._feed_fault_l = ""
        self._feed_fault_r = ""
        self._stage2_armed = False
        self._stage2_gen = 0
        self._stage2_fault = ""

    def clear_motion_reached_flag(self) -> None:
        """Solo Idle/Reached — conserva LengthOK/NG del prefetch en paralelo."""
        self._motion_idle_or_reached.clear()

    def wait_motion_idle_or_reached(self, timeout_s: float) -> bool:
        return self._motion_idle_or_reached.wait(timeout=timeout_s)

    def arm_stage2(self) -> None:
        """CLEAR/ARM Stage2 — descarta resultado previo; no usa flags Feed/Reached."""
        self._stage2_armed = True
        self._stage2_gen = 0
        self._stage2_fault = ""

    def cmd_motion_stage2_start(
        self, piece_mm: float | None = None, sides: str = "LR", *, target_abs_mm: float | None = None
    ) -> bool:
        """START Stage2. CYCLE pasa target_abs_mm=abs(model.mm)+cutOffsetMm."""
        if not self._stage2_armed:
            self.arm_stage2()
        host = getattr(self._client, "_host", DEFAULT_HOST)
        try:
            data = motion_http_stage2_start(
                piece_mm if target_abs_mm is None else None,
                sides=str(sides or "LR"),
                host=str(host),
                target_abs_mm=target_abs_mm,
            )
        except Exception as exc:  # noqa: BLE001 — enlace HTTP Motion
            self._stage2_fault = f"Stage2 start: {exc}"
            self._stage2_armed = False
            return False
        if not data.get("ok"):
            self._stage2_fault = str(data.get("error") or "Stage2 start rechazado")
            self._stage2_armed = False
            return False
        self._stage2_gen = int(data.get("gen", 0) or 0)
        if self._stage2_gen <= 0:
            self._stage2_fault = "Stage2 start sin gen"
            self._stage2_armed = False
            return False
        return True

    def wait_stage2_result(
        self,
        timeout_s: float,
        *,
        abort_event: threading.Event | None = None,
    ) -> str:
        """Espera resultado final Stage2 (OK/NG). Ignora ASDA Reached intermedios.

        Returns: ok | ng | timeout | aborted
        """
        host = getattr(self._client, "_host", DEFAULT_HOST)
        deadline = time.monotonic() + float(timeout_s)
        last_phase = ""
        while time.monotonic() < deadline:
            if abort_event is not None and abort_event.is_set():
                self._stage2_armed = False
                return "aborted"
            if not self._stage2_armed:
                return "aborted"
            try:
                st = motion_http_stage2_status(host=str(host))
            except Exception as exc:  # noqa: BLE001
                self._stage2_fault = f"Stage2 status: {exc}"
                time.sleep(0.1)
                continue
            gen = int(st.get("gen", 0) or 0)
            result = str(st.get("result") or "NONE")
            phase = str(st.get("phase") or "")
            if phase and phase != last_phase:
                last_phase = phase
                self.cycle_log(f"Stage2 phase={phase} gen={gen}")
            if self._stage2_gen and gen == self._stage2_gen:
                if result == "OK":
                    self._stage2_armed = False
                    self._log_stage2_snapshot(st, "OK")
                    return "ok"
                if result == "NG":
                    fault = str(st.get("fault") or "").strip()
                    self._stage2_fault = fault or "Stage2 NG"
                    self._stage2_armed = False
                    self._log_stage2_snapshot(st, "NG")
                    return "ng"
            time.sleep(0.05)
        return "timeout"

    def stage2_fault(self) -> str:
        return self._stage2_fault

    def _log_stage2_snapshot(self, st: dict[str, Any], result: str) -> None:
        self.cycle_log(
            "Stage2 {res} gen={gen} piece={piece} target={tgt} "
            "apPct={ap} apMm={apMm} fast={fast} fine={fine} "
            "asda={asda} omL={omL} omR={omR} omAvg={omAvg} "
            "fault={fault}".format(
                res=result,
                gen=st.get("gen"),
                piece=st.get("pieceMm"),
                tgt=st.get("targetMm"),
                ap=st.get("stage2ApproachPct"),
                apMm=st.get("approachMm"),
                fast=st.get("stage2FastRpm"),
                fine=st.get("stage2FineRpm"),
                asda=st.get("asdaMm"),
                omL=st.get("omL"),
                omR=st.get("omR"),
                omAvg=st.get("omAvg") or st.get("finalAverageMm"),
                fault=st.get("fault") or "",
            )
        )

    def wait_feed_length_ok(self, timeout_s: float) -> bool:
        # OK en ambos lados; NG en cualquiera = fallo. No OK global por un solo lado.
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            if self._feed_ng_l.is_set() or self._feed_ng_r.is_set():
                return False
            if self._feed_ok_l.is_set() and self._feed_ok_r.is_set():
                return True
            time.sleep(0.05)
        return self._feed_ok_l.is_set() and self._feed_ok_r.is_set()

    def wait_feed_length_ok_for(
        self,
        sides: list[str],
        timeout_s: float,
        *,
        abort_event: threading.Event | None = None,
    ) -> dict[str, str]:
        """Espera LengthOK/NG solo en los lados pedidos. Valores: ok|ng|timeout|aborted."""
        need = {s for s in sides if s in ("R", "L")}
        if not need:
            return {}
        deadline = time.monotonic() + timeout_s
        done: dict[str, str] = {}
        while time.monotonic() < deadline and len(done) < len(need):
            if abort_event is not None and abort_event.is_set():
                break
            if "L" in need and "L" not in done:
                if self._feed_ng_l.is_set():
                    done["L"] = "ng"
                elif self._feed_ok_l.is_set():
                    done["L"] = "ok"
            if "R" in need and "R" not in done:
                if self._feed_ng_r.is_set():
                    done["R"] = "ng"
                elif self._feed_ok_r.is_set():
                    done["R"] = "ok"
            if len(done) >= len(need):
                break
            time.sleep(0.05)
        aborted = abort_event is not None and abort_event.is_set()
        for s in need:
            if s not in done:
                done[s] = "aborted" if aborted else "timeout"
        return done

    def feed_fault_for(self, side: str) -> str:
        """Motivo LengthNG del último Feed armado (vacío si no hubo detalle)."""
        if side == "R":
            return self._feed_fault_r
        if side == "L":
            return self._feed_fault_l
        return ""

    def cmd_motion_move_mm(self, mm: float, rpm: float) -> bool:
        """CMD_MOVE (0x05) con recuperación de transporte acotada.

        True = send salió (o Motion ya activo). El caller espera Reached.
        False = rechazo ASDA inmediato (E012/E013) o transporte agotado (E065).
        No espera el ACK lento: no reenvía si hay evidencia de recepción.
        """
        mm = abs(float(mm))
        lat_mark("0", source="motion", cmd="move_mm", mm=mm)
        with self._lock:
            self._move_target_mm = mm
            self._move_start_mm = self._last_position_mm
            self._last_move_fail_kind = ""
        lat_mark("1", source="motion", cmd="move_mm")
        return self._cmd_motion_move_with_recovery(mm, rpm)

    def last_move_fail_kind(self) -> str:
        """transport | rejected | '' tras el último cmd_motion_move_mm."""
        with self._lock:
            return self._last_move_fail_kind

    def move_ack_rejected(self) -> str:
        """Si el ACK de rechazo llegó después del send: mensaje. '' si no."""
        with self._lock:
            pending = self._pending_asda_ack
            if pending is None or pending.ok is not False:
                return ""
            msg = str(pending.message or "")
            if "ocupado" in msg.lower():
                return ""
            self._last_move_fail_kind = "rejected"
        self._clear_asda_ack()
        return msg or "rechazado"

    def _arm_asda_ack(self, byte_code: int) -> int:
        with self._lock:
            self._asda_ack_gen += 1
            gen = self._asda_ack_gen
            self._pending_asda_ack = _PendingAsdaAck(byte=int(byte_code), gen=gen)
            return gen

    def _clear_asda_ack(self, gen: int | None = None) -> None:
        with self._lock:
            if self._pending_asda_ack is None:
                return
            if gen is None or self._pending_asda_ack.gen == gen:
                self._pending_asda_ack = None

    def _wait_asda_ack(self, gen: int, timeout_s: float) -> bool | None:
        """True=ok, False=rechazo, None=timeout / cancelado."""
        with self._lock:
            pending = self._pending_asda_ack
        if pending is None or pending.gen != gen:
            return None
        if not pending.event.wait(timeout=timeout_s):
            return None
        with self._lock:
            if self._pending_asda_ack is None or self._pending_asda_ack.gen != gen:
                return None
            return self._pending_asda_ack.ok

    def _asda_ack_snapshot(self, gen: int) -> tuple[bool | None, str]:
        with self._lock:
            pending = self._pending_asda_ack
            if pending is None or pending.gen != gen:
                return None, ""
            return pending.ok, pending.message or ""

    def _motion_move_evidence_active(self, gen: int) -> bool:
        """True si Motion ya aceptó / ejecuta el MOVE — no reenviar CMD_MOVE.

        No usa Idle genérico: tras reconnect Motion emite Idle y eso no prueba
        que el CMD_MOVE haya llegado (ni debe satisfacer wait_motion).
        """
        ok, msg = self._asda_ack_snapshot(gen)
        if ok is True:
            return True
        if ok is False and "ocupado" in (msg or "").lower():
            return True
        with self._lock:
            if self._last_state_byte == TX_BUSY:
                return True
            # TX_REACHED limpia _move_target_mm; Idle de reconnect no.
            if self._move_target_mm is None and self._motion_idle_or_reached.is_set():
                return True
        return False

    def _wait_motion_link(self, timeout_s: float) -> bool:
        """Espera enlace Motion (bg reconnect o reconnect explícito)."""
        deadline = time.monotonic() + float(timeout_s)
        while time.monotonic() < deadline:
            if self._client.connected:
                return True
            time.sleep(0.05)
        return self._client.connected

    def _cmd_motion_move_with_recovery(self, mm: float, rpm: float) -> bool:
        """Envía CMD_MOVE; reintenta solo si no hay evidencia de recepción."""
        for attempt in range(1, MOTION_MOVE_TX_ATTEMPTS + 1):
            if not self._wait_motion_link(MOTION_MOVE_LINK_WAIT_S):
                self.cycle_log(
                    f"CMD_MOVE: sin enlace Motion (intento {attempt}/{MOTION_MOVE_TX_ATTEMPTS})"
                )
                if attempt < MOTION_MOVE_TX_ATTEMPTS:
                    self._client.reconnect(silent=True)
                    continue
                break

            gen = self._arm_asda_ack(CMD_MOVE)
            # Idle de reconnect no debe colarse como Reached del MOVE actual.
            self._motion_idle_or_reached.clear()
            sent = self._client.cmd_move_mm(mm, rpm)
            if not sent:
                self.cycle_log(
                    f"CMD_MOVE: fallo de transporte send "
                    f"(intento {attempt}/{MOTION_MOVE_TX_ATTEMPTS}) — recuperando…"
                )
                self._client.reconnect(silent=True)
                if not self._wait_motion_link(MOTION_MOVE_LINK_WAIT_S):
                    self._clear_asda_ack(gen)
                    continue
                if self._motion_move_evidence_active(gen):
                    self._clear_asda_ack(gen)
                    # Busy en curso: esperar Idle real de este movimiento.
                    if self._move_target_mm is not None:
                        self._motion_idle_or_reached.clear()
                    self.cycle_log(
                        "CMD_MOVE: Motion ya activo tras corte — no se reenvía"
                    )
                    return True
                self._clear_asda_ack(gen)
                continue

            # Peek: si el ACK ya llegó, úsalo. Si no, no congelar el ciclo
            # (HOME ya es fire-and-forget). El caller espera Reached.
            ack = self._wait_asda_ack(gen, MOTION_MOVE_ACK_PEEK_S)
            if ack is True:
                self._clear_asda_ack(gen)
                self._motion_idle_or_reached.clear()
                return True
            if ack is False:
                _, msg = self._asda_ack_snapshot(gen)
                self._clear_asda_ack(gen)
                if "ocupado" in (msg or "").lower():
                    self._motion_idle_or_reached.clear()
                    self.cycle_log(
                        "CMD_MOVE: ACK ocupado — se asume movimiento en curso"
                    )
                    return True
                with self._lock:
                    self._last_move_fail_kind = "rejected"
                self.cycle_log(
                    f"CMD_MOVE: rechazado por Motion — {msg or 'sin detalle'}"
                )
                return False

            if self._client.connected or self._motion_move_evidence_active(gen):
                if self._move_target_mm is not None:
                    self._motion_idle_or_reached.clear()
                return True

            self.cycle_log(
                f"CMD_MOVE: send ok pero enlace caído "
                f"(intento {attempt}/{MOTION_MOVE_TX_ATTEMPTS})"
            )
            self._client.reconnect(silent=True)
            if self._wait_motion_link(MOTION_MOVE_LINK_WAIT_S) and self._motion_move_evidence_active(
                gen
            ):
                if self._move_target_mm is not None:
                    self._motion_idle_or_reached.clear()
                self.cycle_log(
                    "CMD_MOVE: tras reconectar Motion ya activo — no se reenvía"
                )
                return True
            self._clear_asda_ack(gen)

        with self._lock:
            self._last_move_fail_kind = "transport"
        self.cycle_log("CMD_MOVE: transporte agotado — E065")
        self._apply_detail_error("E065", source="motion")
        return False

    def cmd_motion_move_zero(self, rpm: float | None = None) -> bool:
        lat_mark("0", source="motion", cmd="move_zero")
        with self._lock:
            self._move_target_mm = 0.0
            self._move_start_mm = self._last_position_mm
            use_rpm = float(rpm) if rpm is not None else self._rpm
        lat_mark("1", source="motion", cmd="move_zero")
        return self._client.cmd_move_zero(use_rpm)

    def cmd_motion_stop(self) -> bool:
        return self._client.cmd_stop()

    def cmd_motion_feed_l(self, *, skip_validate: bool = False, feed_mm: float | None = None) -> bool:
        self._feed_ok_l.clear()
        self._feed_ng_l.clear()
        self._feed_fault_l = ""
        self._feed_gen_l += 1
        self._feed_armed_l = True
        ok = self._client.cmd_feed_l(skip_validate=skip_validate, feed_mm=feed_mm)
        if not ok:
            self._feed_armed_l = False
        return ok

    def cmd_motion_feed_r(self, *, skip_validate: bool = False, feed_mm: float | None = None) -> bool:
        self._feed_ok_r.clear()
        self._feed_ng_r.clear()
        self._feed_fault_r = ""
        self._feed_gen_r += 1
        self._feed_armed_r = True
        ok = self._client.cmd_feed_r(skip_validate=skip_validate, feed_mm=feed_mm)
        if not ok:
            self._feed_armed_r = False
        return ok

    def cmd_motion_enc_set0_r(self) -> bool:
        return self._client.cmd_enc_set0_r()

    def cmd_motion_enc_set0_l(self) -> bool:
        return self._client.cmd_enc_set0_l()

    def plc_valve_is_on(self, byte_code: int) -> bool | None:
        """Estado lógico HMI de una válvula PLC (None = desconocido)."""
        with self._lock:
            valve = self._plc["valves"].get(str(byte_code))
            if not valve:
                return None
            on = valve.get("on")
            if on is None:
                return None
            return bool(on)

    def _cmd_plc_valve_desired(
        self, byte_code: int, on: bool, send, *, force: bool = False
    ) -> bool:
        """Envía pulso KEEP solo si el estado lógico no coincide (evita invertir).

        force=True: siempre TX (p.ej. par Set→Res del cortador en ciclo). El PLC
        sigue sin pulsar si su ``st`` ya coincide — no invertimos el KEEP ahí.
        """
        if not force:
            cur = self.plc_valve_is_on(byte_code)
            if cur is not None and cur == on:
                return True
        ok = bool(send(on))
        if ok:
            with self._lock:
                key = str(byte_code)
                if key in self._plc["valves"]:
                    self._plc["valves"][key]["on"] = on
        return ok

    def cmd_plc_holder(self, on: bool) -> bool:
        return self._cmd_plc_valve_desired(
            CMD_HOLDER, on, self._plc_client.cmd_holder
        )

    def cmd_plc_encoder(self, on: bool) -> bool:
        return self._cmd_plc_valve_desired(
            CMD_ENCODER, on, self._plc_client.cmd_encoder
        )

    def cmd_plc_gripper(self, on: bool) -> bool:
        return self._plc_client.cmd_gripper(on)

    def cmd_plc_cutters(
        self, on: bool, *, sides: str | None = None, force: bool = False
    ) -> bool:
        """Pulso KEEP de cortadores.

        sides: ``L`` | ``R`` | ``LR``. ``None`` = ambos (safe / manual).
        Con LR usa un solo setOut CUTTERS (PLC pulsa R+L en paralelo).
        force: no omitir por caché HMI (ciclo Set→Res).
        """
        mode = "LR" if not sides else CycleConfig.normalize_feed_sides(sides)
        if mode == "LR":
            if not force:
                cur_r = self.plc_valve_is_on(CMD_CUTTER_R)
                cur_l = self.plc_valve_is_on(CMD_CUTTER_L)
                if (
                    cur_r is not None
                    and cur_l is not None
                    and cur_r == on
                    and cur_l == on
                ):
                    return True
            ok = self._plc_client.cmd_set_out("CUTTERS", on)
            if ok:
                with self._lock:
                    for code in (CMD_CUTTER_R, CMD_CUTTER_L):
                        key = str(code)
                        if key in self._plc["valves"]:
                            self._plc["valves"][key]["on"] = on
            return ok
        if mode == "L":
            return self._cmd_plc_valve_desired(
                CMD_CUTTER_L, on, self._plc_client.cmd_cutter_l, force=force
            )
        return self._cmd_plc_valve_desired(
            CMD_CUTTER_R, on, self._plc_client.cmd_cutter_r, force=force
        )

    def cmd_plc_cutter_r(self, on: bool) -> bool:
        return self._cmd_plc_valve_desired(
            CMD_CUTTER_R, on, self._plc_client.cmd_cutter_r
        )

    def cmd_plc_cutter_l(self, on: bool) -> bool:
        return self._cmd_plc_valve_desired(
            CMD_CUTTER_L, on, self._plc_client.cmd_cutter_l
        )

    def cmd_plc_tools_safe(self) -> bool:
        """Cutters + grippers OFF. No toca holder/encoder (deben quedar cerrados)."""
        ok_c = self.cmd_plc_cutters(False, force=True)  # ambos OFF
        ok_g = self.cmd_plc_gripper(False)
        return ok_c and ok_g

    def cmd_plc_all_safe(self) -> bool:
        """HOME PLC: All Off (pulsos Res de cada válvula lógica ON)."""
        ok = self._plc_client.cmd_all_off()
        if ok:
            with self._lock:
                self._plc_apply_home_valve_cache()
        return ok

    def cycle_is_active(self) -> bool:
        return self._cycle.is_active()

    def feed_wait_timeout_s(self) -> float:
        return float(self._cycle.get_config().feed_wait_timeout_s)

    def cutter_pulse_ms(self) -> int:
        return int(self._cycle.get_config().cutter_pulse_ms)

    def cmd_pf_start(self) -> bool:
        return self._pf_client.cmd_start()

    def cmd_pf_stop(self) -> bool:
        return self._pf_client.cmd_stop()

    def cmd_pf_in_process(self, on: bool = True) -> bool:
        """In process vía Master. ON solo a feedSides; OFF a L+R (congelar ambos)."""
        if not on:
            return self._manual_pf(lambda: self._pf_client.cmd_in_process(False))
        mode = CycleConfig.normalize_feed_sides(self._cycle.get_config().feed_sides)
        if mode == "LR":
            return self._manual_pf(lambda: self._pf_client.cmd_in_process(True))
        other = "R" if mode == "L" else "L"
        ok = self._manual_pf(lambda: self._pf_client.cmd_in_process(True, mode))
        self._manual_pf(lambda: self._pf_client.cmd_in_process(False, other))
        return ok

    def cmd_pf_trigger_r(self) -> bool:
        return self._manual_pf(self._pf_client.cmd_trigger_r)

    def cmd_pf_trigger_l(self) -> bool:
        return self._manual_pf(self._pf_client.cmd_trigger_l)

    def get_cycle_config(self) -> dict:
        return self._cycle.get_config().to_dict()

    def set_cycle_config(self, data: dict) -> dict:
        cfg = self._cycle.update_config(data)
        self._notify()
        return cfg.to_dict()

    def reload_cycle_config(self) -> dict:
        cfg = self._cycle.reload_config()
        with self._lock:
            _append_log(self._main_log, "Cycle config recargada")
        self._notify()
        return cfg.to_dict()

    # --- Comandos máquina / ciclo ---

    def cmd_start(self, qty: int | None = None) -> dict:
        """Start máquina (0x040): lote Cycle con mm/qty del modelo."""
        with self._lock:
            blocked = self._work_blocked_error()
            if blocked:
                return {"ok": False, "error": blocked}
            mm, rpm = self._mm, self._rpm
            model = self._models[self._selected_model_idx] if self._models else {}
            model_qty = int(model.get("qty", model.get("cantidad", 1)))
            use_qty = int(qty) if qty is not None and qty >= 1 else model_qty
            self._progress = 0
        # Start produce: salir de Materialist (HMI + PF) en vez de quedarse bloqueado.
        if self._cycle.snapshot().get("materialist") or self.pf_is_materialist():
            off = self.cmd_cycle_materialist(False)
            if not off.get("ok"):
                return off
        if self._pf_client.connected:
            if not self._manual_pf(lambda: self._pf_client.cmd_start()):
                return {"ok": False, "error": "PreFeeder no aceptó Start"}
        res = self._cycle.request_start(mm, use_qty, rpm)
        if not res.get("ok"):
            err = str(res.get("error") or "Start rechazado")
            if "Materialist" in err:
                with self._lock:
                    self._banner = {"text": err, "kind": "error"}
                    _append_log(self._main_log, f"Start rechazado: {err}")
                self._notify()
        return res

    def cmd_stop(self) -> dict:
        if self._cycle.is_active() or self._cycle.snapshot().get("paused"):
            return self._cycle.request_stop()
        # Sin ciclo activo: stop Motion/PF. PLC no se toca (All Off / Reset propios).
        ok_m = self._manual_motion(lambda: self._client.cmd_stop())
        if self._pf_client.connected:
            self._manual_pf(lambda: self._pf_client.cmd_stop())
        return {"ok": ok_m}

    def cmd_resume(self) -> dict:
        snap = self._cycle.snapshot()
        if snap.get("paused"):
            if self._pf_client.connected:
                if not self._manual_pf(lambda: self._pf_client.cmd_start()):
                    return {"ok": False, "error": "PreFeeder no aceptó Start/Init"}
            return self._cycle.request_resume()
        return {"ok": self._manual_motion(lambda: self._client.cmd_resume())}

    def cmd_cycle_pause(self) -> dict:
        return self._cycle.request_pause()

    def cmd_cycle_reset(self) -> dict:
        return self.cmd_error_reset(confirm=False, do_home=False)

    def _plc_reset_reflect_off(self, *, log_label: str = "Reset PLC") -> bool:
        """Reset PLC 0x1E y refleja válvulas OFF en caché HMI. Soft C2/C3 no usa esto."""
        if not self._plc_client.connected:
            return False
        ok = self._manual_plc(lambda: self._plc_client.cmd_reset())
        if ok:
            with self._lock:
                self._plc_apply_home_valve_cache()
                self._plc["status"] = {
                    "text": f"{log_label} — estados OFF (0x01E)",
                    "kind": "ok",
                }
        return bool(ok)

    def cmd_machine_home(self) -> dict:
        """
        Home de máquina (control de máquina):
        ASDA → posición 0 + encoders Set0 L/R + All Off neumática.
        No es Buscar HOME (0x01) ni Reset HMI.
        """
        if self._cycle.is_active():
            return {"ok": False, "error": "Ciclo activo — detén o Reset antes de Home"}

        home_ok = self.cmd_motion_move_zero()
        enc_r = bool(self.cmd_motion_enc_set0_r())
        enc_l = bool(self.cmd_motion_enc_set0_l())

        all_off_ok = False
        if self._plc_client.connected:
            all_off_ok = bool(self._manual_plc(lambda: self._plc_client.cmd_all_off()))
            if all_off_ok:
                with self._lock:
                    self._plc_apply_home_valve_cache()
                    self._plc["status"] = {
                        "text": "All Off — Home máquina",
                        "kind": "ok",
                    }

        ok = bool(home_ok) and enc_r and enc_l and (
            all_off_ok if self._plc_client.connected else True
        )
        _append_log(
            self._main_log,
            f"Home máquina · ASDA→0={'OK' if home_ok else 'FALLÓ'} · "
            f"EncR={'OK' if enc_r else 'FALLÓ'} · EncL={'OK' if enc_l else 'FALLÓ'} · "
            f"AllOff={'OK' if all_off_ok else ('N/A' if not self._plc_client.connected else 'FALLÓ')}",
        )
        self._banner = {
            "text": "Home máquina OK" if ok else "Home máquina con fallos",
            "kind": "ok" if ok else "error",
        }
        # HOME is an independent operator action and never clears EXXX.
        self._notify()
        return {
            "ok": ok,
            "asdaZero": bool(home_ok),
            "encSet0R": enc_r,
            "encSet0L": enc_l,
            "allOff": all_off_ok,
        }

    def cmd_error_reset(self, confirm: bool = False, do_home: bool = False) -> dict:
        """Machine RESET: reset affected modules, validate the real fault, never HOME.

        RESET only releases the HMI latch when the source condition is confirmed
        clear. An active lot remains in recovery/paused state; the operator must
        press Resume after a successful reset.
        """
        latch = self._error_policy.latch
        if not latch.active:
            self._cycle.request_reset()
            self._broadcast_machine_state(MACH_RESET)
            self._broadcast_machine_state(MACH_IDLE)
            self._banner = {"text": "Listo.", "kind": "ok"}
            self._notify()
            return {"ok": True, "cleared": None, "homeOk": None}

        ui = latch.ui_text
        module = (latch.module or "").lower()
        active_lot = self._cycle.is_active()

        # The error handler already put the active lot into the common hold.
        # Do not abort the CycleRunner here; RESET only validates the source.
        # RESET de máquina siempre rearma el PreFeeder. Si el PF es la fuente
        # del EXXX, su Reset debe ser confirmado; para otros errores un fallo de
        # Reset del PF no borra el error ajeno ni cambia su diagnóstico.
        pf_reset_ok = True
        if self._pf_client.connected:
            pf_reset_ok = bool(self._pf_client.cmd_reset())
            if pf_reset_ok:
                with self._lock:
                    self._pf["status"] = {
                        "text": "Reset enviado L+R (0x02C)",
                        "kind": "ok",
                    }
        elif "pre" in module or "feeder" in module:
            pf_reset_ok = False

        module_reset_ok = True
        if "motion" in module:
            module_reset_ok = bool(self._client.cmd_reset_errors())
        elif "plc" in module:
            module_reset_ok = bool(self._plc_reset_reflect_off(log_label="Reset PLC"))
        elif "pre" in module or "feeder" in module:
            module_reset_ok = pf_reset_ok

        if not module_reset_ok:
            self._set_banner(ui, "error")
            self._broadcast_machine_state(MACH_ERROR)
            self._notify()
            return {
                "ok": False,
                "error": f"Reset no confirmado para {ui}",
                "cleared": None,
                "homeOk": None,
            }

        # Refresh the relevant module state before deciding whether the fault is gone.
        if "pre" in module or "feeder" in module:
            try:
                self._pf_client.cmd_status()
            except Exception:
                pass

        healthy = self._latch_source_is_healthy()
        if not healthy:
            # Keep the latch and ERROR. No Idle/Resume window.
            self._set_banner(ui, "error")
            self._broadcast_machine_state(MACH_ERROR)
            self._notify()
            return {
                "ok": False,
                "error": f"{ui} sigue activo",
                "persisted": True,
                "cleared": None,
                "homeOk": None,
            }

        old = self._error_policy.clear()
        self._cycle.clear_fault_mirror()

        # Reset does not HOME. The normal state depends on whether a lot is alive.
        if active_lot:
            self._cycle.request_pause()
            self._broadcast_machine_state(MACH_RESET)
            self._broadcast_machine_state(MACH_PAUSE)
            self._banner = {
                "text": "Error reseteado — Resume para continuar",
                "kind": "ok",
            }
        else:
            self._cycle.request_reset()
            self._broadcast_machine_state(MACH_RESET)
            self._broadcast_machine_state(MACH_IDLE)
            self._banner = {"text": "Errores reseteados", "kind": "ok"}

        self._notify()
        return {
            "ok": True,
            "cleared": old.snapshot(),
            "homeOk": None,
            "resumeEnabled": bool(active_lot),
        }

    def _broadcast_machine_state(self, byte: int) -> None:
        """Estado máquina → Andon (si hay enlace). No envía EXXX detalle."""
        try:
            self._andon_client.send_machine_byte(int(byte) & 0xFF)
        except Exception:
            pass

    def _apply_detail_error(self, code_or_byte: str | int, *, source: str = "") -> bool:
        """Latch one EXXX and enter the common ERROR hold."""
        result = self._error_policy.set_error(code_or_byte)
        if result is None:
            return False

        ui = result["ui"]
        code = result["code"]
        if result.get("duplicate"):
            return True

        _append_log(self._main_log, ui)
        if source == "motion":
            _append_log(self._motion_log, ui)
            self._set_motion_status(ui, "error")
        elif source == "plc":
            _append_log(self._plc_log, ui)
            self._set_plc_status(ui, "error")
        elif source == "prefeeder":
            _append_log(self._pf_log, ui)
            self._set_pf_status(ui, "error")
        elif source == "andon":
            _append_log(self._main_log, ui)

        self._set_banner(ui, "error")

        e050_lot = (
            code == "E050"
            and self._cycle.is_active()
            and not self._cycle.is_refill_active()
        )
        if e050_lot:
            self._cycle.apply_e050_policy(ui)
        else:
            self._cycle.apply_error_policy(ui)

        # 0x46 is the TCM/Andon machine state only. Healthy module states are not
        # converted to ERROR merely because the machine sequence stopped.
        self._broadcast_machine_state(MACH_ERROR)
        self._notify()
        return True

    def _load_app_config(self) -> None:
        try:
            if not APP_CONFIG_PATH.is_file():
                return
            data = json.loads(APP_CONFIG_PATH.read_text(encoding="utf-8"))
            if "andonBuzzerMute" in data:
                self._andon_buzzer_mute = bool(data["andonBuzzerMute"])
            if "debugPassword" in data and str(data["debugPassword"]).strip():
                self._debug_password = str(data["debugPassword"]).strip()
        except Exception:
            pass

    def _save_app_config(self) -> None:
        try:
            APP_CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
            APP_CONFIG_PATH.write_text(
                json.dumps(
                    {
                        "andonBuzzerMute": self._andon_buzzer_mute,
                        "debugPassword": self._debug_password,
                    },
                    indent=2,
                    ensure_ascii=False,
                )
                + "\n",
                encoding="utf-8",
            )
        except Exception:
            pass

    def get_app_config(self) -> dict[str, Any]:
        with self._lock:
            # No exponer debugPassword al cliente
            return {"andonBuzzerMute": self._andon_buzzer_mute}

    def set_app_config(self, data: dict[str, Any]) -> dict[str, Any]:
        with self._lock:
            if "andonBuzzerMute" in data:
                self._andon_buzzer_mute = bool(data["andonBuzzerMute"])
            if "debugPassword" in data and str(data["debugPassword"]):
                self._debug_password = str(data["debugPassword"])
            self._save_app_config()
        # Preferencia ya guardada; empujar a Andon (si no hay enlace, al reconnect).
        self._push_andon_prefs()
        self._notify()
        return self.get_app_config()

    def check_debug_password(self, password: str) -> bool:
        # Releer disco: cambiar debugPassword no exige reiniciar app.py
        self._load_app_config()
        with self._lock:
            return str(password).strip() == str(self._debug_password).strip()

    def _push_andon_prefs(self) -> None:
        """HMI es fuente de verdad del mute; Andon no persiste buzzerMuted."""
        mute = self._andon_buzzer_mute
        if not self._andon_client.set_buzzer_mute(mute):
            if self._andon_client.connected:
                _append_log(
                    self._andon_log,
                    f"Mute buzzer no enviado (pref={'ON' if mute else 'OFF'})",
                )

    def _on_andon_connection(self, connected: bool) -> None:
        with self._lock:
            self._andon["connected"] = connected
        if connected:
            _append_log(self._main_log, "Enlace Andon OK")
            self._push_andon_prefs()
            self._broadcast_machine_state(self._cycle.state_byte())
        self._notify()

    def clear_e050_latch_for_materialist(self) -> bool:
        """Limpia solo el latch HMI de E050 para la ruta especial Materialist."""
        latch = self._error_policy.latch
        if not latch.active or latch.code != "E050":
            return False
        old = self._error_policy.clear()
        self._cycle.clear_fault_mirror()
        _append_log(self._main_log, f"E050: latch limpiado para Materialist · {old.ui_text}")
        self._notify()
        return True

    def cmd_cycle_materialist(self, on: bool = True) -> dict:
        """Materialist (0x049) → Andon + PF Materialista; apaga In process.

        El flag HMI solo queda ON si el PreFeeder aceptó 0x3F (HTML local idleMode).
        OFF: si no hay enlace PF, igual se sale del interlock HMI.
        """
        if on and self._cycle.is_active() and not self._cycle.is_e050_materialist_wait():
            return {"ok": False, "error": "No Materialist con ciclo activo"}
        pf_ok = self._manual_pf(lambda: self._pf_client.cmd_materialist(bool(on)))
        if on and pf_ok:
            self._manual_pf(lambda: self._pf_client.cmd_in_process(False))
        if on and not pf_ok:
            with self._lock:
                err = str(
                    (self._pf.get("status") or {}).get("text")
                    or "Materialist no enviado al PreFeeder"
                )
            return {"ok": False, "error": err}
        res = self._cycle.set_materialist(on)
        if not res.get("ok"):
            self._manual_pf(lambda: self._pf_client.cmd_materialist(False))
            return res
        with self._lock:
            self._pf_materialist = bool(on)
            self._pf_materialist_force_off = not bool(on)
            for sk in ("L", "R"):
                runtime = self._pf.setdefault("sides", {}).setdefault(sk, {})
                runtime["idleMode"] = bool(on)
                if not on:
                    runtime["refillMaterial"] = False
                    runtime["refillDereeler"] = False
                    runtime["refillServo"] = False
                    runtime["refillFeeder"] = False
                    self._pf_refill_bump_side(sk)
            if not on and "Materialist" in str(self._banner.get("text") or ""):
                self._banner = {"text": "Listo.", "kind": "ok"}
        self._notify()
        return res

    def cmd_cycle_busy(self, on: bool = True) -> dict:
        """Busy (0x045) → Andon + PF In process; apaga Materialista."""
        res = self._cycle.set_busy(on)
        if not res.get("ok"):
            return res
        with self._lock:
            if on:
                self._pf_materialist = False
                self._pf_materialist_force_off = True
                for sk in ("L", "R"):
                    runtime = self._pf.setdefault("sides", {}).setdefault(sk, {})
                    runtime["idleMode"] = False
        if on:
            self._manual_pf(lambda: self._pf_client.cmd_materialist(False))
            self._manual_pf(lambda: self._pf_client.cmd_in_process(True))
        else:
            self._manual_pf(lambda: self._pf_client.cmd_in_process(False))
        return res

    def cmd_cycle_step_by_step(self, on: bool = True) -> dict:
        return self._cycle.set_step_by_step(on)

    def cmd_cycle_refill(
        self,
        *,
        feed_mm: float | None = None,
        asda_mm: float | None = None,
    ) -> dict:
        """Purga/refill material: ASDA park → feed → corte → confirm → HOME."""
        with self._lock:
            blocked = self._work_blocked_error()
            if blocked:
                return {"ok": False, "error": blocked}
            rpm = self._rpm
        return self._cycle.request_refill(rpm, feed_mm=feed_mm, asda_mm=asda_mm)

    def cmd_cycle_refill_confirm(self, ok: bool = True) -> dict:
        return self._cycle.confirm_refill(ok)

    def cmd_cycle_refill_retry(self, feed_mm: float | None = None) -> dict:
        return self._cycle.retry_refill(feed_mm=feed_mm)

    def cmd_recovery_review(self, ok: bool = True) -> dict:
        return self._cycle.confirm_recovery_review(ok)

    def cmd_motion(self, action: str, **kwargs) -> dict:
        handlers = {
            "move": lambda: self.cmd_motion_move_mm(
                float(kwargs.get("mm", self._mm)),
                float(kwargs.get("rpm", kwargs.get("speedRpm", self._rpm))),
            ),
            "stop": lambda: self._client.cmd_stop(),
            "on": lambda: self._client.cmd_on(),
            "off": lambda: self._client.cmd_off(),
            "home": lambda: self._client.cmd_home(
                str(kwargs.get("direction", "F")),
            ),
            "move_zero": lambda: self._client.cmd_move_zero(
                float(kwargs.get("rpm", kwargs.get("speedRpm", self._rpm)))
            ),
            "enc_poll": lambda: self._client.cmd_enc_poll_both(),
            "enc_set0_r": lambda: self._client.cmd_enc_set0_r(),
            "enc_set0_l": lambda: self._client.cmd_enc_set0_l(),
            "feed_l": lambda: self.cmd_motion_feed_l(),
            "feed_r": lambda: self.cmd_motion_feed_r(),
            "motion_reset": lambda: self._client.cmd_reset_errors(),
        }
        fn = handlers.get(action)
        if not fn:
            err = f"Acción desconocida: {action}"
            with self._lock:
                _append_log(self._motion_log, err)
                self._set_motion_status(err, "error")
            self._notify()
            return {"ok": False, "error": err}
        if action == "move":
            with self._lock:
                mm = abs(float(kwargs.get("mm", self._mm)))
                self._move_target_mm = mm
                self._move_start_mm = self._last_position_mm
                self._progress = 0
        elif action == "move_zero":
            with self._lock:
                self._move_target_mm = 0.0
                self._move_start_mm = self._last_position_mm
                self._progress = 0
        elif action == "home":
            with self._lock:
                self._move_target_mm = 0.0
                self._move_start_mm = self._last_position_mm
                self._progress = 0
        ok = self._manual_motion(fn)
        if ok and action == "motion_reset":
            with self._lock:
                self._stopped_pending_resume = False
                self._last_state_byte = None
                self._motion["status"] = {
                    "text": "Errores limpiados (0x016)",
                    "kind": "ok",
                }
            self._notify()
        if ok and action in ("on", "off"):
            with self._lock:
                pending = (
                    "Servo ON enviado (0x004)"
                    if action == "on"
                    else "Servo OFF enviado (0x003)"
                )
                _append_log(self._motion_log, pending)
                self._set_motion_status(pending, "info")
            self._notify()
        return {"ok": ok}

    def _load_plc_config(self) -> None:
        try:
            if not PLC_CONFIG_PATH.is_file():
                return
            data = json.loads(PLC_CONFIG_PATH.read_text(encoding="utf-8"))
            sec = float(data.get("blowerSec", DEFAULT_BLOWER_SEC))
            self._plc["blowerSec"] = max(0.2, min(300.0, sec))
        except (OSError, ValueError, TypeError, json.JSONDecodeError):
            self._plc["blowerSec"] = DEFAULT_BLOWER_SEC

    def _save_plc_config(self) -> None:
        try:
            PLC_CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
            PLC_CONFIG_PATH.write_text(
                json.dumps({"blowerSec": self._plc["blowerSec"]}, indent=2) + "\n",
                encoding="utf-8",
            )
        except OSError:
            pass

    def set_blower_sec(self, sec: float) -> float:
        sec = max(0.2, min(300.0, float(sec)))
        with self._lock:
            self._plc["blowerSec"] = sec
            self._save_plc_config()
        self._notify()
        return sec

    def _plc_apply_home_valve_cache(self) -> None:
        """Estado HOME en caché HMI: todas las válvulas OFF (sin auto-holder)."""
        for label, cmd, _, _ in PLC_VALVES:
            key = str(cmd)
            if key not in self._plc["valves"]:
                continue
            self._plc["valves"][key]["on"] = False
            self._plc["valves"][key]["error"] = False

    def _plc_sync_valves_from_status(self, msg: dict) -> None:
        """Sincroniza válvulas desde JSON type=status del PLCA.

        Usa cutterR/cutterL por separado. Ignora el agregado ``cutters``
        (OR lógico) para no marcar ambas como ON.
        """
        field_map = {
            "cutterR": CMD_CUTTER_R,
            "cutterL": CMD_CUTTER_L,
            "grippers": CMD_GRIPPER,
            "holder": CMD_HOLDER,
            "encoder": CMD_ENCODER,
            "fgtray": CMD_ENCODER,
            "blower": CMD_BLOWER,
        }
        for field, cmd_b in field_map.items():
            if field not in msg:
                continue
            key = str(cmd_b)
            if key in self._plc["valves"]:
                self._plc["valves"][key]["on"] = bool(msg[field])

    def cmd_plc(self, action: str, **kwargs) -> dict:
        if action == "valve":
            byte_code = int(kwargs.get("byte", 0))
            on = bool(kwargs.get("on", True))
            label = PLC_VALVE_LABELS.get(byte_code, f"0x{byte_code:02X}")
            out_name = PLC_VALVE_OUT_NAMES.get(byte_code, "?")
            duration_sec = None
            if byte_code == CMD_BLOWER and on:
                raw = kwargs.get("durationSec", kwargs.get("blowerSec"))
                if raw is None:
                    with self._lock:
                        duration_sec = float(self._plc.get("blowerSec", DEFAULT_BLOWER_SEC))
                else:
                    duration_sec = float(raw)
                duration_sec = max(0.2, min(300.0, duration_sec))
            with self._lock:
                extra = f" {duration_sec:g}s" if duration_sec is not None else ""
                _append_log(
                    self._plc_log,
                    f"CMD {label} byte=0x{byte_code:02X} ({byte_code}) "
                    f"setOut={out_name} → {'ON' if on else 'OFF'}{extra}",
                )
            ok = self._manual_plc(
                lambda: self._plc_client.cmd_valve_by_byte(
                    byte_code, on=on, duration_sec=duration_sec
                )
            )
            if ok:
                with self._lock:
                    key = str(byte_code)
                    if key in self._plc["valves"]:
                        # Optimista para UI manual; status/event PLC es autoritativo.
                        self._plc["valves"][key]["on"] = on
                    self._plc["status"] = {
                        "text": f"{label} → {'ON' if on else 'OFF'}"
                        + (f" ({duration_sec:g}s)" if duration_sec else ""),
                        "kind": "ok",
                    }
                self._notify()
        elif action == "set_blower_sec":
            sec = self.set_blower_sec(float(kwargs.get("blowerSec", kwargs.get("sec", DEFAULT_BLOWER_SEC))))
            return {"ok": True, "blowerSec": sec}
        elif action == "reset":
            # Reset PLC propio (también lo dispara Reset HMI hard).
            ok = self._plc_reset_reflect_off()
            if ok:
                self._notify()
        elif action == "all_off":
            ok = self._manual_plc(lambda: self._plc_client.cmd_all_off())
            if ok:
                with self._lock:
                    self._plc_apply_home_valve_cache()
                    self._plc["status"] = {"text": "All Off — Home/Off", "kind": "ok"}
                self._notify()
        else:
            return {"ok": False, "error": f"Acción PLC desconocida: {action}"}
        return {"ok": ok}

    _PF_REFILL_CHANS = ("material", "dereeler", "servo", "feeder")
    _PF_REFILL_KEYS = {
        "material": "refillMaterial",
        "dereeler": "refillDereeler",
        "servo": "refillServo",
        "feeder": "refillFeeder",
    }

    def _pf_refill_channels(self, channel: str) -> tuple[str, ...]:
        if channel == "material":
            return self._PF_REFILL_CHANS
        return (channel,)

    def _pf_refill_bump(self, side: str, channels: tuple[str, ...]) -> None:
        for ch in channels:
            key = (side, ch)
            self._pf_refill_timer_gen[key] = int(
                self._pf_refill_timer_gen.get(key, 0)
            ) + 1

    def _pf_refill_bump_side(self, side: str) -> None:
        self._pf_refill_bump(side, self._PF_REFILL_CHANS)

    def _pf_refill_pulse_s(self, side: str) -> float:
        raw = (self._pf.get("sides") or {}).get(side, {}).get("refillPulseS", 1.0)
        try:
            sec = float(raw)
        except (TypeError, ValueError):
            sec = 1.0
        return max(0.2, min(10.0, sec))

    def _pf_refill_schedule_off(
        self, side: str, channels: tuple[str, ...], pulse_s: float
    ) -> None:
        self._pf_refill_bump(side, channels)
        for ch in channels:
            gen = self._pf_refill_timer_gen[(side, ch)]
            timer = threading.Timer(
                pulse_s, self._pf_refill_auto_off, args=(side, ch, gen)
            )
            timer.daemon = True
            timer.start()

    def _pf_refill_auto_off(self, side: str, channel: str, gen: int) -> None:
        """Apaga el indicador JOG al expirar el pulso del esclavo."""
        key = (side, channel)
        with self._lock:
            if self._pf_refill_timer_gen.get(key) != gen:
                return
            runtime = self._pf.get("sides", {}).get(side)
            if not isinstance(runtime, dict):
                return
            flag = self._PF_REFILL_KEYS[channel]
            if not runtime.get(flag):
                return
            runtime[flag] = False
            if channel == "material":
                runtime["refillDereeler"] = False
                runtime["refillServo"] = False
                runtime["refillFeeder"] = False
            else:
                runtime["refillMaterial"] = bool(
                    runtime.get("refillDereeler")
                    and runtime.get("refillServo")
                    and runtime.get("refillFeeder")
                )
        self._notify()

    def cmd_pf_refill(self, side: str, channel: str, on: bool = True) -> dict:
        """JOG Materialista: refill HTML L/R. Pulso configurable en el esclavo."""
        side_u = str(side or "").strip().upper()
        ch = str(channel or "").strip().lower()
        if side_u not in ("L", "R"):
            return {"ok": False, "error": "side L|R requerido"}
        if ch not in self._PF_REFILL_CHANS:
            return {"ok": False, "error": "canal refill inválido"}
        side_idle = bool(
            (self._pf.get("sides") or {}).get(side_u, {}).get("idleMode")
        )
        if not self._pf_materialist and not side_idle:
            return {"ok": False, "error": "Refill solo en Materialist"}
        ok = self._manual_pf(lambda: self._pf_client.cmd_refill(ch, bool(on), side_u))
        if not ok:
            with self._lock:
                err = str(
                    (self._pf.get("status") or {}).get("text")
                    or f"Fallo refill {ch} {side_u}"
                )
            return {"ok": False, "error": err}
        channels = self._pf_refill_channels(ch)
        with self._lock:
            runtime = self._pf.setdefault("sides", {}).setdefault(side_u, {})
            key = self._PF_REFILL_KEYS[ch]
            runtime[key] = bool(on)
            if ch == "material":
                runtime["refillDereeler"] = bool(on)
                runtime["refillServo"] = bool(on)
                runtime["refillFeeder"] = bool(on)
                runtime["refillMaterial"] = bool(on)
            else:
                runtime["refillMaterial"] = bool(
                    runtime.get("refillDereeler")
                    and runtime.get("refillServo")
                    and runtime.get("refillFeeder")
                )
            pulse_s = self._pf_refill_pulse_s(side_u)
            if on:
                self._pf_refill_schedule_off(side_u, channels, pulse_s)
            else:
                self._pf_refill_bump(side_u, channels)
            text = f"Refill {ch} {side_u} → {'ON' if on else 'OFF'}"
            _append_log(self._pf_log, text)
            self._pf["status"] = {"text": text, "kind": "ok"}
        self._notify()
        return {"ok": True, "pulseS": pulse_s}

    def cmd_pf(self, action: str, **kwargs) -> dict:
        if action == "materialist":
            return self.cmd_cycle_materialist(True)
        if action in ("in_process", "busy"):
            return self.cmd_cycle_busy(True)
        if action == "refill":
            return self.cmd_pf_refill(
                str(kwargs.get("side", "")),
                str(kwargs.get("channel", "")),
                bool(kwargs.get("on", True)),
            )
        handlers = {
            "start": self._pf_client.cmd_start,
            "stop": self._pf_client.cmd_stop,
            "reset": self._pf_client.cmd_reset,
            "trigger_r": self._pf_client.cmd_trigger_r,
            "trigger_l": self._pf_client.cmd_trigger_l,
        }
        fn = handlers.get(action)
        if not fn:
            return {"ok": False, "error": f"Acción PF desconocida: {action}"}
        ok = self._manual_pf(fn)
        if not ok:
            with self._lock:
                err = str(
                    (self._pf.get("status") or {}).get("text")
                    or f"Fallo al enviar comando PreFeeder ({action})"
                )
            return {"ok": False, "error": err}
        if action == "reset":
            # Reset local PF (0x02C): limpia ErrorState + latch HMI.
            # No request_reset de ciclo ni Home/PLC — eso es Reset de máquina y pierde setup.
            with self._lock:
                self._pf["status"] = {"text": "Reset enviado L+R (0x02C)", "kind": "ok"}

            self._notify()
        if action == "start":
            with self._lock:
                text = "Start enviado L+R (0x02A)"
                _append_log(self._pf_log, text)
                self._pf["status"] = {"text": text, "kind": "ok"}
            self._notify()
        if action == "stop":
            with self._lock:
                text = "Stop enviado L+R (0x02B)"
                _append_log(self._pf_log, text)
                self._pf["status"] = {"text": text, "kind": "ok"}
            self._notify()
        if action in ("trigger_r", "trigger_l"):
            side = "R" if action == "trigger_r" else "L"
            opcode = "0x4C" if side == "R" else "0x51"
            with self._lock:
                text = f"Trigger {side} enviado ({opcode})"
                _append_log(self._pf_log, text)
                self._pf["status"] = {"text": text, "kind": "ok"}
            self._notify()
        return {"ok": True}

    def _manual_andon(self, action: Callable[[], bool]) -> bool:
        if not self._andon_client.connected:
            with self._lock:
                msg = f"Sin enlace Andon ({ANDON_HOST}:{ANDON_PORT})"
                _append_log(self._andon_log, msg)
            self._notify()
            return False
        return action()

    def _apply_andon_status(self, msg: dict) -> None:
        self._andon["green"] = bool(msg.get("green"))
        self._andon["yellow"] = bool(msg.get("yellow"))
        self._andon["red"] = bool(msg.get("red"))
        self._andon["buzzer"] = bool(msg.get("buzzer"))
        self._andon["manual"] = bool(msg.get("manual"))
        # Mute: preferencia HMI (app_config). No pisar con status Andon
        # (RAM volatile; race al conectar dejaba UI en mute y buzzer sonando).
        if "mute" in msg and bool(msg.get("mute")) != bool(self._andon_buzzer_mute):
            self._andon_mute_resync = True

    def cmd_andon(self, action: str, **kwargs: Any) -> dict:
        if action == "set_out":
            out = str(kwargs.get("out", "")).strip().lower()
            on = bool(kwargs.get("on", True))
            if out not in ("green", "yellow", "red", "buzzer"):
                return {"ok": False, "error": "Salida Andon desconocida"}
            ok = self._manual_andon(
                lambda: self._andon_client.set_output(out, on)
            )
            if ok:
                with self._lock:
                    key = out if out != "buzzer" else "buzzer"
                    self._andon[key] = on
                    self._andon["manual"] = True
                    _append_log(
                        self._andon_log,
                        f"{out.upper()} {'ON' if on else 'OFF'}",
                    )
                self._notify()
            return {"ok": ok}

        if action == "all_off":
            ok = self._manual_andon(lambda: self._andon_client.all_off())
            if ok:
                with self._lock:
                    self._andon["green"] = False
                    self._andon["yellow"] = False
                    self._andon["red"] = False
                    self._andon["buzzer"] = False
                    self._andon["manual"] = True
                    _append_log(self._andon_log, "Torre OFF (manual)")
                self._notify()
            return {"ok": ok}

        if action == "resume_auto":
            ok = self._manual_andon(lambda: self._andon_client.resume_auto())
            if ok:
                with self._lock:
                    self._andon["manual"] = False
                    _append_log(self._andon_log, "Torre → automático (HMI)")
                self._notify()
            return {"ok": ok}

        if action == "state":
            byte = int(kwargs.get("byte", 0))
            from machine_states import STATE_LABELS

            label = STATE_LABELS.get(byte, f"0x{byte:02X}")
            ok = self._manual_andon(
                lambda: self._andon_client.send_machine_byte(byte)
            )
            if ok:
                with self._lock:
                    self._andon["manual"] = False
                    _append_log(self._andon_log, f"Estado máquina · {label}")
                self._notify()
            return {"ok": ok}

        return {"ok": False, "error": f"Acción Andon desconocida: {action}"}

    def _manual_motion(self, action: Callable[[], bool]) -> bool:
        if not self._client.connected:
            with self._lock:
                _append_log(
                    self._main_log,
                    f"Sin enlace Motion ({DEFAULT_HOST}:{DEFAULT_PORT})",
                )
                self._banner = {
                    "text": f"Sin enlace Motion ({DEFAULT_HOST}:{DEFAULT_PORT})",
                    "kind": "error",
                }
            self._notify()
            return False
        return action()

    def _manual_plc(self, action: Callable[[], bool]) -> bool:
        if not self._plc_client.connected:
            with self._lock:
                msg = f"Sin enlace PLC ({PLC_HOST}:{PLC_PORT})"
                _append_log(self._plc_log, msg)
                self._plc["status"] = {"text": msg, "kind": "error"}
            self._notify()
            return False
        return action()

    def _manual_pf(self, action: Callable[[], bool]) -> bool:
        if not self._pf_client.connected:
            with self._lock:
                msg = f"Sin enlace PreFeeder ({PF_HOST}:{PF_PORT})"
                _append_log(self._pf_log, msg)
                self._pf["status"] = {"text": msg, "kind": "error"}
            self._notify()
            return False
        return action()

    # --- TCP callbacks ---

    def _enqueue(self, source: str, msg: dict) -> None:
        lat_mark("8", source=source, mtype=msg.get("type", ""))
        with self._lock:
            self._msg_queue.append((source, msg))
        self._flush_queue()

    def _flush_queue(self) -> None:
        with self._lock:
            batch = self._msg_queue
            self._msg_queue = []
        changed = False
        for source, msg in batch:
            if source == "plc":
                if self._handle_plc_message(msg):
                    changed = True
            elif source == "prefeeder":
                if self._handle_prefeeder_message(msg):
                    changed = True
            elif source == "andon":
                if self._handle_andon_message(msg):
                    changed = True
            else:
                if self._handle_motion_message(msg):
                    changed = True
        if changed:
            lat_mark("9", source="flush")
            self._notify()

    def _handle_andon_message(self, msg: dict) -> bool:
        mtype = msg.get("type", "")
        if mtype == "status":
            with self._lock:
                self._apply_andon_status(msg)
                need_mute_push = self._andon_mute_resync
                self._andon_mute_resync = False
            if need_mute_push:
                self._push_andon_prefs()
            return True
        if mtype == "ack":
            with self._lock:
                if msg.get("byte") is not None:
                    pass
                if msg.get("message"):
                    _append_log(self._andon_log, str(msg.get("message")))
            return True
        if mtype == "event":
            byte_code = int(msg.get("byte", 0))
            if byte_code == 0x50:
                return self._apply_detail_error(0x50, source="andon")
        if mtype == "local_error":
            return False
        return False

    def _apply_feed_offset_values(self, offset_l: float, offset_r: float) -> None:
        self._motion["feedOffsetMmL"] = clamp_feed_offset_mm(offset_l)
        self._motion["feedOffsetMmR"] = clamp_feed_offset_mm(offset_r)

    def refresh_feed_offset(self) -> dict[str, Any]:
        try:
            data = motion_http_get_feed_offset()
            offset_l = float(data.get("offsetMm", 0.0))
            offset_r = float(data.get("offsetMmB", 0.0))
            with self._lock:
                self._apply_feed_offset_values(offset_l, offset_r)
            self._notify()
            return {
                "ok": True,
                "feedOffsetMmL": offset_l,
                "feedOffsetMmR": offset_r,
            }
        except (OSError, ValueError, TypeError, json.JSONDecodeError) as exc:
            return {"ok": False, "error": str(exc)}

    def set_feed_offset(self, offset_l: float, offset_r: float) -> dict[str, Any]:
        try:
            data = motion_http_set_feed_offset(offset_l, offset_r)
            offset_l = float(data.get("offsetMm", offset_l))
            offset_r = float(data.get("offsetMmB", offset_r))
            with self._lock:
                self._apply_feed_offset_values(offset_l, offset_r)
                _append_log(
                    self._motion_log,
                    f"Feed offset L={offset_l:.2f} mm R={offset_r:.2f} mm",
                )
                self._motion["status"] = {
                    "text": f"Offset feed L={offset_l:.1f} R={offset_r:.1f} mm",
                    "kind": "ok",
                }
            self._notify()
            return {
                "ok": True,
                "feedOffsetMmL": offset_l,
                "feedOffsetMmR": offset_r,
            }
        except (OSError, ValueError, TypeError, json.JSONDecodeError) as exc:
            with self._lock:
                _append_log(self._motion_log, f"Feed offset: {exc}")
                self._motion["status"] = {
                    "text": f"Feed offset: {exc}",
                    "kind": "error",
                }
            self._notify()
            return {"ok": False, "error": str(exc)}

    def _clear_link_error_if(self, code: str) -> None:
        """Link recovery never clears the EXXX latch automatically."""
        if self._error_policy.latch.active and self._error_policy.latch.code == code:
            _append_log(
                self._main_log,
                f"Enlace recuperado · {code} — RESET requerido para liberar ERROR",
            )
            self._notify()

    def _motion_is_healthy(self) -> bool:
        """Motion source condition: link up, a known state, and not ErrorState."""
        if not self._motion.get("connected"):
            return False
        b = self._last_state_byte
        if b is None:
            return False
        return b != TX_ERROR

            return False
        b = self._last_state_byte
        if b is None:
            return False
        return b != TX_ERROR

    def _plc_primary_active_error_byte(self) -> int | None:
        """EXXX PLC activo en caché de válvulas (Cutter/Gripper/Holder/Encoder)."""
        for _label, cmd, err_b, _sf in PLC_VALVES:
            if err_b is None:
                continue
            v = self._plc.get("valves", {}).get(str(cmd), {})
            if isinstance(v, dict) and v.get("error"):
                return err_b
        return None

    def _plc_has_fault(self) -> bool:
        """Fallo PLC en caché: ErrorState o sensor de seguridad. No exige latch."""
        if self._plc.get("last_state_byte") == TX_PLC_ERROR:
            return True
        return self._plc_primary_active_error_byte() is not None

    def _plc_try_latch_once(self) -> bool:
        """Un solo Set/log/Andon por fallo PLC. Espejo de _pf_try_latch_once."""
        if self._error_policy.latch.active:
            return False
        primary = self._plc_primary_active_error_byte()
        if primary is None:
            return False
        return self._apply_detail_error(primary, source="plc")

    def _work_blocked_error(self) -> str | None:
        """Motivo para rechazar Start/Refill. Latch EXXX o fallo PLC en caché."""
        latch = self._error_policy.latch
        if latch.active:
            ui = latch.ui_text or latch.code or "error activo"
            return f"Error activo — {ui} (Reset antes de Start)"
        if self._plc.get("connected") and self._plc_has_fault():
            st = (self._plc.get("status") or {}).get("text") or "PLC en error"
            return f"Error activo — {st} (Reset antes de Start)"
        return None

    def _plc_is_healthy(self) -> bool:
        """PLC OK: enlace up, no ErrorState, sin sensor de seguridad activo."""
        if not self._plc.get("connected"):
            return False
        b = self._plc.get("last_state_byte")
        if b is None or b == TX_PLC_ERROR:
            return False
        return self._plc_primary_active_error_byte() is None

    def _pf_is_generic_state_text(self, text: str) -> bool:
        s = (text or "").lower()
        return "errorstate" in s or "0x03c" in s

    def _pf_is_operator_stop_only(self) -> bool:
        """PF-007 / parada: no es EXXX. No pintar ErrorState al técnico."""
        if self._pf_primary_active_error_byte() is not None:
            return False
        ui = str(self._pf.get("master_error_ui") or "").lower()
        return (
            "pf-007" in ui
            or "operator_stop" in ui
            or "parada operador" in ui
        )

    def _pf_cached_has_fault(self) -> bool:
        """Fallo PF del lote (EXXX / sensor). 0x3C, PF-007 y lado no alimentado no cuentan."""
        # Materialist: el esclavo ignora manguera/sensores; no re-pintar E056/E062.
        if self._pf_materialist_now():
            return False
        if self._pf_is_operator_stop_only():
            return False
        if self._pf_primary_active_error_byte() is not None:
            return True
        if not self._pf.get("error_any"):
            return False
        ui = str(self._pf.get("master_error_ui") or "")
        if not ui or self._pf_is_generic_state_text(ui):
            return False
        return self._pf_master_error_in_lot()

    def _pf_is_healthy(self) -> bool:
        """PreFeeder OK: enlace up y sin EXXX/errorAny (no exige salir de 0x3C)."""
        if not self._pf.get("connected"):
            return False
        return not self._pf_cached_has_fault()

    def _latch_source_is_healthy(self) -> bool:
        """Validate the actual source condition represented by the EXXX latch."""
        latch = self._error_policy.latch
        if not latch.active:
            return True

        mod = (latch.module or "").lower()
        code = latch.code or ""

        if "motion" in mod:
            return self._motion_is_healthy()
        if "plc" in mod:
            return self._plc_is_healthy()
        if "pre" in mod or "feeder" in mod:
            return self._pf_is_healthy()

        # Cycle/Main/Andon errors have no independent module fault signal.
        # Their reset is valid once the relevant sequence is no longer active.
        if code.startswith("E06") or code == "E068":
            return (
                self._motion.get("connected")
                and self._plc.get("connected")
                and self._pf.get("connected")
            )
        return not self._cycle.is_active()

    def _try_auto_clear_error_if_healthy(self) -> bool:
        """Legacy hook kept inert: EXXX is released only by explicit machine RESET."""
        return False

    def _cancel_link_down(self, key: str) -> None:
        self._link_down_gen[key] = self._link_down_gen.get(key, 0) + 1

    def _arm_link_down(self, key: str, code: str, *, source: str) -> None:
        """Latchea E06x solo si el nodo sigue caído tras LINK_DOWN_CONFIRM_SEC."""
        self._link_down_gen[key] = self._link_down_gen.get(key, 0) + 1
        gen = self._link_down_gen[key]

        def fire() -> None:
            time.sleep(LINK_DOWN_CONFIRM_SEC)
            if self._link_down_gen.get(key) != gen:
                return
            with self._lock:
                still_down = {
                    "motion": not self._motion["connected"],
                    "plc": not self._plc["connected"],
                    "prefeeder": not self._pf["connected"],
                }.get(key, True)
            if not still_down:
                return
            self._apply_detail_error(code, source=source)
            self._notify()

        threading.Thread(target=fire, daemon=True).start()

    def _on_motion_connection(self, connected: bool) -> None:
        with self._lock:
            was = self._motion["connected"]
            self._motion["connected"] = connected
            if connected:
                self._banner = {
                    "text": f"Enlace Motion — {DEFAULT_HOST}:{DEFAULT_PORT}",
                    "kind": "ok",
                }
                if not self._motion_boot_prep_done:
                    # Armar; la rutina servo espera Init/Idle de Motion (no solo socket).
                    self._motion_boot_prep_gen += 1
                    self._motion_boot_armed = True
                    self._motion_boot_worker_launched = False
                    self._motion_boot_awaiting_on_ack = False
                    self._motion_boot_on_fails = 0
                    self._motion_boot_last_wait_log = 0.0
            else:
                self._last_state_byte = None
                self._stopped_pending_resume = False
                self._motion["asdaPositionMm"] = None
                self._motion["enc_r"] = "—"
                self._motion["enc_l"] = "—"
                self._om_official_l = None
                self._om_official_r = None
                self._motion["laserR"] = False
                self._motion["laserL"] = False
                self._motion["safetyExhaust"] = False
                # Caída/reboot del micro: permitir Servo ON→Home al reconectar.
                self._motion_boot_prep_done = False
                self._motion_boot_prep_gen += 1
                self._motion_boot_awaiting_on_ack = False
                self._motion_boot_armed = False
                self._motion_boot_worker_launched = False
                self._motion_boot_on_fails = 0
                self._motion_boot_last_wait_log = 0.0
                self._banner = {
                    "text": f"Reconectando Motion ({DEFAULT_HOST}:{DEFAULT_PORT})…",
                    "kind": "warn",
                }
        if connected:
            self._cancel_link_down("motion")
            self._clear_link_error_if("E065")
        elif not connected and was:
            self._arm_link_down("motion", "E065", source="motion")
        self._notify()
        if connected:
            # Init/Idle pueden haber llegado antes de armar (callback diferido).
            threading.Thread(
                target=self._motion_boot_try_launch,
                daemon=True,
                name="motion-boot-try",
            ).start()
            threading.Thread(
                target=self.refresh_feed_offset, daemon=True
            ).start()

    def _motion_boot_try_launch(self) -> None:
        """Si Motion ya reportó Init/Idle y la prep está armada, lanza Servo ON/Home."""
        boot_gen: int | None = None
        with self._lock:
            if (
                self._motion_boot_prep_done
                or not self._motion_boot_armed
                or self._motion_boot_worker_launched
                or not self._motion["connected"]
            ):
                return
            if self._last_state_byte not in MOTION_BOOT_READY_STATES:
                return
            self._motion_boot_worker_launched = True
            boot_gen = self._motion_boot_prep_gen
        if boot_gen is not None:
            threading.Thread(
                target=self._motion_boot_prep_worker,
                args=(boot_gen,),
                daemon=True,
                name="motion-boot-prep",
            ).start()

    def _motion_boot_retry_after_on_fail(self) -> None:
        """Reintenta Servo ON: el drive puede alimentarse después del MCU."""
        time.sleep(MOTION_BOOT_RETRY_SEC)
        self._motion_boot_try_launch()

    def _motion_boot_attempt_failed(self, reason: str) -> None:
        """Servo ON no aceptado: ASDA aún sin alimentación/Modbus. Reintenta."""
        notify = False
        with self._lock:
            if self._motion_boot_prep_done or not self._motion_boot_armed:
                return
            self._motion_boot_awaiting_on_ack = False
            self._motion_boot_worker_launched = False
            self._motion_boot_on_fails += 1
            fails = self._motion_boot_on_fails
            now = time.monotonic()
            if (
                fails == 1
                or (now - self._motion_boot_last_wait_log) >= MOTION_BOOT_LOG_EVERY_SEC
            ):
                self._motion_boot_last_wait_log = now
                _append_log(
                    self._motion_log,
                    f"Arranque lineal: esperando alimentación del ASDA — {reason} "
                    f"(intento {fails})",
                )
                self._set_motion_status(
                    "Esperando alimentación ASDA (Servo ON)…", "warn"
                )
                notify = True
        if notify:
            self._notify()
        threading.Thread(
            target=self._motion_boot_retry_after_on_fail,
            daemon=True,
            name="motion-boot-retry",
        ).start()

    def _motion_boot_prep_worker(self, gen: int) -> None:
        """TCP+Init/Idle → settle → Servo ON (sonda Modbus); Home al ack OK."""
        time.sleep(MOTION_BOOT_SETTLE_SEC)
        first_try = False
        with self._lock:
            if gen != self._motion_boot_prep_gen or self._motion_boot_prep_done:
                return
            if not self._motion["connected"]:
                return
            # No mover ASDA si ya hay ciclo (p.ej. reconnect tardío).
            if self._cycle.is_active():
                self._motion_boot_prep_done = True
                self._motion_boot_armed = False
                return
            first_try = self._motion_boot_on_fails == 0
            if first_try:
                _append_log(
                    self._motion_log,
                    "Arranque lineal: TCP OK → Servo ON (0x004)…",
                )
                self._set_motion_status("Arranque lineal: Servo ON (0x004)…", "info")
            self._motion_boot_awaiting_on_ack = True
        if first_try:
            self._notify()
        if not self._client.cmd_on():
            self._motion_boot_attempt_failed("Servo ON no enviado")
            return
        # Sin ack (Motion mudo / ack perdido) el arranque quedaba colgado: reintentar.
        deadline = time.monotonic() + MOTION_BOOT_ACK_TIMEOUT_SEC
        while time.monotonic() < deadline:
            time.sleep(0.2)
            with self._lock:
                if gen != self._motion_boot_prep_gen:
                    return
                if self._motion_boot_prep_done or not self._motion_boot_awaiting_on_ack:
                    return
        self._motion_boot_attempt_failed("sin respuesta al Servo ON")

    def _motion_boot_send_home(self) -> None:
        """Home (0x01) tras ack OK de Servo ON + asentamiento del drive."""
        time.sleep(MOTION_BOOT_POST_ON_SEC)
        if not self._client.connected:
            return
        with self._lock:
            # Reconnect/aborto durante el asentamiento: no disparar Home.
            if not self._motion_boot_prep_done:
                return
            self._move_target_mm = 0.0
            self._move_start_mm = self._last_position_mm
            self._progress = 0
            self._motion_boot_armed = False
            _append_log(
                self._motion_log,
                "Arranque lineal: servo ON asentado → Home (0x001)",
            )
            self._set_motion_status("Arranque lineal: Home (0x001)", "info")
        self._notify()
        ok = self._client.cmd_home("F")
        if not ok:
            with self._lock:
                _append_log(self._motion_log, "Arranque lineal: Home no enviado")
                self._set_motion_status("Arranque lineal: Home no enviado", "error")
            self._notify()

    def _on_plc_connection(self, connected: bool) -> None:
        with self._lock:
            was = self._plc["connected"]
            self._plc["connected"] = connected
            if connected:
                self._banner = {
                    "text": f"Enlace PLC — {PLC_HOST}:{PLC_PORT}",
                    "kind": "ok",
                }
            else:
                self._plc["last_state_byte"] = None
                for v in self._plc["valves"].values():
                    v["on"] = None
                    v["error"] = None
                self._banner = {
                    "text": f"Reconectando PLC ({PLC_HOST}:{PLC_PORT})…",
                    "kind": "warn",
                }
        if connected:
            self._cancel_link_down("plc")
            self._clear_link_error_if("E066")
        elif not connected and was:
            self._arm_link_down("plc", "E066", source="plc")
        self._notify()

    def _on_pf_connection(self, connected: bool) -> None:
        with self._lock:
            was = self._pf["connected"]
            self._pf["connected"] = connected
            if connected:
                self._banner = {
                    "text": f"Enlace PreFeeder — {PF_HOST}:{PF_PORT}",
                    "kind": "ok",
                }
            else:
                self._pf["last_state_byte"] = None
                self._pf["error_any"] = False
                self._pf["master_error_ui"] = ""
                self._pf["master_error_side"] = ""
                for e in self._pf["errors"].values():
                    e["active"] = None
                for k in self._pf["fault_active"]:
                    self._pf["fault_active"][k] = False
                for sk in ("L", "R"):
                    self._pf.setdefault("sides", {}).setdefault(
                        sk,
                        {
                            "autoState": None,
                            "triggerActive": None,
                            "idleMode": False,
                            "refillMaterial": False,
                            "refillDereeler": False,
                            "refillServo": False,
                            "refillFeeder": False,
                            "refillPulseS": 1.0,
                        },
                    )
                    self._pf["sides"][sk]["autoState"] = None
                    self._pf["sides"][sk]["triggerActive"] = None
                    self._pf["sides"][sk]["idleMode"] = False
                    self._pf["sides"][sk]["refillMaterial"] = False
                    self._pf["sides"][sk]["refillDereeler"] = False
                    self._pf["sides"][sk]["refillServo"] = False
                    self._pf["sides"][sk]["refillFeeder"] = False
                    self._pf["sides"][sk]["refillPulseS"] = 1.0
                    self._pf_refill_bump_side(sk)
                self._banner = {
                    "text": f"Reconectando PreFeeder ({PF_HOST}:{PF_PORT})…",
                    "kind": "warn",
                }
        if connected:
            self._cancel_link_down("prefeeder")
            self._clear_link_error_if("E067")
        elif not connected and was:
            self._arm_link_down("prefeeder", "E067", source="prefeeder")
        self._notify()

    def reconnect_all_modules(self) -> dict[str, bool]:
        """Fuerza reconexión TCP a Motion, PLC, PreFeeder y Andon (en paralelo)."""
        results: dict[str, bool] = {}
        lock = threading.Lock()
        clients: list[tuple[str, ModuleTcpClient]] = [
            ("motion", self._client),
            ("plc", self._plc_client),
            ("prefeeder", self._pf_client),
        ]
        if os.environ.get("ANDON_ENABLE", "1") == "1":
            clients.append(("andon", self._andon_client))

        def worker(name: str, client: ModuleTcpClient) -> None:
            ok = client.reconnect(silent=True)
            with lock:
                results[name] = ok

        threads = [
            threading.Thread(target=worker, args=(name, client), daemon=True)
            for name, client in clients
        ]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=CONNECT_TIMEOUT + VERIFY_TIMEOUT_SEC + 1.0)

        _append_log(
            self._main_log,
            "Reconexión red: "
            + ", ".join(f"{k}={'OK' if results.get(k) else 'OFF'}" for k, _ in clients),
        )
        self._notify()
        return results

    def _cycle_live_progress(self, cycle_snap: dict[str, Any]) -> int:
        if cycle_snap.get("completed"):
            return 100
        total = int(cycle_snap.get("totalReps") or 0)
        pieces_done = int(cycle_snap.get("piecesDone") or 0)
        runner_prog = int(cycle_snap.get("progress") or 0)
        if total > 0 and pieces_done > 0:
            piece_prog = int(min(100, round(100.0 * pieces_done / total)))
            runner_prog = max(runner_prog, piece_prog)
        if not cycle_snap.get("active"):
            return runner_prog
        rep = int(cycle_snap.get("rep") or 0)
        step = int(cycle_snap.get("step") or 0)
        if total < 1 or step < 1:
            return runner_prog
        intra = (
            self._progress / 100.0
            if self._move_target_mm is not None
            else 0.0
        )
        frac = ((rep - 1) + (step - 1 + intra) / max(PROGRESS_STEPS, 1)) / total
        step_prog = int(min(100, max(0, round(frac * 100))))
        return max(runner_prog, step_prog)

    def apply_detail_error(self, code_or_byte: str | int) -> bool:
        return self._apply_detail_error(code_or_byte)

    def error_is_latched(self) -> bool:
        return bool(self._error_policy.latch.active)

    def _set_banner(self, text: str, kind: str) -> None:
        if self._banner.get("text") == text and self._banner.get("kind") == kind:
            return
        self._banner = {"text": text, "kind": kind}

    def _set_motion_status(self, text: str, kind: str) -> None:
        cur = self._motion.get("status", {})
        if cur.get("text") == text and cur.get("kind") == kind:
            return
        self._motion["status"] = {"text": text, "kind": kind}

    def _set_plc_status(self, text: str, kind: str) -> None:
        cur = self._plc.get("status", {})
        if cur.get("text") == text and cur.get("kind") == kind:
            return
        self._plc["status"] = {"text": text, "kind": kind}

    def _set_pf_status(self, text: str, kind: str) -> None:
        if self._pf_is_generic_state_text(text):
            detail = self._pf_detail_ui_text()
            if not detail:
                return
            text = detail
            kind = "error"
        cur = self._pf.get("status", {})
        if cur.get("text") == text and cur.get("kind") == kind:
            return
        self._pf["status"] = {"text": text, "kind": kind}

    def _update_progress(self, position_mm: float) -> None:
        self._last_position_mm = position_mm
        if self._move_target_mm is None:
            return
        target = self._move_target_mm
        start = (
            self._move_start_mm if self._move_start_mm is not None else position_mm
        )
        total = abs(target - start)
        if total < 0.05:
            if abs(position_mm - target) < 0.5:
                self._progress = 100
            return
        done = abs(position_mm - start)
        self._progress = int(min(100, max(0, round(100.0 * done / total))))

    def _apply_state_byte(self, byte_code: int) -> None:
        if byte_code == self._last_state_byte:
            return
        prev = self._last_state_byte
        self._last_state_byte = byte_code
        if byte_code in (TX_IDLE, TX_RETURN):
            self._motion_idle_or_reached.set()
            # Módulo salió de ErrorState → Res del EXXX Motion en HMI (evita
            # ERROR global con Motion en espera / OK).
            if prev == TX_ERROR:
                # EXXX remains latched until explicit machine RESET.
                pass
        text = STATE_TEXT.get(byte_code, f"Estado 0x{byte_code:02X}")
        kind = "info"
        if byte_code == TX_ERROR:
            self._set_banner(text, "error")
            _append_log(self._main_log, text)
            return
        if byte_code == TX_STOP_STATE:
            kind = "warn"
            self._stopped_pending_resume = True
            text = f"{text} — reanudar disponible"
        elif byte_code == TX_RETURN:
            kind = "ok"
            self._stopped_pending_resume = False
        elif byte_code == TX_IDLE:
            kind = "ok"
            if self._stopped_pending_resume:
                return
        elif byte_code == TX_BUSY:
            kind = "info"
            lat_mark("9", source="motion", state="BUSY")
            if self._move_target_mm is not None and self._progress < 5:
                self._progress = 5
        self._set_banner(text, kind)
        self._set_motion_status(text, kind)
        # Arranque ASDA solo cuando Motion ya reportó Init/Idle (no al abrir socket).
        if byte_code in MOTION_BOOT_READY_STATES:
            # Salir del lock del handler vía hilo: try_launch toma el lock de nuevo.
            threading.Thread(
                target=self._motion_boot_try_launch,
                daemon=True,
                name="motion-boot-try",
            ).start()

    def _handle_motion_message(self, msg: dict) -> bool:
        changed = False
        with self._lock:
            mtype = msg.get("type", "")
            if mtype == "local_error":
                text = msg.get("message", "Error local")
                _append_log(self._main_log, text)
                _append_log(self._motion_log, text)
                self._set_banner(text, "error")
                self._set_motion_status(text, "error")
                return True
            if mtype == "hello":
                return False
            if mtype == "state":
                actuator = msg.get("actuator", "motion")
                if actuator not in ("motion", "asda"):
                    return False
                byte_code = int(msg.get("byte", 0))
                if byte_code not in MOTION_STATE_BYTES:
                    return False
                self._apply_state_byte(byte_code)
                return True
            if mtype == "event":
                byte_code = int(msg.get("byte", 0))
                if byte_code in (CMD_ENC_MEASURE_R, CMD_ENC_MEASURE_L):
                    side = msg.get(
                        "side", "R" if byte_code == CMD_ENC_MEASURE_R else "L"
                    )
                    mm_off = float(msg.get("mmOfficial", 0.0))
                    mm_sig = float(msg.get("mm", 0.0))
                    settled = bool(msg.get("settled", False))
                    # mmOfficial = lectura base (Set0 → 0); offset OM no contamina la UI.
                    text = f"{mm_off:.2f} mm"
                    if settled:
                        text += f"  (raw {mm_sig:.2f})"
                    else:
                        text += "  (moviendo…)"
                    if side == "R":
                        self._motion["enc_r"] = text
                        self._om_official_r = mm_off
                    else:
                        self._motion["enc_l"] = text
                        self._om_official_l = mm_off
                    return True
                if byte_code == TX_REACHED:
                    pos = msg.get("positionPuu")
                    pos_mm = msg.get("positionMm")
                    if pos_mm is not None:
                        self._motion["asdaPositionMm"] = float(pos_mm)
                        self._last_position_mm = float(pos_mm)
                    self._progress = 100
                    self._motion_idle_or_reached.set()
                    self._move_target_mm = None
                    if self._cycle.is_active():
                        return True
                    if pos_mm is not None:
                        text = f"Posicion alcanzada ({float(pos_mm):.2f} mm)"
                    else:
                        text = f"Posicion alcanzada (PUU {pos})"
                    self._set_banner(text, "ok")
                    self._set_motion_status(text, "ok")
                    return True
                if byte_code == TX_ENC_ERROR:
                    # Polling periódico del encoder: no spamear log (0x011).
                    return True
                if byte_code in (TX_LENGTH_OK, TX_LENGTH_OK_L):
                    if not self._feed_armed_l:
                        return True  # stale / cross-op
                    self._feed_armed_l = False
                    self._feed_fault_l = ""
                    self._feed_ok_l.set()
                    if not self._cycle.is_active():
                        text = "Feed OK L — longitud en tolerancia (0x014)"
                        self._set_banner(text, "ok")
                        self._set_motion_status(text, "ok")
                    return True
                if byte_code in (TX_LENGTH_NG, TX_LENGTH_NG_L):
                    if not self._feed_armed_l:
                        return True
                    self._feed_armed_l = False
                    if not self._feed_fault_l:
                        self._feed_fault_l = format_ui(TX_LENGTH_NG_L)
                    self._feed_ng_l.set()
                    self._apply_detail_error(TX_LENGTH_NG_L, source="motion")
                    return True
                if byte_code == TX_LENGTH_OK_R:
                    if not self._feed_armed_r:
                        return True
                    self._feed_armed_r = False
                    self._feed_fault_r = ""
                    self._feed_ok_r.set()
                    if not self._cycle.is_active():
                        text = "Feed OK R — longitud en tolerancia (0x04A)"
                        self._set_banner(text, "ok")
                        self._set_motion_status(text, "ok")
                    return True
                if byte_code == TX_LENGTH_NG_R:
                    if not self._feed_armed_r:
                        return True
                    self._feed_armed_r = False
                    if not self._feed_fault_r:
                        self._feed_fault_r = format_ui(TX_LENGTH_NG_R)
                    self._feed_ng_r.set()
                    self._apply_detail_error(TX_LENGTH_NG_R, source="motion")
                    return True
                if byte_code in MOTION_DETAIL_ERROR_BYTES:
                    # laserR/L = material presente; E004/E005 = sin material → False.
                    if byte_code == TX_LASER_R:
                        self._motion["laserR"] = False
                    elif byte_code == TX_LASER_L:
                        self._motion["laserL"] = False
                    elif byte_code == TX_EXHAUST:
                        self._motion["safetyExhaust"] = True
                    self._apply_detail_error(byte_code, source="motion")
                    return True
                return False
            if mtype == "ack":
                ok = msg.get("ok", True)
                detail = msg.get("message", "")
                byte_code = int(msg.get("byte", 0) or 0)
                if (
                    self._pending_asda_ack is not None
                    and msg.get("actuator") == "asda"
                    and byte_code == self._pending_asda_ack.byte
                ):
                    self._pending_asda_ack.ok = bool(ok)
                    self._pending_asda_ack.message = str(detail or "")
                    self._pending_asda_ack.event.set()
                if msg.get("actuator") == "encoder" and not ok:
                    side = (
                        "R"
                        if byte_code in (CMD_ENC_MEASURE_R, CMD_ENC_SET0_R)
                        else "L"
                    )
                    text = "No instalado"
                    if detail:
                        text += f" — {detail}"
                    if side == "R":
                        self._motion["enc_r"] = text
                    else:
                        self._motion["enc_l"] = text
                    return True
                if msg.get("actuator") == "motion" and ok and byte_code == 0x16:
                    self._stopped_pending_resume = False
                    self._last_state_byte = None
                    text = detail or "Errores limpiados (0x016)"
                    self._set_banner(text, "ok")
                    self._set_motion_status(text, "ok")
                    return True
                if not ok:
                    # Feeder CAN no listo → EXXX E023 (0x61), no texto suelto.
                    detail_l = (detail or "").lower()
                    if msg.get("actuator") == "feeder" and (
                        "e023" in detail_l
                        or "can" in detail_l
                        or "servo can" in detail_l
                    ):
                        self._apply_detail_error(0x61, source="motion")
                        return True
                    if (
                        msg.get("actuator") == "asda"
                        and byte_code == CMD_ON
                        and self._motion_boot_awaiting_on_ack
                    ):
                        # Drive sin alimentación: el write Modbus falla. No es
                        # error de operación — reintentar sin spam de banner.
                        threading.Thread(
                            target=self._motion_boot_attempt_failed,
                            args=(detail or "Servo ON rechazado",),
                            daemon=True,
                            name="motion-boot-fail",
                        ).start()
                        return True
                    text = detail or "Comando rechazado"
                    _append_log(self._main_log, text)
                    _append_log(self._motion_log, text)
                    self._set_banner(text, "error")
                    self._set_motion_status(text, "error")
                    return True
                # Ack OK ASDA: Servo ON (0x04) / OFF (0x03)
                if msg.get("actuator") == "asda" and byte_code in (CMD_ON, CMD_OFF):
                    text = detail or (
                        "Servo ON (0x004)" if byte_code == CMD_ON else "Servo OFF (0x003)"
                    )
                    _append_log(self._motion_log, text)
                    self._set_banner(text, "ok")
                    self._set_motion_status(text, "ok")
                    # Arranque: tras Servo ON OK → Home (0x01), una sola vez.
                    if (
                        byte_code == CMD_ON
                        and self._motion_boot_awaiting_on_ack
                        and not self._motion_boot_prep_done
                    ):
                        self._motion_boot_awaiting_on_ack = False
                        self._motion_boot_prep_done = True
                        if self._motion_boot_on_fails:
                            _append_log(
                                self._motion_log,
                                "Arranque lineal: ASDA alimentado — Servo ON OK "
                                f"tras {self._motion_boot_on_fails} intentos",
                            )
                        threading.Thread(
                            target=self._motion_boot_send_home,
                            daemon=True,
                            name="motion-boot-home",
                        ).start()
                    elif byte_code == CMD_ON:
                        self._motion_boot_awaiting_on_ack = False
                    return True
                return False
            if mtype == "status":
                if msg.get("alarm", False):
                    err = msg.get("message", "") or "Alarma en módulo Motion"
                    if self._last_state_byte != TX_ERROR:
                        self._apply_state_byte(TX_ERROR)
                    _append_log(self._main_log, err)
                    _append_log(self._motion_log, err)
                    self._set_banner(err, "error")
                if "positionMm" in msg:
                    pos_mm = float(msg["positionMm"])
                    self._motion["asdaPositionMm"] = pos_mm
                    self._last_position_mm = pos_mm
                if self._move_target_mm is not None and "positionMm" in msg:
                    self._update_progress(float(msg["positionMm"]))
                if "laserR" in msg:
                    self._motion["laserR"] = bool(msg["laserR"])
                if "laserL" in msg:
                    self._motion["laserL"] = bool(msg["laserL"])
                if "safetyExhaust" in msg:
                    # Solo indicador de nivel IO. Set E006 = event 0x4F (edge Exhaust).
                    # No re-Set desde status: si no, tras Reset/reconnect el nivel
                    # activo (p.ej. pin flotando) vuelve a enclavar C1 sin edge.
                    self._motion["safetyExhaust"] = bool(msg["safetyExhaust"])
                return True
        return changed

    def _handle_plc_message(self, msg: dict) -> bool:
        with self._lock:
            mtype = msg.get("type", "")
            if mtype == "local_error":
                text = msg.get("message", "Error local")
                _append_log(self._plc_log, text)
                self._set_plc_status(text, "error")
                return True
            if mtype == "state" and msg.get("actuator") == "plc":
                byte_code = int(msg.get("byte", 0))
                if byte_code == self._plc["last_state_byte"]:
                    return False
                prev = self._plc["last_state_byte"]
                self._plc["last_state_byte"] = byte_code
                text = PLC_STATE_TEXT.get(byte_code, f"Estado PLC 0x{byte_code:02X}")
                kind = "info"
                if byte_code == TX_PLC_ERROR:
                    kind = "error"
                    self._plc_try_latch_once()
                    # No pisar EXXX (p.ej. E047) con "Error (0x027)" genérico.
                    latch = self._error_policy.latch
                    if (
                        latch.active
                        and latch.ui_text
                        and "plc" in (latch.module or "").lower()
                    ):
                        text = latch.ui_text
                    else:
                        # ErrorState sin EXXX aún: banner de máquina (como Motion 0x0C).
                        self._set_banner(text, "error")
                        _append_log(self._main_log, text)
                elif byte_code == TX_PLC_STOP:
                    kind = "warn"
                elif byte_code in (TX_PLC_IDLE, TX_PLC_RETURN):
                    # Res solo si sensores ya OK. Si sigue valve.error, el latch queda.
                    kind = "ok"
                self._set_plc_status(text, kind)
                return True
            if mtype == "event":
                byte_code = int(msg.get("byte", 0))
                if msg.get("actuator") == "plc":
                    if byte_code in PLC_ERROR_BYTES:
                        self._apply_detail_error(byte_code, source="plc")
                        for label, cmd, err_b, _ in PLC_VALVES:
                            if err_b == byte_code:
                                self._plc["valves"][str(cmd)]["error"] = True
                        return True
                    if "on" in msg:
                        cmd_b = byte_code
                        key = str(cmd_b)
                        if key in self._plc["valves"]:
                            self._plc["valves"][key]["on"] = bool(msg["on"])
                        return True
                field = msg.get("field", "")
                sensor_log = {
                    "sensorGripper": "Gripper",
                    "sensorHolder": "Holder",
                    "sensorCutter": "Cutter",
                    "sensorTray": "Encoder/tray",
                }
                if field in sensor_log:
                    active = bool(msg.get("value", False))
                    for label, cmd, _, sf in PLC_VALVES:
                        if sf == field:
                            self._plc["valves"][str(cmd)]["error"] = active
                    if active:
                        _append_log(
                            self._plc_log,
                            f"{sensor_log[field]} — sensor de seguridad activo",
                        )
                        self._plc_try_latch_once()
                    return True
                valve_field_map = {
                    "cutterR": CMD_CUTTER_R,
                    "cutterL": CMD_CUTTER_L,
                    "grippers": CMD_GRIPPER,
                    "holder": CMD_HOLDER,
                    "encoder": CMD_ENCODER,
                    "fgtray": CMD_ENCODER,
                    "blower": CMD_BLOWER,
                }
                # ``cutters`` es OR de R|L — no usarlo para el estado de botones.
                if field == "cutters":
                    return False
                if field in valve_field_map:
                    cmd_b = valve_field_map[field]
                    self._plc["valves"][str(cmd_b)]["on"] = bool(
                        msg.get("value", False)
                    )
                    return True
                return False
            if mtype == "status" and msg.get("role") == "plca":
                self._plc_sync_valves_from_status(msg)
                for _label, cmd, _err_b, sf in PLC_VALVES:
                    if sf and sf in msg:
                        self._plc["valves"][str(cmd)]["error"] = bool(msg[sf])
                self._plc_try_latch_once()
                return True
            if mtype == "ack":
                ok = msg.get("ok", True)
                detail = msg.get("message", "")
                if msg.get("actuator") == "plc":
                    if ok:
                        self._set_plc_status(detail or "Comando PLC OK", "ok")
                    else:
                        text = detail or "Comando rechazado"
                        _append_log(self._plc_log, text)
                        self._set_plc_status(text, "error")
                    return True
            return False

    def _apply_pf_side_sensors(self, side: dict, is_left: bool) -> bool:
        """Actualiza indicadores L/R desde snapshot status del Master."""
        changed = False
        for field, byte_l, byte_r in _PF_SIDE_SENSOR_FIELDS:
            if field not in side:
                continue
            key = str(byte_l if is_left else byte_r)
            if key not in self._pf["errors"]:
                continue
            active = bool(side.get(field))
            if self._pf["errors"][key]["active"] != active:
                self._pf["errors"][key]["active"] = active
                changed = True
        side_key = "L" if is_left else "R"
        runtime = self._pf.setdefault("sides", {}).setdefault(
            side_key,
            {
                "autoState": None,
                "triggerActive": None,
                "idleMode": False,
                "refillMaterial": False,
                "refillDereeler": False,
                "refillServo": False,
                "refillFeeder": False,
                "refillPulseS": 1.0,
            },
        )
        if "idleMode" in side:
            idle = bool(side.get("idleMode"))
            if runtime.get("idleMode") != idle:
                runtime["idleMode"] = idle
                changed = True
        if "autoState" in side:
            st = str(side.get("autoState") or "").strip().lower() or None
            if runtime.get("autoState") != st:
                runtime["autoState"] = st
                changed = True
        if "triggerActive" in side:
            trig = bool(side.get("triggerActive"))
            if runtime.get("triggerActive") != trig:
                runtime["triggerActive"] = trig
                changed = True
        for src, dst in (
            ("refillMaterial", "refillMaterial"),
            ("refillDereeler", "refillDereeler"),
            ("refillServo", "refillServo"),
            ("refillFeeder", "refillFeeder"),
        ):
            if src not in side:
                continue
            val = bool(side.get(src))
            if runtime.get(dst) != val:
                runtime[dst] = val
                changed = True
        if "refillPulseS" in side:
            try:
                pulse = max(0.2, min(10.0, float(side.get("refillPulseS"))))
            except (TypeError, ValueError):
                pulse = 1.0
            if runtime.get("refillPulseS") != pulse:
                runtime["refillPulseS"] = pulse
                changed = True
        return changed

    def _pf_feed_sides(self) -> str:
        try:
            return CycleConfig.normalize_feed_sides(self._cycle.get_config().feed_sides)
        except Exception:
            return "LR"

    def _pf_side_in_lot(self, is_left: bool) -> bool:
        """¿Este lado alimenta el lote? L-only no cuenta fallos de R (y al revés)."""
        mode = self._pf_feed_sides()
        if mode == "LR":
            return True
        return is_left if mode == "L" else (not is_left)

    def _pf_error_byte_in_lot(self, byte: int) -> bool:
        """¿El opcode EXXX pertenece al lado del lote?"""
        mode = self._pf_feed_sides()
        if mode == "LR":
            return True
        if mode == "L":
            return byte in PF_ERROR_BYTES_L
        return byte in PF_ERROR_BYTES_R

    def _pf_ok_when_active_in_lot(self, byte: int) -> bool:
        """Buffer/Holgura del lado del lote. El lado que no alimenta no enclava EXXX."""
        if byte not in _PF_OK_WHEN_ACTIVE:
            return True
        return self._pf_error_byte_in_lot(byte)

    def _pf_master_error_in_lot(self) -> bool:
        """errorAny del Master solo bloquea si el lado es el del lote."""
        if not self._pf.get("error_any"):
            return False
        if self._pf_is_operator_stop_only():
            return False
        side = str(self._pf.get("master_error_side") or "").upper()
        mode = self._pf_feed_sides()
        if side in ("L", "R"):
            return mode == "LR" or side == mode
        return self._pf_primary_active_error_byte() is not None

    def _pf_side_wire_err_id(self, side: dict) -> int:
        wire = int(side.get("errorCode") or 0)
        if 21 <= wire <= 27:
            return wire - 20
        if 31 <= wire <= 37:
            return wire - 30
        return 0

    def _pf_side_error_in_lot(self, side: dict, is_left: bool) -> bool:
        """¿El error enclavado del lado cuenta para la máquina (lote)?"""
        if not isinstance(side, dict) or not side.get("error"):
            return False
        if not self._pf_side_in_lot(is_left):
            return False
        err_id = self._pf_side_wire_err_id(side)
        if err_id == 5:
            byte = TX_BUFFER_FULL_L if is_left else TX_BUFFER_FULL_R
            return self._pf_ok_when_active_in_lot(byte)
        if err_id == 6:
            byte = TX_HOLGURA_L if is_left else TX_HOLGURA_R
            return self._pf_ok_when_active_in_lot(byte)
        return True

    def _apply_pf_side_fault(self, side: dict, is_left: bool) -> bool:
        """Sincroniza fault_active Buffer/Holgura desde error enclavado del lado."""
        if not isinstance(side, dict):
            return False
        err_on = bool(side.get("error"))
        err_id = self._pf_side_wire_err_id(side)
        # PfErrorId: BUFFER=5, HOLGURA=6 → opcodes TCP
        want: dict[int, bool] = {
            (TX_BUFFER_FULL_L if is_left else TX_BUFFER_FULL_R): False,
            (TX_HOLGURA_L if is_left else TX_HOLGURA_R): False,
        }
        if err_on and err_id == 5:
            want[TX_BUFFER_FULL_L if is_left else TX_BUFFER_FULL_R] = True
        elif err_on and err_id == 6:
            want[TX_HOLGURA_L if is_left else TX_HOLGURA_R] = True
        changed = False
        for byte, active in want.items():
            key = str(byte)
            if self._pf["fault_active"].get(key) != active:
                self._pf["fault_active"][key] = active
                changed = True
        return changed

    def _pf_primary_active_error_byte(self) -> int | None:
        """Return one active PF EXXX without using C1/C2/C3 classification."""
        if self._pf_materialist_now():
            return None
        active: list[int] = []
        for key, info in self._pf["errors"].items():
            try:
                byte = int(key)
            except (TypeError, ValueError):
                continue
            if not self._pf_error_byte_in_lot(byte):
                continue
            if byte in _PF_OK_WHEN_ACTIVE:
                if not self._pf["fault_active"].get(key):
                    continue
            elif not info.get("active"):
                continue
            if lookup(byte) is not None:
                active.append(byte)
        return min(active) if active else None

    def _pf_status_kind_for_byte(self, byte_code: int) -> str:
        if byte_code == TX_PF_ERROR:
            return "error" if self._pf_cached_has_fault() else "info"
        if byte_code == TX_PF_STOP:
            return "warn"
        if byte_code in (TX_PF_IDLE, TX_PF_RETURN):
            return "ok"
        return "info"

    def _pf_latch_is_prefeeder(self) -> bool:
        latch = self._error_policy.latch
        if not latch.active:
            return False
        mod = (latch.module or "").lower()
        return "pre" in mod or "feeder" in mod

    def _pf_cache_master_error(self, msg: dict) -> None:
        """Guarda el detalle del Master. Descarta ErrorState 0x3C."""
        me = msg.get("masterError")
        if not isinstance(me, dict):
            return
        if not me.get("active"):
            self._pf["master_error_ui"] = ""
            self._pf["master_error_side"] = ""
            return
        side = str(me.get("side") or "").strip().upper()
        self._pf["master_error_side"] = side if side in ("L", "R") else ""
        ui = str(me.get("ui") or "").strip()
        low = ui.lower()
        if ui and "errorstate" not in low and "0x03c" not in low:
            self._pf["master_error_ui"] = ui
            return
        exxx = str(me.get("exxx") or me.get("tag") or "").strip()
        if exxx.startswith("E") and lookup(exxx):
            self._pf["master_error_ui"] = format_ui(exxx)
            return
        low_tag = exxx.lower()
        if "007" in exxx or "operator" in low_tag:
            self._pf["master_error_ui"] = "PF-007"
            return
        self._pf["master_error_ui"] = ""

    def _pf_detail_ui_text(self) -> str | None:
        """Texto para el técnico: EXXX (o UI Master). Nunca 0x3C ni PF-007."""
        if self._pf_is_operator_stop_only():
            return None
        if self._pf_latch_is_prefeeder():
            ui = (self._error_policy.latch.ui_text or "").strip()
            if ui:
                return ui
        primary = self._pf_primary_active_error_byte()
        if primary is not None:
            return format_ui(primary)
        ui = str(self._pf.get("master_error_ui") or "").strip()
        return ui or None

    def _pf_paint_detail_status(self) -> bool:
        """Pinta el EXXX en la tarjeta PF. True si cambió el texto."""
        ui = self._pf_detail_ui_text()
        if not ui:
            return False
        before = (self._pf.get("status") or {}).get("text")
        self._set_pf_status(ui, "error")
        return before != ui

    def _refresh_pf_status_from_state(self) -> None:
        """Tras Res: no dejar el banner del módulo en EXXX si el Master ya salió."""
        if self._pf_cached_has_fault():
            self._pf_paint_detail_status()
            return
        byte_code = self._pf.get("last_state_byte")
        if not byte_code:
            self._set_pf_status("Listo.", "ok")
            return
        if int(byte_code) == TX_PF_ERROR:
            self._set_pf_status("Listo.", "ok")
            return
        text = PF_STATE_TEXT.get(
            int(byte_code), f"Estado PreFeeder 0x{int(byte_code):02X}"
        )
        self._set_pf_status(text, self._pf_status_kind_for_byte(int(byte_code)))

    def _pf_apply_state_byte(self, byte_code: int) -> bool:
        """Actualiza estado PF. True si cambió. Res latch al salir de ErrorState."""
        if not byte_code or byte_code == self._pf["last_state_byte"]:
            return False
        prev_byte = self._pf["last_state_byte"]
        self._pf["last_state_byte"] = byte_code
        if (
            prev_byte == TX_PF_ERROR
            and byte_code in _PF_CLEAR_LATCH_STATES
        ):
            # Auto ON → Busy (no Idle): antes solo Idle/Return hacían Res.

        if self._pf_cached_has_fault():
            # Idle/Busy no deben tapar un EXXX del lote (UI verde + ciclo bloqueado).
            self._pf_paint_detail_status()
            return True
        if byte_code == TX_PF_ERROR:
            # Como PLC: 0x3C no se pinta. El técnico ve el EXXX o nada aún.
            return True
        text = PF_STATE_TEXT.get(
            byte_code, f"Estado PreFeeder 0x{byte_code:02X}"
        )
        self._set_pf_status(text, self._pf_status_kind_for_byte(byte_code))
        return True

    def _pf_master_has_fault(self, msg: dict) -> bool:
        """¿Master reporta fallo del lote? El lado que no alimenta no cuenta."""
        if self._pf_materialist_now():
            return False
        has_sides = False
        for side_key, is_left in (("l", True), ("r", False)):
            side = msg.get(side_key)
            if not isinstance(side, dict):
                continue
            has_sides = True
            if self._pf_side_error_in_lot(side, is_left):
                return True
        if has_sides:
            return False
        if "errorAny" in msg and bool(msg.get("errorAny")):
            return self._pf_master_error_in_lot()
        me = msg.get("masterError")
        if isinstance(me, dict) and me.get("active"):
            return self._pf_master_error_in_lot()
        return False

    def _pf_try_clear_if_healthy(self, msg: dict) -> bool:
        """PF health updates never clear the global EXXX latch automatically."""
        if self._pf_master_has_fault(msg):
            return False
        st = self._pf.get("status") or {}
        if st.get("kind") == "error":
            self._refresh_pf_status_from_state()
            return True
        return False

    def _pf_try_latch_once(self) -> bool:
        """Un solo Set/log/Andon por fallo PF. Sensores viven en el panel HMI."""
        if self._error_policy.latch.active:
            # Latch ya puesto: no dejar la tarjeta en 0x3C genérico.
            if self._pf_latch_is_prefeeder():
                self._pf_paint_detail_status()
            return False
        primary = self._pf_primary_active_error_byte()
        if primary is None:
            return False
        return self._apply_detail_error(primary, source="prefeeder")

    def _handle_prefeeder_message(self, msg: dict) -> bool:
        with self._lock:
            mtype = msg.get("type", "")
            if mtype == "local_error":
                text = msg.get("message", "Error local")
                _append_log(self._pf_log, text)
                self._set_pf_status(text, "error")
                return True
            if mtype == "hello" and msg.get("role") == "prefeeder":
                return False
            if mtype == "status" and msg.get("actuator") == "prefeeder":
                changed = False
                entered_error = False
                any_rose = False
                byte_code = int(msg.get("byte") or 0)
                prev_byte = self._pf["last_state_byte"]
                if "errorAny" in msg:
                    any_fault = bool(msg.get("errorAny"))
                    any_rose = any_fault and not bool(self._pf.get("error_any"))
                    if bool(self._pf.get("error_any")) != any_fault:
                        changed = True
                    self._pf["error_any"] = any_fault
                self._pf_cache_master_error(msg)
                side_l = msg.get("l")
                side_r = msg.get("r")
                # Lados primero: el EXXX debe existir antes de pintar estado.
                prev_faults = {
                    k: bool(v) for k, v in self._pf["fault_active"].items()
                }
                if isinstance(side_l, dict):
                    changed = self._apply_pf_side_sensors(side_l, True) or changed
                    changed = self._apply_pf_side_fault(side_l, True) or changed
                if isinstance(side_r, dict):
                    changed = self._apply_pf_side_sensors(side_r, False) or changed
                    changed = self._apply_pf_side_fault(side_r, False) or changed
                pf_idle = bool(
                    (self._pf.get("sides") or {}).get("L", {}).get("idleMode")
                    or (self._pf.get("sides") or {}).get("R", {}).get("idleMode")
                )
                if self._pf_materialist_force_off:
                    if not pf_idle:
                        self._pf_materialist_force_off = False
                        if self._pf_materialist:
                            self._pf_materialist = False
                            changed = True
                elif self._pf_materialist != pf_idle:
                    self._pf_materialist = pf_idle
                    changed = True
                if self._pf_apply_state_byte(byte_code):
                    changed = True
                    entered_error = (
                        byte_code == TX_PF_ERROR and prev_byte != TX_PF_ERROR
                    )
                fault_rose = any(
                    bool(self._pf["fault_active"].get(k)) and not prev_faults.get(k)
                    for k in self._pf["fault_active"]
                )
                if self._pf_cached_has_fault():
                    if entered_error or fault_rose or any_rose:
                        changed = self._pf_try_latch_once() or changed
                    changed = self._pf_paint_detail_status() or changed
                elif self._pf_try_clear_if_healthy(msg):
                    changed = True
                elif (
                    byte_code == TX_PF_ERROR
                    and not self._pf_cached_has_fault()
                ):
                    self._refresh_pf_status_from_state()
                    changed = True
                return changed
            if mtype == "state" and msg.get("actuator") == "prefeeder":
                byte_code = int(msg.get("byte", 0))
                prev_byte = self._pf["last_state_byte"]
                if not self._pf_apply_state_byte(byte_code):
                    return False
                # Buffer/Holgura: fault_active ya viene de eventos previos o status.
                if byte_code == TX_PF_ERROR and prev_byte != TX_PF_ERROR:
                    self._pf_try_latch_once()
                    self._pf_paint_detail_status()
                return True
            if mtype == "event":
                byte_code = int(msg.get("byte", 0))
                if byte_code in PF_ERROR_BYTES:
                    active = bool(msg.get("active", True))
                    key = str(byte_code)
                    if byte_code in _PF_OK_WHEN_ACTIVE:
                        # Espejo para state ERROR en runtime. No Set aquí:
                        # Master antiguo publicaba sensor Buffer Full ON como active
                        # y al reconnect HMI enclavaba E058/E052 fantasma.
                        # Set real = status.error (+ errorCode) o state ERROR tras espejo.
                        prev = self._pf["fault_active"].get(key)
                        self._pf["fault_active"][key] = active
                        return prev != active
                    prev = self._pf["errors"].get(key, {}).get("active")
                    if key in self._pf["errors"]:
                        self._pf["errors"][key]["active"] = active
                    # Sensores → panel. Un solo Set/log/Andon (el EXXX primario).
                    if active and prev is not True:
                        self._pf_try_latch_once()
                        self._pf_paint_detail_status()
                    return prev != active
            if mtype == "ack":
                ok = msg.get("ok", True)
                detail = msg.get("message", "")
                if msg.get("actuator") == "prefeeder":
                    if ok:
                        self._set_pf_status(detail or "Comando PreFeeder OK", "ok")
                    else:
                        text = detail or "Comando rechazado"
                        _append_log(self._pf_log, text)
                        self._set_pf_status(text, "error")
                    return True
            return False


# Singleton para Flask
_state: HmiState | None = None


def get_state() -> HmiState:
    global _state
    if _state is None:
        _state = HmiState()
        _state.start()
    return _state
