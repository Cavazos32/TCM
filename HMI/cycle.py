"""Cycle — orquestador de lote TCM (secuencia; no perfiles de Motion).
Habla con Motion / PLC / PreFeeder por TCP. Delays de secuencia son
configurables (cycle_config.json). Estado máquina bytes 0x40–0x49.
"""
from __future__ import annotations
import json
import threading
import time
from dataclasses import asdict, dataclass, fields
from datetime import datetime
from pathlib import Path
from typing import Any, Callable, Protocol
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
from error_catalog import format_ui, lookup

HMI_ROOT = Path(__file__).resolve().parent
CONFIG_PATH = HMI_ROOT / "config" / "cycle_config.json"
# Timing por operación (cmd→fin) → log de ciclo + .md de análisis.
TIMING_LOG_DIR = HMI_ROOT / "logs" / "cycle_timing"
# Hueco muerto entre fin de una acción y el cmd de la siguiente (Pause excluida).
# Los delays de proceso no se reportan aquí: solo si se pasan de cfg + slack.
TIMING_GAP_REPORT_S = 0.050
TIMING_DELAY_OVERSHOOT_MS = 15
# cmd/ACK lento (ciclo bloqueado esperando respuesta, no el movimiento).
TIMING_CMD_SLOW_S = 0.080

# Lineal producción: CMD_MOVE TCP abs(model.mm)+cutOffset vigente (carrera ABS).
# cutOffset no se congela al Start: cada lineal / depósito relee cycle_config.
# Stage2 HTTP queda para pruebas locales Motion; el ciclo no lo usa.
# El test HTML de Motion sigue usando pieceMm = L (target = L−55).
# No modificar Feed / FEED_TARGET_FIXED_MM / Move ABS manual.
# Excepción refill: skipValidate + mm opcional (Long feed). Ciclo de lote sigue en 55 mm.

# Purga: feed largo desde el prompt after_feed (Motion skipValidate).
REFILL_LONG_FEED_MM = 100.0

# WIP Delivery (HOME): soplo al volver a 0.
# start = magnitud ABS final tras depósito y despeje post-pinzas (gripperClearanceMm).
#
# WIP_BLOWER_CONTINUOUS=True (activo):
#   pinzas abiertas → MOVE→0 → delay corto → blower ON ≡ |L| (modelo).
#   El timer cubre solo la dimensión de pieza, no el HOME de batches apilados.
#   PLC apaga solo. No se espera ventana |pos|≤|L|.
# WIP_BLOWER_CONTINUOUS=False: stop–soplo–stop en fin/inicio (rollback).
WIP_BLOWER_CONTINUOUS = True
# Cada soplo (modo stop): ON(blowerSec) → dwell → OFF explícito.
# Cada soplo (modo continuo): ON(duración≡|L|) sin dwell HMI.
WIP_BLOWER_START_DELAY_S = 0.050
WIP_BLOWER_MIN_TRAVEL_MM = 8.0
# Evita move de offset trivial (misma convención firmada que cmd_motion_move_mm).
WIP_BLOWER_OFFSET_EPS_MM = 0.5
# Si hay caché ASDA post-Reached, debe coincidir con la ref de depósito.
WIP_START_CACHE_TOL_MM = 5.0
# Piso/techo duración soplo match-lineal (solo si CONTINUOUS=True).
WIP_BLOWER_LINEAL_MATCH_MIN_S = 0.2
WIP_BLOWER_LINEAL_MATCH_MAX_S = 30.0
# Piso de asiento cortador antes de MOVE depósito (PLC pulso KEEP ~100 ms
# + gap ~50 ms + retracción neumática). cutter_post_ms suele ser más corto.
CUTTER_SETTLE_BEFORE_DEPOSIT_MS = 250

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
    {"id": 25, "key": "home", "label": "HOME: MOVE→0 + delay + blower ≡ |L|", "kind": "action", "sbsPause": True},
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
    # Gap entre batches al apilar: depósito(n) = depósito(n−1) + |L| + gap.
    deposit_stack_gap_mm: float = 20.0
    # Tope carrera ASDA (mm ABS). Start rechaza si el último batch + despeje lo supera.
    deposit_max_travel_mm: float = 1500.0
    # Avance corto tras abrir pinzas (aleja mordazas de la pieza).
    # La ref WIP (soplo fin) = |depósito o Stage2| + este clearance.
    gripper_clearance_mm: float = 10.0
    cut_offset_mm: float = 0.0
    # Offset blower desde cada punta hacia el centro (mm).
    # Fin (grippers): soplo en start − offset. Inicio (cortador): soplo en +offset. Luego HOME.
    wip_blower_inicio_offset_mm: float = 8.0
    motion_wait_timeout_s: float = 25.0
    feed_wait_timeout_s: float = 15.0
    pf_ready_timeout_s: float = 15.0
    # Pieza 1 ≈ 5–6 s (incluye feed). Join prefetch no usa este tope.
    piece_watch_timeout_s: float = 20.0
    # Feed / Stage2 OM: "L" | "R" | "LR" (producción = ambos)
    feed_sides: str = "LR"
    # Tfeed entre piezas (paso 3). False = omitir siempre; solo helper holgura.
    # 1ª pieza y C2 ya omiten aunque esté True. Default ON (producción).
    pf_trigger_enabled: bool = True
    # Refill / purga: Retry = refill_mm (55). Long feed = 100 mm (skipValidate).
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
            "depositStackGapMm": "deposit_stack_gap_mm",
            "depositMaxTravelMm": "deposit_max_travel_mm",
            "gripperClearanceMm": "gripper_clearance_mm",
            "cutOffsetMm": "cut_offset_mm",
            "wipBlowerInicioOffsetMm": "wip_blower_inicio_offset_mm",
            "motionWaitTimeoutS": "motion_wait_timeout_s",
            "feedWaitTimeoutS": "feed_wait_timeout_s",
            "pfReadyTimeoutS": "pf_ready_timeout_s",
            "pieceWatchTimeoutS": "piece_watch_timeout_s",
            "feedSides": "feed_sides",
            "pfTriggerEnabled": "pf_trigger_enabled",
            "refillMm": "refill_mm",
            "refillAsdaMm": "refill_asda_mm",
        }

    @staticmethod
    def _as_bool(raw: Any, default: bool = True) -> bool:
        if raw is None:
            return default
        if isinstance(raw, str):
            return raw.strip().lower() in ("1", "true", "yes", "on")
        if isinstance(raw, (int, float)):
            return raw != 0
        return bool(raw)
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
            "deposit_stack_gap_mm",
            "deposit_max_travel_mm",
            "gripper_clearance_mm",
            "cut_offset_mm",
            "wip_blower_inicio_offset_mm",
            "motion_wait_timeout_s",
            "feed_wait_timeout_s",
            "pf_ready_timeout_s",
            "piece_watch_timeout_s",
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
                elif attr == "pf_trigger_enabled":
                    kw[attr] = cls._as_bool(raw, True)
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
                    or (
                        "depositStackGapMm" not in raw
                        and "deposit_stack_gap_mm" not in raw
                    )
                    or (
                        "depositMaxTravelMm" not in raw
                        and "deposit_max_travel_mm" not in raw
                    )
                    or (
                        "pieceWatchTimeoutS" not in raw
                        and "piece_watch_timeout_s" not in raw
                    )
                    or (
                        "pfTriggerEnabled" not in raw
                        and "pf_trigger_enabled" not in raw
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
    def pf_has_fault(self) -> bool: ...
    def pf_is_ready(self) -> bool: ...
    def pf_fault_detail(self) -> str: ...
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
    def om_official_mm(self, side: str) -> float | None: ...
    def motion_laser_on(self, side: str) -> bool: ...
    def refresh_motion_lasers(self) -> bool: ...
    def cmd_motion_move_mm(self, mm: float, rpm: float) -> bool: ...
    def last_move_fail_kind(self) -> str: ...
    def move_ack_rejected(self) -> str: ...
    def cmd_motion_move_zero(self, rpm: float) -> bool: ...
    def cmd_motion_stop(self) -> bool: ...
    def cmd_motion_feed_l(self, *, skip_validate: bool = False, feed_mm: float | None = None) -> bool: ...
    def cmd_motion_feed_r(self, *, skip_validate: bool = False, feed_mm: float | None = None) -> bool: ...
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
    def cmd_cycle_materialist(self, on: bool = True) -> dict[str, Any]: ...
    def clear_e050_latch_for_materialist(self) -> bool: ...
    def apply_detail_error(self, code_or_byte: str | int) -> bool: ...
    def error_is_latched(self) -> bool: ...

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
        self._refill_mode = False
        self._refill_awaiting_confirm = False
        self._refill_prompt = ""  # "" | after_feed | after_cut
        self._refill_confirm = threading.Event()
        self._refill_reject = threading.Event()
        self._refill_retry = threading.Event()
        self._refill_next_feed_mm: float | None = None
        self._step = 0
        self._parallel_group = ""
        self._rep = 0
        self._pieces_done = 0
        self._total_reps = 0
        self._progress = 0
        self._last_ok = False
        self._abort_needs_ack = False
        self._fault = ""
        self._fault_class = ""
        self._c3_stop_after_step = False  # legado: pausar en próximo _enter
        # C3 en lote: completar pieza en curso (hasta post_piece / corte) y pausar.
        self._c3_finish_piece = False
        self._recovery = ""  # home | restart_from_0 | retry_process | e050_materialist
        # Tras error C2/C3: lote vivo → Reset → Resume → pieza → review → purga → Continuar.
        self._recovery_after_error = False
        self._recovery_prompt = ""  # "" | review_piece | continue_cycle | e050_materialist
        self._recovery_awaiting = False
        self._recovery_confirm = threading.Event()
        self._recovery_reject = threading.Event()
        # E050 + Materialist: terminar pieza en curso si existe, HOME y Materialist.
        self._e050_finish_piece = False
        self._e050_materialist_requested = False
        self._e050_materialist_wait = False
        self._e050_normal_recovery = ""
        self._refill_skip_cut = False
        # Start / C2 / recovery: validar láser antes de alimentar (ON → omitir).
        self._restart_piece = False
        self._c2_laser_skip_feed = False
        self._c2_skip_pf_trigger = False
        self._flow_interrupt = threading.Event()
        # In process OFF por Pause/Error; Resume/Busy rearma. Evita doble OFF/ON.
        self._pf_held_idle = False
        # Resume: misma espera Buffer Full que Start (la consume el hilo de ciclo).
        self._resume_need_buffer_full = False
        self._lot_rpm = 1200.0
        self._lot_length_mm: float = 0.0
        self._last_lineal_sec: float = 0.0
        self._last_lineal_mm: float = 0.0
        # monotonic() del último Res de cortador (asiento antes de depósito).
        self._cutter_res_mono: float | None = None
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
        # Timing cmd→fin por operación (excluye Pause; mismos criterios que CT).
        self._piece_timings: list[dict[str, Any]] = []
        self._piece_metro: list[dict[str, Any]] = []
        self._lot_timing_pieces: list[dict[str, Any]] = []
        self._timing_md_path: Path | None = None
        self._timing_lot_meta: dict[str, Any] = {}
        self._timing_last_end: float | None = None
        self._timing_last_end_pause: float = 0.0
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
                "recoveryAfterError": self._recovery_after_error,
                "recoveryPrompt": (
                    self._recovery_prompt
                    if (self._active and self._recovery_prompt)
                    else ""
                ),
                "recoveryAwaitingConfirm": self._recovery_awaiting,
                "e050FinishPiece": self._e050_finish_piece,
                "e050MaterialistWait": self._e050_materialist_wait,
                "refillSkipCut": bool(self._refill_skip_cut and self._refill_mode),
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

    def is_refill_active(self) -> bool:
        with self._lock:
            return bool(self._refill_mode and self._active)

    def is_e050_materialist_wait(self) -> bool:
        with self._lock:
            return bool(self._e050_materialist_wait)

    # --- comandos máquina ---
    def request_start(self, length_mm: float, qty: int, rpm: float) -> dict[str, Any]:
        with self._lock:
            if self._materialist:
                return {
                    "ok": False,
                    "error": "Desactiva el modo Materialist para iniciar el ciclo",
                }
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
            return {
                "ok": False,
                "error": "Desactiva el modo Materialist para iniciar el ciclo",
            }
        self.reload_config()
        travel_err = self._check_deposit_travel(float(length_mm), int(qty))
        if travel_err:
            self._host.cycle_log(f"Cycle Start rechazado: {travel_err}")
            return {"ok": False, "error": travel_err}
        self._stop.clear()
        self._pause.clear()
        self._aborted = False
        self._fault = ""
        self._fault_class = ""
        self._c3_stop_after_step = False
        self._c3_finish_piece = False
        self._recovery = ""
        self._clear_recovery_gate()
        self._restart_piece = False
        self._c2_laser_skip_feed = False
        self._c2_skip_pf_trigger = False
        self._flow_interrupt.clear()
        self._e050_finish_piece = False
        self._e050_materialist_requested = False
        self._e050_materialist_wait = False
        self._e050_normal_recovery = ""
        self._refill_skip_cut = False
        self._last_ok = False
        self._abort_needs_ack = False
        self._pf_held_idle = False
        self._resume_need_buffer_full = False
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
        self._c2_skip_pf_trigger = False
        self._pause.clear()
        self._refill_reject.set()
        self._recovery_reject.set()
        self._e050_finish_piece = False
        self._e050_materialist_requested = False
        self._e050_materialist_wait = False
        self._e050_normal_recovery = ""
        with self._lock:
            self._refill_awaiting_confirm = False
            self._refill_prompt = ""
            self._recovery_awaiting = False
            self._recovery_prompt = ""
        # Desarma waits Stage2/Feed/Reached — si no, el hilo queda active ~3 min
        # y Reset responde "Detener ciclo antes de Reset".
        self._host.clear_motion_wait_flags()
        self._host.cmd_motion_stop()
        self._host.cmd_pf_stop()
        self._resume_need_buffer_full = False
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
                "error": "Purga espera confirmación (Cutting / ASDA a 0)",
            }
        if self._recovery_awaiting:
            return {
                "ok": False,
                "error": "Espera confirmación del operador (pieza / lote)",
            }
        if self._pause.is_set():
            # Tras error: Resume termina la pieza (secuencia actual), no reinicia step 0.
            if self._recovery_after_error:
                self._host.cycle_log("Cycle Resume → terminar pieza")
            elif self._recovery == "restart_from_0":
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
            # Flag antes de quitar Pause: el hilo de ciclo espera Buffer Full
            # (como Start) antes de seguir. No bloquear este request.
            self._resume_need_buffer_full = True
            self._pause.clear()
            with self._lock:
                self._sync_pause_exclusion_locked(time.monotonic())
            self._leave_pause_andon()
            self._host.cycle_notify()
            return {"ok": True}
        return {"ok": False, "error": "Ciclo no está en Pause"}

    def confirm_recovery_review(self, ok: bool = True) -> dict[str, Any]:
        """Resuelve la decisión E050 o las confirmaciones genéricas de recovery."""
        prompt = self._recovery_prompt
        if not self._recovery_awaiting or prompt not in (
            "review_piece",
            "continue_cycle",
            "e050_materialist",
        ):
            return {"ok": False, "error": "Sin confirmación de recuperación pendiente"}
        if prompt == "e050_materialist":
            if not ok:
                self._recovery_awaiting = False
                self._recovery_prompt = ""
                self._e050_materialist_requested = False
                self._e050_finish_piece = False
                self._recovery_after_error = False
                self._recovery = self._e050_normal_recovery
                self._host.cycle_log("E050: NO Materialist → recuperación normal del error")
                self.apply_error_policy("finish_step", self._fault, self._fault_class, self._e050_normal_recovery)
                self._host.cycle_notify()
                return {"ok": True, "materialist": False, "normalRecovery": True}
            if not self._host.clear_e050_latch_for_materialist():
                return {"ok": False, "error": "No se pudo limpiar E050 para iniciar Materialist"}
            self._e050_materialist_requested = True
            self._e050_finish_piece = self._piece_t0 is not None
            self._recovery_after_error = False
            self._recovery = "e050_materialist"
            self._recovery_awaiting = False
            self._recovery_prompt = ""
            self._fault = ""
            self._fault_class = ""
            self._pause.clear()
            with self._lock:
                self._sync_pause_exclusion_locked(time.monotonic())
            self._leave_pause_andon()
            self._host.cycle_log("E050: SÍ Materialist → " + ("terminar pieza actual y después HOME" if self._e050_finish_piece else "ir a HOME"))
            self._host.cycle_notify()
            return {"ok": True, "materialist": True, "finishingPiece": bool(self._e050_finish_piece)}
        if ok:
            self._recovery_confirm.set()
            self._host.cycle_log("Recovery: operador OK — " + ("continuar ciclo" if prompt == "continue_cycle" else "pieza revisada"))
        else:
            self._recovery_reject.set()
            self._host.cycle_log("Recovery: operador rechazó la etapa")
        self._host.cycle_notify()
        return {"ok": True}

    def request_refill(
        self,
        rpm: float,
        *,
        feed_mm: float | None = None,
        asda_mm: float | None = None,
    ) -> dict[str, Any]:
        """Purga/refill: ASDA park → holder+encoder → feed → (retry|cut) → home.

        Tras feed: operador Retry, Long feed (100 mm) o Next Cutting.
        Tras corte: Next Return ASDA to 0.
        Feed físico sin validación láser ni OM (skipValidate en Motion).
        Retry = refillMm (55). Long feed = 100 mm (campo mm en FEED 0x12/0x13).
        """
        with self._lock:
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
        self._refill_next_feed_mm = None
        self._refill_skip_cut = False
        self._pf_held_idle = False
        self._resume_need_buffer_full = False
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
                if self._refill_skip_cut:
                    self._host.cycle_log("Refill: Continuar → ASDA a 0")
                else:
                    self._host.cycle_log("Refill: Next → Cutting")
            else:
                self._host.cycle_log("Refill: Next → ASDA a 0")
        else:
            self._refill_reject.set()
            self._host.cycle_log("Refill: operador canceló (ASDA permanece en park)")
        self._host.cycle_notify()
        return {"ok": True}

    def retry_refill(self, feed_mm: float | None = None) -> dict[str, Any]:
        """Reintenta solo el feed (válido tras alimentar, antes del corte).

        feed_mm=None → Retry con refillMm. feed_mm=100 → Long feed.
        """
        if not self._refill_awaiting_confirm:
            return {"ok": False, "error": "Sin refill pendiente de confirmación"}
        if self._refill_prompt != "after_feed":
            return {"ok": False, "error": "Retry solo tras alimentar (antes del corte)"}
        next_mm: float | None = None
        if feed_mm is not None:
            next_mm = float(feed_mm)
            if next_mm < 1.0 or next_mm > 200.0:
                return {"ok": False, "error": "feedMm inválido"}
        self._refill_next_feed_mm = next_mm
        self._refill_retry.set()
        if next_mm is not None:
            self._host.cycle_log(f"Refill: Long feed {next_mm:g} mm")
        else:
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
        self._clear_recovery_gate()
        self._restart_piece = False
        self._c2_laser_skip_feed = False
        self._c2_skip_pf_trigger = False
        self._flow_interrupt.clear()
        self._e050_finish_piece = False
        self._refill_skip_cut = False
        self._last_ok = False
        self._abort_needs_ack = False
        self._materialist = False
        self._busy_mode = False
        self._refill_mode = False
        self._refill_awaiting_confirm = False
        self._refill_prompt = ""
        self._refill_confirm.clear()
        self._refill_reject.clear()
        self._refill_retry.clear()
        self._refill_next_feed_mm = None
        self._pieces_done = 0
        self._pf_held_idle = False
        with self._lock:
            self._reset_ct_clocks_locked()
        self._set_progress(0, 0, 0)
        self._set_state(TX_IDLE)
        self._host.cycle_log("Cycle Reset (0x043)")
        return {"ok": True}

    def _clear_recovery_gate(self) -> None:
        self._recovery_after_error = False
        self._recovery_prompt = ""
        self._recovery_awaiting = False
        self._recovery_confirm.clear()
        self._recovery_reject.clear()
        self._e050_finish_piece = False

    def _cancel_wip_blower(self) -> None:
        """Corta durationSec del blower. No es All Off ni pulso KEEP."""
        try:
            self._host.cmd_plc_blower(False)
        except Exception:
            pass

    def _arm_recovery_pause(self) -> None:
        """C2/C3: lote vivo en Pause. Resume terminará la pieza."""
        if not self.is_active():
            return
        self._recovery_after_error = True
        self._pause.set()
        with self._lock:
            if self._pause_t0 is None:
                self._pause_t0 = time.monotonic()
        self._cancel_wip_blower()

    def clear_fault_mirror(self) -> None:
        """Limpia el espejo EXXX del ciclo sin exigir lote activo.

        El flip-flop vive en ErrorPolicy; cycle._fault solo refleja para UI/snapshot.
        Si el Res limpia el latch pero deja _fault, la UI sigue en ERROR (p.ej. tras
        lote completado o Reset local de Motion).
        """
        self._fault = ""
        self._fault_class = ""
        self._recovery = ""
        # No borrar _recovery_after_error: el lote sigue en recuperación.
        self._abort_needs_ack = False

    def clear_error_for_resume(self, recovery: str = "") -> dict[str, Any]:
        """Res suave C2/C3: limpia fault; mantiene lote vivo.

        - C2 (ya en Pause): sigue en Pause → operador Resume.
        - C3 finish-piece: no pausar aún; el hilo corta y pausa en post_piece.
        """
        if not self.is_active():
            return {"ok": False, "error": "Sin ciclo activo para Resume"}
        self._cancel_wip_blower()
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
        if on and self.is_active() and not self._e050_materialist_wait:
            return {"ok": False, "error": "No Materialist con ciclo activo"}
        self._materialist = bool(on)
        self._busy_mode = False
        if self._materialist:
            self._set_state(TX_MATERIALIST)
            self._host.cycle_log("Cycle Materialist ON (0x049)")
        else:
            self._set_state(TX_BUSY if self.is_active() else TX_IDLE)
            self._host.cycle_log("Cycle Materialist OFF → " + ("Busy" if self.is_active() else "Idle"))
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

    def _use_prefeeder(self) -> bool:
        """PreFeeder participa en el ciclo si hay enlace."""
        return self._host.pf_connected()

    def _pf_idle_for_hold(self, reason: str) -> None:
        """Pause/Error máquina → In process OFF (Idle). No usa Stop 0x2B (PF-007)."""
        if not self._use_prefeeder() or self._pf_held_idle:
            return
        if self._host.cmd_pf_in_process(False):
            self._pf_held_idle = True
            self._host.cycle_log(f"PreFeeder: In process OFF → Idle ({reason})")
        else:
            self._host.cycle_log("PreFeeder: In process OFF falló")

    def _pf_rearm_in_process(self, reason: str) -> None:
        """Resume/Busy con lote vivo → rearmar In process (sensores + Tfeed)."""
        if (
            not self._use_prefeeder()
            or not self.is_active()
            or not self._pf_held_idle
            or self.is_refill_active()
        ):
            return
        if self._host.cmd_pf_in_process(True):
            self._pf_held_idle = False
            self._host.cycle_log(f"PreFeeder: In process ON ({reason})")
        else:
            self._host.cycle_log("PreFeeder: In process ON falló")

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
        if byte in (TX_PAUSE, TX_ERROR):
            self._pf_idle_for_hold("Pause" if byte == TX_PAUSE else "Error")
        elif byte == TX_BUSY:
            self._pf_rearm_in_process("Busy")

    def _enter_pause_andon(self) -> None:
        """Pausa operativa → amarillo (0x48). No pisa Error (prioridad)."""
        with self._lock:
            if self._pause_t0 is None:
                self._pause_t0 = time.monotonic()
            if self._state_byte == TX_ERROR:
                already_error = True
            else:
                already_error = False
        if already_error:
            # Error manda Andon; igual congelar PF (Idle) si aún está In process.
            self._pf_idle_for_hold("Error")
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
        self._enter_pause_andon()
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
        if not self._ensure_pf_buffer_full_after_resume():
            return True
        return self._should_abort()

    def _wait_recovery_prompt(self, prompt: str) -> str:
        """'ok' | 'reject'. Abort/Stop → 'reject'. OK: deja el prompt al caller."""
        self._recovery_confirm.clear()
        self._recovery_reject.clear()
        with self._lock:
            self._recovery_prompt = prompt
            self._recovery_awaiting = True
        self._pause.set()
        with self._lock:
            if self._pause_t0 is None:
                self._pause_t0 = time.monotonic()
        self._enter_pause_andon()
        self._host.cycle_notify()
        result = "reject"
        try:
            while True:
                if self._should_abort() or self._recovery_reject.is_set():
                    return "reject"
                if self._recovery_confirm.is_set():
                    result = "ok"
                    return "ok"
                time.sleep(0.05)
        finally:
            self._pause.clear()
            with self._lock:
                self._sync_pause_exclusion_locked(time.monotonic())
                self._recovery_awaiting = False
                if result != "ok":
                    self._recovery_prompt = ""
            self._leave_pause_andon()
            self._host.cycle_notify()

    def _wait_recovery_resume_hold(self) -> bool:
        """True = Resume (seguir). False = abortar lote."""
        if self._should_abort():
            return False
        if not self._pause.is_set():
            self._pause.set()
            with self._lock:
                if self._pause_t0 is None:
                    self._pause_t0 = time.monotonic()
        self._host.cycle_log(
            "Recovery: lote vivo — Reset → Resume para terminar la pieza"
        )
        self._host.cycle_notify()
        while True:
            if self._should_abort():
                return False
            if not self._pause.is_set():
                with self._lock:
                    self._sync_pause_exclusion_locked(time.monotonic())
                return self._ensure_pf_buffer_full_after_resume()
            time.sleep(0.05)

    def _recovery_review_purge_decide(self) -> bool:
        """Review → purga → Continuar ciclo. False solo si Stop / purga no OK."""
        self._host.cycle_log("Recovery: revisa la pieza y confirma OK")
        if self._wait_recovery_prompt("review_piece") != "ok":
            return False
        self._host.cycle_log("Recovery: purga obligatoria (secuencia refill)")
        cfg = self.get_config()
        rpm = float(self._lot_rpm or 1200.0)
        with self._lock:
            self._recovery_prompt = ""
            self._refill_mode = True
            self._refill_prompt = "working"
        self._host.cycle_notify()
        try:
            status = self._execute_refill_body(
                rpm, None, float(cfg.refill_asda_mm)
            )
        finally:
            with self._lock:
                self._refill_mode = False
                self._refill_awaiting_confirm = False
                self._refill_prompt = ""
        if status == "cancel":
            self._host.cycle_log("Recovery: purga cancelada — tools safe")
            self._host.cmd_plc_tools_safe()
            self._host.cycle_notify()
            return False
        if status != "ok":
            self._host.cycle_log("Recovery: purga incompleta")
            self._host.cycle_notify()
            return False
        if self._should_abort():
            return False
        self._host.cycle_log("Recovery: purga lista — espera Continuar ciclo")
        if self._wait_recovery_prompt("continue_cycle") != "ok":
            return False
        with self._lock:
            self._recovery_prompt = ""
        self._host.cycle_log(
            "Recovery: Continuar ciclo — siguiente pieza (validar referencia láser)"
        )
        self._host.cycle_notify()
        return True

    def _run_e050_materialist_recovery(self) -> bool:
        """E050 especial: HOME → Materialist ON → esperar Materialist OFF."""
        if self._should_abort():
            return False
        if self._e050_finish_piece:
            self._host.cycle_log("E050: pieza terminada y depositada → HOME")
        else:
            self._host.cycle_log("E050: sin pieza en curso → HOME")
            self._host.clear_motion_wait_flags()
            if not self._host.cmd_motion_move_zero(self._lot_rpm):
                if not self._fault:
                    self._raise_fault("home_cmd")
                return False
            if not self._wait_motion() or self._should_abort():
                return False
        with self._lock:
            self._e050_materialist_wait = True
            self._recovery_prompt = "e050_materialist_wait"
        self._host.cycle_log("E050: HOME OK → activar Materialist")
        self._host.cycle_notify()
        res = self._host.cmd_cycle_materialist(True)
        if not res.get("ok"):
            with self._lock:
                self._e050_materialist_wait = False
                self._recovery_prompt = ""
            self._raise_fault(str(res.get("error") or "Materialist rechazado"))
            self._host.cycle_notify()
            return False
        self._host.cycle_log("E050: Materialist activo — esperar que el operador lo apague")
        self._host.cycle_notify()
        while self._host.pf_is_materialist():
            if self._should_abort():
                return False
            time.sleep(0.05)
        with self._lock:
            self._e050_materialist_wait = False
            self._recovery_prompt = ""
            self._recovery_awaiting = False
            self._e050_materialist_requested = False
            self._e050_finish_piece = False
            self._recovery_after_error = False
            self._recovery = ""
        self._host.cycle_log("E050: Materialist OFF → continuar lote")
        self._host.cycle_notify()
        return True

    def abort_needs_ack(self) -> bool:
        """True si el último lote abortó: Res observacional no aplica."""
        return bool(self._abort_needs_ack)

    def _raise_current_pf_fault(self, log_prefix: str, *, fallback: str = "E068") -> None:
        """Set del EXXX PF actual (o fallback). Aplica C1/C2/C3."""
        detail = ""
        if hasattr(self._host, "pf_fault_detail"):
            detail = str(self._host.pf_fault_detail() or "").strip()
        self._host.cycle_log(log_prefix + (f" — {detail}" if detail else ""))
        code = detail.split(":", 1)[0].strip() if detail else ""
        if code.startswith("E") and lookup(code):
            self._raise_fault(code)
        elif detail:
            self._raise_fault(detail)
        else:
            self._raise_fault(fallback)

    def _ensure_failed_lot_latched(self) -> None:
        """Lote NO OK: el flip-flop debe existir o la HMI queda verde."""
        if hasattr(self._host, "error_is_latched") and self._host.error_is_latched():
            return
        saved = self._fault
        self._fault = ""
        code = saved.split(":", 1)[0].strip() if saved else ""
        if code.startswith("E") and lookup(code):
            self._raise_fault(code)
            return
        self._raise_fault("E068")

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

    def apply_error_policy(
        self,
        action: str,
        ui: str,
        err_class: str = "",
        recovery: str = "",
    ) -> dict[str, Any]:
        """Enter the unified ERROR hold.

        C1/C2/C3 are no longer used to select recovery. Every EXXX stops the
        sequence at a safe point and waits for RESET. A live lot may later be
        resumed through the common recovery flow.
        """
        self._fault = ui
        self._fault_class = err_class
        self._recovery = recovery
        self._c3_stop_after_step = False
        self._c3_finish_piece = False
        self._aborted = False
        self._stop.set()
        self._pause.set()
        self._resume_need_buffer_full = False
        self._clear_recovery_gate()
        try:
            self._host.clear_motion_wait_flags()
        except Exception:
            pass

        try:
            self._host.cmd_motion_stop()
        except Exception:
            pass

        self._set_state(TX_ERROR, ui)
        self._host.cycle_notify()
        return {"ok": True, "action": "error_state"}

    def apply_e050_policy(
        self,
        ui: str,
        err_class: str,
        recovery: str = "retry_process",
    ) -> dict[str, Any]:
        """E050 durante lote: primero pregunta si requiere Materialist."""
        self._fault = ui
        self._fault_class = err_class
        self._recovery = "e050_materialist"
        self._e050_normal_recovery = recovery or "retry_process"
        self._c3_stop_after_step = False
        self._c3_finish_piece = False
        self._e050_finish_piece = False
        self._e050_materialist_requested = False
        self._e050_materialist_wait = False
        self._recovery_after_error = False
        self._recovery_confirm.clear()
        self._recovery_reject.clear()
        with self._lock:
            self._recovery_prompt = "e050_materialist"
            self._recovery_awaiting = True
        if self.is_active():
            self._pause.set()
            with self._lock:
                if self._pause_t0 is None:
                    self._pause_t0 = time.monotonic()
            self._cancel_wip_blower()
        if not self.is_active():
            self._last_ok = False
        self._set_state(TX_ERROR, ui)
        self._host.cycle_log("E050: Pause — ¿Requiere Materialist?")
        self._host.cycle_notify()
        return {"ok": True, "action": "pause", "e050": True}

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
        op = self._begin_op(f"delay:{key}")
        aborted = self._pausable_delay(ms)
        sec = self._end_op(op, ok=not aborted, extra={"cfg_ms": ms, "kind": "delay"})
        if not aborted and ms > 0:
            overshoot_ms = (sec * 1000.0) - float(ms)
            if overshoot_ms > TIMING_DELAY_OVERSHOOT_MS:
                self._host.cycle_log(
                    f"Delay overshoot · {label}: {self._fmt_op(sec)} "
                    f"(cfg={ms} ms, +{overshoot_ms:.0f} ms)"
                )
            else:
                self._host.cycle_log(
                    f"Delay done · {label}: {self._fmt_op(sec)} (cfg={ms} ms)"
                )
        if aborted:
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
                if not self._ensure_pf_buffer_full_after_resume():
                    return True
                continue
            if not self._ensure_pf_buffer_full_after_resume():
                return True
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
        if not self._ensure_pf_buffer_full_after_resume():
            return True
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
        if not self._ensure_pf_buffer_full_after_resume():
            return True
        return bool(self._restart_piece)
    def _wait_motion(self) -> bool:
        """Espera Idle/Reached. El caller debe limpiar flags ANTES del comando
        (clear_motion_wait_flags / clear_motion_reached_flag) — no limpiar aquí
        o se pierde el evento si Motion responde entre el cmd y el wait."""
        cfg = self.get_config()
        limit = float(cfg.motion_wait_timeout_s)
        remain = self._piece_watch_remaining_s()
        watch = remain is not None
        if watch:
            limit = min(limit, remain)
        if limit <= 0:
            self._raise_piece_watch("timeout_motion")
            return False
        deadline = time.monotonic() + limit
        while True:
            if self._should_abort() or self._restart_piece:
                return False
            if self._pause.is_set():
                while self._pause.is_set():
                    if self._should_abort() or self._restart_piece:
                        return False
                    time.sleep(0.05)
                if not self._ensure_pf_buffer_full_after_resume():
                    return False
                remain = self._piece_watch_remaining_s()
                limit = float(cfg.motion_wait_timeout_s)
                if remain is not None:
                    limit = min(limit, remain)
                deadline = time.monotonic() + max(0.1, limit)
                continue
            if not self._ensure_pf_buffer_full_after_resume():
                return False
            if hasattr(self._host, "move_ack_rejected"):
                reject = self._host.move_ack_rejected()
                if reject:
                    if not self._should_abort() and not self._fault:
                        self._raise_fault("move_cmd")
                    self._host.cycle_log(
                        f"MOVE: ACK rechazo tardío — {reject}"
                    )
                    return False
            if self._host.wait_motion_idle_or_reached(0.1):
                return True
            if time.monotonic() >= deadline:
                break
            if watch and (self._piece_watch_remaining_s() or 0.0) <= 0:
                self._raise_piece_watch("timeout_motion")
                return False
        if watch and (self._piece_watch_remaining_s() or 0.0) <= 0:
            self._raise_piece_watch("timeout_motion")
            return False
        self._raise_fault("timeout_motion")
        self._host.cycle_log(format_ui("E008"))
        return False

    def _consume_restart_piece(self) -> bool:
        """True si C2 Resume pidió reinicio de pieza; limpia el flag."""
        if not self._restart_piece:
            return False
        self._restart_piece = False
        self._c2_skip_pf_trigger = True
        self._flow_interrupt.clear()
        return True

    def _feed_sides_laser_present(self) -> bool:
        """True si el láser de todos los lados de feed detecta material."""
        for side in self._feed_side_list():
            if not self._host.motion_laser_on(side):
                return False
        return True

    def _feed_reference_visible(self) -> bool:
        """Valida láser desde caché TCP (eventos Motion). Sin HTTP a /api/status."""
        present = self._feed_sides_laser_present()
        sides_txt = "".join(self._feed_side_list())
        self._host.cycle_log(
            f"Feed: validar referencia láser lados={sides_txt} "
            f"({'visible' if present else 'no visible'} · caché)"
        )
        return present

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
            if not self._ensure_pf_buffer_full_after_resume():
                return False
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

    def _piece_elapsed_s(self) -> float | None:
        """CT de la pieza en curso (Pause excluida). None si no hay reloj."""
        now = time.monotonic()
        with self._lock:
            if self._piece_t0 is None:
                return None
            self._sync_pause_exclusion_locked(now)
            return max(
                0.0,
                now
                - self._piece_t0
                - (float(self._pause_excluded_sec) - float(self._piece_pause_base)),
            )

    def _piece_watch_remaining_s(self) -> float | None:
        """Segundos que quedan del watchdog de pieza. None = no aplica."""
        limit = float(self.get_config().piece_watch_timeout_s)
        if limit <= 0:
            return None
        elapsed = self._piece_elapsed_s()
        if elapsed is None:
            return None
        return max(0.0, limit - elapsed)

    def _raise_piece_watch(self, slug: str) -> None:
        elapsed = self._piece_elapsed_s()
        limit = float(self.get_config().piece_watch_timeout_s)
        self._raise_fault(slug)
        self._host.cycle_log(
            f"Pieza sin avance · {float(elapsed or 0.0):.1f}s "
            f"(timeout {limit:.0f}s; pieza 1 ≈ 5–6 s)"
        )
        if slug == "timeout_motion":
            self._host.cycle_log(format_ui("E008"))
        elif slug == "timeout_feed":
            self._host.cycle_log(format_ui("E009"))

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
        self._timing_flush_piece(rep, qty, piece_sec)

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

    # --- Timing cmd→fin (log + .md) -------------------------------------------

    @staticmethod
    def _fmt_op(sec: float) -> str:
        s = max(0.0, float(sec))
        if s < 1.0:
            return f"{s * 1000.0:.0f} ms"
        return f"{s:.3f}s"

    def _begin_op(self, name: str) -> dict[str, Any]:
        now = time.monotonic()
        with self._lock:
            self._sync_pause_exclusion_locked(now)
            pause_base = float(self._pause_excluded_sec)
        last = self._timing_last_end
        if last is not None:
            gap = (now - last) - (pause_base - float(self._timing_last_end_pause))
            if gap >= TIMING_GAP_REPORT_S:
                gap_name = f"gap:before:{name}"
                self._piece_timings.append(
                    {
                        "name": gap_name,
                        "sec": gap,
                        "ok": True,
                        "kind": "gap",
                    }
                )
                self._host.cycle_log(
                    f"Hueco muerto · {name}: {self._fmt_op(gap)} "
                    f"(>{TIMING_GAP_REPORT_S * 1000.0:.0f} ms; no es delay de proceso)"
                )
        return {"name": str(name), "t0": now, "pause_base": pause_base}

    def _end_op(
        self,
        op: dict[str, Any] | None,
        *,
        ok: bool = True,
        extra: dict[str, Any] | None = None,
    ) -> float:
        if not op:
            return 0.0
        now = time.monotonic()
        with self._lock:
            self._sync_pause_exclusion_locked(now)
            pause_now = float(self._pause_excluded_sec)
            excluded = pause_now - float(op["pause_base"])
        sec = max(0.0, now - float(op["t0"]) - excluded)
        row: dict[str, Any] = {
            "name": str(op["name"]),
            "sec": sec,
            "ok": bool(ok),
        }
        if extra:
            row.update(extra)
        self._piece_timings.append(row)
        self._timing_last_end = now
        self._timing_last_end_pause = pause_now
        return sec

    def _timing_reset_piece(self) -> None:
        self._piece_timings = []
        self._piece_metro = []
        self._timing_last_end = None
        self._timing_last_end_pause = 0.0

    @staticmethod
    def _fmt_mm(v: float | None, *, signed: bool = False) -> str:
        if v is None:
            return "—"
        return f"{float(v):+.2f}" if signed else f"{float(v):.2f}"

    def _metro_snap(self, tag: str, *, target_mm: float | None = None) -> None:
        """Caché TCP ASDA/OM. No HTTP ni comando nuevo."""
        asda = None
        om_l = None
        om_r = None
        try:
            asda = self._host.asda_position_mm()
        except Exception:
            asda = None
        om_fn = getattr(self._host, "om_official_mm", None)
        if callable(om_fn):
            try:
                om_l = om_fn("L")
                om_r = om_fn("R")
            except Exception:
                om_l = None
                om_r = None
        tgt = abs(float(target_mm)) if target_mm is not None else None
        delta = None
        if asda is not None and tgt is not None:
            delta = float(asda) - tgt
        self._piece_metro.append(
            {
                "tag": str(tag),
                "asda": None if asda is None else float(asda),
                "om_l": None if om_l is None else float(om_l),
                "om_r": None if om_r is None else float(om_r),
                "target": tgt,
                "delta": delta,
            }
        )

    def _timing_open_session(
        self, *, length_mm: float, qty: int, rpm: float
    ) -> None:
        """Abre .md de análisis al inicio del lote productivo."""
        self._lot_timing_pieces = []
        self._piece_timings = []
        self._piece_metro = []
        self._timing_last_end = None
        self._timing_last_end_pause = 0.0
        self._timing_md_path = None
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        self._timing_lot_meta = {
            "started": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
            "length_mm": float(length_mm),
            "qty": int(qty),
            "rpm": float(rpm),
            "stamp": stamp,
        }
        try:
            TIMING_LOG_DIR.mkdir(parents=True, exist_ok=True)
            path = TIMING_LOG_DIR / f"cycle_{stamp}.md"
            sides = CycleConfig.normalize_feed_sides(self.get_config().feed_sides)
            lines = [
                f"# Cycle timing — {self._timing_lot_meta['started']}",
                "",
                "Duraciones **cmd → fin** (Pause excluida). Una sección por pieza.",
                "",
                "## Lote",
                "",
                f"- Archivo: `{path.name}`",
                f"- Longitud modelo: `{length_mm:.1f}` mm",
                f"- Cantidad: `{qty}`",
                f"- RPM: `{rpm:g}`",
                f"- Lados feed: `{sides}`",
                "",
                "Metrología por pieza: ASDA y OM desde caché TCP "
                "(Reached / GetMeasured). OM = feed ~55 mm, no el largo de corte.",
                "",
            ]
            path.write_text("\n".join(lines) + "\n", encoding="utf-8")
            self._timing_md_path = path
            self._host.cycle_log(f"Timing: análisis → {path}")
        except Exception as exc:
            self._timing_md_path = None
            self._host.cycle_log(f"Timing: no se pudo crear .md ({exc})")

    def _timing_append_md(self, text: str) -> None:
        path = self._timing_md_path
        if path is None:
            return
        try:
            with path.open("a", encoding="utf-8") as fh:
                fh.write(text)
                if not text.endswith("\n"):
                    fh.write("\n")
        except Exception as exc:
            self._host.cycle_log(f"Timing: error escribiendo .md ({exc})")

    def _timing_flush_piece(
        self, rep: int, qty: int, piece_sec: float
    ) -> None:
        """Log resumen + sección .md al cerrar pieza OK."""
        samples = list(self._piece_timings)
        metro = list(self._piece_metro)
        self._lot_timing_pieces.append(
            {
                "rep": int(rep),
                "piece_sec": float(piece_sec),
                "samples": samples,
                "metro": metro,
            }
        )
        if not samples and piece_sec <= 0:
            return

        # Resumen compacto en log de ciclo (ops físicas + delays).
        highlight = (
            "feed_cmd",
            "feed",
            "lineal_cmd",
            "lineal",
            "corte",
            "depósito_cmd",
            "depósito",
            "despeje_cmd",
            "despeje",
            "home_cmd",
            "home",
            "prefetch_join",
            "pf_trigger",
        )
        parts: list[str] = []
        delay_sum = 0.0
        by_name: dict[str, float] = {}
        for s in samples:
            name = str(s["name"])
            sec = float(s["sec"])
            by_name[name] = by_name.get(name, 0.0) + sec
            if name.startswith("delay:"):
                delay_sum += sec
        for key in highlight:
            if key in by_name:
                parts.append(f"{key}={self._fmt_op(by_name[key])}")
        for s in samples:
            name = str(s["name"])
            if name.startswith("gap:"):
                parts.append(f"{name}={self._fmt_op(float(s['sec']))}")
        if delay_sum > 0:
            parts.append(f"delays={self._fmt_op(delay_sum)}")
        if parts:
            self._host.cycle_log(
                f"Timing pieza {rep}/{qty}: " + " · ".join(parts)
            )

        # Markdown
        rows = [
            f"## Pieza {rep}/{qty}",
            "",
            f"- CT pieza: **{piece_sec:.3f}s**"
            if piece_sec > 0
            else "- CT pieza: *(n/d)*",
            f"- Hora: `{datetime.now().strftime('%H:%M:%S')}`",
            "",
            "| Paso | Duración | OK |",
            "|------|---------:|:--:|",
        ]
        sum_ops = 0.0
        for s in samples:
            name = str(s["name"])
            sec = float(s["sec"])
            sum_ops += sec
            mark = "✓" if s.get("ok", True) else "✗"
            rows.append(
                f"| `{name}` | {sec:.3f}s ({self._fmt_op(sec)}) | {mark} |"
            )
        rows.extend(
            [
                "",
                f"- Suma pasos registrados: **{sum_ops:.3f}s**",
                (
                    f"- CT pieza: **{piece_sec:.3f}s**"
                    if piece_sec > 0
                    else ""
                ),
                "",
            ]
        )
        anomalies = self._timing_anomaly_lines(samples)
        rows.append("### Huecos / delays fuera de spec")
        rows.append("")
        rows.append(
            "Solo si el tiempo hasta la siguiente acción supera lo establecido "
            f"(hueco ≥ {TIMING_GAP_REPORT_S * 1000.0:.0f} ms, o delay > cfg + "
            f"{TIMING_DELAY_OVERSHOOT_MS} ms). Delays de proceso en spec no se listan."
        )
        rows.append("")
        if anomalies:
            rows.extend(anomalies)
        else:
            rows.append(
                "_Ninguno — delays de proceso en spec; sin huecos muertos._"
            )
        rows.append("")
        if metro:
            rows.extend(self._timing_metro_lines(metro))
            rows.append("")
        self._timing_append_md("\n".join(rows))
        self._piece_timings = []
        self._piece_metro = []

    @staticmethod
    def _metro_pick(metro: list[dict[str, Any]], *tags: str) -> dict[str, Any] | None:
        by_tag = {str(s.get("tag") or ""): s for s in metro}
        for tag in tags:
            if tag in by_tag:
                return by_tag[tag]
        return None

    def _timing_metro_lines(self, metro: list[dict[str, Any]]) -> list[str]:
        notes = {
            "post-feed": "feed síncrono de esta pieza (~55 mm)",
            "handoff": "prefetch de esta pieza (join previo)",
            "c2-skip-feed": "C2/recovery: láser ON, sin feed nuevo",
            "start-skip-feed": "Start: láser ON, sin feed",
            "post-lineal": "ASDA vs target de corte",
            "post-depósito": "ASDA tras extra",
            "post-home": "ASDA tras HOME",
            "post-join": "feed de la *siguiente* pieza (prefetch)",
        }
        rows = [
            "### Metrología",
            "",
            "Caché TCP. `—` = aún no hubo evento. "
            "OM ≠ largo físico de la manguera.",
            "",
            "| Instante | ASDA | target | Δ ASDA | OM L | OM R | nota |",
            "|----------|-----:|-------:|-------:|-----:|-----:|------|",
        ]
        for s in metro:
            tag = str(s.get("tag") or "")
            rows.append(
                "| `{tag}` | {asda} | {tgt} | {dlt} | {ol} | {or_} | {note} |".format(
                    tag=tag,
                    asda=self._fmt_mm(s.get("asda")),
                    tgt=self._fmt_mm(s.get("target")),
                    dlt=self._fmt_mm(s.get("delta"), signed=True),
                    ol=self._fmt_mm(s.get("om_l")),
                    or_=self._fmt_mm(s.get("om_r")),
                    note=notes.get(tag, ""),
                )
            )
        return rows

    @staticmethod
    def _timing_anomaly_lines(samples: list[dict[str, Any]]) -> list[str]:
        """Huecos muertos y delays que se pasaron de cfg. El resto no se lista."""
        lines: list[str] = []
        for s in samples:
            name = str(s["name"])
            sec = float(s["sec"])
            if str(s.get("kind") or "") == "gap" or name.startswith("gap:"):
                lines.append(f"- `{name}`: **{sec:.3f}s** ({CycleRunner._fmt_op(sec)})")
                continue
            if not name.startswith("delay:"):
                continue
            cfg_ms = s.get("cfg_ms")
            if cfg_ms is None:
                continue
            overshoot_ms = (sec * 1000.0) - float(cfg_ms)
            if overshoot_ms > TIMING_DELAY_OVERSHOOT_MS:
                lines.append(
                    f"- `{name}`: **{sec:.3f}s** "
                    f"(cfg={int(cfg_ms)} ms, +{overshoot_ms:.0f} ms)"
                )
        return lines

    def _timing_close_session(self, *, ok: bool, ct_sec: float) -> None:
        """Cierra .md con tabla resumen del lote."""
        path = self._timing_md_path
        if path is None:
            return
        # Si quedó una pieza a medias sin flush (p.ej. race), volcarla.
        if self._piece_timings:
            self._timing_flush_piece(
                max(1, int(self._rep or 1)),
                max(1, int(self._total_reps or 1)),
                0.0,
            )
        pieces = list(self._lot_timing_pieces)
        keys: list[str] = []
        seen: set[str] = set()
        for p in pieces:
            for s in p.get("samples") or []:
                n = str(s["name"])
                if n not in seen:
                    seen.add(n)
                    keys.append(n)
        # Columnas prioritarias primero.
        preferred = [
            "pf_trigger",
            "feed_cmd",
            "feed",
            "grippers_on",
            "holder_off",
            "lineal_cmd",
            "lineal",
            "corte",
            "depósito_cmd",
            "depósito",
            "grippers_off",
            "despeje_cmd",
            "despeje",
            "home_cmd",
            "home",
            "prefetch_join",
        ]
        ordered = [k for k in preferred if k in seen]
        ordered.extend(
            k for k in keys if k not in ordered and not k.startswith("delay:")
        )
        ordered.extend(k for k in keys if k not in ordered)

        lines = [
            "## Resumen lote",
            "",
            f"- Resultado: **{'OK' if ok else 'NO OK'}**",
            f"- CT lote: **{ct_sec:.3f}s**" if ct_sec > 0 else "- CT lote: *(n/d)*",
            f"- Piezas con timing: **{len(pieces)}**",
            "",
        ]
        if pieces and ordered:
            header = "| Pieza | CT |" + "".join(f" {k} |" for k in ordered)
            sep = "|------:|---:|" + "".join("------:|" for _ in ordered)
            lines.extend([header, sep])
            for p in pieces:
                by_name: dict[str, float] = {}
                for s in p.get("samples") or []:
                    n = str(s["name"])
                    by_name[n] = by_name.get(n, 0.0) + float(s["sec"])
                ct = float(p.get("piece_sec") or 0.0)
                cells = [f"| {p.get('rep')} | {ct:.3f} |"]
                for k in ordered:
                    v = by_name.get(k)
                    cells.append(f" {v:.3f} |" if v is not None else " — |")
                lines.append("".join(cells))
            lines.append("")
        # Promedios de ops clave
        if pieces:
            lines.append("### Promedios (ops clave)")
            lines.append("")
            for k in preferred:
                vals = []
                for p in pieces:
                    for s in p.get("samples") or []:
                        if str(s["name"]) == k and s.get("ok", True):
                            vals.append(float(s["sec"]))
                if vals:
                    avg = sum(vals) / len(vals)
                    lines.append(
                        f"- `{k}`: avg **{avg:.3f}s** "
                        f"(n={len(vals)}, min={min(vals):.3f}, max={max(vals):.3f})"
                    )
            lines.append("")
        if pieces and any(p.get("metro") for p in pieces):
            lines.append("### Metrología lote")
            lines.append("")
            lines.append(
                "| Pieza | ASDA lineal | Δ ASDA | OM L | OM R | ASDA dep | ASDA home |"
            )
            lines.append(
                "|------:|------------:|-------:|-----:|-----:|---------:|----------:|"
            )
            for p in pieces:
                metro = list(p.get("metro") or [])
                feed = self._metro_pick(
                    metro, "post-feed", "handoff", "c2-skip-feed", "start-skip-feed"
                )
                lin = self._metro_pick(metro, "post-lineal")
                dep = self._metro_pick(metro, "post-depósito")
                home = self._metro_pick(metro, "post-home")
                lines.append(
                    "| {rep} | {asda} | {dlt} | {ol} | {or_} | {dep} | {home} |".format(
                        rep=p.get("rep"),
                        asda=self._fmt_mm((lin or {}).get("asda")),
                        dlt=self._fmt_mm((lin or {}).get("delta"), signed=True),
                        ol=self._fmt_mm((feed or {}).get("om_l")),
                        or_=self._fmt_mm((feed or {}).get("om_r")),
                        dep=self._fmt_mm((dep or {}).get("asda")),
                        home=self._fmt_mm((home or {}).get("asda")),
                    )
                )
            lines.append("")
        lines.append(f"_Generado: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}_")
        lines.append("")
        self._timing_append_md("\n".join(lines))
        self._host.cycle_log(f"Timing: guardado {path}")
        self._timing_md_path = None
        self._lot_timing_pieces = []
        self._piece_timings = []

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
        op = self._begin_op("home")
        if WIP_BLOWER_CONTINUOUS:
            ok = self._home_with_wip_delivery_continuous(
                start_mm, rpm, prefetch_running=prefetch_running
            )
        else:
            ok = self._home_with_wip_delivery_stop(
                start_mm, rpm, prefetch_running=prefetch_running
            )
        sec = self._end_op(op, ok=ok)
        self._metro_snap("post-home", target_mm=0.0)
        if ok:
            self._host.cycle_log(f"HOME/WIP total · {self._fmt_op(sec)}")
        return ok

    def _wip_blower_duration_for_piece(self, start_abs: float) -> tuple[float, str]:
        """Duración de soplo ≡ |L| modelo (no el HOME de depósito apilado).

        Escala el tiempo del lineal de corte: t = t_lineal × (|L| / carrera_lineal).
        Nunca usa start_abs (trayecto batch 2+) como numerador.
        """
        piece_mm = abs(float(self._lot_length_mm or 0.0))
        lineal_mm = abs(float(self._last_lineal_mm or 0.0))
        lineal_s = float(self._last_lineal_sec or 0.0)
        if piece_mm < 0.5:
            piece_mm = lineal_mm if lineal_mm >= 0.5 else 0.0
        if lineal_s >= WIP_BLOWER_LINEAL_MATCH_MIN_S and lineal_mm >= 0.5 and piece_mm >= 0.5:
            blower_s = lineal_s * (piece_mm / lineal_mm)
            match_src = f"pieza {piece_mm:.1f} mm"
        else:
            blower_s = float(self._host.plc_blower_sec())
            match_src = "blowerSec(fallback)"
        # No soplar más que el tiempo estimado del tramo que queda (HOME).
        travel = max(0.5, abs(float(start_abs)))
        if lineal_s >= WIP_BLOWER_LINEAL_MATCH_MIN_S and lineal_mm >= 0.5:
            home_est_s = lineal_s * (travel / lineal_mm)
            blower_s = min(blower_s, home_est_s)
        blower_s = max(
            WIP_BLOWER_LINEAL_MATCH_MIN_S,
            min(WIP_BLOWER_LINEAL_MATCH_MAX_S, blower_s),
        )
        return blower_s, match_src

    def _home_with_wip_delivery_continuous(
        self,
        start_mm: float | None,
        rpm: float,
        *,
        prefetch_running: bool,
    ) -> bool:
        """Pinzas ya abiertas: MOVE→0 → delay → blower ON ≡ |L| → Reached."""
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

        piece_mm = abs(float(self._lot_length_mm or 0.0))
        if piece_mm < 0.5:
            piece_mm = abs(float(self._last_lineal_mm or 0.0))
        blower_s, match_src = self._wip_blower_duration_for_piece(start_abs)

        self._host.cycle_log(
            f"WIP Delivery (continuo/match-pieza): desde={start_abs:.1f} mm "
            f"→ HOME=0 (blower={blower_s:.2f}s ≡ |L|={piece_mm:.1f} mm "
            f"({match_src}); no trayecto batch)"
        )

        self._arm_motion_leg(prefetch_running)
        self._host.cycle_log(
            f"WIP Delivery MOVE continuo → HOME=0.0 mm "
            f"(desde={start_abs:.1f}; blower {blower_s:.2f}s match={match_src})"
        )
        if not self._host.cmd_motion_move_zero(rpm):
            if not self._should_abort() and not self._fault:
                kind = ""
                if hasattr(self._host, "last_move_fail_kind"):
                    kind = self._host.last_move_fail_kind()
                self._raise_fault("E065" if kind == "transport" else "home_cmd")
            return False
        # Idle/Reached del despeje no debe cerrar este HOME.
        self._host.clear_motion_reached_flag()
        if not self._wait_duration_s(WIP_BLOWER_START_DELAY_S):
            return False
        pos = self._host.asda_position_mm()
        if pos is None or abs(float(pos)) > WIP_BLOWER_MIN_TRAVEL_MM:
            self._host.clear_motion_reached_flag()

        if not self._host.cmd_plc_blower(True, duration_sec=blower_s):
            self._raise_fault("wip_blower")
            self._host.cycle_log(
                f"WIP Delivery blower ON FAIL hold={blower_s:g}s"
            )
            return False
        self._host.cycle_log(
            f"WIP Delivery blower ON @ HOME en vuelo hold={blower_s:g}s "
            f"(≡ {piece_mm:.1f} mm; PLC timer + OFF al Reached)"
        )

        if not self._wait_motion():
            self._cancel_wip_blower()
            return False
        self._host.cycle_log("WIP Delivery move ok @ HOME=0.0 mm (continuo)")
        # En 0 el soplo ya no cubre pieza. Corta leftover si el timer
        # salió largo (ACK metido en t_lineal, HOME más corto que |L|).
        self._cancel_wip_blower()
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

    def _wait_feed(self, *, log_ok: bool = True, apply_piece_watch: bool = True) -> bool:
        cfg = self.get_config()
        sides = self._feed_side_list()
        timeout_s = float(cfg.feed_wait_timeout_s)
        remaining = timeout_s
        while remaining > 0:
            if self._should_abort() or self._restart_piece:
                return False
            if self._pause.is_set():
                while self._pause.is_set():
                    if self._should_abort() or self._restart_piece:
                        return False
                    time.sleep(0.05)
                if not self._ensure_pf_buffer_full_after_resume():
                    return False
                remaining = timeout_s
                # Tras error: si la referencia ya está en el láser, no esperar otro feed.
                if self._recovery_after_error and self._feed_reference_visible():
                    sides_txt = "".join(sides)
                    if log_ok:
                        self._host.cycle_log(f"Feed OK lados={sides_txt}")
                    return True
                continue
            if not self._ensure_pf_buffer_full_after_resume():
                return False
            slice_s = min(0.2, remaining)
            remain = self._piece_watch_remaining_s() if apply_piece_watch else None
            watch = remain is not None
            if watch:
                slice_s = min(slice_s, max(0.05, remain))
            outcomes = self._host.wait_feed_length_ok_for(
                sides,
                slice_s,
                abort_event=self._feed_abort_event(),  # type: ignore[arg-type]
            )
            if self._should_abort() or self._restart_piece:
                return False
            if self._pause.is_set():
                continue
            bad = [s for s, v in outcomes.items() if v != "ok"]
            if not bad:
                sides_txt = "".join(sides)
                if log_ok:
                    self._host.cycle_log(f"Feed OK lados={sides_txt}")
                return True
            if any(outcomes.get(s) == "aborted" for s in bad):
                return False
            ng_sides = [s for s in bad if outcomes.get(s) == "ng"]
            if ng_sides:
                # LengthNG ≠ E009. E009 es C1; NG es E002/E003 (C3) u otro EXXX de Feed.
                return self._fail_feed_ng(ng_sides, outcomes)
            only_timeout = bool(bad) and all(
                outcomes.get(s) == "timeout" for s in bad
            )
            if only_timeout:
                remaining -= slice_s
                if watch and (self._piece_watch_remaining_s() or 0.0) <= 0:
                    break
                continue
            for s in bad:
                detail = self._host.feed_fault_for(s) or outcomes.get(s, "fail")
                self._host.cycle_log(f"Feed {s}: {detail}")
            if self._fault:
                return False
            if watch and (self._piece_watch_remaining_s() or 0.0) <= 0:
                self._raise_piece_watch("timeout_feed")
                return False
            self._raise_fault("timeout_feed")
            self._host.cycle_log(format_ui("E009"))
            return False
        if self._should_abort() or self._restart_piece or self._pause.is_set():
            return False
        if self._fault:
            return False
        if apply_piece_watch and (self._piece_watch_remaining_s() or 1.0) <= 0:
            self._raise_piece_watch("timeout_feed")
            return False
        self._raise_fault("timeout_feed")
        self._host.cycle_log(format_ui("E009"))
        return False

    def _fail_feed_ng(self, ng_sides: list[str], outcomes: dict[str, str]) -> bool:
        """Cierra el wait por LengthNG sin pisar el EXXX real con E009 C1."""
        for s in ng_sides:
            detail = self._host.feed_fault_for(s) or outcomes.get(s, "ng")
            self._host.cycle_log(f"Feed {s}: {detail}")
        if self._fault:
            return False
        side = ng_sides[0]
        self._raise_fault("E002" if side == "L" else "E003")
        return False

    def _feed_side_list(self) -> list[str]:
        mode = CycleConfig.normalize_feed_sides(self.get_config().feed_sides)
        if mode == "L":
            return ["L"]
        if mode == "R":
            return ["R"]
        return ["L", "R"]

    def _pf_side_omits_tfeed(self, side: str) -> str | None:
        """Holgura ausente o helper/Tfeed activo: holgura gana. None = puede mandar."""
        if self._host.pf_holgura_present(side) is False:
            return "holgura"
        if self._host.pf_trigger_active(side) is True:
            return "holgura"
        return None

    def _do_pf_trigger(self, rep: int) -> bool:
        """Tfeed a lados de feedSides. True = OK / omitido; False = fault.

        Contrato Doc/pf_trigger.md: omite si pfTriggerEnabled=False, 1ª pieza
        o C2; holgura ausente/activa gana.
        """
        c2_skip = self._c2_skip_pf_trigger
        self._c2_skip_pf_trigger = False
        if not self.get_config().pf_trigger_enabled:
            self._host.cycle_log(
                "trigger PreFeeder: omitido (deshabilitado — solo holgura)"
            )
            return True
        if not self._use_prefeeder():
            self._host.cycle_log("trigger PreFeeder: omitido (sin enlace)")
            return True
        if int(rep) <= 1:
            self._host.cycle_log(
                "trigger PreFeeder: omitido (1ª pieza — feed de referencia)"
            )
            return True
        if c2_skip:
            self._host.cycle_log(
                "trigger PreFeeder: omitido (C2 — Tfeed ya mandado)"
            )
            return True
        sides = CycleConfig.normalize_feed_sides(self.get_config().feed_sides)
        want_r = "R" in sides
        want_l = "L" in sides
        why_r = self._pf_side_omits_tfeed("R") if want_r else "lado"
        why_l = self._pf_side_omits_tfeed("L") if want_l else "lado"
        send_r = want_r and why_r is None
        send_l = want_l and why_l is None
        if not send_r and not send_l:
            bits = []
            if want_r:
                bits.append(f"R={why_r}")
            if want_l:
                bits.append(f"L={why_l}")
            self._host.cycle_log(
                "trigger PreFeeder: omitido (" + ", ".join(bits) + ")"
            )
            return True
        op = self._begin_op("pf_trigger")
        ok_r = self._host.cmd_pf_trigger_r() if send_r else True
        ok_l = self._host.cmd_pf_trigger_l() if send_l else True

        def _txt(want: bool, send: bool, ok: bool, why: str | None) -> str:
            if not want:
                return "omit"
            if not send:
                return f"omit({why})"
            return "ok" if ok else "fail"

        r_txt = _txt(want_r, send_r, ok_r, why_r)
        l_txt = _txt(want_l, send_l, ok_l, why_l)
        if (send_r and not ok_r) or (send_l and not ok_l):
            self._end_op(op, ok=False)
            self._raise_fault("pf_trigger")
            self._host.cycle_log(
                f"trigger PreFeeder falló lados={sides} "
                f"R(0x4C)={r_txt} L(0x51)={l_txt}"
            )
            return False
        sec = self._end_op(op, ok=True)
        self._host.cycle_log(
            f"trigger PreFeeder Tfeed lados={sides} — "
            f"R(0x4C)={r_txt} L(0x51)={l_txt} · {self._fmt_op(sec)}"
        )
        return True

    def _pf_side_buffer_full(self, side: str) -> tuple[bool, str]:
        """True si el lado ya publicó Buffer Full (sensor home ON)."""
        full = self._host.pf_buffer_full(side)
        if full is True:
            return True, f"{side}:Full"
        if full is False:
            return False, f"{side}:buffer≠Full"
        return False, f"{side}:buffer=?"

    def _wait_pf_buffer_full_on_start(
        self, timeout_s: float, *, reason: str = "Start"
    ) -> bool:
        """Buffer Full confirmado (Start / Resume) antes de producir.

        Ya Full → sale al primer tick. Vacío → espera relleno. Stop aborta.
        Timeout o EXXX PF (E052/E058) fallan el lote; no se alimenta a ciegas.
        """
        sides = self._feed_side_list()
        self._host.pf_request_status()
        deadline = time.monotonic() + max(0.0, float(timeout_s))
        last_why = ""
        while True:
            if self._should_abort():
                return False
            if self._host.pf_has_fault():
                self._raise_current_pf_fault(
                    "PreFeeder: EXXX durante espera Buffer Full",
                    fallback="prefeeder_all_ok",
                )
                return False
            reasons: list[str] = []
            all_ok = True
            for s in sides:
                ok, why = self._pf_side_buffer_full(s)
                if not ok:
                    all_ok = False
                reasons.append(why)
            why_txt = " ".join(reasons)
            if all_ok:
                self._host.cycle_log(
                    f"PreFeeder: Buffer Full confirmado · {why_txt}"
                )
                return True
            if why_txt != last_why:
                self._host.cycle_log(
                    f"PreFeeder: espera Buffer Full ({reason}) · {why_txt}"
                )
                last_why = why_txt
            if time.monotonic() >= deadline:
                missing = [
                    s for s in sides if self._host.pf_buffer_full(s) is not True
                ]
                self._host.cycle_log(
                    f"PreFeeder: timeout {timeout_s:.1f}s Buffer Full ({why_txt})"
                )
                if "L" in missing:
                    self._raise_fault("E058")
                elif "R" in missing:
                    self._raise_fault("E052")
                else:
                    self._raise_fault("prefeeder_all_ok")
                return False
            self._host.pf_request_status()
            time.sleep(0.1)

    def _ensure_pf_buffer_full_after_resume(self) -> bool:
        """Tras Resume: misma espera Buffer Full que Start. False = abort/fault.

        request_resume no bloquea: deja el flag y rearma In process (Busy).
        Si el hilo de ciclo gana la carrera, rearma aquí antes de esperar.
        La espera no suma a CT (Start también es prep fuera de reloj).
        """
        with self._lock:
            if not self._resume_need_buffer_full:
                return True
            self._resume_need_buffer_full = False
        if not self._use_prefeeder():
            return True
        if self._pf_held_idle:
            self._pf_rearm_in_process("Resume")
        t0 = time.monotonic()
        ok = self._wait_pf_buffer_full_on_start(
            timeout_s=float(self.get_config().pf_ready_timeout_s),
            reason="Resume",
        )
        with self._lock:
            if self._cycle_t0 is not None:
                self._pause_excluded_sec += max(0.0, time.monotonic() - t0)
        return ok

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
        cmd_op = self._begin_op("lineal_cmd")
        sent = self._host.cmd_motion_move_mm(abs_mm, self._lot_rpm)
        cmd_sec = self._end_op(cmd_op, ok=sent)
        if cmd_sec >= TIMING_CMD_SLOW_S:
            self._host.cycle_log(
                f"Lineal MOVE TCP: ACK/cmd lento {self._fmt_op(cmd_sec)} "
                f"(ciclo bloqueado esperando respuesta, no el avance)"
            )
        if not sent:
            if not self._should_abort() and not self._fault:
                kind = ""
                if hasattr(self._host, "last_move_fail_kind"):
                    kind = self._host.last_move_fail_kind()
                self._raise_fault("E065" if kind == "transport" else "move_cmd")
            self._host.cycle_log("Lineal MOVE TCP: comando rechazado")
            self._last_lineal_sec = 0.0
            self._last_lineal_mm = 0.0
            return False
        op = self._begin_op("lineal")
        if not self._wait_motion():
            self._end_op(op, ok=False)
            self._metro_snap("post-lineal", target_mm=abs_mm)
            self._last_lineal_sec = 0.0
            self._last_lineal_mm = 0.0
            return False
        # Solo el avance (Reached). El ACK/cmd no es recorrido: si entra
        # aquí, el soplo HOME dura de más y el blower sigue ON en la pieza.
        sec = self._end_op(op, ok=True)
        self._last_lineal_sec = sec
        self._last_lineal_mm = abs_mm
        self._metro_snap("post-lineal", target_mm=abs_mm)
        self._host.cycle_log(
            f"Lineal MOVE TCP OK targetAbsMm={abs_mm:.1f} rpm={self._lot_rpm:g}"
            f" · move={self._fmt_op(sec)} cmd={self._fmt_op(cmd_sec)}"
        )
        return not self._should_abort()

    def _effective_mm(self, length_mm: float) -> float:
        """|L| + cutOffset actual (get_config, no snapshot de Start)."""
        cfg = self.get_config()
        return abs(float(length_mm)) + float(cfg.cut_offset_mm)

    def _live_cut_target_mm(self) -> float:
        """Target lineal del lote: longitud de pieza + offset HMI vigente."""
        return self._effective_mm(self._lot_length_mm)

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
    def _run_feed(self, *, skip_validate: bool = False, feed_mm: float | None = None) -> bool:
        self._host.clear_motion_wait_flags()
        sides = self._feed_side_list()
        cmd_op = self._begin_op("feed_cmd")
        failed: list[str] = []
        if "L" in sides:
            if not self._host.cmd_motion_feed_l(
                skip_validate=skip_validate, feed_mm=feed_mm
            ):
                failed.append("L")
        if "R" in sides:
            if not self._host.cmd_motion_feed_r(
                skip_validate=skip_validate, feed_mm=feed_mm
            ):
                failed.append("R")
        ok_all = not failed
        cmd_sec = self._end_op(cmd_op, ok=ok_all)
        note = " (purga: sin láser/OM)" if skip_validate else ""
        self._host.cycle_log(f"Feed start lados={''.join(sides)}{note}")
        if cmd_sec >= TIMING_CMD_SLOW_S:
            self._host.cycle_log(
                f"Feed: cmd TCP lento {self._fmt_op(cmd_sec)} "
                f"(ciclo bloqueado en send, no en el servo)"
            )
        if failed:
            self._host.cycle_log(f"Feed: start falló lados={''.join(failed)}")
            self._raise_fault("feed_cmd")
            return False
        op = self._begin_op("feed")
        if not self._wait_feed(log_ok=False):
            self._end_op(op, ok=False)
            self._metro_snap("post-feed")
            return False
        sec = self._end_op(op, ok=True)
        self._metro_snap("post-feed")
        self._host.cycle_log(
            f"Feed OK lados={''.join(sides)} · wait={self._fmt_op(sec)} "
            f"cmd={self._fmt_op(cmd_sec)}"
        )
        return True

    def _wait_refill_operator_decision(self, prompt: str) -> str:
        """'ok' | 'reject' | 'retry'. Abort/Stop → 'reject'.

        prompt: after_feed (Retry / Long feed / Next Cutting) | after_cut (Next ASDA 0).
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
            if self._refill_skip_cut:
                self._host.cycle_log(
                    f"Refill: feed listo — Retry, Long feed {REFILL_LONG_FEED_MM:g} mm "
                    "o Continuar → ASDA a 0"
                )
            else:
                self._host.cycle_log(
                    f"Refill: feed listo — Retry, Long feed {REFILL_LONG_FEED_MM:g} mm "
                    "o Next → Cutting"
                )
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

    def _execute_refill_body(
        self,
        rpm: float,
        feed_mm: float | None,
        asda_mm: float,
        *,
        skip_cut: bool = False,
    ) -> str:
        """Purga existente. 'ok' | 'cancel' | 'fail'. No cierra el lote.

        skip_cut: E050 vaciar — feed + ASDA 0, sin pulso de corte.
        """
        self._refill_skip_cut = bool(skip_cut)
        cfg = self.get_config()
        self._host.cycle_log(
            f"Refill: tools safe + ASDA → {asda_mm:g} mm (área libre)"
        )
        self._host.cmd_plc_tools_safe()
        if self._should_abort():
            return "fail"
        self._host.clear_motion_wait_flags()
        if not self._host.cmd_motion_move_mm(asda_mm, rpm):
            self._raise_fault("move_cmd")
            return "fail"
        if not self._wait_motion() or self._should_abort():
            return "fail"
        with self._lock:
            self._progress = 20

        self._host.cycle_log("Refill: Holder+Encoder ON")
        self._arm_holder_encoder()
        time.sleep(max(0, cfg.holder_on_ms) / 1000.0)
        if self._should_abort():
            return "fail"
        with self._lock:
            self._progress = 35

        use_feed: float | None = feed_mm
        while True:
            shown = float(use_feed) if use_feed is not None else 55.0
            self._host.cycle_log(
                f"Refill: alimentar {shown:g} mm (sin validación láser/OM)"
            )
            if not self._run_feed(skip_validate=True, feed_mm=use_feed):
                return "fail"
            with self._lock:
                self._progress = 55
            if self._should_abort():
                return "fail"

            decision = self._wait_refill_operator_decision("after_feed")
            if decision == "ok":
                break
            if decision == "retry":
                use_feed = self._refill_next_feed_mm
                self._refill_next_feed_mm = None
                shown = float(use_feed) if use_feed is not None else 55.0
                self._host.cycle_log(
                    f"Refill: reintento — feed {shown:g} mm (ASDA en park)"
                )
                with self._lock:
                    self._progress = 35
                continue
            return "cancel"

        if not skip_cut:
            if not self._run_cutter_pulse():
                return "fail"
            with self._lock:
                self._progress = 75

            decision = self._wait_refill_operator_decision("after_cut")
            if decision != "ok":
                return "cancel"

        self._host.cycle_log("Refill: ASDA → 0")
        self._host.clear_motion_wait_flags()
        if not self._host.cmd_motion_move_zero(rpm):
            self._raise_fault("home_cmd")
            return "fail"
        if not self._wait_motion() or self._should_abort():
            return "fail"
        with self._lock:
            self._progress = 100
        self._host.cycle_log("Refill OK — ASDA en 0")
        return "ok"

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
            status = self._execute_refill_body(rpm, feed_mm, asda_mm)
            if status == "ok":
                with self._lock:
                    self._pieces_done = 1
                self._finish(True)
                return
            if status == "cancel":
                self._refill_cancel_park()
                return
            self._finish(False)
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

    def _deposit_batch_index(self, rep: int) -> int:
        batch = max(1, int(self.get_config().deposit_batch_size))
        return (max(1, int(rep)) - 1) // batch + 1

    def _deposit_extra_mm(self, rep: int) -> float:
        """Offset respecto a target_mm (corte). Apila batches en la misma dirección.

        batch 1: depositExtraMm
        batch n: depositExtraMm + (n−1) × (|L| + depositStackGapMm)
        Sin flip +/−. Todas las piezas del mismo batch comparten el punto.
        """
        cfg = self.get_config()
        n = self._deposit_batch_index(rep)
        L = abs(float(self._lot_length_mm))
        gap = max(0.0, float(cfg.deposit_stack_gap_mm))
        return float(cfg.deposit_extra_mm) + (n - 1) * (L + gap)

    def _deposit_target_mm(self, rep: int, target_mm: float) -> float:
        return float(target_mm) + self._deposit_extra_mm(rep)

    def _mark_cutter_res(self) -> None:
        self._cutter_res_mono = time.monotonic()

    def _ensure_cutter_settled_before_deposit(self, cut_sides: str) -> bool:
        """True = abort. Reafirma Res y espera asiento mín. antes de MOVE depósito.

        Evita arrancar el lineal de depósito con el cortador aún en carrera
        (Res TCP es fire-and-forget; cutter_post_ms a menudo << pulso PLC).
        """
        need_ms = max(
            int(self.get_config().cutter_post_ms or 0),
            int(CUTTER_SETTLE_BEFORE_DEPOSIT_MS),
        )
        # Reafirma OFF: el PLC solo pulsa si el KEEP lógico seguía ON.
        self._host.cmd_plc_cutters(False, sides=cut_sides, force=True)
        now = time.monotonic()
        if self._cutter_res_mono is None:
            self._cutter_res_mono = now
            remain = need_ms
        else:
            remain = int(
                max(0.0, need_ms - (now - self._cutter_res_mono) * 1000.0)
            )
        if remain > 0:
            self._host.cycle_log(
                f"Depósito: espera asiento cortador {remain} ms "
                f"(mín {need_ms} ms desde Res)"
            )
            op = self._begin_op("cutter_settle")
            aborted = self._pausable_delay(remain)
            self._end_op(op, ok=not aborted)
            if aborted:
                return True
        else:
            self._host.cycle_log(
                f"Depósito: cortador asentado (ya ≥{need_ms} ms desde Res)"
            )
        return False

    def _check_deposit_travel(self, length_mm: float, qty: int) -> str | None:
        """None si cabe en carrera; mensaje de error si el último batch + despeje > máx."""
        cfg = self.get_config()
        L = abs(float(length_mm))
        qty_i = max(1, int(qty))
        batch = max(1, int(cfg.deposit_batch_size))
        n_batches = (qty_i + batch - 1) // batch
        target_mm = L + float(cfg.cut_offset_mm)
        gap = max(0.0, float(cfg.deposit_stack_gap_mm))
        extra0 = float(cfg.deposit_extra_mm)
        clearance = abs(float(cfg.gripper_clearance_mm))
        max_travel = abs(float(cfg.deposit_max_travel_mm))
        if max_travel <= 0:
            return None
        # Lineal de corte.
        if abs(target_mm) > max_travel + 1e-6:
            return (
                f"Lineal {abs(target_mm):.1f} mm > carrera máx "
                f"{max_travel:g} mm"
            )
        last_extra = extra0 + (n_batches - 1) * (L + gap)
        last_deposit = target_mm + last_extra
        reach = abs(last_deposit) + clearance
        if reach > max_travel + 1e-6:
            # Máx. batches que caben.
            step = L + gap
            if step <= 1e-9:
                max_batches = 1 if abs(target_mm + extra0) + clearance <= max_travel else 0
            else:
                # abs(target+extra0+(n-1)*step)+clearance <= max
                # Con stacking típico (deposit positivo): target+extra0+(n-1)*step + clearance <= max
                base = target_mm + extra0
                if abs(base) + clearance > max_travel + 1e-6:
                    max_batches = 0
                elif base >= 0:
                    max_batches = 1 + int(max(0.0, max_travel - clearance - base) // step)
                else:
                    # Extra negativo: al apilar hacia +L se acerca a 0; evaluar n=1..
                    max_batches = 0
                    for n in range(1, n_batches + 1):
                        d = base + (n - 1) * step
                        if abs(d) + clearance <= max_travel + 1e-6:
                            max_batches = n
                        else:
                            break
            max_pcs = max_batches * batch
            return (
                f"Depósito fuera de carrera: {n_batches} batch(es) -> "
                f"alcance {reach:.1f} mm (depósito {last_deposit:.1f} + "
                f"despeje {clearance:g}) > máx {max_travel:g} mm. "
                f"Máximo ~{max_batches} batch(es) / {max_pcs} piezas "
                f"con L={L:g}, gap={gap:g}, extra={extra0:g}"
            )
        return None

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
            self._lot_length_mm = abs(float(length_mm))
            self._last_lineal_sec = 0.0
            self._last_lineal_mm = 0.0
            self._started_at = time.monotonic()
            self._reset_ct_clocks_locked()
        self._host.cycle_notify()
        try:
            if not self._prepare_before_cut():
                self._finish(False)
                return
            self._timing_open_session(
                length_mm=length_mm, qty=qty, rpm=rpm
            )
            # Armar PreFeeder: Start + esperar listo (enlace + sin EXXX).
            # ErrorState 0x3C sin errorAny/EXXX no bloquea (HMI vs HTML desfasados).
            if self._use_prefeeder():
                self._host.cmd_pf_start()
                self._host.pf_request_status()
                timeout_s = float(self.get_config().pf_ready_timeout_s)
                t0 = time.monotonic()
                ready = False
                while time.monotonic() - t0 < timeout_s:
                    if self._should_abort():
                        self._finish(False)
                        return
                    if self._host.pf_is_ready():
                        ready = True
                        break
                    time.sleep(0.1)
                if not ready:
                    if self._host.pf_has_fault():
                        self._raise_current_pf_fault(
                            f"PreFeeder: timeout {timeout_s:.0f}s con EXXX activo",
                            fallback="prefeeder_all_ok",
                        )
                    else:
                        self._raise_fault("prefeeder_all_ok")
                        self._host.cycle_log(
                            f"PreFeeder: timeout {timeout_s:.0f}s sin estado listo"
                        )
                    self._finish(False)
                    return
                # Busy máquina → In process en Master/L+R (arma sensores + acepta Tfeed).
                # Sin esto, PF_LR NACK el trigger TCP (sensorsMotionArmed=false).
                if self._host.cmd_pf_in_process(True):
                    self._pf_held_idle = False
                    self._host.cycle_log("PreFeeder: In process ON (ciclo Busy)")
                else:
                    self._host.cycle_log("PreFeeder: In process ON falló")
                # Start y Resume: no alimentar hasta Buffer Full confirmado.
                # Si ya está Full (lote previo settled), sale al primer tick.
                if not self._wait_pf_buffer_full_on_start(
                    timeout_s=float(self.get_config().pf_ready_timeout_s)
                ):
                    self._finish(False)
                    return
            target_mm = 0.0
            completed = 0
            handoff_ready = False
            prefetch_running = False
            for rep in range(1, qty + 1):
                while True:
                    early_exit = True
                    materialist_only = False
                    for _piece_attempt in (0,):
                        if self._gate("listo" if rep == 1 else "rep-start"):
                            break
                        if self._e050_materialist_requested and not self._e050_finish_piece:
                            if not self._run_e050_materialist_recovery():
                                early_exit = True
                                break
                            materialist_only = True
                            early_exit = True
                            break
                        self._set_progress(rep, 0, qty)
                        self._last_lineal_sec = 0.0
                        self._last_lineal_mm = 0.0
                        self._mark_piece_clock_start()
                        self._timing_reset_piece()
                        self._cutter_res_mono = None
                        # 1–2 Holder+Encoder ON + delay (solo 1ª; se mantienen el lote)
                        if rep == 1:
                            if self._enter(rep, qty, "holder_on"):
                                break
                            hold_op = self._begin_op("holder_on")
                            self._arm_holder_encoder()
                            self._end_op(hold_op)
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
                        if not self._do_pf_trigger(rep):
                            break
                        if self._after_step("pf_trigger"):
                            break
                        # 4 Feed / handoff
                        if self._enter(rep, qty, "feed"):
                            break
                        if handoff_ready:
                            handoff_ready = False
                            self._c2_laser_skip_feed = False
                            self._metro_snap("handoff")
                            self._host.cycle_log("Feed: handoff (prefetch ya listo)")
                        elif (
                            (
                                rep == 1
                                or self._c2_laser_skip_feed
                                or self._recovery_after_error
                            )
                            and self._feed_reference_visible()
                        ):
                            c2_or_rec = (
                                self._c2_laser_skip_feed or self._recovery_after_error
                            )
                            self._c2_laser_skip_feed = False
                            sides_txt = "".join(self._feed_side_list())
                            if c2_or_rec:
                                self._metro_snap("c2-skip-feed")
                                self._host.cycle_log(
                                    f"Feed omitido (C2/recovery): láser ya ON "
                                    f"lados={sides_txt}"
                                )
                            else:
                                self._metro_snap("start-skip-feed")
                                self._host.cycle_log(
                                    f"Feed omitido (referencia láser visible) "
                                    f"lados={sides_txt}"
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
                        grip_op = self._begin_op("grippers_on")
                        self._host.cmd_plc_gripper(True)
                        self._end_op(grip_op)
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
                        hold_off_op = self._begin_op("holder_off")
                        self._open_holder_encoder()
                        self._end_op(hold_off_op)
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
                        # Tras _enter (pausa SBS): releer offset/L para no usar el del Start.
                        target_mm = self._live_cut_target_mm()
                        abs_target_mm = abs(float(target_mm))
                        cut_off = abs_target_mm - abs(float(self._lot_length_mm))
                        sides = CycleConfig.normalize_feed_sides(self.get_config().feed_sides)
                        self._host.cycle_log(
                            f"Lineal MOVE TCP modelMm={length_mm:.1f} "
                            f"cutOffset={cut_off:.1f}→targetAbsMm={abs_target_mm:.1f} "
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
                        precut_op = self._begin_op("holder_precut")
                        self._arm_holder_encoder()
                        self._end_op(precut_op)
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
                        # Solo actuar con EXXX/errorAny real — no por 0x3C huérfano.
                        # Set del EXXX aquí: un break mudo dejaba Andon en Error y HMI verde.
                        if (
                            self._use_prefeeder()
                            and not self._c3_finish_piece
                            and self._host.pf_has_fault()
                        ):
                            self._raise_current_pf_fault("Corte: PreFeeder en error")
                            if self._c3_finish_piece:
                                self._host.cycle_log(
                                    "C3: completar corte pese a EXXX PF"
                                )
                            elif self._should_abort():
                                self._host.cycle_log("Corte abortado: PreFeeder Error")
                                break
                            elif self._pause.is_set():
                                if self._wait_paused_for_resume(
                                    "C2: Pause por EXXX PF antes del corte"
                                ):
                                    break
                                break
                            else:
                                self._host.cycle_log("Corte abortado: PreFeeder Error")
                                break
                        cut_op = self._begin_op("corte")
                        self._host.cycle_log(f"Cortador ON (Set) lados={cut_sides}")
                        self._host.cmd_plc_cutters(True, sides=cut_sides, force=True)
                        if self._after_step("cutter_on"):
                            self._end_op(cut_op, ok=False)
                            self._host.cmd_plc_cutters(False, sides=cut_sides, force=True)
                            break
                        if self._do_wait(rep, qty, "wait_cutter_pulse", "cutter_pulse_ms"):
                            self._end_op(cut_op, ok=False)
                            self._host.cmd_plc_cutters(False, sides=cut_sides, force=True)
                            break
                        if self._enter(rep, qty, "cutter_off"):
                            self._end_op(cut_op, ok=False)
                            self._host.cmd_plc_cutters(False, sides=cut_sides, force=True)
                            break
                        self._host.cmd_plc_cutters(False, sides=cut_sides, force=True)
                        self._mark_cutter_res()
                        cut_sec = self._end_op(cut_op, ok=True)
                        self._host.cycle_log(
                            f"Cortador OFF (Res) lados={cut_sides}"
                            f" · {self._fmt_op(cut_sec)}"
                        )
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
                            batch_n = self._deposit_batch_index(rep)
                            # No mover ASDA hasta cortador asentado (Res + piso 250 ms).
                            if self._ensure_cutter_settled_before_deposit(cut_sides):
                                break
                            self._host.clear_motion_wait_flags()
                            self._host.cycle_log(
                                f"Depósito MOVE → {deposit_target:.1f} mm "
                                f"(batch {batch_n}, extra={extra:.1f})"
                            )
                            dep_cmd = self._begin_op("depósito_cmd")
                            dep_sent = self._host.cmd_motion_move_mm(deposit_target, rpm)
                            dep_cmd_sec = self._end_op(dep_cmd, ok=dep_sent)
                            if dep_cmd_sec >= TIMING_CMD_SLOW_S:
                                self._host.cycle_log(
                                    f"Depósito MOVE: ACK/cmd lento {self._fmt_op(dep_cmd_sec)}"
                                )
                            if not dep_sent:
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
                            dep_op = self._begin_op("depósito")
                            if not self._wait_motion():
                                self._end_op(dep_op, ok=False)
                                self._metro_snap(
                                    "post-depósito", target_mm=deposit_target
                                )
                                break
                            wip_pos_signed = float(deposit_target)
                            wip_start_mm = abs(wip_pos_signed)
                            dep_sec = self._end_op(dep_op, ok=True)
                            self._metro_snap("post-depósito", target_mm=deposit_target)
                            self._host.cycle_log(
                                f"Depósito MOVE ok → {deposit_target:.1f} mm "
                                f"(WIP start ref {wip_start_mm:.1f})"
                                f" · move={self._fmt_op(dep_sec)} "
                                f"cmd={self._fmt_op(dep_cmd_sec)}"
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
                            failed: list[str] = []
                            if "L" in sides and not self._host.cmd_motion_feed_l():
                                failed.append("L")
                            if "R" in sides and not self._host.cmd_motion_feed_r():
                                failed.append("R")
                            if failed:
                                self._host.cycle_log(
                                    f"∥ Prefetch: start falló lados={''.join(failed)}"
                                )
                                self._raise_fault("feed_cmd")
                                break
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
                        grip_off_op = self._begin_op("grippers_off")
                        self._host.cmd_plc_gripper(False)
                        self._end_op(grip_off_op)
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
                            clr_cmd = self._begin_op("despeje_cmd")
                            clr_sent = self._host.cmd_motion_move_mm(clearance_target, rpm)
                            clr_cmd_sec = self._end_op(clr_cmd, ok=clr_sent)
                            if clr_cmd_sec >= TIMING_CMD_SLOW_S:
                                self._host.cycle_log(
                                    f"Despeje MOVE: ACK/cmd lento {self._fmt_op(clr_cmd_sec)}"
                                )
                            if not clr_sent:
                                if not self._should_abort() and not self._fault:
                                    kind = ""
                                    if hasattr(self._host, "last_move_fail_kind"):
                                        kind = self._host.last_move_fail_kind()
                                    if kind == "transport":
                                        self._raise_fault("E065")
                                    else:
                                        self._raise_fault("clearance_cmd")
                                break
                            clr_op = self._begin_op("despeje")
                            if not self._wait_motion():
                                self._end_op(clr_op, ok=False)
                                break
                            wip_pos_signed = float(clearance_target)
                            wip_start_mm = abs(wip_pos_signed)
                            clr_sec = self._end_op(clr_op, ok=True)
                            self._host.cycle_log(
                                f"Despeje MOVE ok → {wip_pos_signed:.1f} mm; "
                                f"WIP fin ref={wip_start_mm:.1f} mm"
                                f" · move={self._fmt_op(clr_sec)} "
                                f"cmd={self._fmt_op(clr_cmd_sec)}"
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
                            join_op = self._begin_op("prefetch_join")
                            # El reloj de esta pieza ya cerró el trabajo (HOME).
                            # No cortar el feed de la siguiente con el watchdog de 1ª.
                            if self._wait_feed(log_ok=False, apply_piece_watch=False):
                                join_sec = self._end_op(join_op, ok=True)
                                handoff_ready = True
                                self._metro_snap("post-join")
                                self._host.cycle_log(
                                    f"∥ Join: prefetch listo (handoff)"
                                    f" · {self._fmt_op(join_sec)}"
                                )
                            else:
                                self._end_op(join_op, ok=False)
                                self._metro_snap("post-join")
                                handoff_ready = False
                                if self._restart_piece:
                                    break
                                # C3: pieza ya cortada — no abortar; ir a post_piece → Pause.
                                # Abortar aquí hacía break del while y el for seguía
                                # disparando Tfeed/Feed en las piezas restantes.
                                if self._c3_finish_piece:
                                    self._host.cycle_log(
                                        "∥ Join: prefetch falló — C3 sigue a "
                                        "post_piece (Pause; Reset→Resume)"
                                    )
                                else:
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
                        # Congelar CT al entrar a post_piece (antes Finish).
                        piece_sec = self._freeze_piece_clock()
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
                        if self._e050_finish_piece and self._e050_materialist_requested:
                            if not self._run_e050_materialist_recovery():
                                early_exit = True
                                break
                            handoff_ready = False
                            prefetch_running = False
                            self._c2_laser_skip_feed = True
                            self._c2_skip_pf_trigger = False
                        elif self._recovery_after_error:
                            if not self._recovery_review_purge_decide():
                                early_exit = True
                                break
                            self._recovery_after_error = False
                            handoff_ready = False
                            prefetch_running = False
                            self._c2_laser_skip_feed = True
                            self._c2_skip_pf_trigger = False
                        early_exit = False
                    if materialist_only:
                        continue
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
                    if (
                        self._recovery_after_error
                        and not self._should_abort()
                        and self._wait_recovery_resume_hold()
                    ):
                        handoff_ready = False
                        prefetch_running = False
                        self._c2_laser_skip_feed = True
                        self._c2_skip_pf_trigger = True
                        continue
                    break  # fallo / abort → salir del while
                # Critico: el break anterior solo sale del while; sin esto el for
                # seguía con Tfeed/Feed en las piezas restantes (spam PF + E009).
                if early_exit:
                    break
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
        # Antes de In process OFF: si no, PF Idle dispara Res auto y la HMI queda verde.
        if not ok and not soft_cancel:
            self._ensure_failed_lot_latched()
            self._abort_needs_ack = True
        elif ok or soft_cancel:
            self._abort_needs_ack = False
        self._pause.clear()
        self._clear_recovery_gate()
        self._resume_need_buffer_full = False
        self._restart_piece = False
        self._c2_laser_skip_feed = False
        self._c2_skip_pf_trigger = False
        self._flow_interrupt.clear()
        refill = False
        with self._lock:
            refill = self._refill_mode
            self._refill_awaiting_confirm = False
            self._refill_prompt = ""
        self._refill_confirm.clear()
        self._refill_reject.clear()
        self._refill_retry.clear()
        self._refill_next_feed_mm = None
        self._refill_skip_cut = False
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
                self._pf_idle_for_hold("fin de lote" if ok else "post-error")
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
        # Cierra .md de timing (lote productivo; refill no abre sesión).
        if not refill and self._timing_md_path is not None:
            self._timing_close_session(ok=ok, ct_sec=elapsed)
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
        # Lote cerrado: no hay Resume que rearma; el próximo Start arma de nuevo.
        self._pf_held_idle = False
        self._host.cycle_notify()
