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

# Lineal producción: CMD_MOVE TCP abs(model.mm)+cutOffset (carrera ABS).
# Stage2 HTTP queda para pruebas locales Motion; el ciclo no lo usa.
# El test HTML de Motion sigue usando pieceMm = L (target = L−55).
# No modificar Feed / FEED_TARGET_FIXED_MM / Move ABS manual.

# WIP Delivery (HOME): soplo al volver a 0.
# start = magnitud ABS final tras depósito y despeje post-pinzas (gripperClearanceMm).
#
# WIP_BLOWER_CONTINUOUS=True (activo): un solo MOVE→0; al arrancar, blower ON
# durante el mismo tiempo que duró el lineal. PLC apaga solo (no OFF al llegar).
# WIP_BLOWER_CONTINUOUS=False: stop–soplo–stop en fin/inicio (rollback).
WIP_BLOWER_CONTINUOUS = True
# Cada soplo (modo stop): ON(blowerSec) → dwell → OFF explícito.
# Cada soplo (modo continuo): ON(duración=lineal) sin dwell HMI.
WIP_BLOWER_MIN_TRAVEL_MM = 8.0
# Evita move de offset trivial (misma convención firmada que cmd_motion_move_mm).
WIP_BLOWER_OFFSET_EPS_MM = 0.5
# Si hay caché ASDA post-Reached, debe coincidir con la ref de depósito.
WIP_START_CACHE_TOL_MM = 5.0
# Piso/techo duración soplo match-lineal (solo si CONTINUOUS=True).
WIP_BLOWER_LINEAL_MATCH_MIN_S = 0.2
WIP_BLOWER_LINEAL_MATCH_MAX_S = 30.0

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
    # Tfeed ANTES del feed (contrato Review / secuencia productiva).
    {"id": 3, "key": "pf_trigger", "label": "Trigger PreFeeder (Tfeed)", "kind": "action", "sbsPause": False},
    {"id": 4, "key": "feed", "label": "Alimentación (feed / handoff)", "kind": "action", "sbsPause": True},
    {"id": 5, "key": "offset", "label": "Offset alimentación (Motion, paso lógico)", "kind": "action", "sbsPause": False},
    {"id": 6, "key": "grippers_on", "label": "Pinzas cierran", "kind": "action", "sbsPause": False},
    {"id": 7, "key": "wait_grippers_on", "label": "Delay tras cerrar pinzas", "kind": "wait", "delayKey": "grippersOnMs", "sbsPause": False},
    {"id": 8, "key": "enc_set0", "label": "OM ref (no usado por lineal TCP)", "kind": "action", "sbsPause": True},
    {"id": 9, "key": "holder_off", "label": "Holder+Encoder OFF (abre para lineal)", "kind": "action", "sbsPause": False},
    {"id": 10, "key": "wait_holder_open", "label": "Delay Holder/Encoder OFF", "kind": "wait", "delayKey": "holderOpenMs", "sbsPause": True},
    {"id": 11, "key": "lineal_fwd", "label": "Lineal ASDA MOVE TCP (0→ABS)", "kind": "action", "sbsPause": False},
    {"id": 12, "key": "wait_linear_done", "label": "Delay antes del corte", "kind": "wait", "delayKey": "linearDoneMs", "sbsPause": True},
    {"id": 13, "key": "holder_precut", "label": "Holder ON / Encoder ON (pre-corte)", "kind": "action", "sbsPause": False},
    {"id": 14, "key": "wait_holder_precut", "label": "Delay tras cerrar holder", "kind": "wait", "delayKey": "holderOnMs", "sbsPause": True},
    {"id": 15, "key": "cutter_on", "label": "Cortador ON (+ All OK PreFeeder)", "kind": "action", "sbsPause": False},
    {"id": 16, "key": "wait_cutter_pulse", "label": "Delay entre Set y Res cortador", "kind": "wait", "delayKey": "cutterPulseMs", "sbsPause": False, "delayEditable": False},
    {"id": 17, "key": "cutter_off", "label": "Cortador OFF", "kind": "action", "sbsPause": False},
    {"id": 18, "key": "wait_cutter_post", "label": "Delay post-corte", "kind": "wait", "delayKey": "cutterPostMs", "sbsPause": True},
    # Depósito ANTES del prefetch: la manguera debe salir del área antes de pre-alimentar.
    {"id": 19, "key": "deposit", "label": "Extra / depósito lineal", "kind": "action", "sbsPause": False},
    {"id": 20, "key": "wait_deposit_dwell", "label": "Delay tras depósito", "kind": "wait", "delayKey": "dwellAtDestMs", "sbsPause": True},
    {
        "id": 21,
        "key": "prefetch_start",
        "label": "Prefetch feed — arranca en background",
        "kind": "parallel",
        "parallelRole": "start",
        "sbsPause": False,
    },
    {"id": 22, "key": "grippers_off", "label": "Pinzas abren", "kind": "action", "sbsPause": False},
    {"id": 23, "key": "wait_gripper_release", "label": "Delay tras abrir pinzas", "kind": "wait", "delayKey": "gripperReleaseMs", "sbsPause": True},
    {
        "id": 24,
        "key": "gripper_clearance",
        "label": "Despeje ASDA post-pinzas (+clearance)",
        "kind": "action",
        "sbsPause": True,
    },
    {"id": 25, "key": "home", "label": "HOME: WIP blower continuo (match lineal) → 0", "kind": "action", "sbsPause": True},
    {
        "id": 26,
        "key": "handoff",
        "label": "Join — espera fin del prefetch (handoff)",
        "kind": "parallel",
        "parallelRole": "join",
        "sbsPause": False,
    },
    {"id": 27, "key": "wait_asentar", "label": "Delay asentar", "kind": "wait", "delayKey": "asentarMs", "sbsPause": False},
    {"id": 28, "key": "post_piece", "label": "Post-pieza (safety / peer / holgura)", "kind": "action", "sbsPause": False},
]
PROGRESS_STEPS = len(FLOW_STEPS)
STEP_NAMES = {0: "idle", **{s["id"]: s["key"] for s in FLOW_STEPS}}
STEP_BY_KEY = {s["key"]: s for s in FLOW_STEPS}
# Keys de control de lote (no están en FLOW_STEPS): nunca pausan en SBS.
STEP_BY_STEP_NO_PAUSE = frozenset({"listo", "rep-start"})


class _OrEvent:
    """Event compuesto: is_set() si cualquiera de los eventos está activo."""

    __slots__ = ("_events",)

    def __init__(self, *events: threading.Event) -> None:
        self._events = events

    def is_set(self) -> bool:
        return any(e.is_set() for e in self._events)


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
    # Avance corto tras abrir pinzas (aleja mordazas de la pieza).
    # La ref WIP (soplo fin) = |depósito o Stage2| + este clearance.
    gripper_clearance_mm: float = 10.0
    cut_offset_mm: float = 0.0
    # Offset blower desde cada punta hacia el centro (mm).
    # Fin (grippers): soplo en start − offset. Inicio (cortador): soplo en +offset. Luego HOME.
    wip_blower_inicio_offset_mm: float = 8.0
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
            "gripperClearanceMm": "gripper_clearance_mm",
            "cutOffsetMm": "cut_offset_mm",
            "wipBlowerInicioOffsetMm": "wip_blower_inicio_offset_mm",
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
            "gripper_clearance_mm",
            "cut_offset_mm",
            "wip_blower_inicio_offset_mm",
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
        # Pulso lógico Set→Res ≥ pulso eléctrico KEEP del PLC (100 ms) + gap.
        if "cutter_pulse_ms" in kw and kw["cutter_pulse_ms"] < 150:
            kw["cutter_pulse_ms"] = 150
        cfg = cls(**kw)
        if cfg.cutter_pulse_ms < 150:
            cfg.cutter_pulse_ms = 150
        return cfg
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
                    or (
                        "wipBlowerInicioOffsetMm" not in raw
                        and "wip_blower_inicio_offset_mm" not in raw
                    )
                    or (
                        "gripperClearanceMm" not in raw
                        and "gripper_clearance_mm" not in raw
                    )
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
    def pf_holgura_present(self, side: str) -> bool | None: ...
    def pf_buffer_full(self, side: str) -> bool | None: ...
    def pf_trigger_active(self, side: str) -> bool | None: ...
    def pf_auto_filling(self, side: str) -> bool | None: ...
    def pf_request_status(self) -> bool: ...
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
    def motion_laser_on(self, side: str) -> bool: ...
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
    def cmd_plc_cutters(
        self, on: bool, *, sides: str | None = None, force: bool = False
    ) -> bool: ...
    def cmd_plc_tools_safe(self) -> bool: ...
    def cmd_plc_all_safe(self) -> bool: ...
    def cmd_pf_start(self) -> bool: ...
    def cmd_pf_stop(self) -> bool: ...
    def cmd_pf_in_process(self, on: bool = True) -> bool: ...
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
        self._busy_mode = False
        self._step_by_step = False
        self._trial_mode = False
        self._refill_mode = False
        self._refill_awaiting_confirm = False
        self._refill_prompt = ""  # "" | after_feed | after_cut
        self._refill_confirm = threading.Event()
        self._refill_reject = threading.Event()
        self._refill_retry = threading.Event()
        self._step = 0
        self._parallel_group = ""
        self._rep = 0
        self._pieces_done = 0
        self._total_reps = 0
        self._progress = 0
        self._last_ok = False
        self._fault = ""
        self._fault_class = ""
        self._c3_stop_after_step = False  # legado: pausar en próximo _enter
        # C3 en lote: completar pieza en curso (hasta post_piece / corte) y pausar.
        self._c3_finish_piece = False
        self._recovery = ""  # home | restart_from_0 | retry_process
        # C2 Resume → reinicio pieza desde step 0; feed omitible si láser ON.
        self._restart_piece = False
        self._c2_laser_skip_feed = False
        self._flow_interrupt = threading.Event()
        self._lot_rpm = 1200.0
        self._last_lineal_sec: float = 0.0
        self._last_lineal_mm: float = 0.0
        # Reloj de lote (wall, incluye prep) — refill / diagnóstico.
        self._started_at: float | None = None
        # CT productivo: 1ª pieza (holder/feed) → última freeze (antes post_piece).
        # No incluye prep / Finish / settled. Pause no suma.
        self._cycle_t0: float | None = None
        self._piece_t0: float | None = None
        self._piece_times: list[float] = []
        self._last_piece_sec: float = 0.0
        self._avg_piece_sec: float = 0.0
        self._finished_elapsed = 0.0
        self._pause_t0: float | None = None
        self._pause_excluded_sec: float = 0.0
        self._piece_pause_base: float = 0.0
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
                "busy": self._busy_mode or (
                    self._state_byte == TX_BUSY and self._active
                ),
                "stepByStep": self._step_by_step,
                "trialMode": self._trial_mode,
                "refillActive": self._refill_mode and self._active,
                "refillAwaitingConfirm": self._refill_awaiting_confirm,
                # Prompt se mantiene durante feed/corte/home para que la UI no parpadee.
                "refillPrompt": (
                    self._refill_prompt
                    if (self._refill_mode and self._active and self._refill_prompt)
                    else ""
                ),
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
                "elapsedSec": self._ct_elapsed_locked(),
                "lastPieceSec": float(self._last_piece_sec),
                "avgPieceSec": float(self._avg_piece_sec),
                "completed": self._last_ok and not self._active,
                "lastOk": self._last_ok,
                "fault": self._fault,
                "faultClass": self._fault_class,
                "recovery": self._recovery,
                "c3Pending": self._c3_stop_after_step or self._c3_finish_piece,
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

    def update_config(self, data: dict[str, Any]) -> CycleConfig:
        with self._lock:
            # cutterPulseMs no es editable (UI/API): piso PLC KEEP; no aceptar override.
            incoming = dict(data or {})
            incoming.pop("cutterPulseMs", None)
            incoming.pop("cutter_pulse_ms", None)
            merged = self._cfg.to_dict()
            merged.update(incoming)
            # Mantener el valor vigente (ya ≥150 vía from_dict / _do_wait).
            merged["cutterPulseMs"] = max(150, int(self._cfg.cutter_pulse_ms or 150))
            self._cfg = CycleConfig.from_dict(merged)
            save_cycle_config(self._cfg)
            cfg = CycleConfig(**asdict(self._cfg))
        self._host.cycle_log("Cycle config guardada")
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
        self.reload_config()
        self._stop.clear()
        self._pause.clear()
        self._aborted = False
        self._fault = ""
        self._fault_class = ""
        self._c3_stop_after_step = False
        self._c3_finish_piece = False
        self._recovery = ""
        self._restart_piece = False
        self._c2_laser_skip_feed = False
        self._flow_interrupt.clear()
        self._last_ok = False
        with self._lock:
            self._refill_mode = False
            self._refill_awaiting_confirm = False
            self._refill_prompt = ""
            self._reset_ct_clocks_locked()
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
        self._flow_interrupt.set()
        self._restart_piece = False
        self._c2_laser_skip_feed = False
        self._pause.clear()
        self._refill_reject.set()
        with self._lock:
            self._refill_awaiting_confirm = False
            self._refill_prompt = ""
        # Desarma waits Stage2/Feed/Reached — si no, el hilo queda active ~3 min
        # y Reset responde "Detener ciclo antes de Reset".
        self._host.clear_motion_wait_flags()
        self._host.cmd_motion_stop()
        self._host.cmd_pf_stop()
        # Stop no toca PLC (válvulas: All Off / Reset PLC propios).
        self._set_state(TX_STOP, "Stop (0x042)")
        self._host.cycle_log("Cycle Stop (0x041)")
        return {"ok": True}
    def request_pause(self) -> dict[str, Any]:
        if not self.is_active():
            return {"ok": False, "error": "Sin ciclo activo"}
        self._pause.set()
        with self._lock:
            if self._pause_t0 is None:
                self._pause_t0 = time.monotonic()
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
            # C2: reinicio pieza desde step 0 (norma). Feed se omite si láser ON.
            if self._recovery == "restart_from_0":
                self._restart_piece = True
                self._flow_interrupt.set()
                try:
                    self._host.clear_motion_wait_flags()
                    self._host.cmd_motion_stop()
                except Exception:
                    pass
                self._host.cycle_log(
                    "Cycle Resume (C2) → reinicio pieza desde step 0"
                )
            else:
                self._host.cycle_log("Cycle Resume")
            self._pause.clear()
            with self._lock:
                self._sync_pause_exclusion_locked(time.monotonic())
            self._leave_pause_andon()
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
        """Purga/refill: ASDA park → holder+encoder → feed → (retry|cut) → home.

        Tras feed: operador Retry o Next Cutting.
        Tras corte: Next Return ASDA to 0.
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
        self._c3_finish_piece = False
        self._recovery = ""
        self._last_ok = False
        self._refill_confirm.clear()
        self._refill_reject.clear()
        self._refill_retry.clear()
        with self._lock:
            self._refill_awaiting_confirm = False
            self._refill_prompt = ""
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
        """Next (ok) o cancelar (park) según el prompt actual del refill."""
        if not self._refill_awaiting_confirm:
            return {"ok": False, "error": "Sin refill pendiente de confirmación"}
        prompt = self._refill_prompt
        if ok:
            self._refill_confirm.set()
            if prompt == "after_feed":
                self._host.cycle_log("Refill: Next → Cutting")
            else:
                self._host.cycle_log("Refill: Next → ASDA a 0")
        else:
            self._refill_reject.set()
            self._host.cycle_log("Refill: operador canceló (ASDA permanece en park)")
        self._host.cycle_notify()
        return {"ok": True}

    def retry_refill(self) -> dict[str, Any]:
        """Reintenta solo el feed (válido tras alimentar, antes del corte)."""
        if not self._refill_awaiting_confirm:
            return {"ok": False, "error": "Sin refill pendiente de confirmación"}
        if self._refill_prompt != "after_feed":
            return {"ok": False, "error": "Retry solo tras alimentar (antes del corte)"}
        self._refill_retry.set()
        self._host.cycle_log("Refill: operador pide reintentar alimentación")
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
        self._c3_finish_piece = False
        self._recovery = ""
        self._restart_piece = False
        self._c2_laser_skip_feed = False
        self._flow_interrupt.clear()
        self._last_ok = False
        self._materialist = False
        self._busy_mode = False
        self._refill_mode = False
        self._refill_awaiting_confirm = False
        self._refill_prompt = ""
        self._refill_confirm.clear()
        self._refill_reject.clear()
        self._refill_retry.clear()
        self._pieces_done = 0
        with self._lock:
            self._reset_ct_clocks_locked()
        self._set_progress(0, 0, 0)
        self._set_state(TX_IDLE)
        self._host.cycle_log("Cycle Reset (0x043)")
        return {"ok": True}

    def clear_fault_mirror(self) -> None:
        """Limpia el espejo EXXX del ciclo sin exigir lote activo.

        El flip-flop vive en ErrorPolicy; cycle._fault solo refleja para UI/snapshot.
        Si el Res limpia el latch pero deja _fault, la UI sigue en ERROR (p.ej. tras
        lote completado o Reset local de Motion).
        """
        self._fault = ""
        self._fault_class = ""
        self._recovery = ""

    def clear_error_for_resume(self, recovery: str = "") -> dict[str, Any]:
        """Res suave C2/C3: limpia fault; mantiene lote vivo.

        - C2 (ya en Pause): sigue en Pause → operador Resume.
        - C3 finish-piece: no pausar aún; el hilo corta y pausa en post_piece.
        """
        if not self.is_active():
            return {"ok": False, "error": "Sin ciclo activo para Resume"}
        self.clear_fault_mirror()
        if recovery:
            self._recovery = recovery
        if self._c3_finish_piece:
            # Seguir hasta corte; Pause real en _enter(post_piece).
            self._set_state(TX_BUSY)
            self._host.cycle_log(
                "Cycle soft-Res: C3 continúa hasta cortar pieza (luego Pause)"
            )
            self._host.cycle_notify()
            return {
                "ok": True,
                "paused": False,
                "recovery": self._recovery,
                "soft": True,
                "finishingPiece": True,
            }
        if not self._pause.is_set():
            self._pause.set()
        self._set_state(TX_PAUSE)
        self._host.cycle_log(
            f"Cycle soft-Res → Pause (recovery={self._recovery or '—'}; Resume)"
        )
        self._host.cycle_notify()
        return {
            "ok": True,
            "paused": True,
            "recovery": self._recovery,
            "soft": True,
        }

    def set_materialist(self, on: bool) -> dict[str, Any]:
        if on and self.is_active():
            return {"ok": False, "error": "No Materialist con ciclo activo"}
        self._materialist = bool(on)
        self._busy_mode = False
        if self._materialist:
            self._set_state(TX_MATERIALIST)
            self._host.cycle_log("Cycle Materialist ON (0x049)")
        else:
            self._set_state(TX_IDLE)
            self._host.cycle_log("Cycle Materialist OFF → Idle")
        return {"ok": True, "materialist": self._materialist, "busy": False}

    def set_busy(self, on: bool) -> dict[str, Any]:
        """Busy máquina (0x045) sin arrancar ciclo — Andon Green + PF In process."""
        if on and self.is_active():
            return {"ok": True, "busy": True, "materialist": False}
        if not on and self.is_active():
            return {"ok": False, "error": "Ciclo activo — usar Stop"}
        self._busy_mode = bool(on)
        if self._busy_mode:
            self._materialist = False
            self._set_state(TX_BUSY)
            self._host.cycle_log("Cycle Busy ON (0x045)")
        else:
            self._set_state(TX_IDLE)
            self._host.cycle_log("Cycle Busy OFF → Idle")
        return {"ok": True, "busy": self._busy_mode, "materialist": False}

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

    def _use_prefeeder(self) -> bool:
        """PreFeeder participa en el ciclo si hay enlace."""
        return self._host.pf_connected()
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
            if self._pause_t0 is None:
                self._pause_t0 = time.monotonic()
            if self._state_byte == TX_ERROR:
                return
        self._set_state(TX_PAUSE)

    def _leave_pause_andon(self) -> None:
        """Resume → Busy si el ciclo sigue activo y no hay Error con fault."""
        with self._lock:
            self._sync_pause_exclusion_locked(time.monotonic())
            if self._state_byte == TX_ERROR and self._fault:
                return
            if not self._active:
                return
        self._set_state(TX_BUSY)

    def _wait_paused_for_resume(self, reason: str) -> bool:
        """Pausa cooperativa hasta Resume/Stop. True = abortar."""
        self._pause.set()
        with self._lock:
            if self._pause_t0 is None:
                self._pause_t0 = time.monotonic()
        self._host.cycle_log(reason)
        self._host.cycle_notify()
        while self._pause.is_set():
            if self._should_abort():
                return True
            time.sleep(0.05)
        with self._lock:
            self._sync_pause_exclusion_locked(time.monotonic())
        if self._fault:
            # Soft-Res debió limpiar fault; si sigue, no continuar ciego.
            return True
        self._leave_pause_andon()
        return self._should_abort()

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

        C1/C2/C3 no mandan válvulas ni Reset PLC: el PLC solo se opera con
        pulso/pulso, All Off o su Reset propio.
        """
        self._fault = ui
        self._fault_class = err_class
        self._recovery = recovery
        # EXXX tras lote OK: no dejar "Batch complete" + ERROR a la vez.
        if not self.is_active():
            self._last_ok = False
        if action == "stop_all":
            self._c3_stop_after_step = False
            self._c3_finish_piece = False
            self._aborted = True
            self._stop.set()
            self._pause.clear()
            try:
                self._host.clear_motion_wait_flags()
            except Exception:
                pass
            # Best-effort Motion/PF; PLC intocable desde política de error.
            for fn in (
                self._host.cmd_motion_stop,
                self._host.cmd_pf_stop,
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
            self._c3_finish_piece = False
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
            self._c3_finish_piece = False
            if self.is_active():
                self._pause.set()
            self._set_state(TX_ERROR, ui)
            self._host.cycle_notify()
            return {"ok": True, "action": action}
        if action == "finish_step":
            # C3: terminar la pieza en curso (corte incluido) → Pause → Reset → Resume.
            # No abortar el lote aquí: el hilo pausa en post_piece / borde de pieza.
            self._c3_stop_after_step = True
            self._c3_finish_piece = True
            self._set_state(TX_ERROR, ui)
            self._host.cycle_log(
                "C3: completar pieza en curso (corte) y pausar para Reset/Resume"
            )
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
        # C3: completar pieza (corte) → Pause en post_piece; esperar Reset+Resume.
        # Antes: return True abortaba el lote y Resume quedaba muerto.
        if self._c3_finish_piece:
            if key == "post_piece":
                self._c3_finish_piece = False
                self._c3_stop_after_step = False
                if self._wait_paused_for_resume(
                    "C3: pieza cortada — Pause; Reset → Resume para continuar"
                ):
                    return True
        elif self._c3_stop_after_step:
            self._c3_stop_after_step = False
            if self._wait_paused_for_resume(
                "C3: paso terminado — Pause; Reset → Resume"
            ):
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
        # Set→Res cortador: ≥ VALVE_PULSE_MS(+gap) o el KEEP no ve el Res.
        if cfg_attr == "cutter_pulse_ms" and ms < 150:
            ms = 150
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
            return self._should_abort() or bool(self._restart_piece)
        remaining = ms / 1000.0
        while remaining > 0:
            if self._should_abort() or self._restart_piece:
                return True
            if self._pause.is_set():
                while self._pause.is_set():
                    if self._should_abort() or self._restart_piece:
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
        return self._should_abort() or bool(self._restart_piece)
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
        return bool(self._restart_piece)
    def _gate(self, step_name: str) -> bool:
        """Tras un paso: Pause/Stop. True = salir del intento de pieza."""
        _ = step_name
        if self._should_abort():
            return True
        while self._pause.is_set():
            if self._should_abort():
                return True
            time.sleep(0.05)
        return bool(self._restart_piece)
    def _wait_motion(self) -> bool:
        """Espera Idle/Reached. El caller debe limpiar flags ANTES del comando
        (clear_motion_wait_flags / clear_motion_reached_flag) — no limpiar aquí
        o se pierde el evento si Motion responde entre el cmd y el wait."""
        cfg = self.get_config()
        deadline = time.monotonic() + float(cfg.motion_wait_timeout_s)
        while time.monotonic() < deadline:
            if self._should_abort() or self._restart_piece:
                return False
            while self._pause.is_set():
                if self._should_abort() or self._restart_piece:
                    return False
                time.sleep(0.05)
            if self._host.wait_motion_idle_or_reached(0.1):
                return True
        self._raise_fault("timeout_motion")
        self._host.cycle_log(format_ui("E008"))
        return False

    def _consume_restart_piece(self) -> bool:
        """True si C2 Resume pidió reinicio de pieza; limpia el flag."""
        if not self._restart_piece:
            return False
        self._restart_piece = False
        self._flow_interrupt.clear()
        return True

    def _feed_sides_laser_present(self) -> bool:
        """True si el láser de todos los lados de feed detecta material."""
        for side in self._feed_side_list():
            if not self._host.motion_laser_on(side):
                return False
        return True

    def _feed_abort_event(self) -> threading.Event:
        """Stop o interrupt C2 (reinicio pieza) abortan waits de feed."""
        return _OrEvent(self._stop, self._flow_interrupt)  # type: ignore[return-value]

    def _wait_duration_s(self, sec: float) -> bool:
        """Espera cooperativa (respeta pause/abort). False = abort."""
        deadline = time.monotonic() + max(0.0, float(sec))
        while time.monotonic() < deadline:
            if self._should_abort() or self._restart_piece:
                return False
            while self._pause.is_set():
                if self._should_abort() or self._restart_piece:
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

    def _sync_pause_exclusion_locked(self, now: float) -> None:
        """Acumula tiempo en Pause (caller debe tener _lock)."""
        if self._pause.is_set():
            if self._pause_t0 is None:
                self._pause_t0 = now
            return
        if self._pause_t0 is not None:
            self._pause_excluded_sec += max(0.0, now - self._pause_t0)
            self._pause_t0 = None

    def _ct_elapsed_locked(self) -> int:
        """Segundos CT para UI (congelado tras freeze; no infla con Finish)."""
        now = time.monotonic()
        self._sync_pause_exclusion_locked(now)
        if self._cycle_t0 is not None:
            raw = now - self._cycle_t0 - float(self._pause_excluded_sec)
            return max(0, int(raw))
        return max(0, int(self._finished_elapsed))

    def _mark_piece_clock_start(self) -> None:
        """Arranca CT productivo (1ª pieza) y reloj de la pieza en curso."""
        now = time.monotonic()
        with self._lock:
            self._sync_pause_exclusion_locked(now)
            if self._cycle_t0 is None:
                self._cycle_t0 = now
            self._piece_t0 = now
            self._piece_pause_base = float(self._pause_excluded_sec)

    def _freeze_piece_clock(self) -> float:
        """Congela reloj de pieza/CT (sin log). Devuelve duración pieza (s)."""
        now = time.monotonic()
        with self._lock:
            self._sync_pause_exclusion_locked(now)
            piece_sec = 0.0
            if self._piece_t0 is not None:
                piece_sec = max(
                    0.0,
                    now
                    - self._piece_t0
                    - (float(self._pause_excluded_sec) - float(self._piece_pause_base)),
                )
                self._piece_times.append(piece_sec)
                self._last_piece_sec = piece_sec
                n = len(self._piece_times)
                self._avg_piece_sec = (
                    sum(self._piece_times) / n if n else 0.0
                )
                self._piece_t0 = None
            if self._cycle_t0 is not None:
                self._finished_elapsed = max(
                    0.0,
                    now - self._cycle_t0 - float(self._pause_excluded_sec),
                )
            return piece_sec

    def _log_piece_ok(self, rep: int, qty: int, piece_sec: float) -> None:
        if piece_sec > 0:
            self._host.cycle_log(f"Pieza {rep}/{qty} OK · {piece_sec:.1f}s")
        else:
            self._host.cycle_log(f"Pieza {rep}/{qty} OK")

    def _reset_ct_clocks_locked(self) -> None:
        self._cycle_t0 = None
        self._piece_t0 = None
        self._piece_times = []
        self._last_piece_sec = 0.0
        self._avg_piece_sec = 0.0
        self._finished_elapsed = 0.0
        self._pause_t0 = None
        self._pause_excluded_sec = 0.0
        self._piece_pause_base = 0.0
        self._last_lineal_sec = 0.0
        self._last_lineal_mm = 0.0

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

    def _wip_blower_pulse(self, at_mm: float, blower_sec: float, tag: str) -> bool:
        """ON → dwell → OFF explícito. False = abort/falla (no mover con blower ON)."""
        if self._should_abort():
            return False
        if not self._host.cmd_plc_blower(True, duration_sec=blower_sec):
            self._raise_fault("wip_blower")
            self._host.cycle_log(
                f"WIP Delivery blower ON FAIL @ {tag}={at_mm:.1f} mm "
                f"— no dwell / no continuar"
            )
            return False
        self._host.cycle_log(
            f"WIP Delivery blower ON @ {tag}={at_mm:.1f} mm hold={blower_sec:g}s"
        )
        if not self._wait_duration_s(blower_sec):
            # Abort a mitad: apagar blower best-effort; no continuar
            self._host.cmd_plc_blower(False)
            return False
        if not self._host.cmd_plc_blower(False):
            self._raise_fault("wip_blower")
            self._host.cycle_log(
                f"WIP Delivery blower OFF FAIL @ {tag}={at_mm:.1f} mm "
                f"— no continuar (blower puede seguir activo)"
            )
            return False
        self._host.cycle_log(f"WIP Delivery blower OFF ok @ {tag}={at_mm:.1f} mm")
        return True

    def _wip_motion_zero(self, rpm: float, *, prefetch_running: bool) -> bool:
        """HOME a 0 con blower OFF (modo stop). False = abort/falla."""
        self._arm_motion_leg(prefetch_running)
        self._host.cycle_log("WIP Delivery MOVE → HOME=0.0 mm")
        if not self._host.cmd_motion_move_zero(rpm):
            if not self._should_abort() and not self._fault:
                kind = ""
                if hasattr(self._host, "last_move_fail_kind"):
                    kind = self._host.last_move_fail_kind()
                self._raise_fault("E065" if kind == "transport" else "home_cmd")
            return False
        if not self._wait_motion():
            return False
        self._host.cycle_log("WIP Delivery move ok @ HOME=0.0 mm")
        return True

    def _wip_motion_to(
        self, mm: float, rpm: float, *, prefetch_running: bool, tag: str
    ) -> bool:
        """Move ABS firmado (solo con blower OFF). False = abort/falla."""
        self._arm_motion_leg(prefetch_running)
        self._host.cycle_log(f"WIP Delivery MOVE → {tag}={float(mm):.1f} mm")
        if not self._host.cmd_motion_move_mm(float(mm), rpm):
            if not self._should_abort() and not self._fault:
                kind = ""
                if hasattr(self._host, "last_move_fail_kind"):
                    kind = self._host.last_move_fail_kind()
                self._raise_fault("E065" if kind == "transport" else "home_cmd")
                self._host.cycle_log(
                    f"WIP Delivery move FAIL @ {tag}={float(mm):.1f} mm"
                )
            return False
        if not self._wait_motion():
            return False
        self._host.cycle_log(f"WIP Delivery move ok @ {tag}={float(mm):.1f} mm")
        return True

    def _home_with_wip_delivery(
        self,
        start_mm: float | None,
        rpm: float,
        *,
        prefetch_running: bool,
    ) -> bool:
        if WIP_BLOWER_CONTINUOUS:
            return self._home_with_wip_delivery_continuous(
                start_mm, rpm, prefetch_running=prefetch_running
            )
        return self._home_with_wip_delivery_stop(
            start_mm, rpm, prefetch_running=prefetch_running
        )

    def _home_with_wip_delivery_continuous(
        self,
        start_mm: float | None,
        rpm: float,
        *,
        prefetch_running: bool,
    ) -> bool:
        """Un solo MOVE→0; blower ON match-lineal al arrancar. PLC apaga solo."""
        if start_mm is None:
            self._raise_fault("wip_start")
            self._host.cycle_log(
                "WIP start ausente — falta ref depósito/despeje (no se estima)"
            )
            return False

        start_abs = abs(float(start_mm))
        if not self._validate_wip_start_mm(start_abs):
            return False

        if start_abs < WIP_BLOWER_MIN_TRAVEL_MM:
            self._host.cycle_log(
                f"WIP Delivery: omitido (travel={start_abs:.1f} mm) — HOME directo"
            )
            return self._wip_motion_zero(rpm, prefetch_running=prefetch_running)

        lineal_s = float(self._last_lineal_sec or 0.0)
        if lineal_s < WIP_BLOWER_LINEAL_MATCH_MIN_S:
            lineal_s = float(self._host.plc_blower_sec())
            match_src = "blowerSec(fallback)"
        else:
            match_src = "lineal"
        lineal_s = max(
            WIP_BLOWER_LINEAL_MATCH_MIN_S,
            min(WIP_BLOWER_LINEAL_MATCH_MAX_S, lineal_s),
        )

        self._host.cycle_log(
            f"WIP Delivery (continuo/match-lineal): desde={start_abs:.1f} mm "
            f"→ HOME=0 (blower={lineal_s:.2f}s ({match_src}))"
        )

        self._arm_motion_leg(prefetch_running)
        self._host.cycle_log(
            f"WIP Delivery MOVE continuo → HOME=0.0 mm "
            f"(desde={start_abs:.1f}; blower {lineal_s:.2f}s match={match_src})"
        )
        if not self._host.cmd_motion_move_zero(rpm):
            if not self._should_abort() and not self._fault:
                kind = ""
                if hasattr(self._host, "last_move_fail_kind"):
                    kind = self._host.last_move_fail_kind()
                self._raise_fault("E065" if kind == "transport" else "home_cmd")
            return False
        # Evitar Reached residual del despeje (wait instantáneo).
        try:
            self._host.clear_motion_reached_flag()
        except Exception:
            pass

        if not self._host.cmd_plc_blower(True, duration_sec=lineal_s):
            self._raise_fault("wip_blower")
            self._host.cycle_log(
                f"WIP Delivery blower ON FAIL @ HOME=0.0 mm hold={lineal_s:g}s"
            )
            return False
        self._host.cycle_log(
            f"WIP Delivery blower ON @ HOME=0.0 mm hold={lineal_s:g}s (en vuelo)"
        )

        if not self._wait_motion():
            return False
        self._host.cycle_log("WIP Delivery move ok @ HOME=0.0 mm (continuo)")
        # No OFF forzado: timer PLC apaga. Evita corte prematuro del soplo.
        return True

    def _home_with_wip_delivery_stop(
        self,
        start_mm: float | None,
        rpm: float,
        *,
        prefetch_running: bool,
    ) -> bool:
        """HOME + WIP stop–soplo–stop: fin(+offset)→soplo → inicio→soplo → HOME 0.

        `start_mm` = magnitud ABS en punta final (lado grippers), post depósito
        y despeje. No es estimación.

        Secuencia (blower montado en offset respecto a cada punta):
        1) Desde punta final → move a (start − offset) → soplo
        2) Move a punta inicial (lado cortador = offset) → soplo
        3) HOME 0
        """
        if start_mm is None:
            self._raise_fault("wip_start")
            self._host.cycle_log(
                "WIP start ausente — falta ref depósito/despeje/Stage2 (no se estima)"
            )
            return False

        start_abs = abs(float(start_mm))
        if not self._validate_wip_start_mm(start_abs):
            return False

        travel = start_abs
        blower_sec = float(self._host.plc_blower_sec())
        # Offset >0 = hacia el centro desde cada punta (fin↓, inicio↑ desde 0).
        offset = float(self.get_config().wip_blower_inicio_offset_mm)
        use_offset = abs(offset) > WIP_BLOWER_OFFSET_EPS_MM

        if travel < WIP_BLOWER_MIN_TRAVEL_MM:
            self._host.cycle_log(
                f"WIP Delivery: omitido (travel={travel:.1f} mm) — HOME directo"
            )
            return self._wip_motion_zero(rpm, prefetch_running=prefetch_running)

        fin_mm = start_abs - offset if use_offset else start_abs
        inicio_mm = offset if use_offset else 0.0
        # Evitar cruzar o soplar fuera del tramo [0, start].
        if fin_mm < 0.0:
            fin_mm = 0.0
        if fin_mm < inicio_mm:
            self._host.cycle_log(
                f"WIP Delivery: offset={offset:.1f} mm demasiado grande para "
                f"start={start_abs:.1f} mm — clamp fin={inicio_mm:.1f} mm"
            )
            fin_mm = inicio_mm

        self._host.cycle_log(
            f"WIP Delivery: fin(grippers)={start_abs:.1f} → offset soplo={fin_mm:.1f} "
            f"→ inicio(cortador)={inicio_mm:.1f} → HOME "
            f"({blower_sec:g}s c/u, offset={offset:.1f})"
        )

        # 1) Punta final (grippers): move offset → soplo
        if abs(fin_mm - start_abs) > WIP_BLOWER_OFFSET_EPS_MM:
            if not self._wip_motion_to(
                fin_mm, rpm, prefetch_running=prefetch_running, tag="fin_offset"
            ):
                return False
        if not self._wip_blower_pulse(fin_mm, blower_sec, "fin"):
            return False

        # 2) Punta inicial (cortador): move → soplo
        if abs(inicio_mm - fin_mm) > WIP_BLOWER_OFFSET_EPS_MM:
            if not self._wip_motion_to(
                inicio_mm, rpm, prefetch_running=prefetch_running, tag="inicio"
            ):
                return False
        if not self._wip_blower_pulse(inicio_mm, blower_sec, "inicio"):
            return False

        # 3) HOME 0
        return self._wip_motion_zero(rpm, prefetch_running=prefetch_running)

    def _wait_feed(self) -> bool:
        if self._trial_mode:
            self._host.cycle_log("Trial: feed OK omitido (bypass encoder)")
            return True
        cfg = self.get_config()
        sides = self._feed_side_list()
        outcomes = self._host.wait_feed_length_ok_for(
            sides,
            cfg.feed_wait_timeout_s,
            abort_event=self._feed_abort_event(),  # type: ignore[arg-type]
        )
        if self._restart_piece:
            return False
        bad = [s for s, v in outcomes.items() if v != "ok"]
        if not bad:
            sides_txt = "".join(sides)
            self._host.cycle_log(f"Feed OK lados={sides_txt}")
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

    def _do_pf_trigger(self) -> bool:
        """Tfeed a lados de feedSides. True = OK / omitido; False = fault."""
        if not self._use_prefeeder():
            self._host.cycle_log("trigger PreFeeder: omitido (sin enlace)")
            return True
        sides = CycleConfig.normalize_feed_sides(self.get_config().feed_sides)
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
            return False
        self._host.cycle_log(
            f"trigger PreFeeder Tfeed lados={sides} — "
            f"R(0x4C)={r_txt} L(0x51)={l_txt}"
        )
        return True

    def _force_pf_triggers_if_no_holgura(self, *, where: str) -> bool:
        """Lee holgura L/R; si ausente en un lado de feed, manda Tfeed a ese lado.

        Holgura sensor ON = presente (OK). Ausencia → trigger 0x4C/0x51.
        Pide status fresco al Master (misma ruta que heartbeat) antes de decidir.
        """
        if not self._use_prefeeder():
            return True
        # Status L/R llega async; sondear y esperar un tick de RX.
        self._host.pf_request_status()
        t0 = time.monotonic()
        while time.monotonic() - t0 < 0.25:
            if self._should_abort():
                return False
            time.sleep(0.05)
        sides = self._feed_side_list()
        need: list[str] = []
        for s in sides:
            present = self._host.pf_holgura_present(s)
            if present is False:
                need.append(s)
            elif present is None:
                self._host.cycle_log(
                    f"Holgura {s}: sin dato ({where}) — no forzar trigger"
                )
        if not need:
            return True
        ok_r = True
        ok_l = True
        if "R" in need:
            ok_r = self._host.cmd_pf_trigger_r()
        if "L" in need:
            ok_l = self._host.cmd_pf_trigger_l()
        r_txt = ("ok" if ok_r else "fail") if "R" in need else "omit"
        l_txt = ("ok" if ok_l else "fail") if "L" in need else "omit"
        self._host.cycle_log(
            f"Sin holgura → force trigger ({where}) lados={''.join(need)} "
            f"R(0x4C)={r_txt} L(0x51)={l_txt}"
        )
        if ("R" in need and not ok_r) or ("L" in need and not ok_l):
            self._raise_fault("pf_trigger")
            return False
        return True

    def _pf_side_settled_for_idle(self, side: str) -> tuple[bool, str]:
        """True si el lado puede pasar a Idle sin cortar relleno/Tfeed.

        Misma idea que PF fillUntilReady: Buffer Full + holgura + M2 quieto.
        Si no hay triggerActive (Master viejo): holgura presente basta como proxy.
        """
        full = self._host.pf_buffer_full(side)
        holgura = self._host.pf_holgura_present(side)
        filling = self._host.pf_auto_filling(side)
        trig = self._host.pf_trigger_active(side)
        if full is not True:
            return False, f"{side}:buffer≠Full"
        if holgura is not True:
            return False, f"{side}:sin holgura"
        if filling is True:
            return False, f"{side}:rellenando"
        if trig is True:
            return False, f"{side}:Tfeed/helper activo"
        return True, f"{side}:OK"

    def _wait_pf_settled_before_idle(self, timeout_s: float = 3.0) -> None:
        """Espera Buffer Full + holgura (+ M2 idle) antes de In process OFF.

        Sale al primer tick ya settled, o al timeout / Stop. No falla el lote.
        """
        sides = self._feed_side_list()
        self._host.pf_request_status()
        deadline = time.monotonic() + max(0.0, float(timeout_s))
        last_why = ""
        while True:
            if self._aborted or self._stop.is_set():
                return
            reasons: list[str] = []
            all_ok = True
            for s in sides:
                ok, why = self._pf_side_settled_for_idle(s)
                if not ok:
                    all_ok = False
                reasons.append(why)
            why_txt = " ".join(reasons)
            if all_ok:
                self._host.cycle_log(
                    f"PreFeeder: settled (Full+holgura/M2) → Idle ok · {why_txt}"
                )
                return
            if why_txt != last_why:
                self._host.cycle_log(
                    f"PreFeeder: espera settled antes de Idle · {why_txt}"
                )
                last_why = why_txt
            if time.monotonic() >= deadline:
                self._host.cycle_log(
                    f"PreFeeder: timeout {timeout_s:.1f}s settled "
                    f"({why_txt}) — Idle de todos modos"
                )
                return
            # Status fresco; RX async.
            self._host.pf_request_status()
            time.sleep(0.1)

    def _run_lineal_abs(self, abs_target_mm: float) -> bool:
        """Lineal producción: CMD_MOVE (0x05) + Reached por TCP ASDA.

        No usa Stage2 HTTP (/api/stage2/*): ese camino compite con serviceAsdaTcp
        en el loop de Motion y deja huecos muertos justo tras pinzas/holder.
        Mismo canal que feed (TCP) y que depósito/HOME.
        Guarda duración/distancia en `_last_lineal_*` para soplo match-lineal.
        """
        abs_mm = abs(float(abs_target_mm))
        self._host.clear_motion_wait_flags()
        t0 = time.monotonic()
        if not self._host.cmd_motion_move_mm(abs_mm, self._lot_rpm):
            if not self._should_abort() and not self._fault:
                kind = ""
                if hasattr(self._host, "last_move_fail_kind"):
                    kind = self._host.last_move_fail_kind()
                self._raise_fault("E065" if kind == "transport" else "move_cmd")
            self._host.cycle_log("Lineal MOVE TCP: comando rechazado")
            self._last_lineal_sec = 0.0
            self._last_lineal_mm = 0.0
            return False
        if not self._wait_motion():
            self._last_lineal_sec = 0.0
            self._last_lineal_mm = 0.0
            return False
        self._last_lineal_sec = max(0.0, time.monotonic() - t0)
        self._last_lineal_mm = abs_mm
        self._host.cycle_log(
            f"Lineal MOVE TCP OK targetAbsMm={abs_mm:.1f} rpm={self._lot_rpm:g}"
        )
        return not self._should_abort()

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

        Evita arrancar el lineal desde posición residual. Paso 24 sigue haciendo
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

    def _wait_refill_operator_decision(self, prompt: str) -> str:
        """'ok' | 'reject' | 'retry'. Abort/Stop → 'reject'.

        prompt: after_feed (Retry + Next Cutting) | after_cut (Next ASDA 0).
        """
        with self._lock:
            self._refill_prompt = prompt
            self._refill_awaiting_confirm = True
        self._refill_confirm.clear()
        self._refill_reject.clear()
        self._refill_retry.clear()
        self._pause.set()
        self._enter_pause_andon()
        if prompt == "after_feed":
            self._host.cycle_log("Refill: feed listo — Retry o Next → Cutting")
        else:
            self._host.cycle_log("Refill: corte listo — Next → ASDA a 0")
        self._host.cycle_notify()
        try:
            while True:
                if self._should_abort() or self._refill_reject.is_set():
                    return "reject"
                if self._refill_confirm.is_set():
                    return "ok"
                if prompt == "after_feed" and self._refill_retry.is_set():
                    return "retry"
                time.sleep(0.05)
        finally:
            self._pause.clear()
            self._leave_pause_andon()
            # Solo baja awaiting: el prompt queda hasta finish (ASDA→0 / cancel)
            # para que Retry no quite y ponga el panel de confirmación.
            with self._lock:
                self._refill_awaiting_confirm = False
            self._host.cycle_notify()

    def _run_cutter_pulse(self) -> bool:
        """Pulso Set/Res cortador según feedSides (sin PreFeeder All OK)."""
        cfg = self.get_config()
        cut_sides = CycleConfig.normalize_feed_sides(cfg.feed_sides)
        armed = False
        try:
            self._host.cycle_log(f"Refill cortador ON (Set) lados={cut_sides}")
            self._host.cmd_plc_cutters(True, sides=cut_sides, force=True)
            armed = True
            if self._should_abort():
                return False
            time.sleep(max(150, int(cfg.cutter_pulse_ms or 0)) / 1000.0)
            if self._should_abort():
                return False
            self._host.cycle_log(f"Refill cortador OFF (Res) lados={cut_sides}")
            self._host.cmd_plc_cutters(False, sides=cut_sides, force=True)
            armed = False
            time.sleep(max(0, cfg.cutter_post_ms) / 1000.0)
            return not self._should_abort()
        finally:
            if armed:
                self._host.cycle_log(
                    f"Refill cortador OFF (Res) emergencia lados={cut_sides}"
                )
                self._host.cmd_plc_cutters(False, sides=cut_sides, force=True)

    def _refill_cancel_park(self) -> None:
        if self._aborted or self._stop.is_set():
            self._finish(False)
            return
        self._host.cycle_log(
            "Refill cancelado — ASDA permanece en park; tools safe"
        )
        self._host.cmd_plc_tools_safe()
        self._finish(False, soft_cancel=True)

    def _run_refill(self, rpm: float, feed_mm: float, asda_mm: float) -> None:
        """ASDA park → holder → feed ↔ retry → cut → home (prompts operador)."""
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

            # 3) Feed (+ Retry) hasta Next → Cutting
            while True:
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
                    self._progress = 55
                if self._should_abort():
                    self._finish(False)
                    return

                decision = self._wait_refill_operator_decision("after_feed")
                if decision == "ok":
                    break
                if decision == "retry":
                    self._host.cycle_log(
                        "Refill: reintento — solo feed (ASDA en park)"
                    )
                    with self._lock:
                        self._progress = 35
                    continue
                self._refill_cancel_park()
                return

            # 4) Cortar
            if not self._run_cutter_pulse():
                self._finish(False)
                return
            with self._lock:
                self._progress = 75

            # 5) Next → ASDA a 0
            decision = self._wait_refill_operator_decision("after_cut")
            if decision != "ok":
                self._refill_cancel_park()
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

    @staticmethod
    def _clearance_target_mm(pos_signed: float, fallback_signed: float, clearance_mm: float) -> float:
        """Posición firmada tras despeje: aumenta |pos| en clearance (aleja de 0)."""
        clr = abs(float(clearance_mm))
        pos = float(pos_signed)
        mag = abs(pos)
        if mag < 0.01:
            sign = 1.0 if float(fallback_signed) >= 0 else -1.0
            return sign * clr
        sign = 1.0 if pos >= 0 else -1.0
        return sign * (mag + clr)

    def _run_lot(self, length_mm: float, qty: int, rpm: float) -> None:
        with self._lock:
            self._active = True
            self._pieces_done = 0
            self._total_reps = qty
            self._lot_rpm = float(rpm)
            self._last_lineal_sec = 0.0
            self._last_lineal_mm = 0.0
            self._started_at = time.monotonic()
            self._reset_ct_clocks_locked()
        self._host.cycle_notify()
        try:
            if not self._prepare_before_cut():
                self._finish(False)
                return
            # Armar PreFeeder: Start + esperar salir de ErrorState.
            # Si a timeout sigue mal → fault (no continuar).
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
                # Busy máquina → In process en Master/L+R (arma sensores + acepta Tfeed).
                # Sin esto, PF_LR NACK el trigger TCP (sensorsMotionArmed=false).
                if self._host.cmd_pf_in_process(True):
                    self._host.cycle_log("PreFeeder: In process ON (ciclo Busy)")
                else:
                    self._host.cycle_log("PreFeeder: In process ON falló")
            target_mm = self._effective_mm(length_mm)
            completed = 0
            handoff_ready = False
            prefetch_running = False
            for rep in range(1, qty + 1):
                while True:
                    early_exit = True
                    for _piece_attempt in (0,):
                        if self._gate("listo" if rep == 1 else "rep-start"):
                            break
                        self._set_progress(rep, 0, qty)
                        self._last_lineal_sec = 0.0
                        self._last_lineal_mm = 0.0
                        # Antes de pieza: holgura fuera del CT (status/Tfeed prep).
                        if not self._force_pf_triggers_if_no_holgura(
                            where=f"pre-pieza {rep}/{qty}"
                        ):
                            break
                        self._mark_piece_clock_start()
                        # 1–2 Holder+Encoder ON + delay (solo 1ª; se mantienen el lote)
                        if rep == 1:
                            if self._enter(rep, qty, "holder_on"):
                                break
                            self._arm_holder_encoder()
                            self._host.cycle_log(
                                f"Holder+Encoder ON (inicio pieza {rep})"
                            )
                            if self._after_step("holder_on"):
                                break
                            if self._do_wait(rep, qty, "wait_holder_on", "holder_on_ms"):
                                break
                        # 3 Tfeed ANTES del feed (luego 4 Feed / handoff)
                        if self._enter(rep, qty, "pf_trigger"):
                            break
                        if not self._do_pf_trigger():
                            break
                        if self._after_step("pf_trigger"):
                            break
                        # 4 Feed / handoff
                        if self._enter(rep, qty, "feed"):
                            break
                        if handoff_ready:
                            handoff_ready = False
                            self._c2_laser_skip_feed = False
                            self._host.cycle_log("Feed: handoff (prefetch ya listo)")
                        elif self._c2_laser_skip_feed and self._feed_sides_laser_present():
                            self._c2_laser_skip_feed = False
                            sides_txt = "".join(self._feed_side_list())
                            self._host.cycle_log(
                                f"Feed omitido (C2 Resume): láser ya ON lados={sides_txt}"
                            )
                        else:
                            self._c2_laser_skip_feed = False
                            if not self._run_feed():
                                break
                        if self._after_step("feed"):
                            break
                        # 5 Offset (placeholder)
                        if self._enter(rep, qty, "offset"):
                            break
                        if self._after_step("offset"):
                            break
                        # 6–8 Pinzas + delay + enc set0
                        if self._enter(rep, qty, "grippers_on"):
                            break
                        self._host.cmd_plc_gripper(True)
                        self._host.cycle_log("Pinzas ON (cierran)")
                        if self._after_step("grippers_on"):
                            break
                        if self._do_wait(rep, qty, "wait_grippers_on", "grippers_on_ms"):
                            break
                        if self._enter(rep, qty, "enc_set0"):
                            break
                        # Lineal ASDA-only: no RESET OM en Motion.
                        self._host.cycle_log("enc_set0: omitido (lineal TCP no usa OM)")
                        if self._after_step("enc_set0"):
                            break
                        # 9–12 Holder+Encoder abren para lineal + MOVE TCP + delay
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
                        # Producción: MOVE ABS por TCP (0x05) + Reached — no Stage2 HTTP.
                        abs_target_mm = abs(float(target_mm))
                        sides = CycleConfig.normalize_feed_sides(self.get_config().feed_sides)
                        self._host.cycle_log(
                            f"Lineal MOVE TCP modelMm={length_mm:.1f} "
                            f"cutOffset→targetAbsMm={abs_target_mm:.1f} "
                            f"sides={sides} rpm={self._lot_rpm:g}"
                        )
                        if not self._run_lineal_abs(abs_target_mm):
                            break
                        if self._after_step("lineal_fwd"):
                            break
                        if self._do_wait(rep, qty, "wait_linear_done", "linear_done_ms"):
                            break
                        # 13–14 Cerrar Holder+Encoder de nuevo pre-corte
                        if self._enter(rep, qty, "holder_precut"):
                            break
                        self._arm_holder_encoder()
                        self._host.cycle_log("Holder+Encoder ON (cierran pre-corte)")
                        if self._after_step("holder_precut"):
                            break
                        if self._do_wait(rep, qty, "wait_holder_precut", "holder_on_ms"):
                            break
                        # 15–18 Corte solo en lados de feed (evita pulsar el cortador vacío)
                        # force=True: no omitir Set/Res por caché HMI desfasada.
                        # Si abortamos tras Set, Res de emergencia antes del break.
                        cut_sides = CycleConfig.normalize_feed_sides(self.get_config().feed_sides)
                        if self._enter(rep, qty, "cutter_on"):
                            break
                        # C3 finish-piece: cortar aunque PF siga en ErrorState (p.ej. Buffer Max).
                        if (
                            not self._trial_mode
                            and self._use_prefeeder()
                            and not self._c3_finish_piece
                        ):
                            st = self._host.pf_state_byte()
                            if st == TX_PF_ERROR:
                                self._raise_fault("prefeeder_all_ok")
                                self._host.cycle_log("Corte abortado: PreFeeder Error")
                                break
                        self._host.cycle_log(f"Cortador ON (Set) lados={cut_sides}")
                        self._host.cmd_plc_cutters(True, sides=cut_sides, force=True)
                        if self._after_step("cutter_on"):
                            self._host.cmd_plc_cutters(False, sides=cut_sides, force=True)
                            break
                        if self._do_wait(rep, qty, "wait_cutter_pulse", "cutter_pulse_ms"):
                            self._host.cmd_plc_cutters(False, sides=cut_sides, force=True)
                            break
                        if self._enter(rep, qty, "cutter_off"):
                            self._host.cmd_plc_cutters(False, sides=cut_sides, force=True)
                            break
                        self._host.cycle_log(f"Cortador OFF (Res) lados={cut_sides}")
                        self._host.cmd_plc_cutters(False, sides=cut_sides, force=True)
                        if self._after_step("cutter_off"):
                            break
                        if self._do_wait(rep, qty, "wait_cutter_post", "cutter_post_ms"):
                            break
                        # 19–20 Depósito + delay (manguera fuera del área ANTES del prefetch)
                        # wip_pos_signed = posición firmada confirmada; wip_start_mm = |pos|
                        # (fuente soplo WIP fin; se actualiza tras despeje post-pinzas).
                        wip_pos_signed: float | None = None
                        wip_start_mm: float | None = None
                        prefetch_running = False
                        if self._enter(rep, qty, "deposit"):
                            break
                        extra = self._deposit_extra_mm(rep)
                        if abs(extra) > 0.01:
                            deposit_target = target_mm + extra
                            self._host.clear_motion_wait_flags()
                            self._host.cycle_log(
                                f"Depósito MOVE → {deposit_target:.1f} mm"
                            )
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
                            wip_pos_signed = float(deposit_target)
                            wip_start_mm = abs(wip_pos_signed)
                            self._host.cycle_log(
                                f"Depósito MOVE ok → {deposit_target:.1f} mm "
                                f"(WIP start ref {wip_start_mm:.1f})"
                            )
                        else:
                            # Sin move de depósito: ref = target lineal ya confirmado.
                            wip_pos_signed = float(target_mm)
                            wip_start_mm = abs(wip_pos_signed)
                            self._host.cycle_log(
                                f"WIP start ref=lineal_target {wip_start_mm:.1f} mm "
                                f"(sin depósito extra)"
                            )
                        if self._after_step("deposit"):
                            break
                        if self._do_wait(rep, qty, "wait_deposit_dwell", "dwell_at_dest_ms"):
                            break
                        # 21 Prefetch — inicia paralelo A (tras depósito: manguera ya movida)
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
                        # 22–23 Pinzas OFF + delay (∥ A) — Tfeed ya fue al inicio de pieza
                        if self._enter(rep, qty, "grippers_off"):
                            break
                        self._host.cmd_plc_gripper(False)
                        self._host.cycle_log("Pinzas OFF (abren)")
                        if self._after_step("grippers_off"):
                            break
                        if self._do_wait(rep, qty, "wait_gripper_release", "gripper_release_ms"):
                            break
                        # 24 Despeje ASDA tras abrir pinzas → actualiza ref WIP (soplo fin)
                        if self._enter(rep, qty, "gripper_clearance"):
                            break
                        clr = abs(float(self.get_config().gripper_clearance_mm))
                        if clr > 0.01 and wip_pos_signed is not None:
                            clearance_target = self._clearance_target_mm(
                                wip_pos_signed, float(target_mm), clr
                            )
                            self._arm_motion_leg(prefetch_running)
                            self._host.cycle_log(
                                f"Despeje MOVE → {clearance_target:.1f} mm "
                                f"(+|clearance|={clr:.1f})"
                            )
                            if not self._host.cmd_motion_move_mm(clearance_target, rpm):
                                if not self._should_abort() and not self._fault:
                                    kind = ""
                                    if hasattr(self._host, "last_move_fail_kind"):
                                        kind = self._host.last_move_fail_kind()
                                    if kind == "transport":
                                        self._raise_fault("E065")
                                    else:
                                        self._raise_fault("clearance_cmd")
                                break
                            if not self._wait_motion():
                                break
                            wip_pos_signed = float(clearance_target)
                            wip_start_mm = abs(wip_pos_signed)
                            self._host.cycle_log(
                                f"Despeje MOVE ok → {wip_pos_signed:.1f} mm; "
                                f"WIP fin ref={wip_start_mm:.1f} mm"
                            )
                        else:
                            self._host.cycle_log(
                                "Despeje post-pinzas: omitido "
                                f"(clearance={clr:.1f} mm)"
                            )
                        if self._after_step("gripper_clearance"):
                            break
                        # 25 HOME + WIP continuo (match-lineal) → 0
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
                        # 26 Handoff / join paralelo A
                        if self._enter(rep, qty, "handoff"):
                            break
                        if prefetch_running:
                            if self._wait_feed():
                                handoff_ready = True
                                self._host.cycle_log("∥ Join: prefetch listo (handoff)")
                            else:
                                handoff_ready = False
                                if self._restart_piece:
                                    break
                                if not self._fault:
                                    self._raise_fault("feed_incomplete")
                                break
                            prefetch_running = False
                        if self._after_step("handoff"):
                            break
                        # 27–28 Asentar + post-pieza
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
                        # Congelar CT al entrar a post_piece (antes holgura/Finish).
                        piece_sec = self._freeze_piece_clock()
                        # Post-pieza: re-lee holgura; si quedó ausente, fuerza Tfeed otra vez.
                        if not self._force_pf_triggers_if_no_holgura(
                            where=f"post-pieza {rep}/{qty}"
                        ):
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
                        self._log_piece_ok(rep, qty, piece_sec)
                        self._host.cycle_notify()
                        early_exit = False
                    if not early_exit:
                        break  # pieza OK → siguiente rep
                    if self._consume_restart_piece():
                        handoff_ready = False
                        prefetch_running = False
                        self._c2_laser_skip_feed = True
                        self._host.cycle_log(
                            f"C2: reinicio pieza {rep}/{qty} desde step 0"
                        )
                        continue
                    break  # fallo / abort → salir del lote
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
        self._restart_piece = False
        self._c2_laser_skip_feed = False
        self._flow_interrupt.clear()
        refill = False
        with self._lock:
            refill = self._refill_mode
            self._refill_awaiting_confirm = False
            self._refill_prompt = ""
        self._refill_confirm.clear()
        self._refill_reject.clear()
        self._refill_retry.clear()
        # Stop/C1/C2/C3 no tocan PLC. Fin de lote OK: tools safe + holder/enc.
        # Abort/Stop: dejar válvulas como estén (All Off / Reset PLC manual).
        if not (self._aborted or self._stop.is_set() or self._fault):
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
        # Fin de lote / error: desarmar PreFeeder (In process OFF → Idle).
        # Una sola armada al Start basta para 1…N piezas. No usar Stop 0x2B
        # aquí: enclava PF-007; Stop de operador / C1 ya mandaron 0x2B.
        # No cortar en seco: esperar Buffer Full + holgura (+ M2 idle).
        if self._use_prefeeder() and not (self._aborted or self._stop.is_set()):
            if ok:
                self._wait_pf_settled_before_idle(timeout_s=3.0)
            if not (self._aborted or self._stop.is_set()) and self._use_prefeeder():
                if self._host.cmd_pf_in_process(False):
                    self._host.cycle_log(
                        "PreFeeder: In process OFF → Idle"
                        + (" (post-error, desarmado)" if not ok else "")
                    )
                else:
                    self._host.cycle_log("PreFeeder: In process OFF falló")
        elapsed = 0.0
        with self._lock:
            # CT productivo: congelado en última Pieza OK. Si abort mid-pieza, corta ya.
            if self._cycle_t0 is not None:
                if self._piece_t0 is not None:
                    self._sync_pause_exclusion_locked(time.monotonic())
                    self._finished_elapsed = max(
                        0.0,
                        time.monotonic()
                        - self._cycle_t0
                        - float(self._pause_excluded_sec),
                    )
                elapsed = float(self._finished_elapsed)
                self._cycle_t0 = None
                self._piece_t0 = None
            else:
                elapsed = float(self._finished_elapsed)
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
            # Máquina/Andon → Error; PreFeeder ya quedó Idle (desarmado) arriba.
            self._set_state(TX_ERROR, self._fault or "cycle_failed")
        else:
            self._set_state(TX_FINISH)
            if refill:
                self._host.cycle_log(
                    "Refill completo"
                    + (f" en {elapsed:.0f}s" if elapsed > 0 else "")
                )
            else:
                ct_txt = f" · CT={elapsed:.1f}s" if elapsed > 0 else ""
                self._host.cycle_log(
                    f"Lote completado — {self._total_reps}/{self._total_reps} piezas"
                    + ct_txt
                )
            # Tras FinishParts: Idle máquina (PF ya Idle por In process OFF).
            self._set_state(TX_IDLE)
        self._host.cycle_notify()
