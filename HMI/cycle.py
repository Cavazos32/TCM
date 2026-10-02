"""Cycle — orquestador de lote TCM (secuencia; no perfiles de Motion).
Habla con Motion / PLC / PreFeeder por TCP. Delays de secuencia son
configurables (cycle_config.json). Estado máquina bytes 0x40–0x49.
"""
from __future__ import annotations
import json
import shutil
import threading
import time
from dataclasses import asdict, dataclass, fields
from datetime import datetime, timedelta
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
# Encoder feed típico ~55 mm; |valor| ≥ esto → anomalía en el resumen.
TIMING_ENCODER_OUTLIER_MM = 120.0
# Reescribe el resumen del .md cada N piezas (sobrevive a un cierre abrupto).
TIMING_SUMMARY_EVERY_PIECES = 100
TIMING_OP_ORDER = (
    "feed_cmd",
    "feed",
    "grippers_on",
    "holder_off",
    "lineal_cmd",
    "lineal",
    "corte",
    "pf_trigger",
    "depósito_cmd",
    "depósito",
    "grippers_off",
    "despeje_cmd",
    "despeje",
    "home_cmd",
    "home",
    "feed_next_cmd",
    "feed_next",
)

# Lineal producción: CMD_MOVE TCP abs(model.mm)+cutOffset vigente (carrera ABS).
# cutOffset no se congela al Start: cada lineal / depósito relee cycle_config.
# Stage2 HTTP queda para pruebas locales Motion; el ciclo no lo usa.
# El test HTML de Motion sigue usando pieceMm = L (target = L−55).
# No modificar Feed / FEED_TARGET_FIXED_MM / Move ABS manual.
# Excepción refill: skipValidate → creep hasta láser ON (sin OM). Ciclo de lote sigue en 55 mm.

# Purga: no alimenta sola. Tras park: Alimentar (hasta láser) o Next corte.
# Timeout láser purga ≈ 10 s (Motion FEED_PURGE_LASER_TIMEOUT_MS); lados independientes.
REFILL_LASER_TIMEOUT_S = 10.0
# Legado: ya no se ofrece Long feed en UI; se mantiene por compat API.
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
# PLC/Config.h: cada KEEP bloquea el ESP (pulso + hueco LOW).
# Holder+Encoder precut = 2 slots; si el HMI manda Set de cortador encima,
# el PLC lo procesa tarde y el depósito/despeje arranca con la cuchilla
# aún sin bajar → corte en el extra (~clearance 18 mm, pieza ~20 mm larga).
PLC_VALVE_PULSE_MS = 100
PLC_VALVE_GAP_MS = 50
PLC_VALVE_SLOT_MS = PLC_VALVE_PULSE_MS + PLC_VALVE_GAP_MS  # 150
PLC_PRECUT_VALVE_CMDS = 2  # holder + encoder
# Asiento desde el Set HMI: 2 slots (Set+Res) + carrera neumática.
# 250 ms desde Res era corto: coincidía con el drenaje de Holder/Encoder.
CUTTER_SETTLE_BEFORE_DEPOSIT_MS = 400
# HOME / Start: ASDA en 0 (caché Reached). No alimentar si está fuera.
ASDA_HOME_EPS_MM = 0.5
# Idle de Motion sin Reached en destino no cierra un MOVE (stop por error,
# Reset, Idle viejo). Tolerancia < despeje (5 mm) para detectar MOVE no ejecutado.
MOTION_TARGET_TOL_MM = 3.0
# Idle sin Busy ni Reached en destino durante este tiempo → E015.
MOTION_IDLE_NO_TARGET_S = 1.0

# Protocolo máquina: machine_states.py (0x40–0x49). Andon solo refleja esos bytes.
# Pasos atómicos (acción / delay independientes). Secuencia principal (action|wait).
# Feed de la siguiente pieza: solo tras HOME con ASDA en 0 (no paralelo).
#
# sbsPause: en modo Step by Step, pausa tras completar ese paso (checkpoint
# físico). False = auto (delay / validación interna): visible en la
# lista, pero no exige Next. Un Next avanza el grupo físico + sus internos.
FLOW_STEPS: list[dict[str, Any]] = [
    {"id": 1, "key": "holder_on", "label": "Holder+Encoder ON (solo 1ª pieza)", "kind": "action", "sbsPause": False},
    {"id": 2, "key": "wait_holder_on", "label": "Delay Holder ON", "kind": "wait", "delayKey": "holderOnMs", "sbsPause": True},
    {"id": 3, "key": "feed", "label": "Alimentación (feed / ya listo post-HOME)", "kind": "action", "sbsPause": True},
    {"id": 4, "key": "offset", "label": "Offset alimentación (Motion, paso lógico)", "kind": "action", "sbsPause": False},
    {"id": 5, "key": "grippers_on", "label": "Pinzas cierran", "kind": "action", "sbsPause": False},
    {"id": 6, "key": "wait_grippers_on", "label": "Delay tras cerrar pinzas", "kind": "wait", "delayKey": "grippersOnMs", "sbsPause": False},
    {"id": 7, "key": "enc_set0", "label": "OM ref (no usado por lineal TCP)", "kind": "action", "sbsPause": True},
    {"id": 8, "key": "holder_off", "label": "Holder+Encoder OFF (abre para lineal)", "kind": "action", "sbsPause": False},
    {"id": 9, "key": "wait_holder_open", "label": "Delay Holder/Encoder OFF", "kind": "wait", "delayKey": "holderOpenMs", "sbsPause": True},
    {"id": 10, "key": "lineal_fwd", "label": "Lineal ASDA MOVE TCP (0→ABS)", "kind": "action", "sbsPause": False},
    {"id": 11, "key": "wait_linear_done", "label": "Delay antes del corte", "kind": "wait", "delayKey": "linearDoneMs", "sbsPause": True},
    {"id": 12, "key": "holder_precut", "label": "Holder ON / Encoder ON (pre-corte)", "kind": "action", "sbsPause": False},
    {"id": 13, "key": "wait_holder_precut", "label": "Delay tras cerrar holder", "kind": "wait", "delayKey": "holderOnMs", "sbsPause": True},
    {"id": 14, "key": "cutter_on", "label": "Cortador ON (+ All OK PreFeeder)", "kind": "action", "sbsPause": False},
    {"id": 15, "key": "wait_cutter_pulse", "label": "Delay entre Set y Res cortador", "kind": "wait", "delayKey": "cutterPulseMs", "sbsPause": False, "delayEditable": False},
    {"id": 16, "key": "cutter_off", "label": "Cortador OFF", "kind": "action", "sbsPause": False},
    {"id": 17, "key": "wait_cutter_post", "label": "Delay post-corte", "kind": "wait", "delayKey": "cutterPostMs", "sbsPause": True},
    # Tfeed DESPUÉS del corte: rellena buffer en paralelo con depósito/HOME.
    {"id": 18, "key": "pf_trigger", "label": "Trigger PreFeeder (Tfeed)", "kind": "action", "sbsPause": False},
    {"id": 19, "key": "deposit", "label": "Extra / depósito lineal", "kind": "action", "sbsPause": False},
    {"id": 20, "key": "wait_deposit_dwell", "label": "Delay tras depósito", "kind": "wait", "delayKey": "dwellAtDestMs", "sbsPause": True},
    {"id": 21, "key": "grippers_off", "label": "Pinzas abren", "kind": "action", "sbsPause": False},
    {"id": 22, "key": "wait_gripper_release", "label": "Delay tras abrir pinzas", "kind": "wait", "delayKey": "gripperReleaseMs", "sbsPause": True},
    {
        "id": 23,
        "key": "gripper_clearance",
        "label": "Despeje ASDA post-pinzas (+clearance)",
        "kind": "action",
        "sbsPause": True,
    },
    {"id": 24, "key": "home", "label": "HOME: MOVE→0 + delay + blower ≡ |L|", "kind": "action", "sbsPause": True},
    {
        "id": 25,
        "key": "feed_after_home",
        "label": "Feed post-HOME (ASDA=0)",
        "kind": "action",
        "sbsPause": True,
    },
    {"id": 26, "key": "wait_asentar", "label": "Delay asentar", "kind": "wait", "delayKey": "asentarMs", "sbsPause": False},
    {"id": 27, "key": "post_piece", "label": "Post-pieza (safety / peer / settled)", "kind": "action", "sbsPause": False},
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
    # Pieza 1 ≈ 5–6 s (incluye feed). Feed post-HOME de la *siguiente* no usa este tope.
    piece_watch_timeout_s: float = 20.0
    # Feed / Stage2 OM: "L" | "R" | "LR" (producción = ambos)
    feed_sides: str = "LR"
    # Tfeed tras el corte (paso 18). False = omitir siempre.
    # 1ª pieza del lote omite; C2 omite si esa pieza ya mandó Tfeed. Default ON.
    pf_trigger_enabled: bool = True
    # Refill / purga: Alimentar hasta láser (skipValidate). refill_mm legado.
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
    def mirror_cycle_qty(order_qty: int, feed_sides: Any) -> tuple[int, int, str | None]:
        """Cantidad de ciclos y piezas físicas por ciclo (espejo L+R).

        En L+R cada corte produce 1 pz derecha + 1 pz izquierda: pedir 50 → 25 ciclos.
        Solo L o solo R: 1 ciclo = 1 pieza.
        Returns (cycle_qty, pieces_per_rep, error_or_None).
        """
        sides = CycleConfig.normalize_feed_sides(feed_sides)
        n = int(order_qty)
        if n < 1:
            return 0, 1, "Cantidad inválida"
        if sides == "LR":
            if n < 2:
                return 0, 2, "En L+R se necesitan al menos 2 piezas (1 por lado)"
            if n % 2 != 0:
                return (
                    0,
                    2,
                    "En L+R la cantidad debe ser par (ej. 50 → 25 R y 25 L)",
                )
            return n // 2, 2, None
        return n, 1, None

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
    def motion_busy(self) -> bool: ...
    def holder_encoder_closed(self) -> bool: ...
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
    def pf_status_seq(self) -> int: ...
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
        self._refill_prompt = ""  # "" | await_feed | after_feed | after_cut | working
        self._refill_confirm = threading.Event()
        self._refill_reject = threading.Event()
        self._refill_retry = threading.Event()
        self._refill_next_feed_mm: float | None = None
        # Lote vivo: Purge corre dentro del hilo del lote (feed_mm, asda_mm).
        self._lot_purge_request: tuple[float | None, float | None] | None = None
        self._suspend_piece_watch = False
        self._step = 0
        self._parallel_group = ""
        self._rep = 0
        self._pieces_done = 0
        self._total_reps = 0
        # 2 en L+R (espejo): cada ciclo cuenta 2 piezas físicas en la UI.
        self._pieces_per_rep = 1
        self._progress = 0
        self._last_ok = False
        self._abort_needs_ack = False
        self._fault = ""
        # recovery en lote: completar pieza en curso (hasta post_piece / corte) y pausar.
        self._recovery = ""  # common lot recovery context | e050_materialist
        # Tras error: lote vivo → Reset → purga → abort_decide → Resume → pieza → review → purga.
        self._recovery_after_error = False
        self._recovery_prompt = ""  # "" | pre_purge_decide | abort_decide | review_piece | continue_cycle | tray_full | e050_materialist
        self._recovery_awaiting = False
        self._pending_lot_decision = False
        self._recovery_confirm = threading.Event()
        self._recovery_reject = threading.Event()
        # E050 + Materialista: terminar pieza en curso si existe, HOME y Materialista.
        self._e050_finish_piece = False
        self._e050_materialist_requested = False
        self._e050_materialist_wait = False
        self._e050_normal_recovery = ""
        self._refill_skip_cut = False
        # Start / recovery / recovery: validar láser antes de alimentar (ON → omitir).
        self._restart_piece = False
        self._recovery_skip_feed = False
        # True tras LengthOK (feed+offset Motion). offL/offR suelen dejar láser OFF;
        # sin este flag, Resume/Continuar ciclo re-alimentaba y reaplicaba el offset.
        self._material_feed_done = False
        self._recovery_skip_pf_trigger = False
        self._pf_trigger_sent_this_piece = False
        self._flow_interrupt = threading.Event()
        # In process OFF por Pause/Error; Resume/Busy rearma. Evita doble OFF/ON.
        self._pf_held_idle = False
        # Resume / Continuar ciclo: misma espera Buffer Full que Start (hilo de ciclo).
        self._resume_need_buffer_full = False
        self._lot_rpm = 1200.0
        self._lot_length_mm: float = 0.0
        # Batches que caben en carrera por tray (None = sin tope). Lleno → vaciar.
        self._tray_batches: int | None = None
        self._last_lineal_sec: float = 0.0
        self._last_lineal_mm: float = 0.0
        # monotonic() del Set/Res de cortador (asiento antes de depósito/despeje).
        self._cutter_set_mono: float | None = None
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
        # Diagnóstico de lote: eventos (error / stop / pausa) con hora de pared,
        # agregados por pieza y archivos temporales de índice/detalle (streaming).
        self._timing_events: list[dict[str, Any]] = []
        self._timing_agg: dict[str, Any] = {}
        self._timing_index_path: Path | None = None
        self._timing_detail_path: Path | None = None
        self._timing_piece_pause_n = 0
        self._timing_piece_pause_sec = 0.0
        self._pause_reason_hint: str = ""
        self._pause_reason_cur: str = ""
        self._pause_rep: int | None = None
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
                # UI en piezas físicas: L+R → ×2 (espejo).
                "piecesDone": int(self._pieces_done) * max(1, int(self._pieces_per_rep)),
                "totalReps": int(self._total_reps) * max(1, int(self._pieces_per_rep)),
                "progress": self._progress,
                "elapsedSec": self._ct_elapsed_locked(),
                "lastPieceSec": float(self._last_piece_sec),
                "avgPieceSec": float(self._avg_piece_sec),
                "completed": self._last_ok and not self._active,
                "lastOk": self._last_ok,
                "fault": self._fault,
                "recovery": self._recovery,
                "recoveryAfterError": self._recovery_after_error,
                "recoveryPrompt": (
                    self._recovery_prompt
                    if (self._active and self._recovery_prompt)
                    else ""
                ),
                "recoveryAwaitingConfirm": self._recovery_awaiting,
                "trayPieces": (
                    self._tray_batches * max(1, int(self._cfg.deposit_batch_size))
                    if self._tray_batches
                    else 0
                ),
                "e050FinishPiece": self._e050_finish_piece,
                "e050MaterialistRequested": self._e050_materialist_requested,
                "e050MaterialistWait": self._e050_materialist_wait,
                "refillSkipCut": bool(self._refill_skip_cut and self._refill_mode),
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

    def is_e050_materialist_path(self) -> bool:
        """Sí Materialista ya decidido: terminar pieza / HOME / espera OFF.

        El PLC sigue reportando EncoderE mientras el sensor esté activo.
        No re-preguntar ni re-latchear E050 hasta salir de esta ruta.
        """
        with self._lock:
            return bool(
                self._e050_materialist_requested or self._e050_materialist_wait
            )

    def _e050_finishing_piece(self) -> bool:
        """Sí Materialista, aún terminando/parking la pieza (antes del wait)."""
        return bool(
            self._e050_materialist_requested and not self._e050_materialist_wait
        )

    def _release_e050_finish_pause(self) -> None:
        """El Sí ya continuó: no quedarse en Pause residual de E050."""
        if not self._e050_finishing_piece() or not self._pause.is_set():
            return
        self._pause.clear()
        with self._lock:
            self._sync_pause_exclusion_locked(time.monotonic())
        self._leave_pause_andon()

    # --- comandos máquina ---
    def request_start(self, length_mm: float, qty: int, rpm: float) -> dict[str, Any]:
        with self._lock:
            if self._materialist:
                return {
                    "ok": False,
                    "error": "Desactiva el modo Materialista para iniciar el ciclo",
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
            self._set_state(TX_MATERIALIST, "PreFeeder Materialista")
            return {
                "ok": False,
                "error": "Desactiva el modo Materialista para iniciar el ciclo",
            }
        self.reload_config()
        order_qty = int(qty)
        sides = CycleConfig.normalize_feed_sides(self.get_config().feed_sides)
        cycle_qty, pieces_per_rep, mirror_err = CycleConfig.mirror_cycle_qty(
            order_qty, sides
        )
        if mirror_err:
            self._host.cycle_log(f"Cycle Start rechazado: {mirror_err}")
            return {"ok": False, "error": mirror_err}
        tray_batches, travel_err = self._deposit_tray_capacity(float(length_mm))
        if travel_err:
            self._host.cycle_log(f"Cycle Start rechazado: {travel_err}")
            return {"ok": False, "error": travel_err}
        self._tray_batches = tray_batches
        if tray_batches is not None:
            batch = max(1, int(self.get_config().deposit_batch_size))
            per_tray = tray_batches * batch
            if int(cycle_qty) > per_tray:
                n_trays = (int(cycle_qty) + per_tray - 1) // per_tray
                self._host.cycle_log(
                    f"Tray: caben {tray_batches} batch(es) / {per_tray} depósitos "
                    f"(L={abs(float(length_mm)):g}) — lote {order_qty} pz "
                    f"({cycle_qty} ciclos) → {n_trays} trays, "
                    f"pausa para vaciar cada {per_tray} ciclos"
                )
        self._stop.clear()
        self._pause.clear()
        self._aborted = False
        self._fault = ""
        self._recovery = ""
        self._clear_recovery_gate()
        self._restart_piece = False
        self._recovery_skip_feed = False
        self._material_feed_done = False
        self._recovery_skip_pf_trigger = False
        self._pf_trigger_sent_this_piece = False
        self._stop.clear()
        self._pause.clear()
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
            self._pieces_per_rep = int(pieces_per_rep)
            self._reset_ct_clocks_locked()
        self._set_state(TX_BUSY)
        if pieces_per_rep > 1:
            half = cycle_qty
            self._host.cycle_log(
                f"Cycle Start (0x040) length={length_mm} mm "
                f"pedido={order_qty} pz → {cycle_qty} ciclos "
                f"({half} R · {half} L, espejo)"
            )
        else:
            self._host.cycle_log(
                f"Cycle Start (0x040) length={length_mm} mm qty={order_qty} "
                f"lado={sides}"
            )
        args = (float(length_mm), int(cycle_qty), float(rpm))
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
        self._recovery_skip_feed = False
        self._material_feed_done = False
        self._recovery_skip_pf_trigger = False
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
            self._pending_lot_decision = False
        # Desarma waits Stage2/Feed/Reached — si no, el hilo queda active ~3 min
        # y Reset responde "Detener ciclo antes de Reset".
        self._host.clear_motion_wait_flags()
        self._host.cmd_motion_stop()
        self._host.cmd_pf_stop()
        self._resume_need_buffer_full = False
        # Stop no toca PLC (válvulas: All Off / Reset PLC propios).
        self._timing_note_event("stop", "Stop operador", "0x041")
        self._set_state(TX_STOP, "Stop (0x042)")
        self._host.cycle_log("Cycle Stop (0x041)")
        return {"ok": True}
    def request_pause(self) -> dict[str, Any]:
        if not self.is_active():
            return {"ok": False, "error": "Sin ciclo activo"}
        self._pause_reason_hint = "operator"
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
        if self._materialist:
            return {
                "ok": False,
                "error": "Desactiva el modo Materialista para reanudar",
            }
        if not self._pause.is_set():
            return {"ok": False, "error": "Ciclo no está en Pause"}
        # Unified recovery: Resume never selects a class-specific branch.
        # The running lot will continue through the common recovery gate.
        if self._recovery_after_error:
            self._host.cycle_log("Cycle Resume → recuperación común")
        else:
            self._host.cycle_log("Cycle Resume")
        self._resume_need_buffer_full = True
        self._pause.clear()
        with self._lock:
            self._sync_pause_exclusion_locked(time.monotonic())
        self._leave_pause_andon()
        self._host.cycle_notify()
        return {"ok": True}

    def request_abort_decision(self) -> bool:
        """Tras Reset válido con lote vivo: primero purgar, luego Abortar/Continuar.

        No pisa prompts de recovery en curso (E050 Materialista, review, purga).
        """
        if not self.is_active() or not self._pause.is_set():
            return False
        with self._lock:
            if (
                self._recovery_prompt in ("pre_purge_decide", "abort_decide")
                and self._recovery_awaiting
            ):
                return True
            if self._recovery_prompt or self._refill_awaiting_confirm:
                return False
            self._pending_lot_decision = True
            self._recovery_prompt = "pre_purge_decide"
            self._recovery_awaiting = True
        self._host.cycle_log(
            "Recovery: ¿purgar ahora? Luego continuar o abortar el lote"
        )
        self._host.cycle_notify()
        return True

    def request_lot_continue_or_abort(self) -> bool:
        """Tras purga (o skip): operador elige Abortar o Continuar el lote."""
        if not self.is_active() or not self._pause.is_set():
            return False
        with self._lock:
            if self._recovery_prompt == "abort_decide" and self._recovery_awaiting:
                return True
            if self._refill_awaiting_confirm or self._refill_mode:
                return False
            self._pending_lot_decision = True
            self._recovery_prompt = "abort_decide"
            self._recovery_awaiting = True
        self._host.cycle_log("Recovery: ¿abortar ciclo o continuar el lote?")
        self._host.cycle_notify()
        return True

    def confirm_recovery_review(self, ok: bool = True) -> dict[str, Any]:
        """Resuelve la decisión E050 o las confirmaciones genéricas de recovery."""
        prompt = self._recovery_prompt
        if self._recovery_awaiting and prompt == "pre_purge_decide":
            with self._lock:
                self._recovery_awaiting = False
                self._recovery_prompt = ""
            if ok:
                self._host.cycle_log("Recovery: operador eligió purgar primero")
                self._host.cycle_notify()
                return {"ok": True, "purge": True}
            self._host.cycle_log("Recovery: operador omitió la purga previa")
            asked = self.request_lot_continue_or_abort()
            return {"ok": True, "askedAbort": asked}
        if self._recovery_awaiting and prompt == "abort_decide":
            with self._lock:
                self._recovery_awaiting = False
                self._recovery_prompt = ""
                self._pending_lot_decision = False
            if ok:
                self._host.cycle_log("Recovery: operador eligió continuar el lote")
                self._host.cycle_notify()
                return {"ok": True, "resume": True}
            self._host.cycle_log("Recovery: operador eligió abortar el ciclo")
            self._host.cycle_notify()
            return {"ok": True, "abort": True}
        if not self._recovery_awaiting or prompt not in (
            "review_piece",
            "purge_decide",
            "continue_cycle",
            "tray_full",
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
                self._host.cycle_log("E050: NO Materialista → recuperación normal del error")
                self.apply_error_policy(self._fault, self._e050_normal_recovery)
                self._host.cycle_notify()
                return {"ok": True, "materialist": False, "normalRecovery": True}
            # Marcar la ruta ANTES de liberar el latch: el PLC reenvía EncoderE
            # en el siguiente status y no debe volver a apply_e050_policy.
            self._e050_materialist_requested = True
            self._e050_finish_piece = self._piece_t0 is not None
            self._recovery_after_error = False
            self._recovery = "e050_materialist"
            self._recovery_awaiting = False
            self._recovery_prompt = (
                "e050_finishing" if self._e050_finish_piece else ""
            )
            self._fault = ""
            if not self._host.clear_e050_latch_for_materialist():
                self._e050_materialist_requested = False
                self._e050_finish_piece = False
                self._recovery_prompt = "e050_materialist"
                self._recovery_awaiting = True
                return {"ok": False, "error": "No se pudo liberar E050 para iniciar Materialista"}
            # clear_fault_mirror no debe borrar la ruta E050 ya decidida.
            self._recovery = "e050_materialist"
            if self._e050_finish_piece:
                self._recovery_prompt = "e050_finishing"
            self._pause.clear()
            with self._lock:
                self._sync_pause_exclusion_locked(time.monotonic())
            self._leave_pause_andon()
            self._host.cycle_log("E050: SÍ Materialista → " + ("terminar pieza actual y después HOME" if self._e050_finish_piece else "ir a HOME"))
            self._host.cycle_notify()
            return {"ok": True, "materialist": True, "finishingPiece": bool(self._e050_finish_piece)}
        if ok:
            label = (
                "continuar ciclo"
                if prompt == "continue_cycle"
                else "tray vaciado"
                if prompt == "tray_full"
                else "purga"
                if prompt == "purge_decide"
                else "pieza revisada"
            )
            self._recovery_confirm.set()
            self._host.cycle_log("Recovery: operador OK — " + label)
        else:
            self._recovery_reject.set()
            self._host.cycle_log("Recovery: operador rechazó la etapa")
        self._host.cycle_notify()
        return {"ok": True}

    def hold_lot_and_restart_piece(self, reason: str) -> dict[str, Any]:
        """Pausa el lote vivo y marca re-arranque de pieza. No cierra el lote."""
        if not self.is_active():
            return {"ok": True}
        if not self._pause.is_set():
            self.request_pause()
        self._restart_piece = True
        self._flow_interrupt.set()
        self._host.cycle_log(reason)
        self._host.cycle_notify()
        return {"ok": True}

    def restart_piece_after_manual_home(self) -> dict[str, Any]:
        """Home máquina con lote vivo: no cierra el lote.

        Home (ASDA→0 + All Off) deja la pieza en curso sin pinzas/holder;
        al Resume la pieza se re-arranca desde step 0 (Holder+Encoder, ASDA 0).
        Si el ciclo está en marcha, pasa a Pause.
        """
        return self.hold_lot_and_restart_piece(
            "Home máquina con lote vivo — la pieza se re-arranca al Resume "
            "(progreso conservado)"
        )

    def request_lot_purge(
        self, feed_mm: float | None = None, asda_mm: float | None = None
    ) -> dict[str, Any]:
        """Purge con lote vivo: dentro del lote (no Stop, no borra progreso).

        El hilo del lote sale del paso en curso, corre la purga existente y
        vuelve a Pause; al Resume la pieza se re-arranca desde step 0.
        """
        if not self.is_active():
            return {"ok": False, "error": "Sin lote activo"}
        if self.is_refill_active() or self._lot_purge_request is not None:
            return {"ok": False, "error": "Purga ya en curso"}
        with self._lock:
            # Overlay Abortar/Purgar no puede tapar Retry / Long feed / Next.
            if self._recovery_prompt in (
                "pre_purge_decide",
                "abort_decide",
            ):
                self._recovery_awaiting = False
                self._recovery_prompt = ""
        self._lot_purge_request = (feed_mm, asda_mm)
        self._restart_piece = True
        self._flow_interrupt.set()
        self._host.cycle_log(
            "Purge con lote vivo — dentro del lote (progreso conservado)"
        )
        self._host.cycle_notify()
        return {"ok": True}

    def _run_lot_purge(self) -> None:
        """Purga pedida por request_lot_purge. Termina siempre en Pause (lote vivo)."""
        req = self._lot_purge_request
        self._lot_purge_request = None
        self._restart_piece = False
        self._material_feed_done = False
        self._flow_interrupt.clear()
        if req is None or self._should_abort():
            return
        feed_mm, asda_mm = req
        cfg = self.get_config()
        use_feed = float(feed_mm) if feed_mm is not None else float(cfg.refill_mm)
        use_asda = float(asda_mm) if asda_mm is not None else float(cfg.refill_asda_mm)
        rpm = float(self._lot_rpm or 1200.0)
        with self._lock:
            saved_progress = self._progress
            self._refill_mode = True
            self._refill_prompt = "working"
        self._suspend_piece_watch = True
        # La purga maneja sus propios prompts (Pause); el latch EXXX sigue activo.
        self._pause.clear()
        self._host.cycle_notify()
        status = "fail"
        try:
            status = self._execute_refill_body(rpm, use_feed, use_asda)
        except Exception as exc:
            self._host.cycle_log(f"Purga (lote) exception: {exc}")
        finally:
            with self._lock:
                self._refill_mode = False
                self._refill_awaiting_confirm = False
                self._refill_prompt = ""
                self._progress = saved_progress
            self._refill_skip_cut = False
            self._suspend_piece_watch = False
        if status == "cancel":
            self._host.cmd_plc_tools_safe()
            self._host.cycle_log("Purga (lote) cancelada — lote en Pause")
        elif status != "ok":
            self._host.cycle_log("Purga (lote) incompleta — lote en Pause")
        else:
            self._host.cycle_log("Purga (lote) OK — lote en Pause (Reset → Resume)")
        if self._should_abort():
            return
        self._pause_reason_hint = "lot_purge"
        self._pause.set()
        with self._lock:
            if self._pause_t0 is None:
                self._pause_t0 = time.monotonic()
        self._enter_pause_andon()
        if self._pending_lot_decision:
            self.request_lot_continue_or_abort()
        self._host.cycle_notify()

    def release_for_manual_refill(self, timeout_s: float = 2.0) -> dict[str, Any]:
        """Suelta un lote en Pause (p. ej. E050) para arrancar purga suelta.

        Comando de operador (Purge), no Stop automático por el EXXX.
        """
        if not self.is_active():
            return {"ok": True}
        self.request_stop()
        th = self._thread
        if th is not None and th.is_alive():
            th.join(timeout=max(0.2, float(timeout_s)))
        if self.is_active():
            return {"ok": False, "error": "Detener ciclo para purgar"}
        return {"ok": True}

    def request_refill(
        self,
        rpm: float,
        *,
        feed_mm: float | None = None,
        asda_mm: float | None = None,
    ) -> dict[str, Any]:
        """Purga/refill: ASDA park → holder → Alimentar (láser) / corte → home.

        Tras park: Alimentar hasta láser ON (timeout 10 s, lados independientes)
        o Next Cutting; no hay feed automático.
        Tras feed: Reintentar (otra vez a láser) o Next Cutting.
        Tras corte: Next Return ASDA to 0.
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
        self._recovery = ""
        self._clear_recovery_gate()
        self._e050_materialist_requested = False
        self._e050_materialist_wait = False
        self._e050_normal_recovery = ""
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
            f"Refill Start — ASDA→{use_asda:g} mm · "
            f"Alimentar hasta láser (timeout {REFILL_LASER_TIMEOUT_S:g} s) "
            f"o Next Cutting · "
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
            if prompt in ("await_feed", "after_feed"):
                if self._refill_skip_cut:
                    self._host.cycle_log("Refill: Continuar → ASDA a 0")
                elif prompt == "await_feed":
                    self._host.cycle_log("Refill: Next → Cutting (sin feed)")
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
        """Alimentar / reintentar hasta láser ON. Válido en await_feed y after_feed.

        feed_mm se ignora (legado Long feed); siempre creep a láser.
        """
        if not self._refill_awaiting_confirm:
            return {"ok": False, "error": "Sin refill pendiente de confirmación"}
        if self._refill_prompt not in ("await_feed", "after_feed"):
            return {"ok": False, "error": "Alimentar solo en espera de alimentación"}
        _ = feed_mm  # legado API; ignorado
        self._refill_next_feed_mm = None
        self._refill_retry.set()
        self._host.cycle_log(
            f"Refill: alimentar hasta láser (timeout {REFILL_LASER_TIMEOUT_S:g} s)"
        )
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
        self._recovery = ""
        self._clear_recovery_gate()
        self._restart_piece = False
        self._recovery_skip_feed = False
        self._material_feed_done = False
        self._recovery_skip_pf_trigger = False
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
        self._pending_lot_decision = False
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
        """recovery: lote vivo en Pause. Resume terminará la pieza."""
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
        if not (self._e050_materialist_requested or self._e050_materialist_wait):
            self._recovery = ""
        # No borrar _recovery_after_error: el lote sigue en recuperación.
        self._abort_needs_ack = False

    def set_materialist(self, on: bool) -> dict[str, Any]:
        self._materialist = bool(on)
        self._busy_mode = False
        if self._materialist:
            self._set_state(TX_MATERIALIST)
            self._host.cycle_log("Cycle Materialista ON (0x049)")
        elif self.is_active() and self._pause.is_set():
            if self._fault:
                self._set_state(TX_ERROR)
                self._pf_idle_for_hold("Error")
            else:
                self._enter_pause_andon()
            self._host.cycle_log("Cycle Materialista OFF → Pause (lote vivo)")
        else:
            self._set_state(TX_BUSY if self.is_active() else TX_IDLE)
            self._host.cycle_log("Cycle Materialista OFF → " + ("Busy" if self.is_active() else "Idle"))
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
            return {"ok": False, "error": "No paso a paso en Materialista (0x049)"}
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

    def _pf_arm_fill_for_buffer_full(self, reason: str) -> None:
        """Start 0x2A + In process ON: ventana de relleno abierta.

        Si no hay Buffer Full: el esclavo hace servo y luego DeReeler.
        Full → corte instantáneo. Vacío otra vez → vuelve a alimentar.
        In process se queda ON durante el lote (Tfeed + relleno). No se
        detiene el ciclo por buffer vacío; solo un EXXX del PF para.
        """
        if not self._use_prefeeder() or self.is_refill_active():
            return
        if not self._host.cmd_pf_start():
            self._host.cycle_log(f"PreFeeder: Start 0x2A falló ({reason})")
        if self._host.cmd_pf_in_process(True):
            self._pf_held_idle = False
            self._host.cycle_log(f"PreFeeder: In process ON ({reason})")
        else:
            self._host.cycle_log("PreFeeder: In process ON falló")

    def _pf_rearm_in_process(self, reason: str) -> None:
        """Resume/Busy con lote vivo → Start + In process (relleno si no Full)."""
        if (
            not self._use_prefeeder()
            or not self.is_active()
            or not self._pf_held_idle
            or self.is_refill_active()
        ):
            return
        self._pf_arm_fill_for_buffer_full(reason)

    def mark_init_done(self) -> None:
        with self._lock:
            if self._state_byte != TX_INIT or self._active:
                return
        self._set_state(TX_IDLE)
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
        self._pause_reason_hint = "hold"
        self._pause.set()
        with self._lock:
            if self._pause_t0 is None:
                self._pause_t0 = time.monotonic()
        self._enter_pause_andon()
        self._host.cycle_log(reason)
        self._host.cycle_notify()
        while self._pause.is_set():
            if self._should_abort() or self._restart_piece or self._lot_purge_request is not None:
                return True
            time.sleep(0.05)
        with self._lock:
            self._sync_pause_exclusion_locked(time.monotonic())
        if self._fault:
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
        """True = Resume (seguir). False = solo Stop / Abortar del operador.

        Un error nuevo durante el Resume (p. ej. E058) no cierra el lote: vuelve
        a Pause y espera otro Reset → Resume. El progreso solo se pierde por
        Stop, Abortar o fin de lote.
        """
        if self._should_abort():
            return False
        self._arm_recovery_pause()
        self._host.cycle_log(
            "Recovery: lote vivo — Reset, luego purga y decidir continuar/abortar"
        )
        self._host.cycle_notify()
        while True:
            if self._should_abort():
                return False
            # Home / Purge / Materialista con lote vivo: volver al bucle de piezas.
            if self._restart_piece or self._lot_purge_request is not None:
                return True
            if not self._pause.is_set():
                with self._lock:
                    self._sync_pause_exclusion_locked(time.monotonic())
                if self._ensure_pf_buffer_full_after_resume():
                    return True
                if self._should_abort():
                    return False
                self._arm_recovery_pause()
                self._host.cycle_log(
                    "Recovery: Resume no completó — lote sigue en Pause "
                    "(Reset → Resume)"
                )
                self._host.cycle_notify()
                continue
            time.sleep(0.05)

    def _recovery_continue_cycle(self) -> bool:
        """Continuar ciclo → Buffer Full (igual que Start/Resume)."""
        if self._wait_recovery_prompt("continue_cycle") != "ok":
            return False
        with self._lock:
            self._recovery_prompt = ""
        # El prompt pone Pause → In process OFF. Busy rearma, pero no usa
        # request_resume: hay que esperar Buffer Full igual que Start/Resume.
        if not self._ensure_pf_buffer_full_after_resume(
            "Continuar ciclo", force=True
        ):
            return False
        self._host.cycle_log(
            "Recovery: Continuar ciclo — siguiente pieza (validar referencia láser)"
        )
        self._host.cycle_notify()
        return True

    def _tray_pieces(self) -> int | None:
        if not self._tray_batches:
            return None
        return self._tray_batches * max(1, int(self.get_config().deposit_batch_size))

    def _tray_full_after(self, rep: int, qty: int) -> bool:
        per_tray = self._tray_pieces()
        return bool(per_tray) and int(rep) < int(qty) and int(rep) % per_tray == 0

    def _wait_tray_emptied(self, rep: int, qty: int) -> bool:
        """Tray lleno: Pause → operador vacía y confirma → ASDA 0 → Buffer Full."""
        per_tray = self._tray_pieces() or 0
        self._host.cycle_log(
            f"Tray lleno ({per_tray} piezas) tras pieza {rep}/{qty} — "
            "vaciar tray y confirmar"
        )
        if self._wait_recovery_prompt("tray_full") != "ok":
            return False
        with self._lock:
            self._recovery_prompt = ""
        self._host.cycle_log("Tray vaciado — ASDA a 0, batches desde el 1º")
        if not self._ensure_asda_at_zero(reason="tray vaciado"):
            return False
        if not self._ensure_pf_buffer_full_after_resume("Tray vaciado", force=True):
            return False
        self._host.cycle_notify()
        return True

    def _recovery_review_purge_decide(self) -> bool:
        """Review → decidir purga → opcionalmente purga → Continuar ciclo."""
        self._host.cycle_log("Recovery: revisa la pieza y confirma OK")
        if self._wait_recovery_prompt("review_piece") != "ok":
            return False
        self._host.cycle_log("Recovery: ¿requiere purga?")
        if self._wait_recovery_prompt("purge_decide") != "ok":
            if self._should_abort():
                return False
            self._host.cycle_log("Recovery: operador omitió la purga")
            with self._lock:
                self._recovery_prompt = ""
            self._host.cycle_notify()
            return self._recovery_continue_cycle()

        self._host.cycle_log("Recovery: operador solicitó purga")
        cfg = self.get_config()
        rpm = float(self._lot_rpm or 1200.0)
        self._material_feed_done = False
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
        return self._recovery_continue_cycle()

    def _run_e050_materialist_recovery(self) -> bool:
        """E050 especial: HOME → Materialista ON → esperar Materialista OFF."""
        if self._should_abort():
            return False
        self._release_e050_finish_pause()
        if self._e050_finish_piece:
            self._host.cycle_log("E050: pieza terminada y depositada → confirmar HOME")
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
        self._host.cycle_log("E050: HOME OK → activar Materialista")
        self._host.cycle_notify()
        res = self._host.cmd_cycle_materialist(True)
        if not res.get("ok"):
            with self._lock:
                self._e050_materialist_wait = False
                self._recovery_prompt = ""
            self._raise_fault(str(res.get("error") or "Materialista rechazado"))
            self._host.cycle_notify()
            return False
        self._host.cycle_log("E050: Materialista activo — esperar que el operador lo apague")
        self._host.cycle_notify()
        while True:
            if self._should_abort():
                return False
            if self._lot_purge_request is not None:
                self._run_lot_purge()
                if self._should_abort():
                    return False
            if not (self._materialist or self._host.pf_is_materialist()):
                break
            time.sleep(0.05)
        with self._lock:
            self._e050_materialist_wait = False
            self._recovery_prompt = ""
            self._recovery_awaiting = False
            self._e050_materialist_requested = False
            self._e050_finish_piece = False
            self._recovery_after_error = False
            self._recovery = ""
        self._host.cycle_log("E050: Materialista OFF → continuar lote")
        self._host.cycle_notify()
        return True

    def abort_needs_ack(self) -> bool:
        """True si el último lote abortó: Res observacional no aplica."""
        return bool(self._abort_needs_ack)

    def _raise_current_pf_fault(self, log_prefix: str, *, fallback: str = "E068") -> None:
        """Set del EXXX PF actual (o fallback). Aplica error/recovery."""
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

    def _raise_fault(self, slug_or_code: str) -> None:
        """Latchea fallo EXXX vía política HMI (Set flip-flop)."""
        # Si Motion ya latcheó un EXXX (p.ej. E023 CAN), no pisar con genérico de ciclo.
        if self._fault:
            return
        if hasattr(self._host, "apply_detail_error"):
            if self._host.apply_detail_error(slug_or_code):
                return
        self._fault = format_ui(slug_or_code, fallback=slug_or_code)
        self._timing_note_event("error", self._fault, str(slug_or_code))

    def apply_error_policy(
        self,
        ui: str,
        recovery: str = "",
    ) -> dict[str, Any]:
        """Enter the unified ERROR hold; error/recovery do not select recovery."""
        self._fault = ui
        self._recovery = recovery
        self._recovery_after_error = self.is_active()
        self._resume_need_buffer_full = False
        self._timing_note_event("error", ui, recovery or "error_state")
        self._pause.set()
        with self._lock:
            if self._pause_t0 is None:
                self._pause_t0 = time.monotonic()
        self._cancel_wip_blower()

        # Stop current Motion activity, but do not kill the CycleRunner thread.
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
        recovery: str = "recovery",
    ) -> dict[str, Any]:
        """E050 durante lote: primero pregunta si requiere Materialista."""
        if self._e050_materialist_requested or self._e050_materialist_wait:
            return {"ok": True, "action": "e050_path", "e050": True}
        if self._recovery_awaiting and self._recovery_prompt == "e050_materialist":
            self._fault = ui
            return {"ok": True, "action": "pause", "e050": True}
        self._fault = ui
        self._recovery = "e050_materialist"
        self._e050_normal_recovery = recovery or "recovery"
        self._e050_finish_piece = False
        self._e050_materialist_requested = False
        self._e050_materialist_wait = False
        self._recovery_after_error = False
        self._recovery_confirm.clear()
        self._recovery_reject.clear()
        with self._lock:
            self._recovery_prompt = "e050_materialist"
            self._recovery_awaiting = True
        self._timing_note_event("error", ui, "e050")
        if self.is_active():
            self._pause.set()
            with self._lock:
                if self._pause_t0 is None:
                    self._pause_t0 = time.monotonic()
            self._cancel_wip_blower()
        if not self.is_active():
            self._last_ok = False
        self._set_state(TX_ERROR, ui)
        self._host.cycle_log("E050: Pause — ¿Requiere Materialista?")
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
        # recovery: completar pieza (corte) → Pause en post_piece; esperar Reset+Resume.
        # Antes: return True abortaba el lote y Resume quedaba muerto.
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
            self._release_e050_finish_pause()
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
        self._release_e050_finish_pause()
        paused_here = False
        if self._step_by_step_should_pause(step_key) and not self._e050_finishing_piece():
            meta = STEP_BY_KEY.get(step_key, {})
            label = meta.get("label", step_key)
            self._pause_reason_hint = f"paso_a_paso:{step_key}"
            self._pause.set()
            self._host.cycle_log(f"Paso a paso — {label}")
            self._enter_pause_andon()
            paused_here = True
            self._host.cycle_notify()
        while self._pause.is_set():
            if self._should_abort():
                return True
            if self._restart_piece or self._lot_purge_request is not None:
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
        self._release_e050_finish_pause()
        while self._pause.is_set():
            if self._should_abort() or self._restart_piece or self._lot_purge_request is not None:
                return True
            time.sleep(0.05)
        if not self._ensure_pf_buffer_full_after_resume():
            return True
        return bool(self._restart_piece)
    def _asda_at_target(self, target_mm: float) -> bool | None:
        """True/False según caché ASDA vs destino (magnitud). None = sin dato."""
        pos = self._host.asda_position_mm()
        if pos is None:
            return None
        return abs(abs(float(pos)) - abs(float(target_mm))) <= MOTION_TARGET_TOL_MM

    def _fmt_asda_pos(self) -> str:
        pos = self._host.asda_position_mm()
        return "?" if pos is None else f"{float(pos):.2f}"

    def _wait_motion(
        self,
        target_mm: float | None = None,
        reissue: Callable[[], bool] | None = None,
    ) -> bool:
        """Espera Idle/Reached. El caller debe limpiar flags ANTES del comando
        (clear_motion_wait_flags / clear_motion_reached_flag) — no limpiar aquí
        o se pierde el evento si Motion responde entre el cmd y el wait.

        target_mm: Idle sin posición en destino no cuenta como llegada.
        reissue: tras Resume, si el ASDA quedó fuera de destino (stop por
        error / Reset), reenvía el mismo MOVE en vez de dar el paso por hecho.
        """
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
        idle_off_target_since: float | None = None
        while True:
            if self._should_abort() or self._restart_piece:
                return False
            self._release_e050_finish_pause()
            if self._pause.is_set():
                while self._pause.is_set():
                    if self._should_abort() or self._restart_piece:
                        return False
                    time.sleep(0.05)
                if not self._ensure_pf_buffer_full_after_resume():
                    return False
                if (
                    target_mm is not None
                    and reissue is not None
                    and self._asda_at_target(target_mm) is False
                ):
                    self._host.cycle_log(
                        f"Resume: ASDA pos={self._fmt_asda_pos()} mm ≠ destino "
                        f"{abs(float(target_mm)):.1f} mm — reenviar MOVE"
                    )
                    self._host.clear_motion_reached_flag()
                    if not reissue():
                        if not self._should_abort() and not self._fault:
                            kind = ""
                            if hasattr(self._host, "last_move_fail_kind"):
                                kind = self._host.last_move_fail_kind()
                            self._raise_fault("E065" if kind == "transport" else "move_cmd")
                        return False
                idle_off_target_since = None
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
                if target_mm is None or self._asda_at_target(target_mm) is not False:
                    return True
                # Idle viejo (MOVE aún no arrancó) o MOVE no ejecutado: seguir
                # esperando Reached; si Motion sigue quieto, E015.
                self._host.clear_motion_reached_flag()
                if idle_off_target_since is None:
                    idle_off_target_since = time.monotonic()
            elif idle_off_target_since is not None:
                if self._host.motion_busy():
                    idle_off_target_since = None
                elif (
                    time.monotonic() - idle_off_target_since
                    >= MOTION_IDLE_NO_TARGET_S
                ):
                    self._host.cycle_log(
                        f"MOVE sin llegar a destino: ASDA pos={self._fmt_asda_pos()} mm, "
                        f"destino={abs(float(target_mm)):.1f} mm (Idle sin Reached)"
                    )
                    self._raise_fault("E015")
                    idle_off_target_since = None
                    if reissue is None or not self._pause.is_set():
                        return False
                    continue
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
        """True si recovery Resume pidió reinicio de pieza; limpia el flag."""
        if not self._restart_piece:
            return False
        self._restart_piece = False
        self._recovery_skip_pf_trigger = bool(self._pf_trigger_sent_this_piece)
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
        """Stop o interrupt recovery (reinicio pieza) abortan waits de feed."""
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
            if not self._pause_reason_cur:
                self._pause_reason_cur = self._pause_reason_derive_locked()
                self._pause_rep = int(self._rep or 0) or None
            return
        if self._pause_t0 is not None:
            dur = max(0.0, now - self._pause_t0)
            self._pause_excluded_sec += dur
            self._timing_note_pause_locked(dur)
            self._pause_t0 = None
            self._pause_reason_cur = ""
            self._pause_rep = None

    def _pause_reason_derive_locked(self) -> str:
        """Motivo de la pausa en curso (prompt activo > hint de quien pausó > EXXX)."""
        hint = self._pause_reason_hint
        self._pause_reason_hint = ""
        if self._recovery_awaiting and self._recovery_prompt:
            return f"recovery:{self._recovery_prompt}"
        if self._refill_awaiting_confirm and self._refill_prompt:
            return f"refill:{self._refill_prompt}"
        if hint:
            return hint
        if self._fault:
            code = str(self._fault).split(":", 1)[0].strip()
            return f"error:{code}" if code else "error"
        return "pause"

    def _timing_note_pause_locked(self, dur: float) -> None:
        """Pausa cerrada → evento cronológico + fila en la pieza (caller con _lock)."""
        if self._timing_md_path is None or dur < TIMING_GAP_REPORT_S:
            return
        reason = self._pause_reason_cur or self._pause_reason_derive_locked()
        wall_end = datetime.now()
        wall_start = wall_end - timedelta(seconds=dur)
        rep = self._pause_rep or (int(self._rep or 0) or None)
        self._timing_events.append(
            {
                "kind": "pause",
                "t_start": wall_start,
                "t_end": wall_end,
                "rep": rep,
                "code": reason,
                "note": "",
                "sec": dur,
            }
        )
        self._timing_piece_pause_n += 1
        self._timing_piece_pause_sec += dur
        self._piece_timings.append(
            {
                "name": f"wait:pause:{reason}",
                "sec": dur,
                "ok": True,
                "ct_excluded": True,
                "wall_start": wall_start.strftime("%H:%M:%S"),
                "wall_end": wall_end.strftime("%H:%M:%S"),
            }
        )

    def _timing_note_event(self, kind: str, code: str, note: str = "") -> None:
        """Error / Stop / fallo de paso con hora de pared (orden cronológico)."""
        if self._timing_md_path is None:
            return
        now = datetime.now()
        code = str(code or "").strip() or "?"
        for ev in self._timing_events[-5:]:
            if (
                ev.get("kind") == kind
                and ev.get("code") == code
                and (now - ev["t_start"]).total_seconds() < 2.0
            ):
                return
        self._timing_events.append(
            {
                "kind": str(kind),
                "t_start": now,
                "t_end": None,
                "rep": int(self._rep or 0) or None,
                "code": code,
                "note": str(note or ""),
                "sec": None,
            }
        )

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
            if self._timing_md_path is not None:
                self._timing_agg.setdefault("first_piece_start", now)

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
        if self._suspend_piece_watch:
            return None
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
        ppr = max(1, int(self._pieces_per_rep))
        if ppr > 1:
            phys = int(rep) * ppr
            order = int(qty) * ppr
            base = f"Ciclo {rep}/{qty} OK ({phys}/{order} pz)"
        else:
            base = f"Pieza {rep}/{qty} OK"
        if piece_sec > 0:
            self._host.cycle_log(f"{base} · {piece_sec:.1f}s")
        else:
            self._host.cycle_log(base)
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
        self._pause_reason_cur = ""
        self._pause_rep = None
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
        if not ok:
            self._timing_note_event("step_fail", str(op["name"]), "paso fallido")
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
        """Abre .md de diagnóstico del lote (índice/detalle van a temporales)."""
        self._lot_timing_pieces = []
        self._piece_timings = []
        self._piece_metro = []
        self._timing_last_end = None
        self._timing_last_end_pause = 0.0
        self._timing_md_path = None
        self._timing_index_path = None
        self._timing_detail_path = None
        self._timing_events = []
        with self._lock:
            self._timing_piece_pause_n = 0
            self._timing_piece_pause_sec = 0.0
        now_mono = time.monotonic()
        self._timing_agg = {
            "pieces": 0,
            "ct": [0, 0.0, None, None],
            "real": [0, 0.0, None, None],
            "unreg_sum": 0.0,
            "pause_sum": 0.0,
            "ops": {},
            "metro_out": [],
            "metro_out_n": 0,
        }
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        sides = CycleConfig.normalize_feed_sides(self.get_config().feed_sides)
        self._timing_lot_meta = {
            "started": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
            "length_mm": float(length_mm),
            "qty": int(qty),
            "rpm": float(rpm),
            "stamp": stamp,
            "sides": sides,
            "lot_t0": float(self._started_at)
            if self._started_at is not None
            else now_mono,
        }
        try:
            TIMING_LOG_DIR.mkdir(parents=True, exist_ok=True)
            path = TIMING_LOG_DIR / f"cycle_{stamp}.md"
            index_path = TIMING_LOG_DIR / f"cycle_{stamp}.index.tmp"
            detail_path = TIMING_LOG_DIR / f"cycle_{stamp}.detail.tmp"
            index_path.write_text("", encoding="utf-8")
            detail_path.write_text("", encoding="utf-8")
            self._timing_md_path = path
            self._timing_index_path = index_path
            self._timing_detail_path = detail_path
            self._timing_write_summary(final=False)
            self._host.cycle_log(f"Timing: análisis → {path}")
        except Exception as exc:
            self._timing_md_path = None
            self._timing_index_path = None
            self._timing_detail_path = None
            self._host.cycle_log(f"Timing: no se pudo crear .md ({exc})")

    @staticmethod
    def _fmt_hms(sec: float) -> str:
        s = max(0, int(round(float(sec))))
        h, rem = divmod(s, 3600)
        m, sec_i = divmod(rem, 60)
        if h > 0:
            return f"{h}h {m:02d}m {sec_i:02d}s"
        if m > 0:
            return f"{m}m {sec_i:02d}s"
        return f"{sec_i}s"

    @staticmethod
    def _stat_add(stat: list[Any], v: float) -> None:
        """stat = [n, sum, min, max]."""
        stat[0] += 1
        stat[1] += v
        stat[2] = v if stat[2] is None else min(stat[2], v)
        stat[3] = v if stat[3] is None else max(stat[3], v)

    @staticmethod
    def _stat_txt(stat: list[Any]) -> str:
        n = int(stat[0])
        if n <= 0:
            return "*(n/d)*"
        return (
            f"avg **{stat[1] / n:.3f}s** (min {float(stat[2]):.3f} · "
            f"max {float(stat[3]):.3f} · n={n})"
        )

    def _timing_append_file(self, path: Path | None, text: str) -> None:
        if path is None:
            return
        try:
            with path.open("a", encoding="utf-8") as fh:
                fh.write(text)
                if not text.endswith("\n"):
                    fh.write("\n")
        except Exception as exc:
            self._host.cycle_log(f"Timing: error escribiendo {path.name} ({exc})")

    def _timing_flush_piece(
        self, rep: int, qty: int, piece_sec: float
    ) -> None:
        """Log compacto de la pieza + registro en el .md del lote."""
        samples = list(self._piece_timings)
        metro = list(self._piece_metro)
        self._piece_timings = []
        self._piece_metro = []
        if self._timing_md_path is not None:
            self._timing_record_piece(rep, qty, piece_sec, samples, metro)
        if not samples and piece_sec <= 0:
            return
        self._timing_log_piece(rep, qty, samples)

    def _timing_record_piece(
        self,
        rep: int,
        qty: int,
        piece_sec: float,
        samples: list[dict[str, Any]],
        metro: list[dict[str, Any]],
    ) -> None:
        """Agregados en memoria + índice/detalle a disco (streaming)."""
        now = time.monotonic()
        agg = self._timing_agg
        prev = agg.get("last_piece_end") or agg.get("first_piece_start") or now
        real = max(0.0, now - float(prev))
        agg["last_piece_end"] = now
        with self._lock:
            n_pause = int(self._timing_piece_pause_n)
            pause_sec = float(self._timing_piece_pause_sec)
            self._timing_piece_pause_n = 0
            self._timing_piece_pause_sec = 0.0
        steps_sec = sum(
            float(s["sec"])
            for s in samples
            if not s.get("ct_excluded") and not str(s["name"]).startswith("gap:")
        )
        n_fail = sum(1 for s in samples if not s.get("ok", True))
        unreg = max(0.0, real - steps_sec - pause_sec)
        clock = datetime.now().strftime("%H:%M:%S")

        agg["pieces"] = int(agg.get("pieces") or 0) + 1
        if piece_sec > 0:
            self._stat_add(agg["ct"], float(piece_sec))
        self._stat_add(agg["real"], real)
        agg["unreg_sum"] = float(agg.get("unreg_sum") or 0.0) + unreg
        agg["pause_sum"] = float(agg.get("pause_sum") or 0.0) + pause_sec
        ops: dict[str, list[Any]] = agg["ops"]
        for s in samples:
            name = str(s["name"])
            if s.get("ct_excluded") or name.startswith("gap:") or not s.get("ok", True):
                continue
            self._stat_add(ops.setdefault(name, [0, 0.0, None, None]), float(s["sec"]))
        r = self._metro_log_readings(metro)
        if any(
            r.get(k) is not None and abs(float(r[k])) >= TIMING_ENCODER_OUTLIER_MM
            for k in ("om_l", "om_r")
        ):
            agg["metro_out_n"] = int(agg.get("metro_out_n") or 0) + 1
            if len(agg["metro_out"]) < 1000:
                agg["metro_out"].append((int(rep), r))

        self._timing_append_file(
            self._timing_index_path,
            f"| {rep} | {piece_sec:.3f} | {real:.3f} | {steps_sec:.3f} | "
            f"{unreg:.3f} | {n_pause} | {pause_sec:.3f} | {n_fail} | `{clock}` |",
        )
        block = [
            f"### Pieza {rep}/{qty} · CT **{piece_sec:.3f}s** · "
            f"Real **{real:.3f}s** · `{clock}`",
            "",
            "| Paso | Duración | OK |",
            "|------|---------:|:--:|",
        ]
        for s in samples:
            name = str(s["name"])
            sec = float(s["sec"])
            if s.get("ct_excluded"):
                mark = "pausa"
            else:
                mark = "✓" if s.get("ok", True) else "✗"
            when = ""
            if s.get("wall_start"):
                when = f" · {s['wall_start']}→{s.get('wall_end') or ''}"
            block.append(f"| `{name}` | {sec:.3f}s{when} | {mark} |")
        block.append("")
        block.append(
            f"- Pasos {steps_sec:.3f}s · Pausas {pause_sec:.3f}s ({n_pause}) · "
            f"Sin registrar {unreg:.3f}s · Metro: corte={self._fmt_mm(r.get('corte'))} "
            f"en0={self._fmt_mm(r.get('en0'))} encL={self._fmt_mm(r.get('om_l'))} "
            f"encR={self._fmt_mm(r.get('om_r'))}"
        )
        anomalies = self._timing_anomaly_lines(samples)
        if anomalies:
            block.extend(anomalies)
        block.extend(["", ""])
        self._timing_append_file(self._timing_detail_path, "\n".join(block))
        if agg["pieces"] % TIMING_SUMMARY_EVERY_PIECES == 0:
            self._timing_write_summary(final=False)

    def _timing_log_piece(
        self, rep: int, qty: int, samples: list[dict[str, Any]]
    ) -> None:
        """Resumen compacto en log de ciclo (ops físicas + delays)."""
        highlight = (
            "feed_cmd",
            "feed",
            "lineal_cmd",
            "lineal",
            "corte",
            "pf_trigger",
            "depósito_cmd",
            "depósito",
            "despeje_cmd",
            "despeje",
            "home_cmd",
            "home",
            "feed_next",
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

    @staticmethod
    def _metro_pick(metro: list[dict[str, Any]], *tags: str) -> dict[str, Any] | None:
        by_tag = {str(s.get("tag") or ""): s for s in metro}
        for tag in tags:
            if tag in by_tag:
                return by_tag[tag]
        return None

    def _metro_log_readings(self, metro: list[dict[str, Any]]) -> dict[str, Any]:
        """Corte, llegada a 0 y encoder de la pieza. Sin depósito ni delta."""
        feed = self._metro_pick(
            metro, "post-feed", "handoff", "c2-skip-feed", "start-skip-feed"
        )
        lin = self._metro_pick(metro, "post-lineal")
        home = self._metro_pick(metro, "post-home")
        enc = feed or lin
        return {
            "corte": None if lin is None else lin.get("asda"),
            "en0": None if home is None else home.get("asda"),
            "om_l": None if enc is None else enc.get("om_l"),
            "om_r": None if enc is None else enc.get("om_r"),
        }

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

    def _timing_real_breakdown(self) -> dict[str, float]:
        """Tiempo de pared del lote: total, prep (Start→pieza 1), piezas, cierre."""
        meta = self._timing_lot_meta or {}
        agg = self._timing_agg or {}
        now = time.monotonic()
        lot_t0 = float(meta.get("lot_t0") or now)
        total = max(0.0, now - lot_t0)
        first = agg.get("first_piece_start")
        prep = max(0.0, float(first) - lot_t0) if first is not None else total
        pieces_real = float(agg["real"][1]) if agg.get("real") else 0.0
        tail = max(0.0, total - prep - pieces_real)
        return {"total": total, "prep": prep, "pieces": pieces_real, "tail": tail}

    def _timing_summary_lines(self, *, final: bool, ok: bool, ct_sec: float) -> list[str]:
        meta = self._timing_lot_meta or {}
        agg = self._timing_agg or {}
        qty = int(meta.get("qty") or 0)
        n = int(agg.get("pieces") or 0)
        rb = self._timing_real_breakdown()
        if not final:
            ct_sec = float(agg["ct"][1]) if agg.get("ct") else 0.0
        diff = max(0.0, rb["total"] - ct_sec)
        events = list(self._timing_events)
        pauses = [e for e in events if e.get("kind") == "pause"]
        errors = [e for e in events if e.get("kind") != "pause"]
        pause_sum = sum(float(e.get("sec") or 0.0) for e in pauses)
        per_hour = (n / (rb["total"] / 3600.0)) if rb["total"] > 0 else 0.0
        now_txt = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        status = ("OK" if ok else "NO OK") if final else "EN CURSO"

        lines = [
            f"# Cycle timing — {meta.get('started', '')}",
            "",
            "## Resumen",
            "",
            f"- Resultado: **{status}**",
            f"- Piezas: **{n}**" + (f" / {qty}" if qty > 0 else ""),
            f"- Inicio: `{meta.get('started', '')}` · "
            + (f"Fin: `{now_txt}`" if final else f"Actualizado: `{now_txt}`"),
            f"- **Tiempo REAL** (reloj, Start → fin): **{rb['total']:.1f}s** "
            f"({self._fmt_hms(rb['total'])})",
            f"- **CT productivo** (sin pausas): **{ct_sec:.1f}s** "
            f"({self._fmt_hms(ct_sec)})",
            f"- Diferencia REAL − CT: **{diff:.1f}s** ({self._fmt_hms(diff)})",
            f"  - Prep (Start → inicio pieza 1): {rb['prep']:.1f}s "
            f"({self._fmt_hms(rb['prep'])})",
            f"  - Pausas: {pause_sum:.1f}s ({self._fmt_hms(pause_sum)})",
            f"  - Sin registrar entre pasos (comunicación / huecos / esperas no "
            f"medidas): {float(agg.get('unreg_sum') or 0.0):.1f}s "
            f"({self._fmt_hms(float(agg.get('unreg_sum') or 0.0))})",
            f"  - Cierre (última pieza → fin): {rb['tail']:.1f}s",
            f"- Piezas/hora (real): **{per_hour:.1f}**",
            f"- CT pieza: {self._stat_txt(agg.get('ct') or [0, 0.0, None, None])}",
            f"- Real pieza: {self._stat_txt(agg.get('real') or [0, 0.0, None, None])}",
            f"- Pausas: **{len(pauses)}** · Errores/eventos: **{len(errors)}**",
            f"- Longitud `{float(meta.get('length_mm') or 0):.1f}` mm · "
            f"Cantidad `{qty}` · RPM `{float(meta.get('rpm') or 0):g}` · "
            f"Lados `{meta.get('sides') or '—'}`",
            "",
            "CT = suma de tiempos de proceso sin pausas. REAL = reloj de pared: "
            "incluye pausas, esperas, recovery y tiempo entre pasos no medido.",
            "",
            "## Errores y eventos (cronológico)",
            "",
        ]
        if not errors:
            lines += ["_Ninguno._", ""]
        else:
            lines += [
                "| # | Hora | Pieza | Tipo | Código | Detalle |",
                "|--:|:-----|------:|:-----|:-------|:--------|",
            ]
            for i, e in enumerate(errors, 1):
                lines.append(
                    f"| {i} | `{e['t_start'].strftime('%H:%M:%S')}` | "
                    f"{e.get('rep') or '—'} | {e.get('kind')} | "
                    f"{e.get('code') or '—'} | {e.get('note') or '—'} |"
                )
            lines.append("")
            by_code: dict[str, int] = {}
            for e in errors:
                key = f"{e.get('kind')} · {e.get('code')}"
                by_code[key] = by_code.get(key, 0) + 1
            lines += ["### Conteo por tipo", ""]
            for key, cnt in sorted(by_code.items(), key=lambda kv: (-kv[1], kv[0])):
                lines.append(f"- {key}: **{cnt}**")
            lines.append("")

        lines += ["## Pausas (cronológico)", ""]
        if not pauses:
            lines += ["_Ninguna._", ""]
        else:
            lines += [
                "| # | Inicio | Fin | Pieza | Motivo | Duración |",
                "|--:|:-------|:----|------:|:-------|---------:|",
            ]
            for i, e in enumerate(pauses, 1):
                sec = float(e.get("sec") or 0.0)
                lines.append(
                    f"| {i} | `{e['t_start'].strftime('%H:%M:%S')}` | "
                    f"`{e['t_end'].strftime('%H:%M:%S')}` | {e.get('rep') or '—'} | "
                    f"`{e.get('code')}` | {sec:.1f}s ({self._fmt_hms(sec)}) |"
                )
            lines.append("")
            by_reason: dict[str, list[float]] = {}
            for e in pauses:
                by_reason.setdefault(str(e.get("code")), []).append(
                    float(e.get("sec") or 0.0)
                )
            lines += ["### Pausas por motivo", ""]
            for reason, vals in sorted(by_reason.items(), key=lambda kv: -sum(kv[1])):
                lines.append(
                    f"- `{reason}`: n={len(vals)} · total **{sum(vals):.1f}s** "
                    f"({self._fmt_hms(sum(vals))}) · max {max(vals):.1f}s"
                )
            lines.append("")

        metro_out = list(agg.get("metro_out") or [])
        if metro_out:
            lines += [
                f"## Anomalías encoder (|valor| ≥ {TIMING_ENCODER_OUTLIER_MM:.0f} mm) "
                f"— {int(agg.get('metro_out_n') or 0)}",
                "",
                "| Pieza | Corte | En 0 | Encoder L | Encoder R |",
                "|------:|------:|-----:|----------:|----------:|",
            ]
            for rep, r in metro_out:
                lines.append(
                    f"| {rep} | {self._fmt_mm(r.get('corte'))} | "
                    f"{self._fmt_mm(r.get('en0'))} | {self._fmt_mm(r.get('om_l'))} | "
                    f"{self._fmt_mm(r.get('om_r'))} |"
                )
            lines.append("")

        ops: dict[str, list[Any]] = agg.get("ops") or {}
        if ops:
            order = [k for k in TIMING_OP_ORDER if k in ops]
            order += sorted(k for k in ops if k not in order)
            lines += [
                "## Tiempo por paso (lote)",
                "",
                "| Paso | avg | min | max | n |",
                "|------|----:|----:|----:|--:|",
            ]
            for k in order:
                st = ops[k]
                cnt = int(st[0])
                if cnt <= 0:
                    continue
                lines.append(
                    f"| `{k}` | {st[1] / cnt:.3f} | {float(st[2]):.3f} | "
                    f"{float(st[3]):.3f} | {cnt} |"
                )
            lines.append("")
        return lines

    def _timing_write_summary(self, *, final: bool, ok: bool = False, ct_sec: float = 0.0) -> None:
        """Durante el lote: .md = solo resumen (índice/detalle en .tmp)."""
        path = self._timing_md_path
        if path is None:
            return
        try:
            lines = self._timing_summary_lines(final=final, ok=ok, ct_sec=ct_sec)
            lines += [
                "_Lote en curso: el índice por pieza y el detalle de pasos se agregan "
                "a este archivo al cerrar el lote._",
                "",
            ]
            path.write_text("\n".join(lines), encoding="utf-8")
        except Exception as exc:
            self._host.cycle_log(f"Timing: error escribiendo resumen ({exc})")

    def _timing_close_session(self, *, ok: bool, ct_sec: float) -> None:
        """Escribe .md final: resumen + índice por pieza + detalle de pasos."""
        path = self._timing_md_path
        if path is None:
            return
        with self._lock:
            self._sync_pause_exclusion_locked(time.monotonic())
        if self._piece_timings:
            self._timing_flush_piece(
                max(1, int(self._rep or 1)),
                max(1, int(self._total_reps or 1)),
                0.0,
            )
        index_path = self._timing_index_path
        detail_path = self._timing_detail_path
        rb = self._timing_real_breakdown()
        try:
            lines = self._timing_summary_lines(final=True, ok=ok, ct_sec=ct_sec)
            with path.open("w", encoding="utf-8") as out:
                out.write("\n".join(lines) + "\n")
                out.write(
                    "## Índice por pieza\n\n"
                    "Real = reloj entre fin de pieza anterior y fin de esta "
                    "(incluye pausas). Sin registrar = Real − Pasos − Pausas.\n\n"
                    "| Pieza | CT | Real | Pasos | Sin registrar | Pausas | "
                    "T. pausa | Fallos | Hora fin |\n"
                    "|------:|---:|-----:|------:|--------------:|-------:|"
                    "---------:|-------:|:---------|\n"
                )
                if index_path is not None and index_path.exists():
                    with index_path.open("r", encoding="utf-8") as src:
                        shutil.copyfileobj(src, out)
                out.write("\n## Detalle por pieza (pasos)\n\n")
                if detail_path is not None and detail_path.exists():
                    with detail_path.open("r", encoding="utf-8") as src:
                        shutil.copyfileobj(src, out)
                out.write(
                    f"\n_Generado: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}_\n"
                )
            for tmp in (index_path, detail_path):
                if tmp is not None:
                    try:
                        tmp.unlink()
                    except OSError:
                        pass
            self._host.cycle_log(
                f"Timing: guardado {path} · CT={ct_sec:.1f}s · "
                f"Real={rb['total']:.1f}s ({self._fmt_hms(rb['total'])})"
            )
        except Exception as exc:
            self._host.cycle_log(f"Timing: error cerrando .md ({exc})")
        self._timing_md_path = None
        self._timing_index_path = None
        self._timing_detail_path = None
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
        if not self._wait_motion(
            target_mm=0.0, reissue=lambda: self._host.cmd_motion_move_zero(rpm)
        ):
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
        if not self._wait_motion(
            target_mm=float(mm),
            reissue=lambda: self._host.cmd_motion_move_mm(float(mm), rpm),
        ):
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

        if not self._wait_motion(
            target_mm=0.0, reissue=lambda: self._host.cmd_motion_move_zero(rpm)
        ):
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

    def _wait_feed(
        self,
        *,
        log_ok: bool = True,
        apply_piece_watch: bool = True,
        timeout_s: float | None = None,
    ) -> bool:
        cfg = self.get_config()
        sides = self._feed_side_list()
        timeout_s = float(cfg.feed_wait_timeout_s if timeout_s is None else timeout_s)
        remaining = timeout_s
        while remaining > 0:
            if self._should_abort() or self._restart_piece:
                return False
            self._release_e050_finish_pause()
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
                    self._material_feed_done = True
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
                # NG ya recibido: cerrar ahora. Si no, tras Resume el wait consume
                # el NG viejo sin re-mandar feed y exige otro Reset→Resume.
                ng_now = [s for s, v in outcomes.items() if v == "ng"]
                if ng_now:
                    return self._fail_feed_ng(ng_now, outcomes)
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
                # LengthNG ≠ E009. E009 es error; NG es E002/E003 (recovery) u otro EXXX de Feed.
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
        """Cierra el wait por LengthNG sin pisar el EXXX real con E009 error."""
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

    def _do_pf_trigger(self, rep: int) -> bool:
        """Tfeed a lados de feedSides. True = OK / omitido; False = fault.

        Contrato Doc/pf_trigger.md: omite si pfTriggerEnabled=False, 1ª
        pieza del lote, o C2 (Tfeed ya mandado en esta pieza). Con helper
        # TFEED: el esclavo encola el trigger (independiente de AUTO/InProcess).
        """
        c2_skip = self._recovery_skip_pf_trigger
        self._recovery_skip_pf_trigger = False
        if not self.get_config().pf_trigger_enabled:
            self._host.cycle_log(
                "trigger PreFeeder: omitido (deshabilitado)"
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
            self._pf_trigger_sent_this_piece = True
            self._host.cycle_log(
                "trigger PreFeeder: omitido (recovery — Tfeed ya mandado)"
            )
            return True
        sides = CycleConfig.normalize_feed_sides(self.get_config().feed_sides)
        want_r = "R" in sides
        want_l = "L" in sides
        send_r = want_r
        send_l = want_l
        why_r = None if want_r else "lado"
        why_l = None if want_l else "lado"
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
        self._pf_trigger_sent_this_piece = True
        sec = self._end_op(op, ok=True)
        self._host.cycle_log(
            f"trigger PreFeeder Tfeed lados={sides} pieza={rep} — "
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
        """Buffer Full confirmado (Start / Resume / Continuar ciclo) antes de producir.

        No fía la caché previa: pide status y espera un snapshot nuevo.
        Vacío → el esclavo debe rellenar (Start + In process). Stop aborta.
        Timeout o EXXX PF (E052/E058) fallan el lote; no se alimenta a ciegas.
        """
        sides = self._feed_side_list()
        seq0 = self._host.pf_status_seq()
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
            fresh = self._host.pf_status_seq() > seq0
            reasons: list[str] = []
            all_ok = True
            for s in sides:
                ok, why = self._pf_side_buffer_full(s)
                if not ok:
                    all_ok = False
                reasons.append(why)
            if not fresh:
                all_ok = False
                reasons.append("status=?")
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

    def _ensure_pf_buffer_full_after_resume(
        self, reason: str = "Resume", *, force: bool = False
    ) -> bool:
        """Tras Resume / Continuar ciclo: Start + In process y espera Buffer Full.

        request_resume no bloquea: deja el flag; Busy/este método arman el relleno.
        Continuar ciclo no pasa por request_resume: force=True.
        La espera no suma a CT (Start también es prep fuera de reloj).
        """
        if self._e050_finishing_piece() or self._e050_materialist_wait:
            with self._lock:
                self._resume_need_buffer_full = False
            return True
        with self._lock:
            if not force and not self._resume_need_buffer_full:
                return True
            self._resume_need_buffer_full = False
        if not self._use_prefeeder():
            return True
        # Start + In process: rellenar si no hay Full; el ciclo no se corta por vacío.
        self._pf_arm_fill_for_buffer_full(reason)
        t0 = time.monotonic()
        ok = self._wait_pf_buffer_full_on_start(
            timeout_s=float(self.get_config().pf_ready_timeout_s),
            reason=reason,
        )
        with self._lock:
            if self._cycle_t0 is not None:
                self._pause_excluded_sec += max(0.0, time.monotonic() - t0)
        return ok

    def _pf_side_settled_for_idle(self, side: str) -> tuple[bool, str]:
        """True si el lado puede pasar a Idle sin cortar relleno/Tfeed.

        Tras eliminación de Holgura: Buffer Full + M2 quieto (sin Tfeed activo).
        """
        full = self._host.pf_buffer_full(side)
        filling = self._host.pf_auto_filling(side)
        trig = self._host.pf_trigger_active(side)
        if full is not True:
            return False, f"{side}:buffer≠Full"
        if filling is True:
            return False, f"{side}:rellenando"
        if trig is True:
            return False, f"{side}:Tfeed activo"
        return True, f"{side}:OK"

    def _wait_pf_settled_before_idle(self, timeout_s: float = 3.0) -> None:
        """Espera Buffer Full (+ M2 idle) antes de In process OFF.

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
                    f"PreFeeder: settled (Full+M2) → Idle ok · {why_txt}"
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
        # Sin reissue: tras Reset PLC las pinzas abren y repetir el lineal
        # daría una pieza de largo incorrecto → la pieza se re-arranca.
        if not self._wait_motion(target_mm=abs_mm):
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

    def _ensure_holder_encoder_closed_for_feed(self) -> bool:
        """Feed con Holder/Encoder abiertos: la rueda no toca → E028/E002/E003.

        Reset PLC 0x1E / All Off los dejan OFF aunque la secuencia siga a mitad
        de pieza (p. ej. error en HOME → Reset → Resume → feed post-HOME).
        False = abort.
        """
        if self._host.holder_encoder_closed():
            return True
        self._host.cycle_log(
            "Holder+Encoder abiertos antes del feed (Reset PLC / All Off) — re-cerrar"
        )
        self._arm_holder_encoder()
        return not self._pausable_delay(int(self.get_config().holder_on_ms or 0))

    def _ensure_asda_at_zero(self, *, reason: str = "Start") -> bool:
        """Si ASDA no está en 0 (caché Reached), MOVE_ZERO y esperar Reached.

        Start del lote y, si hace falta, re-home antes del feed post-HOME.
        """
        pos = self._host.asda_position_mm()
        if pos is not None and abs(float(pos)) <= ASDA_HOME_EPS_MM:
            self._host.cycle_log(
                f"ASDA ya en 0 (pos={float(pos):.2f} mm) — sin MOVE_ZERO ({reason})"
            )
            return True
        if pos is not None:
            self._host.cycle_log(
                f"ASDA pos={float(pos):.2f} mm ≠ 0 — MOVE_ZERO ({reason})"
            )
        else:
            self._host.cycle_log(
                f"ASDA pos desconocida — MOVE_ZERO ({reason})"
            )
        self._host.clear_motion_wait_flags()
        if not self._host.cmd_motion_move_zero(self._lot_rpm):
            self._raise_fault("home_cmd")
            return False
        if not self._wait_motion(
            target_mm=0.0,
            reissue=lambda: self._host.cmd_motion_move_zero(self._lot_rpm),
        ):
            return False
        return not self._should_abort()

    def _prepare_before_cut(self) -> bool:
        # Start: tools a seguro + Holder/Encoder ON + ASDA en 0 confirmado.
        # HOME entre piezas: paso home; feed de la siguiente solo con ASDA=0.
        self._host.cycle_log(
            "prepareBeforeCut: cutters/grippers safe + Holder/Encoder cerrados"
        )
        self._host.cmd_plc_tools_safe()
        self._arm_holder_encoder()
        if self._should_abort():
            return False
        return self._ensure_asda_at_zero()
    def _confirm_asda_at_zero_for_feed(self) -> bool:
        """Tras HOME: alimentar solo si ASDA está en 0 (caché TCP / Reached)."""
        pos = self._host.asda_position_mm()
        if pos is not None and abs(float(pos)) <= ASDA_HOME_EPS_MM:
            self._host.cycle_log(
                f"ASDA en 0 confirmado (pos={float(pos):.2f} mm) — feed permitido"
            )
            return True
        if pos is not None:
            self._host.cycle_log(
                f"ASDA pos={float(pos):.2f} mm ≠ 0 tras HOME — "
                "MOVE_ZERO antes del feed"
            )
        else:
            self._host.cycle_log(
                "ASDA pos desconocida tras HOME — MOVE_ZERO antes del feed"
            )
        return self._ensure_asda_at_zero(reason="post-HOME")

    def _run_feed_after_home(self, rep: int, qty: int) -> bool:
        """Feed de la *siguiente* pieza. Exige ASDA en 0. Tfeed ya fue (post-corte)."""
        # Pieza actual ya cortada: el ready anterior no vale para la siguiente.
        self._material_feed_done = False
        if self._e050_materialist_requested:
            self._host.cycle_log("Feed post-HOME: omitido (ruta E050 Materialista)")
            return True
        if self._tray_full_after(rep, qty):
            self._host.cycle_log("Feed post-HOME: omitido (tray lleno — espera vaciado)")
            return True
        if not self._confirm_asda_at_zero_for_feed():
            return False
        if int(rep) >= int(qty):
            self._host.cycle_log("Feed post-HOME: omitido (última pieza)")
            return True
        if self._feed_reference_visible():
            self._material_feed_done = True
            self._metro_snap("post-feed-next")
            self._host.cycle_log(
                "Feed post-HOME omitido (láser ya ON) — handoff listo"
            )
            return True
        return self._run_feed(
            apply_piece_watch=False, timing_tag="feed_next", metro_tag="post-feed-next"
        )

    def _run_feed(
        self,
        *,
        skip_validate: bool = False,
        feed_mm: float | None = None,
        apply_piece_watch: bool = True,
        timing_tag: str = "feed",
        metro_tag: str = "post-feed",
        wait_timeout_s: float | None = None,
    ) -> bool:
        if not self._ensure_holder_encoder_closed_for_feed():
            return False
        self._host.clear_motion_wait_flags()
        sides = self._feed_side_list()
        cmd_op = self._begin_op(f"{timing_tag}_cmd")
        failed: list[str] = []
        # Purga: skipValidate sin mm → creep a láser (lados independientes).
        feed_arg = None if skip_validate else feed_mm
        if "L" in sides:
            if not self._host.cmd_motion_feed_l(
                skip_validate=skip_validate, feed_mm=feed_arg
            ):
                failed.append("L")
        if "R" in sides:
            if not self._host.cmd_motion_feed_r(
                skip_validate=skip_validate, feed_mm=feed_arg
            ):
                failed.append("R")
        ok_all = not failed
        cmd_sec = self._end_op(cmd_op, ok=ok_all)
        note = (
            f" (purga: hasta láser · timeout {REFILL_LASER_TIMEOUT_S:g} s)"
            if skip_validate
            else ""
        )
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
        op = self._begin_op(timing_tag)
        use_timeout = wait_timeout_s
        if use_timeout is None and skip_validate:
            use_timeout = REFILL_LASER_TIMEOUT_S + 2.0
        if not self._wait_feed(
            log_ok=False,
            apply_piece_watch=apply_piece_watch,
            timeout_s=use_timeout,
        ):
            self._end_op(op, ok=False)
            self._metro_snap(metro_tag)
            return False
        sec = self._end_op(op, ok=True)
        self._metro_snap(metro_tag)
        if not skip_validate:
            # LengthOK incluye el relativo offL/offR en Motion (láser puede quedar OFF).
            self._material_feed_done = True
        self._host.cycle_log(
            f"Feed OK lados={''.join(sides)} · wait={self._fmt_op(sec)} "
            f"cmd={self._fmt_op(cmd_sec)}"
        )
        return True

    def _wait_refill_operator_decision(self, prompt: str) -> str:
        """'ok' | 'reject' | 'retry'. Abort/Stop → 'reject'.

        prompt: await_feed (Alimentar láser / Next Cutting)
              | after_feed (Reintentar láser / Next Cutting)
              | after_cut (Next ASDA 0).
        """
        with self._lock:
            self._refill_prompt = prompt
            self._refill_awaiting_confirm = True
        self._refill_confirm.clear()
        self._refill_reject.clear()
        self._refill_retry.clear()
        self._pause.set()
        self._enter_pause_andon()
        if prompt == "await_feed":
            self._host.cycle_log(
                f"Refill: park listo — Alimentar hasta láser "
                f"(timeout {REFILL_LASER_TIMEOUT_S:g} s) o Next → Cutting"
            )
        elif prompt == "after_feed":
            if self._refill_skip_cut:
                self._host.cycle_log(
                    "Refill: láser OK — Reintentar o Continuar → ASDA a 0"
                )
            else:
                self._host.cycle_log(
                    "Refill: láser OK — Reintentar o Next → Cutting"
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
                if prompt in ("await_feed", "after_feed") and self._refill_retry.is_set():
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
        current_asda = self._host.asda_position_mm()
        if current_asda is None:
            self._host.cycle_log("Refill: posición ASDA no disponible → ejecutar MOVE de park")
        elif abs(float(current_asda) - float(asda_mm)) <= 0.5:
            self._host.cycle_log(
                f"Refill: ASDA ya en {current_asda:g} mm ≈ {asda_mm:g} mm → skip MOVE"
            )
        elif not self._host.cmd_motion_move_mm(asda_mm, rpm):
            self._raise_fault("move_cmd")
            return "fail"
        if current_asda is None or abs(float(current_asda) - float(asda_mm)) > 0.5:
            if (
                not self._wait_motion(
                    target_mm=asda_mm,
                    reissue=lambda: self._host.cmd_motion_move_mm(asda_mm, rpm),
                )
                or self._should_abort()
            ):
                return "fail"
        elif self._should_abort():
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

        fed_once = False
        while True:
            if not fed_once:
                decision = self._wait_refill_operator_decision("await_feed")
                if decision == "ok":
                    break
                if decision != "retry":
                    return "cancel"
                self._refill_next_feed_mm = None
            self._host.cycle_log(
                f"Refill: alimentar hasta láser ON "
                f"(timeout {REFILL_LASER_TIMEOUT_S:g} s · sin OM · lados independientes)"
            )
            if not self._run_feed(skip_validate=True, feed_mm=None):
                return "fail"
            fed_once = True
            with self._lock:
                self._progress = 55
            if self._should_abort():
                return "fail"

            decision = self._wait_refill_operator_decision("after_feed")
            if decision == "ok":
                break
            if decision == "retry":
                self._refill_next_feed_mm = None
                self._host.cycle_log(
                    "Refill: reintento — otra vez hasta láser (ASDA en park)"
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
        if (
            not self._wait_motion(
                target_mm=0.0, reissue=lambda: self._host.cmd_motion_move_zero(rpm)
            )
            or self._should_abort()
        ):
            return "fail"
        with self._lock:
            self._progress = 100
        self._host.cycle_log("Refill OK — ASDA en 0")
        return "ok"

    def _run_refill(self, rpm: float, feed_mm: float, asda_mm: float) -> None:
        """ASDA park → holder → Alimentar (láser) ↔ reintento → cut → home."""
        with self._lock:
            self._active = True
            self._refill_mode = True
            self._pieces_done = 0
            self._total_reps = 1
            self._pieces_per_rep = 1
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
        """Batch dentro del tray actual (1…tray_batches); tras vaciar vuelve a 1."""
        batch = max(1, int(self.get_config().deposit_batch_size))
        idx = (max(1, int(rep)) - 1) // batch
        if self._tray_batches:
            idx %= self._tray_batches
        return idx + 1

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

    def _mark_cutter_set(self) -> None:
        self._cutter_set_mono = time.monotonic()

    def _mark_cutter_res(self) -> None:
        self._cutter_res_mono = time.monotonic()

    def _wait_ms_op(self, ms: int, op_name: str, log_txt: str) -> bool:
        """Delay interno con timing. True = abort."""
        wait = max(0, int(ms))
        if wait <= 0:
            return self._should_abort()
        self._host.cycle_log(log_txt)
        op = self._begin_op(op_name)
        aborted = self._pausable_delay(wait)
        self._end_op(op, ok=not aborted)
        return aborted

    def _plc_precut_drain_remain_ms(self, already_ms: int) -> int:
        """Ms que faltan para que el PLC termine Holder+Encoder precut."""
        need = PLC_PRECUT_VALVE_CMDS * PLC_VALVE_SLOT_MS
        return max(0, need - max(0, int(already_ms)))

    def _ensure_cutter_settled_before_travel(self, cut_sides: str) -> bool:
        """True = abort. ASDA no se mueve hasta KEEP Set/Res asentado.

        El Set HMI va fire-and-forget: si Holder+Encoder aún ocupan el ESP,
        el pulso real del cortador llega tarde. El reloj cuenta desde el Set.
        """
        self._host.cmd_plc_cutters(False, sides=cut_sides, force=True)
        need_from_set_ms = (
            2 * PLC_VALVE_SLOT_MS
            + max(
                int(self.get_config().cutter_post_ms or 0),
                int(CUTTER_SETTLE_BEFORE_DEPOSIT_MS),
            )
        )
        now = time.monotonic()
        t0 = self._cutter_set_mono or self._cutter_res_mono
        if t0 is None:
            self._cutter_set_mono = now
            remain = need_from_set_ms
        else:
            remain = int(max(0.0, need_from_set_ms - (now - t0) * 1000.0))
        if remain > 0:
            return self._wait_ms_op(
                remain,
                "cutter_settle",
                f"Post-corte: espera asiento {remain} ms "
                f"(mín {need_from_set_ms} ms desde Set; "
                f"slots PLC {2 * PLC_VALVE_SLOT_MS} ms + carrera)",
            )
        self._host.cycle_log(
            f"Post-corte: cortador asentado (ya ≥{need_from_set_ms} ms desde Set)"
        )
        return False

    def _deposit_tray_capacity(
        self, length_mm: float
    ) -> tuple[int | None, str | None]:
        """(batches por tray, error). None batches = sin tope de carrera.

        Error solo si ni el lineal ni el 1er batch caben. Si el lote no cabe
        entero, el ciclo pausa al llenar el tray (vaciar) y reinicia batches.
        """
        cfg = self.get_config()
        L = abs(float(length_mm))
        target_mm = L + float(cfg.cut_offset_mm)
        gap = max(0.0, float(cfg.deposit_stack_gap_mm))
        extra0 = float(cfg.deposit_extra_mm)
        clearance = abs(float(cfg.gripper_clearance_mm))
        max_travel = abs(float(cfg.deposit_max_travel_mm))
        if max_travel <= 0:
            return None, None
        if abs(target_mm) > max_travel + 1e-6:
            return None, (
                f"Lineal {abs(target_mm):.1f} mm > carrera máx "
                f"{max_travel:g} mm"
            )
        base = target_mm + extra0
        if abs(base) + clearance > max_travel + 1e-6:
            return None, (
                f"Depósito fuera de carrera: 1er batch alcance "
                f"{abs(base) + clearance:.1f} mm (depósito {base:.1f} + "
                f"despeje {clearance:g}) > máx {max_travel:g} mm "
                f"con L={L:g}, extra={extra0:g}"
            )
        step = L + gap
        if step <= 1e-9:
            return None, None
        if base >= 0:
            return 1 + int(max(0.0, max_travel - clearance - base) // step), None
        # Extra negativo: al apilar hacia +L primero se acerca a 0.
        n = 1
        while n < 100000:
            d = base + n * step
            if abs(d) + clearance > max_travel + 1e-6:
                break
            n += 1
        return n, None

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
        self._host.cycle_log(
            "Cycle flow: feed post-HOME (ASDA=0) — sin prefetch paralelo"
        )
        try:
            self._timing_open_session(
                length_mm=length_mm, qty=qty, rpm=rpm
            )
            if not self._prepare_before_cut():
                self._finish(False)
                return
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
                # Start ya se mandó. In process ON abre el relleno (servo→DeReeler
                # si no hay Full) y se queda ON el lote: vacío mid-ciclo = seguir
                # alimentando, no parar. Solo EXXX del PF detiene.
                if self._host.cmd_pf_in_process(True):
                    self._pf_held_idle = False
                    self._host.cycle_log("PreFeeder: In process ON (ciclo Busy)")
                else:
                    self._host.cycle_log("PreFeeder: In process ON falló")
                if not self._wait_pf_buffer_full_on_start(
                    timeout_s=float(self.get_config().pf_ready_timeout_s)
                ):
                    self._finish(False)
                    return
            start_rep = 1
            while self._run_pieces(length_mm, qty, rpm, start_rep):
                start_rep = max(1, int(self._pieces_done) + 1)
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

    def _run_pieces(
        self, length_mm: float, qty: int, rpm: float, start_rep: int
    ) -> bool:
        """Bucle de piezas desde start_rep. True = excepción interna ya en
        Pause y el operador hizo Resume → reentrar desde la pieza en curso.
        Una excepción no cierra el lote (solo Stop / Abortar / fin de lote)."""
        try:
            target_mm = 0.0
            completed = start_rep - 1
            handoff_ready = False
            for rep in range(start_rep, qty + 1):
                self._pf_trigger_sent_this_piece = False
                while True:
                    early_exit = True
                    materialist_only = False
                    if self._lot_purge_request is not None:
                        self._run_lot_purge()
                        if self._should_abort():
                            break
                        handoff_ready = False
                        self._recovery_skip_feed = True
                        continue
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
                        self._cutter_set_mono = None
                        self._cutter_res_mono = None
                        # 1–2 Holder+Encoder ON + delay (1ª pieza; y tras recovery,
                        # porque Reset PLC 0x1E deja Holder/Encoder OFF y el feed
                        # sin rueda de encoder da E028/E002/E003).
                        recovery_restart = self._recovery_skip_feed
                        if rep == 1 or recovery_restart:
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
                        # Pause/error en HOME puede dejar ASDA sin confirmar en 0.
                        if recovery_restart and not self._ensure_asda_at_zero(
                            reason="recovery"
                        ):
                            break
                        # 3 Feed / ya listo post-HOME
                        if self._enter(rep, qty, "feed"):
                            break
                        if handoff_ready:
                            handoff_ready = False
                            self._recovery_skip_feed = False
                            self._material_feed_done = True
                            self._metro_snap("handoff")
                            self._host.cycle_log(
                                "Feed: handoff (ya alimentado post-HOME, ASDA=0)"
                            )
                        elif self._material_feed_done and (
                            self._recovery_skip_feed or self._recovery_after_error
                        ):
                            # Ya hubo LengthOK (+ offset Motion). Tras offL/offR el
                            # láser suele estar OFF: no re-alimentar ni reaplicar offset.
                            self._recovery_skip_feed = False
                            sides_txt = "".join(self._feed_side_list())
                            self._metro_snap("feed-done-skip")
                            self._host.cycle_log(
                                f"Feed omitido (ya alimentado+offset) lados={sides_txt}"
                            )
                        elif (
                            (
                                rep == 1
                                or self._recovery_skip_feed
                                or self._recovery_after_error
                            )
                            and self._feed_reference_visible()
                        ):
                            c2_or_rec = (
                                self._recovery_skip_feed or self._recovery_after_error
                            )
                            self._recovery_skip_feed = False
                            self._material_feed_done = True
                            sides_txt = "".join(self._feed_side_list())
                            if c2_or_rec:
                                self._metro_snap("c2-skip-feed")
                                self._host.cycle_log(
                                    f"Feed omitido (recovery/recovery): láser ya ON "
                                    f"lados={sides_txt}"
                                )
                            else:
                                self._metro_snap("start-skip-feed")
                                self._host.cycle_log(
                                    f"Feed omitido (referencia láser visible) "
                                    f"lados={sides_txt}"
                                )
                        else:
                            self._recovery_skip_feed = False
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
                        # 8–11 Holder+Encoder abren para lineal + MOVE TCP + delay
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
                        # 12–13 Cerrar Holder+Encoder de nuevo pre-corte
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
                        precut_wait = max(0, int(self.get_config().holder_on_ms or 0))
                        drain_ms = self._plc_precut_drain_remain_ms(precut_wait)
                        if drain_ms > 0 and self._wait_ms_op(
                            drain_ms,
                            "plc_precut_drain",
                            f"PLC: drena Holder+Encoder {drain_ms} ms "
                            f"antes del cortador "
                            f"({PLC_PRECUT_VALVE_CMDS}×{PLC_VALVE_SLOT_MS} ms "
                            f"− {precut_wait} ms ya esperados)",
                        ):
                            break
                        # 14–17 Corte solo en lados de feed (evita pulsar el cortador vacío)
                        # force=True: no omitir Set/Res por caché HMI desfasada.
                        # Si abortamos tras Set, Res de emergencia antes del break.
                        cut_sides = CycleConfig.normalize_feed_sides(self.get_config().feed_sides)
                        if self._enter(rep, qty, "cutter_on"):
                            break
                        # recovery finish-piece: cortar aunque PF siga en ErrorState (p.ej. Buffer Max).
                        # Solo actuar con EXXX/errorAny real — no por 0x3C huérfano.
                        # Set del EXXX aquí: un break mudo dejaba Andon en Error y HMI verde.
                        if (
                            self._use_prefeeder()
                            and self._host.pf_has_fault()
                        ):
                            self._raise_current_pf_fault("Corte: PreFeeder en error")
                            if (
                                self._recovery_after_error
                                or self._e050_finish_piece
                            ):
                                self._host.cycle_log(
                                    "recovery: completar corte pese a EXXX PF"
                                )
                            elif self._should_abort():
                                self._host.cycle_log("Corte abortado: PreFeeder Error")
                                break
                            elif self._pause.is_set():
                                if self._wait_paused_for_resume(
                                    "recovery: Pause por EXXX PF antes del corte"
                                ):
                                    break
                                break
                            else:
                                self._host.cycle_log("Corte abortado: PreFeeder Error")
                                break
                        cut_op = self._begin_op("corte")
                        self._host.cycle_log(f"Cortador ON (Set) lados={cut_sides}")
                        self._host.cmd_plc_cutters(True, sides=cut_sides, force=True)
                        self._mark_cutter_set()
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
                        # 18 Tfeed post-corte (1ª omite; 2…N incluido última; C2 si ya se mandó)
                        if self._enter(rep, qty, "pf_trigger"):
                            break
                        if not self._do_pf_trigger(rep):
                            break
                        if self._after_step("pf_trigger"):
                            break
                        # No mover ASDA (depósito ni despeje) hasta KEEP asentado.
                        if self._ensure_cutter_settled_before_travel(cut_sides):
                            break
                        # 19–20 Depósito + delay. Feed de la siguiente: tras HOME (ASDA=0).
                        # wip_pos_signed = posición firmada confirmada; wip_start_mm = |pos|
                        # (fuente soplo WIP fin; se actualiza tras despeje post-pinzas).
                        wip_pos_signed: float | None = None
                        wip_start_mm: float | None = None
                        if self._enter(rep, qty, "deposit"):
                            break
                        extra = self._deposit_extra_mm(rep)
                        if abs(extra) > 0.01:
                            deposit_target = target_mm + extra
                            batch_n = self._deposit_batch_index(rep)
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
                            if not self._wait_motion(
                                target_mm=deposit_target,
                                reissue=lambda: self._host.cmd_motion_move_mm(
                                    deposit_target, rpm
                                ),
                            ):
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
                        # 21–22 Pinzas OFF + delay
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
                        # 23 Despeje ASDA tras abrir pinzas → actualiza ref WIP (soplo fin)
                        if self._enter(rep, qty, "gripper_clearance"):
                            break
                        clr = abs(float(self.get_config().gripper_clearance_mm))
                        if clr > 0.01 and wip_pos_signed is not None:
                            clearance_target = self._clearance_target_mm(
                                wip_pos_signed, float(target_mm), clr
                            )
                            self._arm_motion_leg(False)
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
                            if not self._wait_motion(
                                target_mm=clearance_target,
                                reissue=lambda: self._host.cmd_motion_move_mm(
                                    clearance_target, rpm
                                ),
                            ):
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
                        # 24 HOME + WIP continuo (match-lineal) → 0
                        if self._enter(rep, qty, "home"):
                            break
                        if not self._home_with_wip_delivery(
                            wip_start_mm,
                            self._lot_rpm,
                            prefetch_running=False,
                        ):
                            break
                        if self._after_step("home"):
                            break
                        # 25 Feed de la siguiente: solo con ASDA en 0
                        if self._enter(rep, qty, "feed_after_home"):
                            break
                        if not self._run_feed_after_home(rep, qty):
                            if self._restart_piece or self._fault:
                                break
                            if self._recovery_after_error:
                                self._host.cycle_log(
                                    "Feed post-HOME falló — recovery sigue a "
                                    "post_piece (Pause; Reset→Resume)"
                                )
                            else:
                                self._raise_fault("feed_incomplete")
                                break
                        elif int(rep) < int(qty):
                            handoff_ready = True
                        if self._after_step("feed_after_home"):
                            break
                        # 26–27 Asentar + post-pieza
                        if self._do_wait(rep, qty, "wait_asentar", "asentar_ms"):
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
                            self._recovery_skip_feed = True
                            self._recovery_skip_pf_trigger = False
                        elif self._recovery_after_error:
                            if not self._recovery_review_purge_decide():
                                early_exit = True
                                break
                            self._recovery_after_error = False
                            handoff_ready = False
                            self._recovery_skip_feed = True
                            self._recovery_skip_pf_trigger = False
                        if self._tray_full_after(rep, qty):
                            if not self._wait_tray_emptied(rep, qty):
                                early_exit = True
                                break
                            handoff_ready = False
                            self._recovery_skip_feed = True
                        early_exit = False
                    if materialist_only:
                        continue
                    if not early_exit:
                        break  # pieza OK → siguiente rep
                    if self._consume_restart_piece():
                        handoff_ready = False
                        self._recovery_skip_feed = True
                        self._host.cycle_log(
                            f"recovery: reinicio pieza {rep}/{qty} desde step 0"
                        )
                        continue
                    if (
                        (self._recovery_after_error or self._fault)
                        and not self._should_abort()
                        and self._wait_recovery_resume_hold()
                    ):
                        handoff_ready = False
                        self._recovery_skip_feed = True
                        self._recovery_skip_pf_trigger = bool(
                            self._pf_trigger_sent_this_piece
                        )
                        continue
                    break  # fallo / abort → salir del while
                # Critico: el break anterior solo sale del while; sin esto el for
                # seguía con Tfeed/Feed en las piezas restantes (spam PF + E009).
                if early_exit:
                    break
            self._finish(completed >= qty and not self._aborted and not self._fault)
            return False
        except Exception as exc:
            self._host.cycle_log(
                f"Cycle exception: {exc} — lote en Pause (progreso conservado)"
            )
            with self._lock:
                self._refill_mode = False
                self._refill_awaiting_confirm = False
                self._refill_prompt = ""
            self._suspend_piece_watch = False
            self._raise_fault(f"exception:{exc}")
            if self._should_abort() or not self._wait_recovery_resume_hold():
                self._finish(False)
                return False
            self._restart_piece = False
            self._flow_interrupt.clear()
            self._recovery_skip_feed = True
            self._recovery_skip_pf_trigger = bool(self._pf_trigger_sent_this_piece)
            self._host.cycle_log(
                f"Cycle: Resume tras excepción → reinicio pieza "
                f"{int(self._pieces_done) + 1}/{qty} desde step 0"
            )
            return True

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
        self._lot_purge_request = None
        self._suspend_piece_watch = False
        self._recovery_skip_feed = False
        self._material_feed_done = False
        self._recovery_skip_pf_trigger = False
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
        # Stop/error/recovery no tocan PLC. Fin de lote OK: tools safe + holder/enc.
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
        # aquí: enclava PF-007; Stop de operador / error ya mandaron 0x2B.
        # No cortar en seco: esperar Buffer Full (+ M2 idle).
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
        real_sec = 0.0
        if not refill and self._timing_md_path is not None:
            real_sec = self._timing_real_breakdown()["total"]
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
                real_txt = (
                    f" · Real={real_sec:.1f}s ({self._fmt_hms(real_sec)})"
                    if real_sec > 0
                    else ""
                )
                self._host.cycle_log(
                    (
                        f"Lote completado — "
                        f"{int(self._total_reps) * max(1, int(self._pieces_per_rep))}/"
                        f"{int(self._total_reps) * max(1, int(self._pieces_per_rep))} pz"
                        if max(1, int(self._pieces_per_rep)) > 1
                        else f"Lote completado — {self._total_reps}/{self._total_reps} piezas"
                    )
                    + ct_txt
                    + real_txt
                )
            # Tras FinishParts: Idle máquina (PF ya Idle por In process OFF).
            self._set_state(TX_IDLE)
        # Lote cerrado: no hay Resume que rearma; el próximo Start arma de nuevo.
        self._pf_held_idle = False
        self._host.cycle_notify()
