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
from prefeeder import TX_PF_ERROR
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

# Motion Stage2: CYCLE envía targetAbsMm = abs(model.mm) (carrera ABS).
# El test HTML de Motion sigue usando pieceMm = L (target = L−55).
# No modificar Feed / FEED_TARGET_FIXED_MM / Move ABS manual.

# WIP Delivery (HOME): 1 soplo a mitad del viaje (ABS) hacia 0.
# start = magnitud ABS del depósito confirmado (o Stage2 si no hubo extra).
# Ej. start=255 → mid≈127.5; start=315 (255+60) → mid≈157.5.
# ON(blowerSec) → dwell → OFF explícito → HOME (sin carrera timer HMI/PLC).
WIP_BLOWER_MIN_TRAVEL_MM = 8.0
# Si hay caché ASDA post-Reached, debe coincidir con la ref de depósito.
WIP_START_CACHE_TOL_MM = 5.0

# Protocolo máquina: machine_states.py (0x40–0x49). Andon solo refleja esos bytes.
# Pasos atómicos (acción / delay independientes).
# kind=parallel SOLO en: arranque prefetch (background) y join/handoff.
# El resto es secuencia principal (action|wait) — no implica “todo a la vez”.
#
# sbsPause: en modo Step by Step, pausa tras completar ese paso (checkpoint
# físico). False = auto (delay / validación / join interno): visible en la
# lista, pero no exige Next. Un Next avanza el grupo físico + sus internos.
FLOW_STEPS: list[dict[str, Any]] = [
    {"id": 1, "key": "holder_on", "label": "Holder+Encoder ON (solo 1ª pieza)", "kind": "action", "sbsPause": False},
    {"id": 2, "key": "wait_holder_on", "label": "Delay Holder ON", "kind": "wait", "delayKey": "holderOnMs", "sbsPause": True},
    {"id": 3, "key": "feed", "label": "Alimentación (feed / handoff)", "kind": "action", "sbsPause": True},
    {"id": 4, "key": "offset", "label": "Offset alimentación (Motion, paso lógico)", "kind": "action", "sbsPause": False},
    {"id": 5, "key": "grippers_on", "label": "Pinzas cierran", "kind": "action", "sbsPause": False},
    {"id": 6, "key": "wait_grippers_on", "label": "Delay tras cerrar pinzas", "kind": "wait", "delayKey": "grippersOnMs", "sbsPause": False},
    {"id": 7, "key": "enc_set0", "label": "OM ref (no usado por Stage2 ASDA)", "kind": "action", "sbsPause": True},
    {"id": 8, "key": "holder_off", "label": "Holder+Encoder OFF (abre para lineal)", "kind": "action", "sbsPause": False},
    {"id": 9, "key": "wait_holder_open", "label": "Delay Holder/Encoder OFF", "kind": "wait", "delayKey": "holderOpenMs", "sbsPause": True},
    {"id": 10, "key": "lineal_fwd", "label": "Stage2 lineal ASDA (0→ABS)", "kind": "action", "sbsPause": False},
    {"id": 11, "key": "wait_linear_done", "label": "Delay antes del corte", "kind": "wait", "delayKey": "linearDoneMs", "sbsPause": True},
    {"id": 12, "key": "holder_precut", "label": "Holder ON / Encoder ON (pre-corte)", "kind": "action", "sbsPause": False},
    {"id": 13, "key": "wait_holder_precut", "label": "Delay tras cerrar holder", "kind": "wait", "delayKey": "holderOnMs", "sbsPause": True},
    {"id": 14, "key": "cutter_on", "label": "Cortador ON (+ All OK PreFeeder)", "kind": "action", "sbsPause": False},
    {"id": 15, "key": "wait_cutter_pulse", "label": "Delay entre Set y Res cortador", "kind": "wait", "delayKey": "cutterPulseMs", "sbsPause": False},
    {"id": 16, "key": "cutter_off", "label": "Cortador OFF", "kind": "action", "sbsPause": False},
    {"id": 17, "key": "wait_cutter_post", "label": "Delay post-corte", "kind": "wait", "delayKey": "cutterPostMs", "sbsPause": True},
    # Depósito ANTES del prefetch: la manguera debe salir del área antes de pre-alimentar.
    {"id": 18, "key": "deposit", "label": "Extra / depósito lineal", "kind": "action", "sbsPause": False},
    {"id": 19, "key": "wait_deposit_dwell", "label": "Delay tras depósito", "kind": "wait", "delayKey": "dwellAtDestMs", "sbsPause": True},
    {
        "id": 20,
        "key": "prefetch_start",
        "label": "Prefetch feed — arranca en background",
        "kind": "parallel",
        "parallelRole": "start",
        "sbsPause": False,
    },
    {"id": 21, "key": "grippers_off", "label": "Pinzas abren", "kind": "action", "sbsPause": False},
    {"id": 22, "key": "pf_trigger", "label": "Trigger PreFeeder (Tfeed)", "kind": "action", "sbsPause": False},
    {"id": 23, "key": "wait_gripper_release", "label": "Delay antes de HOME", "kind": "wait", "delayKey": "gripperReleaseMs", "sbsPause": True},
    {"id": 24, "key": "home", "label": "HOME: mid + WIP blower + 0", "kind": "action", "sbsPause": True},
    {
        "id": 25,
        "key": "handoff",
        "label": "Join — espera fin del prefetch (handoff)",
        "kind": "parallel",
        "parallelRole": "join",
        "sbsPause": False,
    },
    {"id": 26, "key": "wait_asentar", "label": "Delay asentar", "kind": "wait", "delayKey": "asentarMs", "sbsPause": False},
    {"id": 27, "key": "post_piece", "label": "Post-pieza (safety / peer / holgura)", "kind": "action", "sbsPause": False},
]
PROGRESS_STEPS = len(FLOW_STEPS)
STEP_NAMES = {0: "idle", **{s["id"]: s["key"] for s in FLOW_STEPS}}
STEP_BY_KEY = {s["key"]: s for s in FLOW_STEPS}
# Keys de control de lote (no están en FLOW_STEPS): nunca pausan en SBS.
STEP_BY_STEP_NO_PAUSE = frozenset({"listo", "rep-start"})
@dataclass

class CycleConfig:
    holder_on_ms: int = 120
    holder_open_ms: int = 60
    grippers_on_ms: int = 60
    gripper_release_ms: int = 200
    # KEEP Set/Res: CMD ON = pulso Set, CMD OFF = pulso Res (mismo GPIO).
    # El PLC bloquea VALVE_PULSE_MS (100) en cada pulso; el TCP HMI es
    # fire-and-forget. Si cutter_pulse_ms < VALVE_PULSE_MS, el Res llega
    # encolado y se dispara al terminar el Set sin hueco LOW → el KEEP
    # no distingue dos impulsos y el cortador queda abajo.
    # Este delay debe ser ≥ VALVE_PULSE_MS (+ margen de asiento).
    cutter_pulse_ms: int = 200
    cutter_post_ms: int = 100
    linear_done_ms: int = 60
    asentar_ms: int = 30
    dwell_at_dest_ms: int = 100
    deposit_batch_size: int = 50
    deposit_extra_mm: float = 30.0
    cut_offset_mm: float = 0.0
    motion_wait_timeout_s: float = 120.0
    feed_wait_timeout_s: float = 30.0
    pf_ready_timeout_s: float = 10.0
    # Feed / Stage2 OM: "L" | "R" | "LR"
    feed_sides: str = "L"
    # Refill / purga: feed n mm (Motion FEED fijo = 55) + park ASDA
    refill_mm: float = 55.0
    refill_asda_mm: float = -300.0

    @staticmethod
    def normalize_feed_sides(raw: Any) -> str:
        if isinstance(raw, (list, tuple, set)):
            parts = {str(x).strip().upper() for x in raw}
            has_l = "L" in parts
            has_r = "R" in parts
        else:
            s = str(raw or "LR").strip().upper().replace(" ", "").replace(",", "")
            if s in ("L", "R", "LR", "RL", "BOTH", "ALL"):
                if s in ("RL", "BOTH", "ALL"):
                    return "LR"
                return s if s != "LR" else "LR"
            has_l = "L" in s
            has_r = "R" in s
        if has_l and has_r:
            return "LR"
        if has_l:
            return "L"
        if has_r:
            return "R"
        return "LR"

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
            "feedSides": "feed_sides",
            "refillMm": "refill_mm",
            "refillAsdaMm": "refill_asda_mm",
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
            "refill_mm",
            "refill_asda_mm",
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
                if attr == "feed_sides":
                    kw[attr] = cls.normalize_feed_sides(raw)
                elif attr in int_attrs:
                    kw[attr] = int(float(raw))
                elif attr in float_attrs:
                    kw[attr] = float(raw)
                else:
                    kw[attr] = raw
            except (TypeError, ValueError):
                continue
        # Pulso lógico Set→Res ≥ pulso eléctrico KEEP del PLC (100 ms).
        if "cutter_pulse_ms" in kw and kw["cutter_pulse_ms"] < 150:
            kw["cutter_pulse_ms"] = 150
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
            raw = json.loads(path.read_text(encoding="utf-8"))
            cfg = CycleConfig.from_dict(raw if isinstance(raw, dict) else {})
            # Migrar claves nuevas si el JSON viejo no las traía
            if isinstance(raw, dict):
                missing = (
                    ("feedSides" not in raw and "feed_sides" not in raw)
                    or ("refillMm" not in raw and "refill_mm" not in raw)
                    or ("refillAsdaMm" not in raw and "refill_asda_mm" not in raw)
                )
                if missing:
                    save_cycle_config(cfg, path)
            return cfg
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
    def wait_feed_length_ok_for(
        self,
        sides: list[str],
        timeout_s: float,
        *,
        abort_event: threading.Event | None = None,
    ) -> dict[str, str]: ...
    def feed_fault_for(self, side: str) -> str: ...
    def arm_stage2(self) -> None: ...
    def cmd_motion_stage2_start(
        self,
        piece_mm: float | None = None,
        sides: str = "LR",
        *,
        target_abs_mm: float | None = None,
    ) -> bool: ...
    def wait_stage2_result(
        self,
        timeout_s: float,
        *,
        abort_event: threading.Event | None = None,
    ) -> str: ...
    def stage2_fault(self) -> str: ...
    def asda_position_mm(self) -> float | None: ...
    def cmd_motion_move_mm(self, mm: float, rpm: float) -> bool: ...
    def last_move_fail_kind(self) -> str: ...
    def cmd_motion_move_zero(self, rpm: float) -> bool: ...
    def cmd_motion_stop(self) -> bool: ...
    def cmd_motion_feed_l(self, *, skip_validate: bool = False) -> bool: ...
    def cmd_motion_feed_r(self, *, skip_validate: bool = False) -> bool: ...
    def cmd_motion_enc_set0_r(self) -> bool: ...
    def cmd_motion_enc_set0_l(self) -> bool: ...
    def plc_valve_is_on(self, byte_code: int) -> bool | None: ...
    def plc_blower_sec(self) -> float: ...
    def cmd_plc_holder(self, on: bool) -> bool: ...
    def cmd_plc_encoder(self, on: bool) -> bool: ...
    def cmd_plc_gripper(self, on: bool) -> bool: ...
    def cmd_plc_blower(self, on: bool = True, duration_sec: float | None = None) -> bool: ...
    def cmd_plc_cutters(self, on: bool, *, sides: str | None = None) -> bool: ...
    def cmd_plc_tools_safe(self) -> bool: ...
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
        self._ignore_prefeeder = False
        self._refill_mode = False
        self._refill_awaiting_confirm = False
        self._refill_confirm = threading.Event()
        self._refill_reject = threading.Event()
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
                "ignorePrefeeder": self._ignore_prefeeder,
                "refillActive": self._refill_mode and self._active,
                "refillAwaitingConfirm": self._refill_awaiting_confirm,
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
        sides = CycleConfig.normalize_feed_sides(cfg.feed_sides)
        return (
            f"Delays — holderOn={cfg.holder_on_ms}ms open={cfg.holder_open_ms}ms "
            f"grippers={cfg.grippers_on_ms}ms cutter={cfg.cutter_pulse_ms}ms "
            f"postCut={cfg.cutter_post_ms}ms linear={cfg.linear_done_ms}ms "
            f"dwell={cfg.dwell_at_dest_ms}ms gripRel={cfg.gripper_release_ms}ms "
            f"asentar={cfg.asentar_ms}ms · feedSides={sides}"
            f" · refill={cfg.refill_mm:g}mm asda={cfg.refill_asda_mm:g}mm"
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
        if not self._ignore_prefeeder and self._host.pf_is_materialist():
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
        with self._lock:
            self._refill_mode = False
            self._refill_awaiting_confirm = False
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
        self._refill_reject.set()
        with self._lock:
            self._refill_awaiting_confirm = False
        # Desarma waits Stage2/Feed/Reached — si no, el hilo queda active ~3 min
        # y Reset responde "Detener ciclo antes de Reset".
        self._host.clear_motion_wait_flags()
        self._host.cmd_motion_stop()
        if not self._ignore_prefeeder:
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
        if self._refill_awaiting_confirm:
            return {
                "ok": False,
                "error": "Refill espera confirmación del operador (Sí/No)",
            }
        if self._pause.is_set():
            self._pause.clear()
            self._leave_pause_andon()
            self._host.cycle_log("Cycle Resume")
            self._host.cycle_notify()
            return {"ok": True}
        return {"ok": False, "error": "Ciclo no está en Pause"}

    def request_refill(
        self,
        rpm: float,
        *,
        feed_mm: float | None = None,
        asda_mm: float | None = None,
    ) -> dict[str, Any]:
        """Purga/refill: ASDA park → holder+encoder → feed n mm → corte → confirm → HOME.

        Feed físico sin validación láser ni OM (skipValidate en Motion).
        Motion FEED físico = 55 mm fijo (FEED_TARGET_FIXED_MM).
        """
        with self._lock:
            if self._materialist:
                return {"ok": False, "error": "Máquina en Materialist (0x049)"}
            if self._active:
                return {"ok": False, "error": "Ciclo ocupado (0x045)"}
            if self._thread and self._thread.is_alive():
                self._thread.join(timeout=0.2)
                if self._thread.is_alive():
                    return {"ok": False, "error": "Ciclo ocupado (0x045)"}
        if not self._host.motion_connected() or not self._host.plc_connected():
            return {"ok": False, "error": "Motion/PLC sin enlace"}
        cfg = self.reload_config()
        use_feed = float(feed_mm) if feed_mm is not None else float(cfg.refill_mm)
        use_asda = float(asda_mm) if asda_mm is not None else float(cfg.refill_asda_mm)
        if use_feed < 1.0:
            return {"ok": False, "error": "refillMm inválido"}
        self._stop.clear()
        self._pause.clear()
        self._aborted = False
        self._fault = ""
        self._fault_class = ""
        self._c3_stop_after_step = False
        self._recovery = ""
        self._last_ok = False
        self._refill_confirm.clear()
        self._refill_reject.clear()
        with self._lock:
            self._refill_awaiting_confirm = False
            self._refill_mode = True
        self._set_state(TX_BUSY)
        self._host.cycle_log(
            f"Refill Start — ASDA→{use_asda:g} mm · feed≈{use_feed:g} mm · "
            f"lados={CycleConfig.normalize_feed_sides(cfg.feed_sides)}"
        )
        args = (float(rpm), use_feed, use_asda)
        self._thread = threading.Thread(
            target=self._run_refill, args=args, daemon=True, name="CycleRefill"
        )
        self._thread.start()
        return {"ok": True}

    def confirm_refill(self, ok: bool = True) -> dict[str, Any]:
        """Operador confirma (Sí → ASDA a 0) o cancela tras el corte del refill."""
        if not self._refill_awaiting_confirm:
            return {"ok": False, "error": "Sin refill pendiente de confirmación"}
        if ok:
            self._refill_confirm.set()
            self._host.cycle_log("Refill: operador confirmó OK → ASDA a 0")
        else:
            self._refill_reject.set()
            self._host.cycle_log("Refill: operador canceló (ASDA permanece en park)")
        self._host.cycle_notify()
        return {"ok": True}
    def request_reset(self) -> dict[str, Any]:
        if self.is_active():
            # Stop ya pedido: dar tiempo a que salga del wait Stage2/Feed.
            if self._stop.is_set() or self._aborted:
                self._host.clear_motion_wait_flags()
                th = self._thread
                if th is not None and th.is_alive():
                    th.join(timeout=2.0)
            if self.is_active():
                return {"ok": False, "error": "Detener ciclo antes de Reset"}
        self._aborted = False
        self._stop.clear()
        self._fault = ""
        self._fault_class = ""
        self._c3_stop_after_step = False
        self._recovery = ""
        self._last_ok = False
        self._materialist = False
        self._refill_mode = False
        self._refill_awaiting_confirm = False
        self._refill_confirm.clear()
        self._refill_reject.clear()
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

    def set_ignore_prefeeder(self, on: bool) -> dict[str, Any]:
        if on and self.is_active():
            return {"ok": False, "error": "No cambiar ignore PreFeeder con ciclo activo"}
        self._ignore_prefeeder = bool(on)
        self._host.cycle_log(
            f"Ignore PreFeeder {'ON' if on else 'OFF'} (bypass 100% PF en ciclo)"
        )
        self._host.cycle_notify()
        return {"ok": True, "ignorePrefeeder": self._ignore_prefeeder}

    def _use_prefeeder(self) -> bool:
        """PreFeeder participa en el ciclo (enlace + no bypass debug)."""
        return (not self._ignore_prefeeder) and self._host.pf_connected()
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
            try:
                self._host.clear_motion_wait_flags()
            except Exception:
                pass
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
            try:
                self._host.clear_motion_wait_flags()
            except Exception:
                pass
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
        # Paso a paso pausa en _after_step (tras ejecutar), no aquí:
        # pausar antes+después obligaba a pulsar Siguiente dos veces por paso
        # (y en pasos vacíos la 2ª pulsación no hacía nada visible).
        return self._gate(key)

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
        """True solo en checkpoints físicos (sbsPause). Delays/validaciones auto."""
        if not self._step_by_step:
            return False
        if step_key in STEP_BY_STEP_NO_PAUSE:
            return False
        meta = STEP_BY_KEY.get(step_key)
        if meta is None:
            return False
        return bool(meta.get("sbsPause", False))
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

    def _wait_duration_s(self, sec: float) -> bool:
        """Espera cooperativa (respeta pause/abort). False = abort."""
        deadline = time.monotonic() + max(0.0, float(sec))
        while time.monotonic() < deadline:
            if self._should_abort():
                return False
            while self._pause.is_set():
                if self._should_abort():
                    return False
                time.sleep(0.05)
            remain = deadline - time.monotonic()
            if remain <= 0:
                break
            time.sleep(min(0.05, remain))
        return True

    def _arm_motion_leg(self, prefetch_running: bool) -> None:
        """Limpia flags Idle/Reached; conserva LengthOK/NG si hay prefetch ∥."""
        if prefetch_running:
            self._host.clear_motion_reached_flag()
        else:
            self._host.clear_motion_wait_flags()

    def _validate_wip_start_mm(self, start_mm: float) -> bool:
        """True si OK. Caché ASDA opcional: si existe, debe cuadrar con la ref."""
        cached = self._host.asda_position_mm()
        if cached is None:
            return True
        err = abs(abs(float(cached)) - abs(float(start_mm)))
        if err <= WIP_START_CACHE_TOL_MM:
            return True
        self._raise_fault("wip_start")
        self._host.cycle_log(
            f"WIP start inválido: ref={abs(float(start_mm)):.1f} mm "
            f"caché ASDA={abs(float(cached)):.1f} mm "
            f"(Δ={err:.1f} > {WIP_START_CACHE_TOL_MM:.0f} mm) — no se estima"
        )
        return False

    def _home_with_wip_delivery(
        self,
        start_mm: float | None,
        rpm: float,
        *,
        prefetch_running: bool,
    ) -> bool:
        """HOME + WIP: mid → blower ON → dwell → blower OFF → HOME 0.

        `start_mm` = magnitud ABS del depósito confirmado (post Idle/Reached)
        o del target Stage2 si no hubo move de depósito. No es estimación.
        """
        if start_mm is None:
            self._raise_fault("wip_start")
            self._host.cycle_log(
                "WIP start ausente — falta ref de depósito/Stage2 (no se estima)"
            )
            return False

        start_abs = abs(float(start_mm))
        if not self._validate_wip_start_mm(start_abs):
            return False

        travel = start_abs
        blower_sec = float(self._host.plc_blower_sec())

        if travel < WIP_BLOWER_MIN_TRAVEL_MM:
            self._host.cycle_log(
                f"WIP Delivery: omitido (travel={travel:.1f} mm) — HOME directo"
            )
            self._arm_motion_leg(prefetch_running)
            if not self._host.cmd_motion_move_zero(rpm):
                self._raise_fault("home_cmd")
                return False
            return self._wait_motion()

        mid_mm = start_abs * 0.5
        self._host.cycle_log(
            f"WIP Delivery: start={start_abs:.1f} mm → mid={mid_mm:.1f} mm "
            f"→ blower {blower_sec:g}s → OFF → HOME"
        )

        # 1) Mitad del viaje hacia 0
        self._arm_motion_leg(prefetch_running)
        if not self._host.cmd_motion_move_mm(mid_mm, rpm):
            if not self._should_abort() and not self._fault:
                kind = ""
                if hasattr(self._host, "last_move_fail_kind"):
                    kind = self._host.last_move_fail_kind()
                self._raise_fault("E065" if kind == "transport" else "home_cmd")
            return False
        if not self._wait_motion():
            return False

        # 2) Soplo: ON debe OK; dwell; OFF explícito antes de mover (anti-carrera)
        if self._should_abort():
            return False
        if not self._host.cmd_plc_blower(True, duration_sec=blower_sec):
            self._raise_fault("wip_blower")
            self._host.cycle_log(
                f"WIP Delivery blower ON FAIL @ mid={mid_mm:.1f} mm "
                f"— no dwell / no HOME"
            )
            return False
        self._host.cycle_log(
            f"WIP Delivery blower ON @ mid={mid_mm:.1f} mm hold={blower_sec:g}s"
        )
        if not self._wait_duration_s(blower_sec):
            # Abort a mitad: apagar blower best-effort; no continuar a HOME
            self._host.cmd_plc_blower(False)
            return False
        if not self._host.cmd_plc_blower(False):
            self._raise_fault("wip_blower")
            self._host.cycle_log(
                "WIP Delivery blower OFF FAIL — no HOME (blower puede seguir activo)"
            )
            return False
        self._host.cycle_log("WIP Delivery blower OFF ok — HOME")

        # 3) Completar HOME (solo con blower OFF confirmado a nivel de comando)
        self._arm_motion_leg(prefetch_running)
        if not self._host.cmd_motion_move_zero(rpm):
            self._raise_fault("home_cmd")
            return False
        return self._wait_motion()

    def _wait_feed(self) -> bool:
        if self._trial_mode:
            self._host.cycle_log("Trial: feed OK omitido (bypass encoder)")
            return True
        cfg = self.get_config()
        sides = self._feed_side_list()
        outcomes = self._host.wait_feed_length_ok_for(
            sides, cfg.feed_wait_timeout_s, abort_event=self._stop
        )
        bad = [s for s, v in outcomes.items() if v != "ok"]
        if not bad:
            return True
        for s in bad:
            detail = self._host.feed_fault_for(s) or outcomes.get(s, "fail")
            self._host.cycle_log(f"Feed {s}: {detail}")
        self._raise_fault("timeout_feed")
        self._host.cycle_log(format_ui("E009"))
        return False

    def _feed_side_list(self) -> list[str]:
        mode = CycleConfig.normalize_feed_sides(self.get_config().feed_sides)
        if mode == "L":
            return ["L"]
        if mode == "R":
            return ["R"]
        return ["L", "R"]

    def _wait_stage2(self) -> bool:
        """Espera Stage2 Complete (OK/NG). No usa ASDA Reached intermedios.

        Timeout HMI ≥ timeout global Stage2 (ASDA ABS único).
        """
        cfg = self.get_config()
        # STAGE2_GLOBAL_TIMEOUT_MS = 180000 → margen de poll HTTP
        timeout_s = max(float(cfg.motion_wait_timeout_s), 185.0)
        outcome = self._host.wait_stage2_result(timeout_s, abort_event=self._stop)
        if outcome == "ok":
            return True
        if outcome == "ng":
            detail = self._host.stage2_fault() or "Stage2 NG"
            self._raise_fault("move_cmd")
            self._host.cycle_log(f"Stage2 NG — {detail}")
            return False
        if outcome == "aborted":
            # Stop/C1 del operador: no latchear EXXX genérico (bloquea Reset).
            if self._should_abort():
                self._host.cycle_log("Stage2 abortado (Stop)")
                return False
            self._raise_fault("move_cmd")
            self._host.cycle_log("Stage2 abortado")
            return False
        self._raise_fault("timeout_motion")
        self._host.cycle_log(format_ui("E008"))
        self._host.cycle_log("Stage2 timeout — sin resultado final")
        return False

    def _effective_mm(self, length_mm: float) -> float:
        cfg = self.get_config()
        return abs(float(length_mm)) + float(cfg.cut_offset_mm)

    def _arm_holder_encoder(self) -> None:
        """Cierra Holder+Encoder (ON). Idempotente: no re-pulsa si ya están ON."""
        self._host.cmd_plc_holder(True)
        self._host.cmd_plc_encoder(True)

    def _open_holder_encoder(self) -> None:
        """Abre Holder+Encoder (OFF) para el avance lineal."""
        self._host.cmd_plc_holder(False)
        self._host.cmd_plc_encoder(False)

    def _ensure_asda_at_zero(self) -> bool:
        """Al Start: si ASDA no está en 0, ir a 0 y confirmar Reached antes del lote.

        Evita arrancar Stage2 desde posición residual. Paso 24 sigue haciendo
        HOME entre piezas; esto cubre el arranque / abort previo.
        """
        pos = self._host.asda_position_mm()
        if pos is not None and abs(float(pos)) <= 0.5:
            self._host.cycle_log(
                f"ASDA ya en 0 (pos={float(pos):.2f} mm) — sin MOVE_ZERO al Start"
            )
            return True
        if pos is not None:
            self._host.cycle_log(
                f"ASDA pos={float(pos):.2f} mm ≠ 0 — MOVE_ZERO al Start"
            )
        else:
            self._host.cycle_log(
                "ASDA pos desconocida — MOVE_ZERO al Start (referencia)"
            )
        self._host.clear_motion_wait_flags()
        if not self._host.cmd_motion_move_zero(self._lot_rpm):
            self._raise_fault("home_cmd")
            return False
        if not self._wait_motion():
            return False
        return not self._should_abort()

    def _prepare_before_cut(self) -> bool:
        # Start: tools a seguro + Holder/Encoder ON + ASDA en 0 confirmado.
        # HOME entre piezas sigue en paso 24.
        self._host.cycle_log(
            "prepareBeforeCut: cutters/grippers safe + Holder/Encoder cerrados"
        )
        self._host.cmd_plc_tools_safe()
        self._arm_holder_encoder()
        if self._should_abort():
            return False
        return self._ensure_asda_at_zero()
    def _run_feed(self, *, skip_validate: bool = False) -> bool:
        self._host.clear_motion_wait_flags()
        sides = self._feed_side_list()
        ok_any = False
        if "L" in sides:
            ok_any = (
                self._host.cmd_motion_feed_l(skip_validate=skip_validate) or ok_any
            )
        if "R" in sides:
            ok_any = (
                self._host.cmd_motion_feed_r(skip_validate=skip_validate) or ok_any
            )
        note = " (purga: sin láser/OM)" if skip_validate else ""
        self._host.cycle_log(f"Feed start lados={''.join(sides)}{note}")
        if not ok_any:
            self._raise_fault("feed_cmd")
            return False
        return self._wait_feed()

    def _wait_refill_operator_confirm(self) -> bool:
        """True = operador confirmó; False = canceló / Stop."""
        with self._lock:
            self._refill_awaiting_confirm = True
        self._refill_confirm.clear()
        self._refill_reject.clear()
        self._pause.set()
        self._enter_pause_andon()
        self._host.cycle_log(
            "Refill: corte listo — confirme que la manguera/pieza está OK"
        )
        self._host.cycle_notify()
        try:
            while True:
                if self._should_abort() or self._refill_reject.is_set():
                    return False
                if self._refill_confirm.is_set():
                    return True
                time.sleep(0.05)
        finally:
            self._pause.clear()
            self._leave_pause_andon()
            with self._lock:
                self._refill_awaiting_confirm = False
            self._host.cycle_notify()

    def _run_cutter_pulse(self) -> bool:
        """Pulso Set/Res cortador según feedSides (sin PreFeeder All OK)."""
        cfg = self.get_config()
        cut_sides = CycleConfig.normalize_feed_sides(cfg.feed_sides)
        self._host.cycle_log(f"Refill cortador ON (Set) lados={cut_sides}")
        self._host.cmd_plc_cutters(True, sides=cut_sides)
        if self._should_abort():
            self._host.cmd_plc_cutters(False, sides=cut_sides)
            return False
        time.sleep(max(0, cfg.cutter_pulse_ms) / 1000.0)
        if self._should_abort():
            self._host.cmd_plc_cutters(False, sides=cut_sides)
            return False
        self._host.cycle_log(f"Refill cortador OFF (Res) lados={cut_sides}")
        self._host.cmd_plc_cutters(False, sides=cut_sides)
        time.sleep(max(0, cfg.cutter_post_ms) / 1000.0)
        return not self._should_abort()

    def _run_refill(self, rpm: float, feed_mm: float, asda_mm: float) -> None:
        """ASDA park → holder+encoder → feed → corte → confirm operador → HOME."""
        with self._lock:
            self._active = True
            self._refill_mode = True
            self._pieces_done = 0
            self._total_reps = 1
            self._lot_rpm = float(rpm)
            self._started_at = time.monotonic()
            self._finished_elapsed = 0.0
            self._progress = 0
            self._step = 0
            self._parallel_group = ""
        self._host.cycle_notify()
        try:
            cfg = self.get_config()
            # 1) Área libre: ASDA a park (convención HMI firmada; Motion usa abs)
            self._host.cycle_log(
                f"Refill: tools safe + ASDA → {asda_mm:g} mm (área libre)"
            )
            self._host.cmd_plc_tools_safe()
            if self._should_abort():
                self._finish(False)
                return
            self._host.clear_motion_wait_flags()
            if not self._host.cmd_motion_move_mm(asda_mm, rpm):
                self._raise_fault("move_cmd")
                self._finish(False)
                return
            if not self._wait_motion() or self._should_abort():
                self._finish(False)
                return
            with self._lock:
                self._progress = 20

            # 2) Holder + Encoder ON
            self._host.cycle_log("Refill: Holder+Encoder ON")
            self._arm_holder_encoder()
            time.sleep(max(0, cfg.holder_on_ms) / 1000.0)
            if self._should_abort():
                self._finish(False)
                return
            with self._lock:
                self._progress = 35

            # 3) Alimentar n mm (purga: sin validación láser/OM)
            if abs(float(feed_mm) - 55.0) > 0.05:
                self._host.cycle_log(
                    f"Refill: config feed={feed_mm:g} mm — Motion FEED físico=55 mm"
                )
            else:
                self._host.cycle_log(
                    f"Refill: alimentar {feed_mm:g} mm (sin validación láser/OM)"
                )
            if not self._run_feed(skip_validate=True):
                self._finish(False)
                return
            with self._lock:
                self._progress = 60
            if self._should_abort():
                self._finish(False)
                return

            # 4) Cortar
            if not self._run_cutter_pulse():
                self._finish(False)
                return
            with self._lock:
                self._progress = 75

            # 5) Confirmación operador
            if not self._wait_refill_operator_confirm():
                if self._aborted or self._stop.is_set():
                    self._finish(False)
                    return
                self._host.cycle_log(
                    "Refill cancelado — ASDA permanece en park; tools safe"
                )
                self._host.cmd_plc_tools_safe()
                self._finish(False, soft_cancel=True)
                return

            # 6) Regresar ASDA a 0
            self._host.cycle_log("Refill: ASDA → 0")
            self._host.clear_motion_wait_flags()
            if not self._host.cmd_motion_move_zero(rpm):
                self._raise_fault("home_cmd")
                self._finish(False)
                return
            if not self._wait_motion() or self._should_abort():
                self._finish(False)
                return
            with self._lock:
                self._progress = 100
                self._pieces_done = 1
            self._host.cycle_log("Refill OK — ASDA en 0")
            self._finish(True)
        except Exception as exc:
            self._raise_fault(f"exception:{exc}")
            self._host.cycle_log(f"Refill exception: {exc}")
            self._finish(False)
        finally:
            with self._lock:
                still_active = self._active
            if still_active:
                self._host.cycle_log(
                    "Refill: hilo terminó sin cierre limpio — revisar logs"
                )
                self._finish(False)

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
            # Armar PreFeeder: Start + esperar salir de ErrorState.
            # Si a timeout sigue mal → fault (no continuar). Bypass: ignorePrefeeder.
            if self._use_prefeeder():
                self._host.cmd_pf_start()
                timeout_s = float(self.get_config().pf_ready_timeout_s)
                t0 = time.monotonic()
                ready = False
                while time.monotonic() - t0 < timeout_s:
                    if self._should_abort():
                        self._finish(False)
                        return
                    st = self._host.pf_state_byte()
                    if st is not None and st != TX_PF_ERROR:
                        ready = True
                        break
                    time.sleep(0.1)
                if not ready:
                    self._raise_fault("prefeeder_all_ok")
                    self._host.cycle_log(
                        f"PreFeeder: timeout {timeout_s:.0f}s esperando salir de ErrorState"
                    )
                    self._finish(False)
                    return
            elif self._ignore_prefeeder:
                self._host.cycle_log("PreFeeder: ignorado (debug bypass)")
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
                # Stage2 es ASDA-only: no RESET OM en Motion.
                self._host.cycle_log("enc_set0: omitido (Stage2 no usa OM)")
                if self._after_step("enc_set0"):
                    break
                # 8–11 Holder+Encoder abren para lineal + Stage2 + delay
                if self._enter(rep, qty, "holder_off"):
                    break
                self._open_holder_encoder()
                self._host.cycle_log(
                    "Holder+Encoder OFF (abren para lineal)"
                )
                if self._after_step("holder_off"):
                    break
                if self._do_wait(rep, qty, "wait_holder_open", "holder_open_ms"):
                    break

                if self._enter(rep, qty, "lineal_fwd"):
                    break
                # Producción: Stage2 FSM. carrera ABS = modelo + cutOffset (resultado final).
                self._host.arm_stage2()
                sides = CycleConfig.normalize_feed_sides(self.get_config().feed_sides)
                abs_target_mm = abs(float(target_mm))
                self._host.cycle_log(
                    f"Stage2 START modelMm={length_mm:.1f} cutOffset→targetAbsMm={abs_target_mm:.1f} "
                    f"sides={sides}; deposit abs={target_mm:.1f} mm"
                )
                if not self._host.cmd_motion_stage2_start(
                    sides=sides, target_abs_mm=abs_target_mm
                ):
                    detail = self._host.stage2_fault() or "stage2_cmd"
                    self._raise_fault("move_cmd")
                    self._host.cycle_log(f"Stage2 START falló — {detail}")
                    break
                if not self._wait_stage2():
                    break
                if self._after_step("lineal_fwd"):
                    break
                if self._do_wait(rep, qty, "wait_linear_done", "linear_done_ms"):
                    break
                # 12–13 Cerrar Holder+Encoder de nuevo pre-corte
                if self._enter(rep, qty, "holder_precut"):
                    break
                self._arm_holder_encoder()
                self._host.cycle_log("Holder+Encoder ON (cierran pre-corte)")
                if self._after_step("holder_precut"):
                    break
                if self._do_wait(rep, qty, "wait_holder_precut", "holder_on_ms"):
                    break
                # 14–17 Corte solo en lados de feed (evita pulsar el cortador vacío)
                cut_sides = CycleConfig.normalize_feed_sides(self.get_config().feed_sides)
                if self._enter(rep, qty, "cutter_on"):
                    break
                if not self._trial_mode and self._use_prefeeder():
                    st = self._host.pf_state_byte()
                    if st == TX_PF_ERROR:
                        self._raise_fault("prefeeder_all_ok")
                        self._host.cycle_log("Corte abortado: PreFeeder Error")
                        break
                self._host.cycle_log(f"Cortador ON (Set) lados={cut_sides}")
                self._host.cmd_plc_cutters(True, sides=cut_sides)
                if self._after_step("cutter_on"):
                    break
                if self._do_wait(rep, qty, "wait_cutter_pulse", "cutter_pulse_ms"):
                    break
                if self._enter(rep, qty, "cutter_off"):
                    break
                self._host.cycle_log(f"Cortador OFF (Res) lados={cut_sides}")
                self._host.cmd_plc_cutters(False, sides=cut_sides)
                if self._after_step("cutter_off"):
                    break
                if self._do_wait(rep, qty, "wait_cutter_post", "cutter_post_ms"):
                    break
                # 18–19 Depósito + delay (manguera fuera del área ANTES del prefetch)
                # wip_start_mm = magnitud ABS del move confirmado (fuente WIP).
                wip_start_mm: float | None = None
                prefetch_running = False
                if self._enter(rep, qty, "deposit"):
                    break
                extra = self._deposit_extra_mm(rep)
                if abs(extra) > 0.01:
                    deposit_target = target_mm + extra
                    self._host.clear_motion_wait_flags()
                    if not self._host.cmd_motion_move_mm(deposit_target, rpm):
                        # Transporte agotado → E065 (ya latcheado). Rechazo ASDA → E013.
                        if not self._should_abort() and not self._fault:
                            kind = ""
                            if hasattr(self._host, "last_move_fail_kind"):
                                kind = self._host.last_move_fail_kind()
                            if kind == "transport":
                                self._raise_fault("E065")
                            else:
                                self._raise_fault("deposit_cmd")
                        break
                    if not self._wait_motion():
                        break
                    # Misma magnitud que envió cmd_motion_move_mm (abs).
                    wip_start_mm = abs(float(deposit_target))
                    self._host.cycle_log(
                        f"WIP start ref=deposit_target {wip_start_mm:.1f} mm "
                        f"(post Idle/Reached)"
                    )
                else:
                    # Sin move de depósito: ref = target Stage2 ya confirmado.
                    wip_start_mm = abs(float(target_mm))
                    self._host.cycle_log(
                        f"WIP start ref=stage2_target {wip_start_mm:.1f} mm "
                        f"(sin depósito extra)"
                    )
                if self._after_step("deposit"):
                    break
                if self._do_wait(rep, qty, "wait_deposit_dwell", "dwell_at_dest_ms"):
                    break
                # 20 Prefetch — inicia paralelo A (tras depósito: manguera ya movida)
                if self._enter(rep, qty, "prefetch_start"):
                    break
                if rep < qty:
                    self._host.clear_motion_wait_flags()
                    sides = self._feed_side_list()
                    if "L" in sides:
                        self._host.cmd_motion_feed_l()
                    if "R" in sides:
                        self._host.cmd_motion_feed_r()
                    prefetch_running = True
                    self._host.cycle_log(
                        f"∥ Prefetch feed lados={''.join(sides)} "
                        f"en paralelo con pinzas/HOME (post-depósito)"
                    )
                if self._after_step("prefetch_start"):
                    break
                # 21–23 Pinzas OFF + Tfeed + delay (∥ A)
                if self._enter(rep, qty, "grippers_off"):
                    break
                self._host.cmd_plc_gripper(False)
                if self._after_step("grippers_off"):
                    break
                if self._enter(rep, qty, "pf_trigger"):
                    break
                if self._use_prefeeder():
                    # Misma política que corte/Stage2: solo lados de feedSides.
                    sides = CycleConfig.normalize_feed_sides(
                        self.get_config().feed_sides
                    )
                    want_r = "R" in sides
                    want_l = "L" in sides
                    ok_r = self._host.cmd_pf_trigger_r() if want_r else True
                    ok_l = self._host.cmd_pf_trigger_l() if want_l else True
                    r_txt = ("ok" if ok_r else "fail") if want_r else "omit"
                    l_txt = ("ok" if ok_l else "fail") if want_l else "omit"
                    if (want_r and not ok_r) or (want_l and not ok_l):
                        self._raise_fault("pf_trigger")
                        self._host.cycle_log(
                            f"trigger PreFeeder falló lados={sides} "
                            f"R(0x4C)={r_txt} L(0x51)={l_txt}"
                        )
                        break
                    self._host.cycle_log(
                        f"trigger PreFeeder Tfeed lados={sides} — "
                        f"R(0x4C)={r_txt} L(0x51)={l_txt}"
                    )
                else:
                    self._host.cycle_log("trigger PreFeeder: omitido (ignore/bypass)")
                if self._after_step("pf_trigger"):
                    break
                if self._do_wait(rep, qty, "wait_gripper_release", "gripper_release_ms"):
                    break
                # 24 HOME + WIP: mid → blower ON/dwell/OFF → 0
                if self._enter(rep, qty, "home"):
                    break
                if not self._home_with_wip_delivery(
                    wip_start_mm,
                    self._lot_rpm,
                    prefetch_running=prefetch_running,
                ):
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
    def _finish(self, ok: bool, *, soft_cancel: bool = False) -> None:
        self._pause.clear()
        refill = False
        with self._lock:
            refill = self._refill_mode
            self._refill_awaiting_confirm = False
        self._refill_confirm.clear()
        self._refill_reject.clear()
        # Stop/C1 ya hicieron all_safe (abre todo). En fin de piezas: cutters/
        # grippers safe y Holder+Encoder quedan cerrados (sin pulso de más).
        if not (self._aborted or self._stop.is_set()):
            self._host.cmd_plc_tools_safe()
            if not soft_cancel:
                self._arm_holder_encoder()
            self._host.cycle_log(
                "Finish: Holder/Encoder cerrados; cutters/grippers OFF"
                if not refill
                else (
                    "Refill cancelado: tools safe"
                    if soft_cancel
                    else "Refill finish: Holder/Encoder ON; cutters/grippers OFF"
                )
            )
        if self._use_prefeeder() and not refill:
            self._host.cmd_pf_stop()
        elapsed = 0.0
        with self._lock:
            if self._started_at:
                elapsed = time.monotonic() - self._started_at
                self._finished_elapsed = elapsed
                self._started_at = None
            self._active = False
            self._refill_mode = False
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
        elif soft_cancel:
            self._set_state(TX_IDLE, "Refill cancelado")
            self._host.cycle_log("Refill cancelado por operador → Idle")
        elif not ok:
            self._set_state(TX_ERROR, self._fault or "cycle_failed")
        else:
            self._set_state(TX_FINISH)
            if refill:
                self._host.cycle_log(
                    "Refill completo"
                    + (f" en {elapsed:.0f}s" if elapsed > 0 else "")
                    + " · FinishParts (0x047)"
                )
            else:
                self._host.cycle_log(
                    f"Lote completo — {self._total_reps}/{self._total_reps} piezas"
                    + (f" en {elapsed:.0f}s" if elapsed > 0 else "")
                    + " · FinishParts (0x047)"
                )
        self._host.cycle_notify()
