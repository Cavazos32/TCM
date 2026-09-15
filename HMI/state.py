"""Estado compartido HMI — lógica TCP y estado para Flask."""

from __future__ import annotations

import json
import os
import threading
import time
from collections import deque
from datetime import datetime
from pathlib import Path
from typing import Any, Callable

HMI_ROOT = Path(__file__).resolve().parent
PLC_CONFIG_PATH = HMI_ROOT / "config" / "plc_config.json"
DEFAULT_BLOWER_SEC = 2.0

from cycle import CycleRunner, PROGRESS_STEPS
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
    TX_LASER_L,
    TX_LASER_R,
    TX_LENGTH_NG,
    TX_LENGTH_NG_L,
    TX_LENGTH_NG_R,
    TX_LENGTH_OK,
    TX_LENGTH_OK_L,
    TX_LENGTH_OK_R,
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
    PreFeederClient,
    TX_PF_BUSY,
    TX_PF_ERROR,
    TX_PF_IDLE,
    TX_PF_RETURN,
    TX_PF_STOP,
)
from error_catalog import format_ui
from error_policy import ErrorPolicy
from andon import AndonClient
from machine_states import MACH_ERROR, MACH_IDLE, MACH_RESET

MODELS_PATH = HMI_ROOT / "config" / "models.json"
MAX_LOG_LINES = 400
# No latchear E065–E067 en microcortes WiFi; solo si el enlace sigue caído.
LINK_DOWN_CONFIRM_SEC = 8.0

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
    0x39: "Inicializando PreFeeder (0x039)",
    TX_PF_IDLE: "PreFeeder en espera (0x03A)",
    TX_PF_BUSY: "PreFeeder ocupado (0x03B)",
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


def _ts() -> str:
    return datetime.now().strftime("%H:%M:%S")


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
        self._pf = {
            "connected": False,
            "status": {"text": "Listo.", "kind": "info"},
            "last_state_byte": None,
            "errors": {
                str(b): {"label": label, "active": None}
                for _, items in PF_ERRORS
                for label, b in items
            },
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

        # Señales para Cycle (espera Idle/Reached / LengthOK L+R)
        self._motion_idle_or_reached = threading.Event()
        self._feed_ok_l = threading.Event()
        self._feed_ok_r = threading.Event()
        self._feed_ng_l = threading.Event()
        self._feed_ng_r = threading.Event()
        self._pf_materialist = False

        self._error_policy = ErrorPolicy()
        self._cycle = CycleRunner(self)
        self._cycle.set_machine_state_hook(self._broadcast_machine_state)
        # Histéresis enlace: gen+1 cancela timer pendiente al recuperar.
        self._link_down_gen = {"motion": 0, "plc": 0, "prefeeder": 0}

        self._load_models()

    def start(self) -> None:
        self._client.start_background()
        self._plc_client.start_background()
        self._pf_client.start_background()
        if os.environ.get("ANDON_ENABLE", "0") == "1":
            self._andon_client.start_background()
        self._cycle.mark_init_done()

    def stop(self) -> None:
        self._enc_poll_stop.set()
        if self._cycle.is_active():
            self._cycle.request_stop()
        self._client.stop_background()
        self._plc_client.stop_background()
        self._pf_client.stop_background()
        if os.environ.get("ANDON_ENABLE", "0") == "1":
            self._andon_client.stop_background()

    def subscribe(self, callback: Callable[[], None]) -> None:
        with self._lock:
            self._listeners.append(callback)

    def unsubscribe(self, callback: Callable[[], None]) -> None:
        with self._lock:
            if callback in self._listeners:
                self._listeners.remove(callback)

    def _notify(self) -> None:
        listeners = list(self._listeners)
        for cb in listeners:
            try:
                cb()
            except Exception:
                pass

    def snapshot(self) -> dict[str, Any]:
        cycle_snap = self._cycle.snapshot()
        with self._lock:
            progress = self._cycle_live_progress(cycle_snap)
            resume = self._stopped_pending_resume or bool(
                cycle_snap.get("paused")
            )
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
                    "main": list(self._main_log),
                    "motion": list(self._motion_log),
                    "plc": list(self._plc_log),
                    "prefeeder": list(self._pf_log),
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
            else:
                logs = {
                    "main": self._main_log,
                    "motion": self._motion_log,
                    "plc": self._plc_log,
                    "prefeeder": self._pf_log,
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
            if self._client.connected:
                self.cmd_motion("enc_poll")
            time.sleep(0.6)

    # --- CycleHost (orquestación) ---

    def cycle_log(self, text: str) -> None:
        with self._lock:
            _append_log(self._main_log, text)
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

    def pf_state_byte(self) -> int | None:
        with self._lock:
            return self._pf.get("last_state_byte")

    def pf_is_materialist(self) -> bool:
        with self._lock:
            return self._pf_materialist

    def clear_motion_wait_flags(self) -> None:
        self._motion_idle_or_reached.clear()
        self._feed_ok_l.clear()
        self._feed_ok_r.clear()
        self._feed_ng_l.clear()
        self._feed_ng_r.clear()

    def clear_motion_reached_flag(self) -> None:
        """Solo Idle/Reached — conserva LengthOK/NG del prefetch en paralelo."""
        self._motion_idle_or_reached.clear()

    def wait_motion_idle_or_reached(self, timeout_s: float) -> bool:
        return self._motion_idle_or_reached.wait(timeout=timeout_s)

    def wait_feed_length_ok(self, timeout_s: float) -> bool:
        # OK en ambos lados; NG en cualquiera = fallo
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            if self._feed_ng_l.is_set() or self._feed_ng_r.is_set():
                return False
            if self._feed_ok_l.is_set() and self._feed_ok_r.is_set():
                return True
            time.sleep(0.05)
        return self._feed_ok_l.is_set() and self._feed_ok_r.is_set()

    def cmd_motion_move_mm(self, mm: float, rpm: float) -> bool:
        mm = abs(float(mm))
        with self._lock:
            self._move_target_mm = mm
            self._move_start_mm = self._last_position_mm
        return self._client.cmd_move_mm(mm, rpm)

    def cmd_motion_move_zero(self, rpm: float | None = None) -> bool:
        with self._lock:
            self._move_target_mm = 0.0
            self._move_start_mm = self._last_position_mm
            use_rpm = float(rpm) if rpm is not None else self._rpm
        return self._client.cmd_move_zero(use_rpm)

    def cmd_motion_stop(self) -> bool:
        return self._client.cmd_stop()

    def cmd_motion_feed_l(self) -> bool:
        return self._client.cmd_feed_l()

    def cmd_motion_feed_r(self) -> bool:
        return self._client.cmd_feed_r()

    def cmd_motion_enc_set0_r(self) -> bool:
        return self._client.cmd_enc_set0_r()

    def cmd_motion_enc_set0_l(self) -> bool:
        return self._client.cmd_enc_set0_l()

    def cmd_plc_holder(self, on: bool) -> bool:
        return self._plc_client.cmd_holder(on)

    def cmd_plc_gripper(self, on: bool) -> bool:
        return self._plc_client.cmd_gripper(on)

    def cmd_plc_cutters(self, on: bool) -> bool:
        ok_r = self._plc_client.cmd_cutter_r(on)
        ok_l = self._plc_client.cmd_cutter_l(on)
        return ok_r and ok_l

    def cmd_plc_all_safe(self) -> bool:
        return self._plc_client.cmd_all_off()

    def cmd_pf_start(self) -> bool:
        return self._pf_client.cmd_start()

    def cmd_pf_stop(self) -> bool:
        return self._pf_client.cmd_stop()

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
            _append_log(
                self._main_log,
                f"Cycle config recargada · {CycleRunner._delay_summary(cfg)}",
            )
        self._notify()
        return cfg.to_dict()

    # --- Comandos máquina / ciclo ---

    def cmd_start(self, qty: int | None = None) -> dict:
        """Start máquina (0x040): lote Cycle con mm/qty del modelo."""
        with self._lock:
            mm, rpm = self._mm, self._rpm
            model = self._models[self._selected_model_idx] if self._models else {}
            model_qty = int(model.get("qty", model.get("cantidad", 1)))
            use_qty = int(qty) if qty is not None and qty >= 1 else model_qty
            self._progress = 0
        return self._cycle.request_start(mm, use_qty, rpm)

    def cmd_stop(self) -> dict:
        if self._cycle.is_active() or self._cycle.snapshot().get("paused"):
            return self._cycle.request_stop()
        return {"ok": self._manual_motion(lambda: self._client.cmd_stop())}

    def cmd_resume(self) -> dict:
        snap = self._cycle.snapshot()
        if snap.get("paused"):
            return self._cycle.request_resume()
        return {"ok": self._manual_motion(lambda: self._client.cmd_resume())}

    def cmd_cycle_pause(self) -> dict:
        return self._cycle.request_pause()

    def cmd_cycle_reset(self) -> dict:
        return self.cmd_error_reset(confirm=False, do_home=False)

    def cmd_error_confirm(self) -> dict:
        """Confirma diálogo C1 (antes de Reset/home)."""
        ok = self._error_policy.confirm()
        if not ok:
            return {"ok": False, "error": "Sin error C1 pendiente de confirmar"}
        _append_log(self._main_log, f"C1 confirmado · {self._error_policy.latch.ui_text}")
        self._notify()
        return {"ok": True, "error": self._error_policy.snapshot()}

    def cmd_error_reset(self, confirm: bool = False, do_home: bool = False) -> dict:
        """
        Res del flip-flop: limpia latch + reset Motion/PLC/PF + ciclo Idle.
        C1: requiere confirm=True (o ya confirmado) y opcional do_home.
        """
        latch = self._error_policy.latch
        if latch.active and latch.needs_confirm:
            if confirm:
                self._error_policy.confirm()
            can, err = self._error_policy.can_reset()
            if not can:
                return {
                    "ok": False,
                    "error": err,
                    "needsConfirm": True,
                    "errorLatch": self._error_policy.snapshot(),
                }

        needs_home = latch.active and latch.needs_home
        recovery = latch.recovery
        ui = latch.ui_text

        # Reset módulos (Res)
        self._client.cmd_reset_errors()
        self._plc_client.cmd_reset()
        self._plc_apply_home_valve_cache()
        if self._pf_client.connected:
            self._pf_client.cmd_reset()

        old = self._error_policy.clear()
        cycle_res = self._cycle.request_reset()
        self._broadcast_machine_state(MACH_RESET)
        self._broadcast_machine_state(MACH_IDLE)

        home_ok = None
        if (do_home or needs_home) and needs_home:
            home_ok = self.cmd_motion_move_zero()
            _append_log(
                self._main_log,
                f"C1 home general · {'OK' if home_ok else 'FALLÓ'}",
            )

        self._banner = {"text": "Errores reseteados", "kind": "ok"}
        if ui:
            _append_log(self._main_log, f"Res flip-flop · {ui} · recovery={recovery}")
        self._notify()
        return {
            "ok": True,
            "cycle": cycle_res,
            "cleared": old.snapshot() if old.active else None,
            "homeOk": home_ok,
            "recovery": recovery,
        }

    def _broadcast_machine_state(self, byte: int) -> None:
        """Estado máquina → Andon (si hay enlace). No envía EXXX detalle."""
        try:
            self._andon_client.send_machine_byte(int(byte) & 0xFF)
        except Exception:
            pass

    def _apply_detail_error(self, code_or_byte: str | int, *, source: str = "") -> bool:
        """
        Set flip-flop + política C1/C2/C3.
        True si se aplicó un EXXX conocido.
        """
        result = self._error_policy.set_error(code_or_byte)
        if result is None:
            return False
        ui = result["ui"]
        cls = result["class"]
        action = result["action"]
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
        # Caída de enlace: no bombardear TCP (el socket ya está muerto → spam E06x).
        link_codes = {"E065", "E066", "E067"}
        if result.get("code") in link_codes:
            self._cycle.apply_error_policy(
                "link_down", ui, cls, result.get("recovery", "")
            )
        else:
            self._cycle.apply_error_policy(
                action, ui, cls, result.get("recovery", "")
            )
        self._broadcast_machine_state(MACH_ERROR)
        self._notify()
        return True

    def _on_andon_connection(self, connected: bool) -> None:
        if connected:
            _append_log(self._main_log, "Enlace Andon OK")
            self._broadcast_machine_state(self._cycle.state_byte())
        self._notify()

    def cmd_cycle_materialist(self, on: bool = True) -> dict:
        with self._lock:
            self._pf_materialist = on
        if on:
            self._manual_pf(lambda: self._pf_client.cmd_materialist())
        return self._cycle.set_materialist(on)

    def cmd_cycle_step_by_step(self, on: bool = True) -> dict:
        return self._cycle.set_step_by_step(on)

    def cmd_cycle_trial_mode(self, on: bool = True) -> dict:
        return self._cycle.set_trial_mode(on)

    def cmd_motion(self, action: str, **kwargs) -> dict:
        handlers = {
            "move": lambda: self._client.cmd_move_mm(
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
            "feed_l": lambda: self._client.cmd_feed_l(),
            "feed_r": lambda: self._client.cmd_feed_r(),
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
                    duration_sec = max(0.2, float(raw))
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
            ok = self._manual_plc(lambda: self._plc_client.cmd_reset())
            if ok:
                with self._lock:
                    self._plc_apply_home_valve_cache()
                    self._plc["status"] = {
                        "text": "Reset PLC — válvulas OFF / Home (0x01E)",
                        "kind": "ok",
                    }
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

    def cmd_pf(self, action: str) -> dict:
        handlers = {
            "start": self._pf_client.cmd_start,
            "stop": self._pf_client.cmd_stop,
            "reset": self._pf_client.cmd_reset,
            "materialist": self._pf_client.cmd_materialist,
            "trigger_r": self._pf_client.cmd_trigger_r,
            "trigger_l": self._pf_client.cmd_trigger_l,
        }
        fn = handlers.get(action)
        if not fn:
            return {"ok": False, "error": f"Acción PF desconocida: {action}"}
        ok = self._manual_pf(fn)
        if ok and action == "materialist":
            with self._lock:
                self._pf_materialist = True
            self._cycle.set_materialist(True)
        if ok and action == "reset":
            with self._lock:
                self._pf["status"] = {"text": "Reset enviado (0x02C)", "kind": "ok"}
                self._pf_materialist = False
            self._cycle.request_reset()
            self._notify()
        return {"ok": ok}

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
            self._notify()

    def _handle_andon_message(self, msg: dict) -> bool:
        mtype = msg.get("type", "")
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
        """Si el latch activo es solo de enlace TCP, límpialo al recuperar el socket."""
        latch = self._error_policy.latch
        if latch.active and latch.code == code:
            self._error_policy.clear()
            _append_log(self._main_log, f"Enlace recuperado · clear {code}")

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
            else:
                self._last_state_byte = None
                self._stopped_pending_resume = False
                self._motion["asdaPositionMm"] = None
                self._motion["enc_r"] = "—"
                self._motion["enc_l"] = "—"
                self._motion["laserR"] = False
                self._motion["laserL"] = False
                self._motion["safetyExhaust"] = False
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
            threading.Thread(
                target=self.refresh_feed_offset, daemon=True
            ).start()

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
                for e in self._pf["errors"].values():
                    e["active"] = None
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
        """Fuerza reconexión TCP a Motion, PLC y PreFeeder (en paralelo)."""
        results: dict[str, bool] = {}
        lock = threading.Lock()
        clients = (
            ("motion", self._client),
            ("plc", self._plc_client),
            ("prefeeder", self._pf_client),
        )

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
        self._last_state_byte = byte_code
        if byte_code in (TX_IDLE, TX_RETURN):
            self._motion_idle_or_reached.set()
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
            if self._move_target_mm is not None and self._progress < 5:
                self._progress = 5
        self._set_banner(text, kind)
        self._set_motion_status(text, kind)

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
                    text = f"{mm_off:.2f} mm"
                    if settled:
                        text += f"  (raw {mm_sig:.2f})"
                    else:
                        text += "  (moviendo…)"
                    if side == "R":
                        self._motion["enc_r"] = text
                    else:
                        self._motion["enc_l"] = text
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
                    self._feed_ok_l.set()
                    self._motion_idle_or_reached.set()
                    if not self._cycle.is_active():
                        text = "Feed OK L — longitud en tolerancia (0x014)"
                        self._set_banner(text, "ok")
                        self._set_motion_status(text, "ok")
                    return True
                if byte_code in (TX_LENGTH_NG, TX_LENGTH_NG_L):
                    self._feed_ng_l.set()
                    self._apply_detail_error(TX_LENGTH_NG_L, source="motion")
                    return True
                if byte_code == TX_LENGTH_OK_R:
                    self._feed_ok_r.set()
                    self._motion_idle_or_reached.set()
                    if not self._cycle.is_active():
                        text = "Feed OK R — longitud en tolerancia (0x04A)"
                        self._set_banner(text, "ok")
                        self._set_motion_status(text, "ok")
                    return True
                if byte_code == TX_LENGTH_NG_R:
                    self._feed_ng_r.set()
                    self._apply_detail_error(TX_LENGTH_NG_R, source="motion")
                    return True
                if byte_code in MOTION_DETAIL_ERROR_BYTES:
                    if byte_code == TX_LASER_R:
                        self._motion["laserR"] = True
                    elif byte_code == TX_LASER_L:
                        self._motion["laserL"] = True
                    elif byte_code == TX_EXHAUST:
                        self._motion["safetyExhaust"] = True
                    self._apply_detail_error(byte_code, source="motion")
                    return True
                return False
            if mtype == "ack":
                ok = msg.get("ok", True)
                detail = msg.get("message", "")
                byte_code = int(msg.get("byte", 0) or 0)
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
                self._plc["last_state_byte"] = byte_code
                text = PLC_STATE_TEXT.get(byte_code, f"Estado PLC 0x{byte_code:02X}")
                kind = "info"
                if byte_code == TX_PLC_ERROR:
                    kind = "error"
                elif byte_code == TX_PLC_STOP:
                    kind = "warn"
                elif byte_code in (TX_PLC_IDLE, TX_PLC_RETURN):
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
                sensor_fields = {
                    "sensorGripper": CMD_GRIPPER,
                    "sensorHolder": CMD_HOLDER,
                    "sensorCutter": CMD_CUTTER_R,
                    "sensorTray": CMD_ENCODER,
                }
                for sf, cmd_b in sensor_fields.items():
                    if sf in msg:
                        self._plc["valves"][str(cmd_b)]["error"] = bool(msg[sf])
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
        return changed

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
                byte_code = int(msg.get("byte") or 0)
                if byte_code and byte_code != self._pf["last_state_byte"]:
                    self._pf["last_state_byte"] = byte_code
                    text = PF_STATE_TEXT.get(
                        byte_code, f"Estado PreFeeder 0x{byte_code:02X}"
                    )
                    kind = "info"
                    if byte_code == TX_PF_ERROR:
                        kind = "error"
                    elif byte_code == TX_PF_STOP:
                        kind = "warn"
                    elif byte_code in (TX_PF_IDLE, TX_PF_RETURN):
                        kind = "ok"
                    self._set_pf_status(text, kind)
                    changed = True
                side_l = msg.get("l")
                side_r = msg.get("r")
                if isinstance(side_l, dict):
                    changed = self._apply_pf_side_sensors(side_l, True) or changed
                if isinstance(side_r, dict):
                    changed = self._apply_pf_side_sensors(side_r, False) or changed
                return changed
            if mtype == "state" and msg.get("actuator") == "prefeeder":
                byte_code = int(msg.get("byte", 0))
                if byte_code == self._pf["last_state_byte"]:
                    return False
                self._pf["last_state_byte"] = byte_code
                text = PF_STATE_TEXT.get(
                    byte_code, f"Estado PreFeeder 0x{byte_code:02X}"
                )
                kind = "info"
                if byte_code == TX_PF_ERROR:
                    kind = "error"
                elif byte_code == TX_PF_STOP:
                    kind = "warn"
                elif byte_code in (TX_PF_IDLE, TX_PF_RETURN):
                    kind = "ok"
                self._set_pf_status(text, kind)
                return True
            if mtype == "event":
                byte_code = int(msg.get("byte", 0))
                if byte_code in PF_ERROR_BYTES:
                    active = bool(msg.get("active", True))
                    key = str(byte_code)
                    prev = self._pf["errors"].get(key, {}).get("active")
                    if key in self._pf["errors"]:
                        self._pf["errors"][key]["active"] = active
                    if active and prev is not True:
                        self._apply_detail_error(byte_code, source="prefeeder")
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
