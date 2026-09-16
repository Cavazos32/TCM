"""Cycle — orquestador de lote TCM (secuencia; no perfiles de Motion).
Habla con Motion / PLC / PreFeeder por TCP. Delays de secuencia son
configurables (cycle_config.json). Estado máquina bytes 0x40–0x49.
"""
from __future__ import annotations
import json
import threading
import time
from dataclasses import asdict, dataclass, fields
from pathlib import Path
from typing import Any, Callable, Protocol
from prefeeder import TX_PF_BUSY, TX_PF_ERROR
from machine_states import (
    CMD_RESET,
    CMD_START,
    CMD_STOP,
    MACHINE_STATE_BYTES,
    STATE_LABELS,
    TX_BUSY,
    TX_ERROR,
    TX_FINISH,
    TX_IDLE,
    TX_INIT,
    TX_MATERIALIST,
    TX_PAUSE,
    TX_STOP,
)
from error_catalog import format_ui

HMI_ROOT = Path(__file__).resolve().parent
CONFIG_PATH = HMI_ROOT / "config" / "cycle_config.json"

# Protocolo máquina: machine_states.py (0x40–0x49). Andon solo refleja esos bytes.
# Pasos atómicos (acción / delay independientes).
# kind=parallel SOLO en: arranque prefetch (background) y join/handoff.
# El resto es secuencia principal (action|wait) — no implica “todo a la vez”.
FLOW_STEPS: list[dict[str, Any]] = [
    {"id": 1, "key": "holder_on", "label": "Holder+Encoder ON (solo 1ª pieza)", "kind": "action"},
    {"id": 2, "key": "wait_holder_on", "label": "Delay Holder ON", "kind": "wait", "delayKey": "holderOnMs"},
    {"id": 3, "key": "feed", "label": "Alimentación (feed / handoff)", "kind": "action"},
    {"id": 4, "key": "offset", "label": "Offset alimentación (Motion, paso lógico)", "kind": "action"},
    {"id": 5, "key": "grippers_on", "label": "Pinzas cierran", "kind": "action"},
    {"id": 6, "key": "wait_grippers_on", "label": "Delay tras cerrar pinzas", "kind": "wait", "delayKey": "grippersOnMs"},
    {"id": 7, "key": "enc_set0", "label": "Encoder Set0 (OM)", "kind": "action"},
    {"id": 8, "key": "holder_off", "label": "Holder+Encoder se mantienen ON", "kind": "action"},
    {"id": 9, "key": "wait_holder_open", "label": "Delay (holder se mantiene)", "kind": "wait", "delayKey": "holderOpenMs"},
    {"id": 10, "key": "lineal_fwd", "label": "Lineal FWD → posición de corte", "kind": "action"},
    {"id": 11, "key": "wait_linear_done", "label": "Delay antes del corte", "kind": "wait", "delayKey": "linearDoneMs"},
    {"id": 12, "key": "holder_precut", "label": "Confirmar Holder+Encoder ON (pre-corte)", "kind": "action"},
    {"id": 13, "key": "wait_holder_precut", "label": "Delay tras confirmar holder", "kind": "wait", "delayKey": "holderOnMs"},
    {"id": 14, "key": "cutter_on", "label": "Cortador ON (+ All OK PreFeeder)", "kind": "action"},
    {"id": 15, "key": "wait_cutter_pulse", "label": "Delay pulso de corte", "kind": "wait", "delayKey": "cutterPulseMs"},
    {"id": 16, "key": "cutter_off", "label": "Cortador OFF", "kind": "action"},
    {"id": 17, "key": "wait_cutter_post", "label": "Delay post-corte", "kind": "wait", "delayKey": "cutterPostMs"},
    {
        "id": 18,
        "key": "prefetch_start",
        "label": "Prefetch feed — arranca en background",
        "kind": "parallel",
        "parallelRole": "start",
    },
    {"id": 19, "key": "deposit", "label": "Extra / depósito lineal", "kind": "action"},
    {"id": 20, "key": "wait_deposit_dwell", "label": "Delay tras depósito", "kind": "wait", "delayKey": "dwellAtDestMs"},
    {"id": 21, "key": "grippers_off", "label": "Pinzas abren", "kind": "action"},
    {"id": 22, "key": "pf_trigger", "label": "Trigger PreFeeder (Tfeed)", "kind": "action"},
    {"id": 23, "key": "wait_gripper_release", "label": "Delay antes de HOME", "kind": "wait", "delayKey": "gripperReleaseMs"},
    {"id": 24, "key": "home", "label": "Lineal HOME", "kind": "action"},
    {
        "id": 25,
        "key": "handoff",
        "label": "Join — espera fin del prefetch (handoff)",
        "kind": "parallel",
        "parallelRole": "join",
    },
    {"id": 26, "key": "wait_asentar", "label": "Delay asentar", "kind": "wait", "delayKey": "asentarMs"},
    {"id": 27, "key": "post_piece", "label": "Post-pieza (safety / peer / holgura)", "kind": "action"},
]
PROGRESS_STEPS = len(FLOW_STEPS)
STEP_NAMES = {0: "idle", **{s["id"]: s["key"] for s in FLOW_STEPS}}
STEP_BY_KEY = {s["key"]: s for s in FLOW_STEPS}
# Pasos sin pausa automática en modo paso a paso (igual que firmware TCM).
STEP_BY_STEP_NO_PAUSE = frozenset({"listo", "rep-start", "post_piece"})
@dataclass

class CycleConfig:
    holder_on_ms: int = 200
    holder_open_ms: int = 100
    grippers_on_ms: int = 100
    gripper_release_ms: int = 350
    cutter_pulse_ms: int = 100
    cutter_post_ms: int = 100
    linear_done_ms: int = 100
    asentar_ms: int = 50
    dwell_at_dest_ms: int = 150
    deposit_batch_size: int = 50
    deposit_extra_mm: float = 30.0
    cut_offset_mm: float = 0.0
    motion_wait_timeout_s: float = 120.0
    feed_wait_timeout_s: float = 30.0
    pf_ready_timeout_s: float = 10.0
    @staticmethod
    def _key_map() -> dict[str, str]:
        return {
            "holderOnMs": "holder_on_ms",
            "holderOpenMs": "holder_open_ms",
            "grippersOnMs": "grippers_on_ms",
            "gripperReleaseMs": "gripper_release_ms",
            "cutterPulseMs": "cutter_pulse_ms",
            "cutterPostMs": "cutter_post_ms",
            "linearDoneMs": "linear_done_ms",
            "asentarMs": "asentar_ms",
            "dwellAtDestMs": "dwell_at_dest_ms",
            "depositBatchSize": "deposit_batch_size",
            "depositExtraMm": "deposit_extra_mm",
            "cutOffsetMm": "cut_offset_mm",
            "motionWaitTimeoutS": "motion_wait_timeout_s",
            "feedWaitTimeoutS": "feed_wait_timeout_s",
            "pfReadyTimeoutS": "pf_ready_timeout_s",
        }
    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> CycleConfig:
        int_attrs = {
            "holder_on_ms",
            "holder_open_ms",
            "grippers_on_ms",
            "gripper_release_ms",
            "cutter_pulse_ms",
            "cutter_post_ms",
            "linear_done_ms",
            "asentar_ms",
            "dwell_at_dest_ms",
            "deposit_batch_size",
        }
        float_attrs = {
            "deposit_extra_mm",
            "cut_offset_mm",
            "motion_wait_timeout_s",
            "feed_wait_timeout_s",
            "pf_ready_timeout_s",
        }
        kw: dict[str, Any] = {}
        for json_key, attr in cls._key_map().items():
            if json_key in data:
                raw = data[json_key]
            elif attr in data:
                raw = data[attr]
            else:
                continue
            try:
                if attr in int_attrs:
                    kw[attr] = int(float(raw))
                elif attr in float_attrs:
                    kw[attr] = float(raw)
                else:
                    kw[attr] = raw
            except (TypeError, ValueError):
                continue
        return cls(**kw)
    def to_dict(self) -> dict[str, Any]:
        inv = {v: k for k, v in self._key_map().items()}
        out: dict[str, Any] = {}
        for f in fields(self):
            out[inv[f.name]] = getattr(self, f.name)
        return out
def load_cycle_config(path: Path = CONFIG_PATH) -> CycleConfig:
    if path.exists():
        try:
            return CycleConfig.from_dict(json.loads(path.read_text(encoding="utf-8")))
        except (json.JSONDecodeError, OSError, TypeError, ValueError):
            pass
    cfg = CycleConfig()
    save_cycle_config(cfg, path)
    return cfg
def save_cycle_config(cfg: CycleConfig, path: Path = CONFIG_PATH) -> None:
    path.write_text(
        json.dumps(cfg.to_dict(), indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )

class CycleHost(Protocol):
    """Puente hacia clientes TCP + señales de estado (implementado por HmiState)."""
    def cycle_log(self, text: str) -> None: ...
    def cycle_notify(self) -> None: ...
    def motion_connected(self) -> bool: ...
    def plc_connected(self) -> bool: ...
    def pf_connected(self) -> bool: ...
    def motion_state_byte(self) -> int | None: ...
    def pf_state_byte(self) -> int | None: ...
    def pf_is_materialist(self) -> bool: ...
    def clear_motion_wait_flags(self) -> None: ...
    def clear_motion_reached_flag(self) -> None: ...
    def wait_motion_idle_or_reached(self, timeout_s: float) -> bool: ...
    def wait_feed_length_ok(self, timeout_s: float) -> bool: ...
    def cmd_motion_move_mm(self, mm: float, rpm: float) -> bool: ...
    def cmd_motion_move_zero(self, rpm: float) -> bool: ...
    def cmd_motion_stop(self) -> bool: ...
    def cmd_motion_feed_l(self) -> bool: ...
    def cmd_motion_feed_r(self) -> bool: ...
    def cmd_motion_enc_set0_r(self) -> bool: ...
    def cmd_motion_enc_set0_l(self) -> bool: ...
    def cmd_plc_holder(self, on: bool) -> bool: ...
    def cmd_plc_encoder(self, on: bool) -> bool: ...
    def cmd_plc_gripper(self, on: bool) -> bool: ...
    def cmd_plc_cutters(self, on: bool) -> bool: ...
    def cmd_plc_all_safe(self) -> bool: ...
    def cmd_pf_start(self) -> bool: ...
    def cmd_pf_stop(self) -> bool: ...
    def cmd_pf_trigger_r(self) -> bool: ...
    def cmd_pf_trigger_l(self) -> bool: ...
    def apply_detail_error(self, code_or_byte: str | int) -> bool: ...

class CycleRunner:
    """Ejecuta el flujo de CycleFlowCopy en un hilo (orquestación)."""
    def __init__(self, host: CycleHost) -> None:
        self._host = host
        self._lock = threading.Lock()
        self._cfg = load_cycle_config()
        self._thread: threading.Thread | None = None
        self._stop = threading.Event()
        self._pause = threading.Event()
        self._pause.clear()  # not paused
        self._state_byte = TX_INIT
        self._active = False
        self._aborted = False
        self._materialist = False
        self._step_by_step = False
        self._trial_mode = False
        self._step = 0
        self._parallel_group = ""
        self._rep = 0
        self._pieces_done = 0
        self._total_reps = 0
        self._progress = 0
        self._last_ok = False
        self._fault = ""
        self._fault_class = ""
        self._c3_stop_after_step = False  # C3: terminar paso actual
        self._recovery = ""  # home | restart_from_0 | retry_process
        self._lot_rpm = 1200.0
        self._started_at: float | None = None
        self._finished_elapsed = 0.0
        self._on_machine_state: Callable[[int], None] | None = None

    def set_machine_state_hook(self, cb: Callable[[int], None] | None) -> None:
        """HMI registra envío a Andon (0x40–0x49)."""
        self._on_machine_state = cb
    # --- snapshot / config ---
    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            return {
                "byte": self._state_byte,
                "name": STATE_LABELS.get(self._state_byte, "—"),
                "active": self._active,
                "paused": self._pause.is_set(),
                "materialist": self._materialist,
                "stepByStep": self._step_by_step,
                "trialMode": self._trial_mode,
                "step": self._step,
                "stepName": STEP_NAMES.get(self._step, ""),
                "stepLabel": next(
                    (s["label"] for s in FLOW_STEPS if s["id"] == self._step),
                    "",
                ),
                "parallelGroup": self._parallel_group,
                "rep": self._rep,
                "piecesDone": self._pieces_done,
                "totalReps": self._total_reps,
                "progress": self._progress,
                "elapsedSec": (
                    int(time.monotonic() - self._started_at)
                    if self._started_at and self._active
                    else int(self._finished_elapsed)
                ),
                "completed": self._last_ok and not self._active,
                "lastOk": self._last_ok,
                "fault": self._fault,
                "faultClass": self._fault_class,
                "recovery": self._recovery,
                "c3Pending": self._c3_stop_after_step,
                "config": self._cfg.to_dict(),
                "flow": FLOW_STEPS,
            }
    def get_config(self) -> CycleConfig:
        with self._lock:
            return CycleConfig(**asdict(self._cfg))

    def reload_config(self) -> CycleConfig:
        """Recarga cycle_config.json — usado al Start para aplicar últimos guardados."""
        with self._lock:
            self._cfg = load_cycle_config()
            return CycleConfig(**asdict(self._cfg))

    @staticmethod
    def _delay_summary(cfg: CycleConfig) -> str:
        return (
            f"Delays — holderOn={cfg.holder_on_ms}ms open={cfg.holder_open_ms}ms "
            f"grippers={cfg.grippers_on_ms}ms cutter={cfg.cutter_pulse_ms}ms "
            f"postCut={cfg.cutter_post_ms}ms linear={cfg.linear_done_ms}ms "
            f"dwell={cfg.dwell_at_dest_ms}ms gripRel={cfg.gripper_release_ms}ms "
            f"asentar={cfg.asentar_ms}ms"
        )

    def update_config(self, data: dict[str, Any]) -> CycleConfig:
        with self._lock:
            merged = self._cfg.to_dict()
            merged.update(data)
            self._cfg = CycleConfig.from_dict(merged)
            save_cycle_config(self._cfg)
            cfg = CycleConfig(**asdict(self._cfg))
        self._host.cycle_log(f"Cycle config guardada · {self._delay_summary(cfg)}")
        return cfg
    def state_byte(self) -> int:
        with self._lock:
            return self._state_byte
    def is_active(self) -> bool:
        with self._lock:
            return self._active
    # --- comandos máquina ---
    def request_start(self, length_mm: float, qty: int, rpm: float) -> dict[str, Any]:
        with self._lock:
            if self._materialist:
                return {"ok": False, "error": "Máquina en Materialist (0x049)"}
            if self._active:
                return {"ok": False, "error": "Ciclo ocupado (0x045)"}
            if self._thread and self._thread.is_alive():
                self._thread.join(timeout=0.2)
                if self._thread.is_alive():
                    return {"ok": False, "error": "Ciclo ocupado (0x045)"}
            if qty < 1:
                return {"ok": False, "error": "Cantidad inválida"}
        if not self._host.motion_connected() or not self._host.plc_connected():
            return {"ok": False, "error": "Motion/PLC sin enlace"}
        if self._host.pf_is_materialist():
            self._set_state(TX_MATERIALIST, "PreFeeder Materialist")
            return {"ok": False, "error": "PreFeeder en Materialist"}
        cfg = self.reload_config()
        self._host.cycle_log(self._delay_summary(cfg))
        self._stop.clear()
        self._pause.clear()
        self._aborted = False
        self._fault = ""
        self._fault_class = ""
        self._c3_stop_after_step = False
        self._recovery = ""
        self._last_ok = False
        self._set_state(TX_BUSY)
        self._host.cycle_log(
            f"Cycle Start (0x040) length={length_mm} mm qty={qty}"
        )
        args = (float(length_mm), int(qty), float(rpm))
        self._thread = threading.Thread(
            target=self._run_lot, args=args, daemon=True, name="CycleRunner"
        )
        self._thread.start()
        return {"ok": True}
    def request_stop(self) -> dict[str, Any]:
        self._aborted = True
        self._stop.set()
        self._pause.clear()
        self._host.cmd_motion_stop()
        self._host.cmd_pf_stop()
        self._host.cmd_plc_all_safe()
        self._set_state(TX_STOP, "Stop (0x042)")
        self._host.cycle_log("Cycle Stop (0x041)")
        return {"ok": True}
    def request_pause(self) -> dict[str, Any]:
        if not self.is_active():
            return {"ok": False, "error": "Sin ciclo activo"}
        self._pause.set()
        self._enter_pause_andon()
        self._host.cycle_log("Cycle Pause (0x048)")
        self._host.cycle_notify()
        return {"ok": True}
    def request_resume(self) -> dict[str, Any]:
        if self._pause.is_set():
            self._pause.clear()
            self._leave_pause_andon()
            self._host.cycle_log("Cycle Resume")
            self._host.cycle_notify()
            return {"ok": True}
        return {"ok": False, "error": "Ciclo no está en Pause"}
    def request_reset(self) -> dict[str, Any]:
        if self.is_active():
            return {"ok": False, "error": "Detener ciclo antes de Reset"}
        self._aborted = False
        self._fault = ""
        self._fault_class = ""
        self._c3_stop_after_step = False
        self._recovery = ""
        self._last_ok = False
        self._materialist = False
        self._pieces_done = 0
        self._finished_elapsed = 0.0
        self._set_progress(0, 0, 0)
        self._set_state(TX_IDLE)
        self._host.cycle_log("Cycle Reset (0x043)")
        return {"ok": True}
    def set_materialist(self, on: bool) -> dict[str, Any]:
        if on and self.is_active():
            return {"ok": False, "error": "No Materialist con ciclo activo"}
        self._materialist = on
        if on:
            self._set_state(TX_MATERIALIST)
            self._host.cycle_log("Cycle Materialist ON (0x049)")
        else:
            self._set_state(TX_IDLE)
            self._host.cycle_log("Cycle Materialist OFF → Idle")
        return {"ok": True}
    def set_step_by_step(self, on: bool) -> dict[str, Any]:
        if on and self._materialist:
            return {"ok": False, "error": "No paso a paso en Materialist (0x049)"}
        if on and self.is_active():
            return {"ok": False, "error": "No cambiar paso a paso con ciclo activo"}
        self._step_by_step = on
        self._host.cycle_log(f"Paso a paso {'ON' if on else 'OFF'}")
        self._host.cycle_notify()
        return {"ok": True, "stepByStep": on}

    def set_trial_mode(self, on: bool) -> dict[str, Any]:
        if on and self.is_active():
            return {"ok": False, "error": "No cambiar modo prueba con ciclo activo"}
        self._trial_mode = on
        self._host.cycle_log(
            f"Modo prueba en vacío {'ON' if on else 'OFF'} (bypass sensores/encoder)"
        )
        self._host.cycle_notify()
        return {"ok": True, "trialMode": on}
    def mark_init_done(self) -> None:
        with self._lock:
            if self._state_byte == TX_INIT and not self._active:
                self._state_byte = TX_IDLE
        self._host.cycle_notify()
    # --- interno ---
    def _set_state(self, byte: int, detail: str = "") -> None:
        with self._lock:
            self._state_byte = byte
            if detail and byte in (TX_ERROR, TX_STOP):
                self._fault = format_ui(detail, fallback=detail)
        self._host.cycle_notify()
        hook = self._on_machine_state
        if hook:
            try:
                hook(byte)
            except Exception:
                pass

    def _enter_pause_andon(self) -> None:
        """Pausa operativa → amarillo (0x48). No pisa Error (prioridad)."""
        with self._lock:
            if self._state_byte == TX_ERROR:
                return
        self._set_state(TX_PAUSE)

    def _leave_pause_andon(self) -> None:
        """Resume → Busy si el ciclo sigue activo y no hay Error."""
        with self._lock:
            if self._state_byte == TX_ERROR:
                return
            if not self._active:
                return
        self._set_state(TX_BUSY)

    def _raise_fault(self, slug_or_code: str, err_class: str = "") -> None:
        """Latchea fallo EXXX vía política HMI (Set flip-flop)."""
        # Si Motion ya latcheó un EXXX (p.ej. E023 CAN), no pisar con genérico de ciclo.
        if self._fault:
            return
        if hasattr(self._host, "apply_detail_error"):
            if self._host.apply_detail_error(slug_or_code):
                return
        self._fault = format_ui(slug_or_code, fallback=slug_or_code)
        if err_class:
            self._fault_class = err_class
        self._set_state(TX_ERROR, self._fault)

    def apply_error_policy(self, action: str, ui: str, err_class: str, recovery: str) -> dict[str, Any]:
        """
        Aplica Set C1/C2/C3 desde HmiState.
        action: stop_all | pause | finish_step
        """
        self._fault = ui
        self._fault_class = err_class
        self._recovery = recovery
        if action == "stop_all":
            self._c3_stop_after_step = False
            self._aborted = True
            self._stop.set()
            self._pause.clear()
            # Best-effort: un módulo caído no debe tumbar la política.
            for fn in (
                self._host.cmd_motion_stop,
                self._host.cmd_pf_stop,
                self._host.cmd_plc_all_safe,
            ):
                try:
                    fn()
                except Exception:
                    pass
            self._set_state(TX_ERROR, ui)
            return {"ok": True, "action": action}
        if action == "link_down":
            # Solo abortar ciclo local; no mandar stop por TCP al nodo caído.
            self._c3_stop_after_step = False
            self._aborted = True
            self._stop.set()
            self._pause.clear()
            self._set_state(TX_ERROR, ui)
            self._host.cycle_notify()
            return {"ok": True, "action": action}
        if action == "pause":
            self._c3_stop_after_step = False
            if self.is_active():
                self._pause.set()
            self._set_state(TX_ERROR, ui)
            self._host.cycle_notify()
            return {"ok": True, "action": action}
        if action == "finish_step":
            self._c3_stop_after_step = True
            self._set_state(TX_ERROR, ui)
            self._host.cycle_notify()
            return {"ok": True, "action": action}
        self._set_state(TX_ERROR, ui)
        return {"ok": True, "action": "error_state"}
    def _set_progress(self, rep: int, step: int, total: int) -> None:
        meta = next((s for s in FLOW_STEPS if s["id"] == step), {})
        with self._lock:
            self._rep = rep
            self._step = step
            self._total_reps = total
            # Solo exponer paralelo en pasos kind=parallel (start/join)
            if meta.get("kind") == "parallel":
                self._parallel_group = str(meta.get("parallelRole") or "parallel")
            else:
                self._parallel_group = ""
            if total > 0:
                frac = ((rep - 1) + step / max(PROGRESS_STEPS, 1)) / total
                self._progress = max(0, min(100, int(frac * 100)))
            else:
                self._progress = 0
        self._host.cycle_notify()
    def _enter(self, rep: int, qty: int, key: str) -> bool:
        """Marca paso atómico. True = abortar."""
        # C3: tras terminar el paso previo, no lanzar el siguiente
        if self._c3_stop_after_step:
            self._c3_stop_after_step = False
            self._pause.set()
            self._host.cycle_log("Paso terminado — pausa para Resume/Reset")
            self._host.cycle_notify()
            return True
        meta = STEP_BY_KEY[key]
        self._set_progress(rep, int(meta["id"]), qty)
        if self._pause_before_step(key):
            return True
        return self._gate(key)

    def _pause_before_step(self, step_key: str) -> bool:
        """Pausa cooperativa antes de ejecutar un paso (modo paso a paso)."""
        if self._should_abort():
            return True
        if not self._step_by_step_should_pause(step_key):
            return False
        meta = STEP_BY_KEY.get(step_key, {})
        label = meta.get("label", step_key)
        self._pause.set()
        self._host.cycle_log(f"Paso a paso — listo: {label}")
        self._enter_pause_andon()
        self._host.cycle_notify()
        while self._pause.is_set():
            if self._should_abort():
                return True
            time.sleep(0.05)
        self._leave_pause_andon()
        return False
    def _do_wait(self, rep: int, qty: int, key: str, cfg_attr: str) -> bool:
        """Paso delay independiente. Lee ms frescos de config. True = abortar."""
        if self._enter(rep, qty, key):
            return True
        ms = int(getattr(self.get_config(), cfg_attr, 0) or 0)
        meta = STEP_BY_KEY.get(key, {})
        label = meta.get("label", key)
        self._host.cycle_log(f"Delay · {label}: {ms} ms")
        if self._pausable_delay(ms):
            return True
        if self._after_step(key):
            return True
        return self._should_abort()
    def _should_abort(self) -> bool:
        return self._stop.is_set() or self._aborted
    def _pausable_delay(self, ms: int) -> bool:
        """Delay de secuencia. El tiempo en Pause no consume el delay. True = abort."""
        if ms <= 0:
            return self._should_abort()
        remaining = ms / 1000.0
        while remaining > 0:
            if self._should_abort():
                return True
            if self._pause.is_set():
                while self._pause.is_set():
                    if self._should_abort():
                        return True
                    time.sleep(0.05)
                self._leave_pause_andon()
                continue
            slice_s = min(0.02, remaining)
            t0 = time.monotonic()
            time.sleep(slice_s)
            # Si entró Pause durante el sleep, no descontar ese tramo.
            if self._pause.is_set():
                continue
            remaining -= time.monotonic() - t0
        return self._should_abort()
    def _step_by_step_should_pause(self, step_key: str) -> bool:
        if not self._step_by_step:
            return False
        if step_key in STEP_BY_STEP_NO_PAUSE:
            return False
        return bool(step_key)
    def _after_step(self, step_key: str) -> bool:
        """Pausa cooperativa tras completar un paso atómico. True = abortar."""
        if self._should_abort():
            return True
        paused_here = False
        if self._step_by_step_should_pause(step_key):
            meta = STEP_BY_KEY.get(step_key, {})
            label = meta.get("label", step_key)
            self._pause.set()
            self._host.cycle_log(f"Paso a paso — {label}")
            self._enter_pause_andon()
            paused_here = True
            self._host.cycle_notify()
        while self._pause.is_set():
            if self._should_abort():
                return True
            time.sleep(0.05)
        if paused_here:
            self._leave_pause_andon()
        return False
    def _gate(self, step_name: str) -> bool:
        """Tras un paso: Pause/Stop. True = salir del lote."""
        _ = step_name
        if self._should_abort():
            return True
        while self._pause.is_set():
            if self._should_abort():
                return True
            time.sleep(0.05)
        return False
    def _wait_motion(self) -> bool:
        """Espera Idle/Reached. El caller debe limpiar flags ANTES del comando
        (clear_motion_wait_flags / clear_motion_reached_flag) — no limpiar aquí
        o se pierde el evento si Motion responde entre el cmd y el wait."""
        cfg = self.get_config()
        ok = self._host.wait_motion_idle_or_reached(cfg.motion_wait_timeout_s)
        if not ok:
            self._raise_fault("timeout_motion")
            self._host.cycle_log(format_ui("E008"))
        return ok
    def _wait_feed(self) -> bool:
        if self._trial_mode:
            self._host.cycle_log("Trial: feed OK omitido (bypass encoder)")
            return True
        cfg = self.get_config()
        ok = self._host.wait_feed_length_ok(cfg.feed_wait_timeout_s)
        if not ok:
            self._raise_fault("timeout_feed")
            self._host.cycle_log(format_ui("E009"))
        return ok
    def _effective_mm(self, length_mm: float) -> float:
        cfg = self.get_config()
        return abs(float(length_mm)) + float(cfg.cut_offset_mm)
    def _arm_holder_encoder(self) -> None:
        """Holder + Encoder (bandeja) ON desde Start hasta fin de pieza/lote."""
        self._host.cmd_plc_holder(True)
        self._host.cmd_plc_encoder(True)

    def _prepare_before_cut(self) -> bool:
        # Start: all_safe (cutters/grippers) y rearmar Holder+Encoder ON.
        # Permanecen ON el lote; liberación solo en _finish (all_safe).
        # El delay holderOnMs es el paso wait_holder_on (solo 1ª pieza).
        self._host.cycle_log("prepareBeforeCut: PLC seguro + Holder/Encoder ON + HOME")
        self._host.cmd_plc_all_safe()
        self._arm_holder_encoder()
        self._host.clear_motion_wait_flags()
        if not self._host.cmd_motion_move_zero(self._lot_rpm):
            self._raise_fault("home_cmd")
            return False
        if not self._wait_motion():
            return False
        return not self._should_abort()
    def _run_feed(self) -> bool:
        self._host.clear_motion_wait_flags()
        ok_l = self._host.cmd_motion_feed_l()
        ok_r = self._host.cmd_motion_feed_r()
        if not (ok_l or ok_r):
            self._raise_fault("feed_cmd")
            return False
        return self._wait_feed()
    def _deposit_extra_mm(self, rep: int) -> float:
        cfg = self.get_config()
        batch = max(1, cfg.deposit_batch_size)
        batch_index = (rep - 1) // batch + 1
        # Misma idea que monolito: extra por lote (signo + hacia depósito)
        if batch_index % 2 == 1:
            return float(cfg.deposit_extra_mm)
        return -float(cfg.deposit_extra_mm)
    def _run_lot(self, length_mm: float, qty: int, rpm: float) -> None:
        with self._lock:
            self._active = True
            self._pieces_done = 0
            self._total_reps = qty
            self._lot_rpm = float(rpm)
            self._started_at = time.monotonic()
            self._finished_elapsed = 0.0
        self._host.cycle_notify()
        try:
            if not self._prepare_before_cut():
                self._finish(False)
                return
            # Armar PreFeeder (In process ≈ Start PF)
            if self._host.pf_connected():
                self._host.cmd_pf_start()
                # Espera blanda Busy/Idle — sin holgura fina aún
                t0 = time.monotonic()
                while time.monotonic() - t0 < self.get_config().pf_ready_timeout_s:
                    if self._should_abort():
                        self._finish(False)
                        return
                    st = self._host.pf_state_byte()
                    if st in (TX_PF_BUSY, None) or st != TX_PF_ERROR:
                        break
                    time.sleep(0.1)
            target_mm = self._effective_mm(length_mm)
            completed = 0
            handoff_ready = False
            prefetch_running = False
            for rep in range(1, qty + 1):
                if self._gate("listo" if rep == 1 else "rep-start"):
                    break
                self._set_progress(rep, 0, qty)
                # 1–2 Holder+Encoder ON + delay (solo 1ª; se mantienen el lote)
                if rep == 1:
                    if self._enter(rep, qty, "holder_on"):
                        break
                    self._arm_holder_encoder()
                    if self._after_step("holder_on"):
                        break
                    if self._do_wait(rep, qty, "wait_holder_on", "holder_on_ms"):
                        break
                # 3 Feed / handoff
                if self._enter(rep, qty, "feed"):
                    break
                if handoff_ready:
                    handoff_ready = False
                    self._host.cycle_log("Feed: handoff (prefetch ya listo)")
                else:
                    if not self._run_feed():
                        break
                if self._after_step("feed"):
                    break
                # 4 Offset (placeholder)
                if self._enter(rep, qty, "offset"):
                    break
                if self._after_step("offset"):
                    break
                # 5–7 Pinzas + delay + enc set0
                if self._enter(rep, qty, "grippers_on"):
                    break
                self._host.cmd_plc_gripper(True)
                if self._after_step("grippers_on"):
                    break
                if self._do_wait(rep, qty, "wait_grippers_on", "grippers_on_ms"):
                    break
                if self._enter(rep, qty, "enc_set0"):
                    break
                if not self._trial_mode:
                    self._host.cmd_motion_enc_set0_r()
                    self._host.cmd_motion_enc_set0_l()
                else:
                    self._host.cycle_log("Trial: enc_set0 omitido")
                if self._after_step("enc_set0"):
                    break
                # 8–11 Holder se mantiene ON (no liberar mid-pieza) + lineal + delay
                if self._enter(rep, qty, "holder_off"):
                    break
                self._arm_holder_encoder()
                self._host.cycle_log("Holder/Encoder: se mantienen ON (sin abrir mid-pieza)")
                if self._after_step("holder_off"):
                    break
                if self._do_wait(rep, qty, "wait_holder_open", "holder_open_ms"):
                    break
                if self._enter(rep, qty, "lineal_fwd"):
                    break
                self._host.clear_motion_wait_flags()
                if not self._host.cmd_motion_move_mm(target_mm, rpm):
                    self._raise_fault("move_cmd")
                    break
                if not self._wait_motion():
                    break
                if self._after_step("lineal_fwd"):
                    break
                if self._do_wait(rep, qty, "wait_linear_done", "linear_done_ms"):
                    break
                # 12–13 Confirmar Holder+Encoder ON pre-corte + delay
                if self._enter(rep, qty, "holder_precut"):
                    break
                self._arm_holder_encoder()
                if self._after_step("holder_precut"):
                    break
                if self._do_wait(rep, qty, "wait_holder_precut", "holder_on_ms"):
                    break
                # 14–17 Corte + delays
                if self._enter(rep, qty, "cutter_on"):
                    break
                if not self._trial_mode and self._host.pf_connected():
                    st = self._host.pf_state_byte()
                    if st == TX_PF_ERROR:
                        self._raise_fault("prefeeder_all_ok")
                        self._host.cycle_log("Corte abortado: PreFeeder Error")
                        break
                self._host.cmd_plc_cutters(True)
                if self._after_step("cutter_on"):
                    break
                if self._do_wait(rep, qty, "wait_cutter_pulse", "cutter_pulse_ms"):
                    break
                if self._enter(rep, qty, "cutter_off"):
                    break
                self._host.cmd_plc_cutters(False)
                if self._after_step("cutter_off"):
                    break
                if self._do_wait(rep, qty, "wait_cutter_post", "cutter_post_ms"):
                    break
                # 18 Prefetch — inicia paralelo A
                prefetch_running = False
                if self._enter(rep, qty, "prefetch_start"):
                    break
                if rep < qty:
                    self._host.clear_motion_wait_flags()
                    self._host.cmd_motion_feed_l()
                    self._host.cmd_motion_feed_r()
                    prefetch_running = True
                    self._host.cycle_log("∥ Prefetch feed en paralelo con depósito/HOME")
                if self._after_step("prefetch_start"):
                    break
                # 19–20 Depósito + delay (∥ A)
                if self._enter(rep, qty, "deposit"):
                    break
                extra = self._deposit_extra_mm(rep)
                if abs(extra) > 0.01:
                    deposit_target = target_mm + extra
                    if prefetch_running:
                        self._host.clear_motion_reached_flag()
                    else:
                        self._host.clear_motion_wait_flags()
                    if not self._host.cmd_motion_move_mm(deposit_target, rpm):
                        self._raise_fault("deposit_cmd")
                        break
                    if not self._wait_motion():
                        break
                if self._after_step("deposit"):
                    break
                if self._do_wait(rep, qty, "wait_deposit_dwell", "dwell_at_dest_ms"):
                    break
                # 21–23 Pinzas OFF + Tfeed + delay (∥ A)
                if self._enter(rep, qty, "grippers_off"):
                    break
                self._host.cmd_plc_gripper(False)
                if self._after_step("grippers_off"):
                    break
                if self._enter(rep, qty, "pf_trigger"):
                    break
                ok_r = self._host.cmd_pf_trigger_r()
                ok_l = self._host.cmd_pf_trigger_l()
                if not ok_r and not ok_l:
                    self._raise_fault("pf_trigger")
                    self._host.cycle_log("trigger PreFeeder falló (R+L)")
                    break
                self._host.cycle_log(
                    f"trigger PreFeeder Tfeed — R(0x4C)={'ok' if ok_r else 'fail'} "
                    f"L(0x51)={'ok' if ok_l else 'fail'}"
                )
                if self._after_step("pf_trigger"):
                    break
                if self._do_wait(rep, qty, "wait_gripper_release", "gripper_release_ms"):
                    break
                # 24 HOME (∥ A) — conservar flags de feed del prefetch
                if self._enter(rep, qty, "home"):
                    break
                if prefetch_running:
                    self._host.clear_motion_reached_flag()
                else:
                    self._host.clear_motion_wait_flags()
                if not self._host.cmd_motion_move_zero(self._lot_rpm):
                    self._raise_fault("home_cmd")
                    break
                if not self._wait_motion():
                    break
                if self._after_step("home"):
                    break
                # 25 Handoff / join paralelo A
                if self._enter(rep, qty, "handoff"):
                    break
                if prefetch_running:
                    if self._wait_feed():
                        handoff_ready = True
                        self._host.cycle_log("∥ Join: prefetch listo (handoff)")
                    else:
                        handoff_ready = False
                        if not self._fault:
                            self._raise_fault("feed_incomplete")
                        break
                    prefetch_running = False
                if self._after_step("handoff"):
                    break
                # 26–27 Asentar + post-pieza
                if not handoff_ready:
                    if self._do_wait(rep, qty, "wait_asentar", "asentar_ms"):
                        break
                else:
                    if self._enter(rep, qty, "wait_asentar"):
                        break
                    self._host.cycle_log("Asentar omitido (handoff listo)")
                    if self._after_step("wait_asentar"):
                        break
                if self._enter(rep, qty, "post_piece"):
                    break
                if self._after_step("post_piece"):
                    break
                completed = rep
                with self._lock:
                    self._pieces_done = rep
                    if qty > 0:
                        self._progress = max(
                            self._progress,
                            int(min(100, round(100.0 * rep / qty))),
                        )
                self._host.cycle_log(f"Pieza {rep}/{qty} OK")
                self._host.cycle_notify()
            self._finish(completed >= qty and not self._aborted and not self._fault)
        except Exception as exc:
            self._raise_fault(f"exception:{exc}")
            self._host.cycle_log(f"Cycle exception: {exc}")
            self._finish(False)
        finally:
            with self._lock:
                still_active = self._active
            if still_active:
                self._host.cycle_log(
                    "Cycle: hilo terminó sin cierre limpio — revisar logs"
                )
                self._finish(False)
    def _finish(self, ok: bool) -> None:
        self._pause.clear()
        self._host.cmd_plc_all_safe()
        if self._host.pf_connected():
            self._host.cmd_pf_stop()
        elapsed = 0.0
        with self._lock:
            if self._started_at:
                elapsed = time.monotonic() - self._started_at
                self._finished_elapsed = elapsed
                self._started_at = None
            self._active = False
            self._last_ok = ok
            if ok:
                self._rep = self._total_reps
                self._pieces_done = self._total_reps
                self._progress = 100
                self._step = 0
                self._parallel_group = ""
            elif self._pieces_done > 0:
                total = max(1, self._total_reps)
                self._progress = max(
                    self._progress,
                    int(min(100, round(100.0 * self._pieces_done / total))),
                )
        if self._aborted or self._stop.is_set():
            self._set_state(TX_STOP, self._fault or "aborted")
        elif not ok:
            self._set_state(TX_ERROR, self._fault or "cycle_failed")
        else:
            self._set_state(TX_FINISH)
            self._host.cycle_log(
                f"Lote completo — {self._total_reps}/{self._total_reps} piezas"
                + (f" en {elapsed:.0f}s" if elapsed > 0 else "")
                + " · FinishParts (0x047)"
            )
        self._host.cycle_notify()
