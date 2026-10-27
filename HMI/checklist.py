"""Checklist de arranque — sesión persistente, pasos guiados, bloqueo de Start."""

from __future__ import annotations

import json
import os
import threading
import time
from datetime import datetime, timezone
from typing import Any, Callable

# Banco / desarrollo: omitir revisión de arranque al reiniciar la HMI.
# Producción: False, o lanzar con HMI_CHECKLIST_SKIP=0
CHECKLIST_SKIP_ON_BOOT = True


def _skip_checklist_on_boot() -> bool:
    env = os.environ.get("HMI_CHECKLIST_SKIP", "").strip().lower()
    if env in ("1", "true", "yes"):
        return True
    if env in ("0", "false", "no"):
        return False
    return CHECKLIST_SKIP_ON_BOOT


CHECKLIST_COUNTER_MAX = 10_000
CHECKLIST_MEASURE_TOL_MM = 2.0
# Longitud de pieza física = medida del operador + 55 mm (modelo HMI sin esos 55).
CHECKLIST_PIECE_EXTRA_MM = 55.0
CHECKLIST_AIR_SETTLE_MS = 200
CHECKLIST_ASDA_TRAVEL_FRACTION = 0.85  # fracción de depositMaxTravelMm
# Misma idea que CycleRunner._wait_motion: Idle sin destino no cuenta como llegada.
CHECKLIST_ASDA_TARGET_TOL_MM = 5.0
CHECKLIST_ASDA_IDLE_OFF_TARGET_S = 1.0

STEP_IDS = (
    "cycle_counter",
    "air_pressure",
    "pf_holgura",
    "asda_roundtrip",
    "feeder_can_purge",
    "piece_measure",
)


def _utc_now_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


class _ChecklistCancelled(Exception):
    """El operador cerró el checklist con un trabajo en curso: no seguir."""


class ChecklistManager:
    """Estado y acciones del checklist de arranque (HMI Main)."""

    def __init__(
        self,
        *,
        log_fn: Callable[[str], None],
        notify_fn: Callable[[], None],
    ) -> None:
        self._log = log_fn
        self._notify = notify_fn
        self._lock = threading.RLock()
        self._required = True
        self._last_completed_at: str | None = None
        self._session: dict[str, Any] | None = None
        self._worker: threading.Thread | None = None
        self._worker_busy = False
        self._worker_cancel: threading.Event | None = None
        self._worker_error = ""
        self._worker_phase = ""
        self._air_test_active = False
        self._air_test_verified = False
        self._piece_test_started = False
        self._step_error = ""

    # --- persistencia ---

    def load_from_config(self, data: dict[str, Any]) -> None:
        with self._lock:
            self._required = bool(data.get("checklistRequired", True))
            self._last_completed_at = data.get("checklistLastCompletedAt")
            raw = data.get("checklistSession")
            self._session = dict(raw) if isinstance(raw, dict) else None
            self._normalize_session_index_locked()
            self._invalidate_unfinished_motion_checks_locked()
            self._repair_stuck_session_locked()
            self._sync_required_with_session_locked()
            self._restore_runtime_flags_from_session_locked()

    def export_to_config(self) -> dict[str, Any]:
        with self._lock:
            return {
                "checklistRequired": self._required,
                "checklistLastCompletedAt": self._last_completed_at,
                "checklistSession": self._session,
            }

    def mark_required_on_boot(self) -> None:
        """Arranque de la HMI (encendido / reinicio del programa): revisión obligada
        y sin sesión a medias guardada en disco."""
        with self._lock:
            self._required = not _skip_checklist_on_boot()
            self._session = None
            self._worker_error = ""
            self._worker_phase = ""
            self._step_error = ""
            self._air_test_active = False
            self._air_test_verified = False
            self._piece_test_started = False
        self._notify()

    # --- API snapshot ---

    def snapshot(self, host: Any) -> dict[str, Any]:
        finalize = False
        with self._lock:
            self._normalize_session_index_locked()
            finalize = self._repair_stuck_session_locked()
        if finalize:
            res = self.complete_checklist(host)
            if res.get("ok") and hasattr(host, "_save_app_config"):
                host._save_app_config()

        with self._lock:
            idx = int(self._session.get("currentStepIndex", 0)) if self._session else 0
            step_id = (
                STEP_IDS[idx] if self._session and 0 <= idx < len(STEP_IDS) else ""
            )
            # El estado puede fijar marcas en la sesión (purga vista / hecha /
            # fallida): calcularlo antes de copiarla para que la respuesta
            # ya las traiga y la UI ofrezca «Reintentar» sin otro refresco.
            status = self._step_status_with_motion(
                host, step_id, self._session_copy_locked()
            )
            session = self._session_copy_locked()
            return {
                "required": self._required or self._session_incomplete_locked(),
                "lastCompletedAt": self._last_completed_at,
                "session": session,
                "currentStepId": step_id,
                "workerBusy": self._worker_busy,
                "workerPhase": self._worker_phase,
                "workerError": self._worker_error,
                "stepError": self._step_error,
                "airTestActive": self._air_test_active,
                "airTestVerified": self._air_test_verified,
                "pieceTestStarted": self._piece_test_started,
                "stepStatus": status,
            }

    def _step_status_with_motion(
        self, host: Any, step_id: str, session: dict[str, Any] | None
    ) -> dict[str, Any]:
        status = self._step_status_locked(host, step_id, session)
        status["motionBlocked"] = self._machine_in_motion(host)
        return status

    def _machine_in_motion(self, host: Any) -> bool:
        if self._worker_busy:
            return True
        try:
            if host.motion_busy():
                return True
        except Exception:  # noqa: BLE001
            pass
        cycle = getattr(host, "_cycle", None)
        if cycle is None:
            return False
        if cycle.is_active() or cycle.is_purge_busy():
            return True
        snap = cycle.snapshot()
        return bool(snap.get("refillActive"))

    def _session_copy_locked(self) -> dict[str, Any] | None:
        if not self._session:
            return None
        return json.loads(json.dumps(self._session))

    @staticmethod
    def _step_index_from_completed(completed: list[str]) -> int:
        done = set(completed)
        for i, sid in enumerate(STEP_IDS):
            if sid not in done:
                return i
        return len(STEP_IDS)

    def _normalize_session_index_locked(self) -> None:
        if not self._session:
            return
        completed = list(self._session.get("completedSteps") or [])
        self._session["currentStepIndex"] = self._step_index_from_completed(completed)

    def _invalidate_unfinished_motion_checks_locked(self) -> None:
        """Descarta marcas físicas guardadas si el paso no se confirmó.

        Una sesión restaurada no debe habilitar «Continuar» por marcas
        previas (p. ej. la antigua omisión sin enlace): si el paso del
        actuador o del alimentador no quedó confirmado, se vuelve a ejecutar.
        """
        if not self._session:
            return
        completed = list(self._session.get("completedSteps") or [])
        if "asda_roundtrip" not in completed:
            self._session.pop("asdaOk", None)
            # Tras restaurar, el operador vuelve a llevar el eje a su
            # posición inicial: una confirmación vieja no vale.
            self._session.pop("asdaHomeAwaitConfirm", None)
            self._session.pop("asdaHomeError", None)
        if "feeder_can_purge" not in completed:
            self._session.pop("stage2Ok", None)
            self._reset_purge_flags_locked()

    def _restore_runtime_flags_from_session_locked(self) -> None:
        if not self._session:
            self._piece_test_started = False
            self._air_test_verified = False
            self._air_test_active = False
            return
        self._piece_test_started = bool(self._session.get("pieceTestStarted"))
        self._air_test_verified = bool(self._session.get("airTestVerified"))
        self._air_test_active = False

    def has_alarm(self) -> bool:
        with self._lock:
            return bool(self._step_error or self._worker_error)

    def set_step_error(self, code: str) -> None:
        with self._lock:
            self._step_error = str(code or "").strip()

    def clear_step_error(self) -> None:
        with self._lock:
            self._step_error = ""

    def _repair_stuck_session_locked(self) -> bool:
        """Si la sesión pasó el último paso, reparar índice o pedir finalizar."""
        if not self._session:
            return False
        idx = int(self._session.get("currentStepIndex", 0))
        if idx < len(STEP_IDS):
            return False
        completed = set(self._session.get("completedSteps") or [])
        if all(sid in completed for sid in STEP_IDS):
            return True
        for i, sid in enumerate(STEP_IDS):
            if sid not in completed:
                self._session["currentStepIndex"] = i
                break
        return False

    def _session_incomplete_locked(self) -> bool:
        if not self._session:
            return False
        idx = int(self._session.get("currentStepIndex", 0))
        if idx < len(STEP_IDS):
            return True
        completed = set(self._session.get("completedSteps") or [])
        return not all(sid in completed for sid in STEP_IDS)

    def _sync_required_with_session_locked(self) -> None:
        if self._session_incomplete_locked():
            self._required = True

    def is_start_blocked(self) -> bool:
        with self._lock:
            return self._required or self._session_incomplete_locked()

    def allow_checklist_piece_start(self) -> bool:
        with self._lock:
            if not self._session:
                return False
            idx = int(self._session.get("currentStepIndex", 0))
            return STEP_IDS[idx] == "piece_measure" and not self._piece_test_started

    def is_piece_test_started(self) -> bool:
        """La prueba de 2 piezas del checklist ya arrancó (lote propio)."""
        with self._lock:
            if not self._session or not self._piece_test_started:
                return False
            idx = int(self._session.get("currentStepIndex", 0))
            return 0 <= idx < len(STEP_IDS) and STEP_IDS[idx] == "piece_measure"

    # --- sesión ---

    def start_session(self, employee_id: str) -> dict[str, Any]:
        emp = str(employee_id or "").strip()
        if not emp.isdigit() or len(emp) != 5:
            return {"ok": False, "error": "invalid_employee"}
        with self._lock:
            if self._worker_busy:
                return {"ok": False, "error": "checklist_busy"}
            if self._session and self._session_incomplete_locked():
                return {"ok": True, "resumed": True}
            self._session = {
                "employeeId": emp,
                "startedAt": _utc_now_iso(),
                "currentStepIndex": 0,
                "completedSteps": [],
                "measurements": {"l": None, "r": None},
                "failedStep": "",
            }
            self._worker_error = ""
            self._worker_phase = ""
            self._air_test_active = False
            self._air_test_verified = False
            self._piece_test_started = False
            self._step_error = ""
            self._required = True
        self._log(f"Checklist: iniciado (empleado {emp})")
        self._notify()
        return {"ok": True}

    def close_session(self, host: Any = None) -> dict[str, Any]:
        """Cerrar el modal solo oculta la revisión: la sesión y el paso
        actual se conservan para reabrir donde se quedó.

        Si hay un trabajo en curso (actuador / alimentador), se cancela y se
        espera a que deje de dar órdenes antes de soltar la pantalla: el
        operador va a manipular la máquina desde fuera y el checklist no
        debe seguir moviéndola por detrás.

        Si la prueba de piezas está corriendo, queda en pausa (lote vivo):
        cerrar la ventana no debe obligar a Detener ni a Reset para poder
        manipular la máquina; solo el arranque sigue bloqueado hasta
        terminar la revisión.

        Reiniciar desde el primer paso solo al arrancar la HMI
        (`mark_required_on_boot`).
        """
        worker = self._worker
        with self._lock:
            busy = self._worker_busy
            if busy and self._worker_cancel is not None:
                self._worker_cancel.set()
            was_air_test = self._air_test_active
            self._air_test_active = False
        if busy and worker is not None and worker.is_alive():
            worker.join(timeout=3.0)
        # La prueba de aire deja las válvulas accionadas: apagarlas al salir.
        if was_air_test and host is not None:
            try:
                if host.plc_connected():
                    self._manual_plc_air_off(host)
            except Exception:  # noqa: BLE001
                pass
        # Prueba de piezas en curso: pausarla, no detenerla. El lote queda
        # vivo y la máquina queda manipulable (ajustes, manuales, mtto,
        # Materialista) sin pasar por Stop/Reset.
        if host is not None and self.is_piece_test_started():
            try:
                cycle = getattr(host, "_cycle", None)
                if (
                    cycle is not None
                    and cycle.is_active()
                    and not cycle.snapshot().get("paused")
                ):
                    res = host.cmd_cycle_pause()
                    if res.get("ok"):
                        self._log(
                            "Checklist: ventana cerrada — "
                            "prueba de piezas en pausa (lote vivo)"
                        )
            except Exception:  # noqa: BLE001
                pass
        with self._lock:
            still_busy = self._worker_busy
            self._worker_error = ""
            self._worker_phase = ""
            self._air_test_active = False
            self._sync_required_with_session_locked()
            self._restore_runtime_flags_from_session_locked()
        self._log("Checklist: ventana cerrada — sesión conservada")
        self._notify()
        return {"ok": True, "workerCancelling": still_busy}

    def _set_failed_step_locked(self, step_id: str) -> None:
        if self._session is not None:
            self._session["failedStep"] = step_id

    def _clear_failed_step_locked(self, step_id: str) -> None:
        if self._session is not None and self._session.get("failedStep") == step_id:
            self._session["failedStep"] = ""

    def complete_checklist(self, host: Any) -> dict[str, Any]:
        with self._lock:
            if self._required is False:
                return {"ok": True, "already": True}
            if not self._session:
                return {"ok": False, "error": "no_session"}
            idx = int(self._session.get("currentStepIndex", 0))
            if idx < len(STEP_IDS):
                return {"ok": False, "error": "steps_pending"}
            emp = self._session.get("employeeId", "?")
            self._required = False
            self._last_completed_at = _utc_now_iso()
            self._session = None
            self._air_test_active = False
            self._piece_test_started = False
        self._log(f"Checklist: completado (empleado {emp})")
        self.clear_step_error()
        self._notify()
        return {"ok": True, "completed": True}

    def _advance_step(self, step_id: str) -> None:
        with self._lock:
            if not self._session:
                return
            completed = list(self._session.get("completedSteps") or [])
            if step_id not in completed:
                completed.append(step_id)
            self._session["completedSteps"] = completed
            self._session["currentStepIndex"] = self._step_index_from_completed(completed)
            if step_id == "asda_roundtrip":
                self._session.pop("asdaOk", None)
            self._clear_failed_step_locked(step_id)
            self._worker_error = ""
            self._worker_phase = ""
            self._step_error = ""

    # --- evaluación por paso ---

    def _step_status_locked(
        self, host: Any, step_id: str, session: dict[str, Any] | None
    ) -> dict[str, Any]:
        if not step_id:
            return {"ready": False, "detail": ""}
        if step_id == "cycle_counter":
            count = int(getattr(host, "_maintenance_cycle_count", 0))
            over = count > CHECKLIST_COUNTER_MAX
            return {
                "ready": not over,
                "detail": str(count),
                "overLimit": over,
                "limit": CHECKLIST_COUNTER_MAX,
            }
        if step_id == "air_pressure":
            pressures_ok, _ = self._air_pressures_ok(host)
            plc_ok = self._air_plc_valves_ok(host)
            continue_ok = self._air_test_verified and pressures_ok and plc_ok
            return {
                "ready": continue_ok,
                "pressuresOk": pressures_ok,
                "airTestVerified": self._air_test_verified,
                "plcValvesOk": plc_ok,
                "testActive": self._air_test_active,
            }
        if step_id == "pf_holgura":
            return {"ready": True, "detail": "manual"}
        if step_id == "asda_roundtrip":
            asda_ok = bool((session or {}).get("asdaOk"))
            busy = self._worker_busy and self._worker_phase.startswith("asda")
            latched = False
            is_latched = getattr(host, "error_is_latched", None)
            if callable(is_latched):
                try:
                    latched = bool(is_latched())
                except Exception:  # noqa: BLE001
                    latched = False
            return {
                "ready": asda_ok and not busy and not self._worker_error,
                "asdaOk": asda_ok,
                "asdaHomeAwaitConfirm": bool(
                    (session or {}).get("asdaHomeAwaitConfirm")
                ),
                "asdaHomeError": bool((session or {}).get("asdaHomeError")),
                "errorLatched": latched,
                "detail": self._worker_phase or self._worker_error,
            }
        if step_id == "feeder_can_purge":
            busy = self._worker_busy and self._worker_phase.startswith("feeder")
            sess = self._session or session or {}
            stage2_ok = bool(sess.get("stage2Ok"))
            cyc = host._cycle.snapshot() if getattr(host, "_cycle", None) else {}
            purge_idle = not host._cycle.is_purge_busy() and not cyc.get("refillActive")
            # La purga solo cuenta como hecha si se vio en curso y terminó
            # bien. Si falló o se resolvió/canceló fuera, queda reintentable.
            if sess.get("purgeStarted") and not purge_idle and self._session is not None:
                self._session["purgeSeenBusy"] = True
            seen_busy = bool(sess.get("purgeSeenBusy") or sess.get("purgeDone"))
            purge_done = bool(
                sess.get("purgeDone")
                or (
                    sess.get("purgeStarted")
                    and seen_busy
                    and purge_idle
                    and cyc.get("lastOk")
                    and not cyc.get("fault")
                )
            )
            if purge_done and self._session is not None:
                self._session["purgeDone"] = True
            # Purga que se vio en curso y ya terminó sin éxito (p. ej. al
            # alimentar, el material no llegó al sensor y la máquina marcó
            # el error en su barra): el paso queda fallido y reintentable
            # aquí mismo, no «esperando» para siempre ni sin botón.
            purge_failed = bool(
                sess.get("purgeStarted")
                and seen_busy
                and purge_idle
                and not purge_done
                and not cyc.get("lastOk")
            )
            if purge_failed and self._session is not None:
                self._session["purgeFailed"] = True
                self._set_failed_step_locked("feeder_can_purge")
            elif purge_done and self._session is not None:
                self._session["purgeFailed"] = False
            return {
                "ready": stage2_ok and purge_done and not busy and not self._worker_error,
                "detail": self._worker_phase or self._worker_error,
                "stage2Ok": stage2_ok,
                "purgeIdle": purge_idle,
                "purgeStarted": bool(sess.get("purgeStarted")),
                "purgeDone": purge_done,
                "purgeFailed": purge_failed,
                "refillPrompt": str(cyc.get("refillPrompt") or ""),
                "refillAwaitingConfirm": bool(cyc.get("refillAwaitingConfirm")),
                "purgeHandsWarning": bool(cyc.get("purgeHandsWarning")),
            }
        if step_id == "piece_measure":
            snap = host._cycle.snapshot() if getattr(host, "_cycle", None) else {}
            pieces = int(snap.get("piecesDone") or 0)
            cycle_active = bool(getattr(host, "_cycle", None) and host._cycle.is_active())
            if not self._piece_test_started:
                return {
                    "ready": False,
                    "detail": "waiting_start",
                    "piecesDone": pieces,
                    "cycleActive": cycle_active,
                }
            meas = (session or {}).get("measurements") or {}
            return {
                "ready": pieces >= 2,
                "detail": "measure",
                "piecesDone": pieces,
                "measurements": meas,
                "cycleActive": cycle_active,
                # Prueba iniciada pero el ciclo ya paró sin completar (p. ej.
                # error resuelto fuera con Stop): se puede cortar de nuevo.
                "canRetry": pieces < 2 and not cycle_active,
            }
        return {"ready": False, "detail": ""}

    def _air_pressures_ok(self, host: Any) -> tuple[bool, str]:
        with host._lock:
            exhaust = bool(host._motion.get("safetyExhaust"))
            pressure = bool(host._andon.get("pressure"))
        ok = not exhaust and not pressure
        return ok, "pressures_ok" if ok else "pressures_bad"

    def _air_plc_valves_ok(self, host: Any) -> bool:
        if not host.plc_connected():
            return True
        with host._lock:
            for cmd in (0x1B, 0x1D):
                if bool(host._plc["valves"].get(str(cmd), {}).get("error")):
                    return False
        return True

    # --- acciones ---

    def action(self, host: Any, name: str, **kwargs: Any) -> dict[str, Any]:
        handlers = {
            "confirm_step": lambda: self._action_confirm_step(host, str(kwargs.get("stepId", ""))),
            "air_test_on": lambda: self._action_air_test(host, True),
            "air_test_off": lambda: self._action_air_test(host, False),
            "start_asda_home": lambda: self._start_worker(host, "asda_home"),
            "confirm_asda_home": lambda: self._action_confirm_asda_home(host),
            "start_asda_roundtrip": lambda: self._start_worker(host, "asda_roundtrip"),
            "start_feeder_can": lambda: self._start_worker(host, "feeder_can_purge"),
            "start_purge": lambda: self._action_start_purge(host),
            "confirm_purge": lambda: self._action_purge_step(host, confirm=True),
            "feed_purge": lambda: self._action_purge_step(host, confirm=False),
            "start_piece_test": lambda: self._action_start_piece_test(host),
            "set_measurements": lambda: self._action_set_measurements(
                host, float(kwargs.get("lengthL", 0)), float(kwargs.get("lengthR", 0))
            ),
        }
        fn = handlers.get(name)
        if not fn:
            return {"ok": False, "error": f"acción desconocida: {name}"}
        return fn()

    def _action_confirm_step(self, host: Any, step_id: str) -> dict[str, Any]:
        with self._lock:
            if not self._session:
                return {"ok": False, "error": "no_session"}
            idx = int(self._session.get("currentStepIndex", 0))
            if idx >= len(STEP_IDS) or STEP_IDS[idx] != step_id:
                return {"ok": False, "error": "wrong_step"}
        if self._machine_in_motion(host):
            return {"ok": False, "error": "machine_in_motion"}
        status = self.snapshot(host)["stepStatus"]
        if step_id == "cycle_counter":
            if status.get("overLimit"):
                return {"ok": False, "error": "counter_over_limit"}
        elif step_id == "air_pressure":
            if not status.get("airTestVerified"):
                return {"ok": False, "error": "air_test_pending"}
            if not status.get("ready"):
                return {"ok": False, "error": "air_not_ready"}
            self._action_air_test(host, False)
        elif step_id == "pf_holgura":
            pass
        elif step_id == "asda_roundtrip":
            with self._lock:
                if not self._session.get("asdaOk"):
                    return {"ok": False, "error": "asda_pending"}
        elif step_id == "feeder_can_purge":
            with self._lock:
                if not self._session.get("stage2Ok"):
                    return {"ok": False, "error": "stage2_pending"}
                if host._cycle.is_purge_busy() or host._cycle.snapshot().get("refillActive"):
                    return {"ok": False, "error": "purge_active"}
                if not status.get("purgeDone"):
                    return {"ok": False, "error": "purge_pending"}
        elif step_id == "piece_measure":
            return self._action_confirm_measurements(host)
        else:
            return {"ok": False, "error": "use_automatic_action"}
        self._log(f"Checklist: paso confirmado — {step_id}")
        self._advance_step(step_id)
        if step_id == "piece_measure":
            return self.complete_checklist(host)
        self.clear_step_error()
        self._notify()
        return {"ok": True}

    def _action_air_test(self, host: Any, on: bool) -> dict[str, Any]:
        pressures_ok, _ = self._air_pressures_ok(host)
        if not pressures_ok:
            return {"ok": False, "error": "air_not_ready"}

        if not on:
            with self._lock:
                self._air_test_active = False
            if host.plc_connected():
                self._manual_plc_air_off(host)
            self._log("Checklist: prueba aire → OFF")
            self._notify()
            return {"ok": True, "on": False}

        if not host.plc_connected():
            return {"ok": False, "error": "air_not_ready"}
        grip = host.cmd_plc_gripper(True)
        enc = host.cmd_plc_encoder(True)
        if not (grip and enc):
            with self._lock:
                self._air_test_active = False
                self._air_test_verified = False
                if self._session:
                    self._session["airTestVerified"] = False
                self._set_failed_step_locked("air_pressure")
            self.set_step_error("air_test_failed")
            return {"ok": False, "error": "air_test_failed"}

        with self._lock:
            self._air_test_active = True
            self._air_test_verified = False
        self._notify()

        time.sleep(CHECKLIST_AIR_SETTLE_MS / 1000.0)

        pressures_ok, _ = self._air_pressures_ok(host)
        plc_ok = self._air_plc_valves_ok(host)
        verified = pressures_ok and plc_ok

        with self._lock:
            self._air_test_verified = verified
            if self._session:
                self._session["airTestVerified"] = verified
            if verified:
                self._clear_failed_step_locked("air_pressure")
            else:
                self._set_failed_step_locked("air_pressure")
                self._air_test_active = False
                self._manual_plc_air_off(host)

        if verified:
            self.clear_step_error()
            self._log("Checklist: prueba aire → OK (válvulas sin error)")
        else:
            self.set_step_error("air_test_failed")
            self._log("Checklist: prueba aire → error detectado en válvulas")
        self._notify()
        return {
            "ok": verified,
            "on": verified,
            "verified": verified,
            "error": None if verified else "air_test_failed",
        }

    def _manual_plc_air_off(self, host: Any) -> None:
        host.cmd_plc_gripper(False)
        host.cmd_plc_encoder(False)

    def _action_start_piece_test(self, host: Any) -> dict[str, Any]:
        with self._lock:
            if not self._session:
                return {"ok": False, "error": "no_session"}
            idx = int(self._session.get("currentStepIndex", 0))
            if STEP_IDS[idx] != "piece_measure":
                return {"ok": False, "error": "wrong_step"}
        if self._machine_in_motion(host):
            return {"ok": False, "error": "machine_in_motion"}
        with self._lock:
            self._piece_test_started = True
            if self._session:
                self._session["pieceTestStarted"] = True
                self._clear_failed_step_locked("piece_measure")
        res = host.cmd_start(qty=2, checklist_bypass=True)
        if not res.get("ok"):
            with self._lock:
                self._piece_test_started = False
                if self._session:
                    self._session["pieceTestStarted"] = False
                    self._set_failed_step_locked("piece_measure")
            self.set_step_error(str(res.get("error") or "worker_failed"))
        else:
            self.clear_step_error()
            self._log("Checklist: prueba 2 piezas iniciada")
        self._notify()
        return res

    def _action_set_measurements(
        self, host: Any, length_l: float, length_r: float
    ) -> dict[str, Any]:
        with self._lock:
            if not self._session:
                return {"ok": False, "error": "no_session"}
            self._session["measurements"] = {"l": length_l, "r": length_r}
        self._notify()
        return {"ok": True}

    def _action_confirm_measurements(self, host: Any) -> dict[str, Any]:
        with self._lock:
            if not self._session:
                return {"ok": False, "error": "no_session"}
            meas = self._session.get("measurements") or {}
            l_val = meas.get("l")
            r_val = meas.get("r")
        if l_val is None or r_val is None:
            return {"ok": False, "error": "measurements_missing"}
        nominal = abs(float(host._mm))
        tol = CHECKLIST_MEASURE_TOL_MM
        extra = CHECKLIST_PIECE_EXTRA_MM
        expected_total = nominal + extra
        l_entered = float(l_val)
        r_entered = float(r_val)
        if abs(l_entered - expected_total) > tol or abs(r_entered - expected_total) > tol:
            self.set_step_error("measurements_out_of_tolerance")
            return {
                "ok": False,
                "error": "measurements_out_of_tolerance",
                "expected": expected_total,
                "tolerance": tol,
            }
        self._log(
            f"Checklist: medidas OK L={l_entered:.2f} R={r_entered:.2f} "
            f"(esperado {expected_total:.2f} mm ±{tol})"
        )
        self._advance_step("piece_measure")
        return self.complete_checklist(host)

    # --- workers async ---

    def _start_worker(self, host: Any, kind: str) -> dict[str, Any]:
        if self._machine_in_motion(host):
            return {"ok": False, "error": "machine_in_motion"}
        with self._lock:
            if self._worker_busy:
                return {"ok": False, "error": "checklist_busy"}
            if not self._session:
                return {"ok": False, "error": "no_session"}
            idx = int(self._session.get("currentStepIndex", 0))
            expected = (
                "feeder_can_purge" if kind == "feeder_can_purge" else "asda_roundtrip"
            )
            if STEP_IDS[idx] != expected:
                return {"ok": False, "error": "wrong_step"}
            self._worker_busy = True
            self._worker_error = ""
            self._worker_phase = f"{kind}:starting"
            self._step_error = ""
            self._worker_cancel = threading.Event()
            self._clear_failed_step_locked(expected)
            if kind in ("asda_home", "asda_roundtrip") and self._session:
                self._session.pop("asdaOk", None)
            if kind == "asda_home" and self._session:
                self._session["asdaHomeAwaitConfirm"] = False
                self._session["asdaHomeError"] = False
            if kind == "asda_roundtrip" and self._session:
                # La validación solo arranca tras la confirmación del
                # operador; al arrancar, esa pregunta ya quedó resuelta.
                self._session["asdaHomeAwaitConfirm"] = False

        def run() -> None:
            try:
                if kind == "asda_home":
                    self._run_asda_home(host)
                elif kind == "asda_roundtrip":
                    self._run_asda_roundtrip(host)
                else:
                    self._run_feeder_purge(host)
            except _ChecklistCancelled:
                # Cierre del operador: el paso no se completó, queda
                # marcado como fallido/reintentable para la reapertura.
                with self._lock:
                    self._set_failed_step_locked(expected)
                    self._worker_error = ""
                    self._step_error = ""
                    self._worker_phase = ""
                self._log(f"Checklist: {kind} interrumpido por cierre")
            except Exception as exc:  # noqa: BLE001
                with self._lock:
                    self._worker_error = str(exc)
                    self._step_error = "worker_failed"
                    self._worker_phase = ""
                    self._set_failed_step_locked(expected)
                self._log(f"Checklist: error en {kind} — {exc}")
            finally:
                with self._lock:
                    self._worker_busy = False
                self._notify()

        self._worker = threading.Thread(target=run, name=f"checklist-{kind}", daemon=True)
        self._worker.start()
        self._notify()
        return {"ok": True}

    def _motion_online(self, host: Any) -> bool:
        return bool(host.motion_connected())

    def _checklist_asda_pos_mm(self, host: Any) -> float | None:
        """Posición ASDA: HTTP con reintentos y luego caché del evento Reached."""
        refresh = getattr(host, "refresh_asda_position_mm", None)
        if callable(refresh):
            for _ in range(4):
                pos = refresh()
                if pos is not None:
                    return float(pos)
                time.sleep(0.12)
        get_pos = getattr(host, "asda_position_mm", None)
        if callable(get_pos):
            pos = get_pos()
            if pos is not None:
                return float(pos)
        return None

    def _checklist_asda_at_target(
        self,
        host: Any,
        target_mm: float,
        *,
        tol_mm: float = CHECKLIST_ASDA_TARGET_TOL_MM,
        live: bool = False,
    ) -> bool | None:
        if live:
            pos = self._checklist_asda_pos_mm(host)
        else:
            get_pos = getattr(host, "asda_position_mm", None)
            pos = get_pos() if callable(get_pos) else None
        if pos is None:
            return None
        return abs(abs(float(pos)) - abs(float(target_mm))) <= float(tol_mm)

    def _wait_asda_motion_target(self, host: Any, target_mm: float, timeout_s: float) -> bool:
        """Espera Reached/Idle validando destino (como el ciclo de producción).

        Un Idle espurio en el origen ya no da el paso por bueno mientras el
        MOVE sigue en curso.
        """
        deadline = time.monotonic() + float(timeout_s)
        idle_off_target_since: float | None = None
        reject_fn = getattr(host, "move_ack_rejected", None)
        while True:
            if self._worker_cancel is not None and self._worker_cancel.is_set():
                raise _ChecklistCancelled()
            is_latched = getattr(host, "error_is_latched", None)
            if callable(is_latched) and is_latched():
                raise RuntimeError("Error de máquina durante el paso")
            if callable(reject_fn):
                reject = str(reject_fn() or "").strip()
                if reject:
                    raise RuntimeError(f"MOVE rechazado — {reject}")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return False
            if host.wait_motion_idle_or_reached(min(0.25, max(0.05, remaining))):
                at = self._checklist_asda_at_target(host, target_mm)
                if at is not False:
                    return True
                host.clear_motion_reached_flag()
                if idle_off_target_since is None:
                    idle_off_target_since = time.monotonic()
            elif idle_off_target_since is not None:
                busy_fn = getattr(host, "motion_busy", None)
                busy = bool(busy_fn()) if callable(busy_fn) else False
                if busy:
                    idle_off_target_since = None
                elif (
                    time.monotonic() - idle_off_target_since
                    >= CHECKLIST_ASDA_IDLE_OFF_TARGET_S
                ):
                    return False

    def _action_confirm_asda_home(self, host: Any) -> dict[str, Any]:
        """El operador vio el eje en su posición inicial: validar recorrido.

        Si el error de máquina sigue enclavado (p. ej. el que saltó al
        buscar la posición estando ya en ella), no se arranca nada: el
        operador primero lo restablece y después vuelve a confirmar aquí.
        La pregunta queda guardada, así que puede cerrar esta ventana,
        restablecer y reabrir sin repetir la búsqueda.
        """
        with self._lock:
            if not self._session:
                return {"ok": False, "error": "no_session"}
            idx = int(self._session.get("currentStepIndex", 0))
            if STEP_IDS[idx] != "asda_roundtrip":
                return {"ok": False, "error": "wrong_step"}
            if not self._session.get("asdaHomeAwaitConfirm"):
                return {"ok": False, "error": "asda_home_pending"}
        is_latched = getattr(host, "error_is_latched", None)
        if callable(is_latched) and is_latched():
            return {"ok": False, "error": "machine_error_latched"}
        return self._start_worker(host, "asda_roundtrip")

    def _wait_home_finished(self, host: Any, timeout_s: float) -> bool:
        """Espera a que termine la búsqueda de posición inicial.

        True = terminó bien. False = terminó con un error enclavado (caso
        típico: el eje ya estaba en su posición y la búsqueda lo marca
        como error). Ese False no es un fallo del paso: la máquina ya
        paró y es el operador quien decide si todo quedó bien.
        """
        deadline = time.monotonic() + timeout_s
        started = time.monotonic()
        seen_busy = False
        while True:
            if self._worker_cancel is not None and self._worker_cancel.is_set():
                raise _ChecklistCancelled()
            if host.wait_motion_idle_or_reached(0.25):
                return True
            try:
                if host.motion_busy():
                    seen_busy = True
            except Exception:  # noqa: BLE001
                pass
            is_latched = getattr(host, "error_is_latched", None)
            if callable(is_latched) and is_latched():
                # Solo vale como «terminó» si el movimiento llegó a
                # arrancar, o si tras unos segundos la máquina sigue sin
                # moverse (la orden fue rechazada con el error ya puesto).
                if (seen_busy and not host.motion_busy()) or (
                    time.monotonic() - started > 5.0 and not host.motion_busy()
                ):
                    return False
            if time.monotonic() >= deadline:
                raise RuntimeError("Timeout esperando posición inicial")

    def _run_asda_home(self, host: Any) -> None:
        if not host._client.connected:
            raise RuntimeError("Sin enlace — actuador lineal no verificado")

        self._set_phase("asda_home:servo_on")
        if not host._manual_motion(lambda: host._client.cmd_on()):
            raise RuntimeError("Servo ON falló")
        time.sleep(0.3)

        self._set_phase("asda_home:buscando")
        host.clear_motion_reached_flag()
        if not host._manual_motion(lambda: host._client.cmd_home("F")):
            raise RuntimeError("Buscar posición inicial falló")
        finished_ok = self._wait_home_finished(host, 120.0)

        with self._lock:
            if self._session:
                self._session["asdaHomeAwaitConfirm"] = True
                self._session["asdaHomeError"] = not finished_ok
        self._log(
            "Checklist: posición inicial terminada"
            + ("" if finished_ok else " (con error enclavado)")
            + " — esperando confirmación del operador"
        )
        self.clear_step_error()
        self._set_phase("")

    def _run_asda_roundtrip(self, host: Any) -> None:
        if not host._client.connected:
            # Igual que el alimentador: sin enlace no hubo recorrido real.
            raise RuntimeError("Sin enlace — actuador lineal no verificado")

        cfg = host._cycle.get_config()
        travel = float(getattr(cfg, "deposit_max_travel_mm", 1500.0) or 1500.0)
        travel *= CHECKLIST_ASDA_TRAVEL_FRACTION

        pos0 = self._checklist_asda_pos_mm(host)
        if pos0 is None:
            raise RuntimeError("No se pudo leer la posición del actuador lineal")

        self._set_phase("asda:ida")
        host.clear_motion_reached_flag()
        if not host.cmd_motion_move_mm(travel, float(host._rpm)):
            raise RuntimeError("MOVE ida falló")
        if not self._wait_asda_motion_target(host, travel, 180.0):
            raise RuntimeError("Timeout MOVE ida")
        at_travel = self._checklist_asda_at_target(host, travel, live=True)
        if at_travel is False:
            raise RuntimeError("ASDA no se movió en ida")
        if at_travel is None:
            pos1 = self._checklist_asda_pos_mm(host)
            if pos1 is None or abs(pos1 - pos0) < CHECKLIST_ASDA_TARGET_TOL_MM:
                raise RuntimeError("ASDA no se movió en ida")

        self._set_phase("asda:vuelta")
        host.clear_motion_reached_flag()
        if not host.cmd_motion_move_zero(float(host._rpm)):
            raise RuntimeError("MOVE a 0 falló")
        if not self._wait_asda_motion_target(host, 0.0, 180.0):
            raise RuntimeError("Timeout MOVE vuelta")
        at_home = self._checklist_asda_at_target(
            host, 0.0, tol_mm=CHECKLIST_MEASURE_TOL_MM, live=True
        )
        if at_home is False:
            raise RuntimeError("ASDA no regresó a 0")
        if at_home is None:
            pos2 = self._checklist_asda_pos_mm(host)
            if pos2 is None or abs(pos2) > CHECKLIST_MEASURE_TOL_MM:
                raise RuntimeError("ASDA no regresó a 0")

        self._log("Checklist: recorrido ASDA ida/vuelta OK")
        with self._lock:
            if self._session:
                self._session["asdaOk"] = True
        self.clear_step_error()
        self._set_phase("")

    def _reset_purge_flags_locked(self, *, done: bool = False) -> None:
        if not self._session:
            return
        self._session["purgeStarted"] = False
        self._session["purgeSeenBusy"] = False
        self._session["purgeDone"] = done
        self._session["purgeFailed"] = False

    def _run_feeder_purge(self, host: Any) -> None:
        if not host._client.connected:
            # Sin enlace no hay preparación real: el paso debe quedar
            # fallido/reintentable, nunca listo ni con la purga cumplida.
            raise RuntimeError("Sin enlace — alimentador no preparado")

        # Primero, la rutina de encendido del alimentador (la misma que
        # corre su controlador al encender: bus + servo). Sin eso, la
        # purga trabaja con un servo que nunca arrancó.
        self._set_phase("feeder:can_init")
        if not host.cmd_motion_feed_can_init():
            fault = host.stage2_fault() or "Arranque del alimentador falló"
            raise RuntimeError(fault)
        can_result = host.wait_feed_can_ready(90.0, abort_event=self._worker_cancel)
        if can_result == "aborted":
            raise _ChecklistCancelled()
        if can_result != "ok":
            fault = host.stage2_fault() or can_result
            raise RuntimeError(f"Arranque del alimentador: {fault}")
        self._log("Checklist: alimentador arrancado (servo listo)")

        self._set_phase("feeder:stage2")
        host.arm_stage2()
        piece = abs(float(host._mm))
        if not host.cmd_motion_stage2_start(piece_mm=piece, sides="LR"):
            fault = host.stage2_fault() or "Stage2 start falló"
            raise RuntimeError(fault)
        result = host.wait_stage2_result(120.0, abort_event=self._worker_cancel)
        if result == "aborted":
            raise _ChecklistCancelled()
        if result != "ok":
            fault = host.stage2_fault() or result
            raise RuntimeError(f"CAN/Stage2: {fault}")

        with self._lock:
            if self._session:
                self._session["stage2Ok"] = True
                self._reset_purge_flags_locked()
        self._log("Checklist: CAN/Stage2 init OK — iniciar purga manual")
        self._set_phase("")

    def _action_start_purge(self, host: Any) -> dict[str, Any]:
        with self._lock:
            if not self._session or not self._session.get("stage2Ok"):
                return {"ok": False, "error": "stage2_pending"}
        # cmd_cycle_refill es keyword-only: pasarle rpm posicional lanzaba
        # TypeError y la purga del checklist nunca arrancaba.
        res = host.cmd_cycle_refill()
        if res.get("ok"):
            with self._lock:
                if self._session:
                    self._session["purgeStarted"] = True
                    self._session["purgeSeenBusy"] = False
                    self._session["purgeDone"] = False
                    self._session["purgeFailed"] = False
                self._clear_failed_step_locked("feeder_can_purge")
                self._worker_error = ""
                self._step_error = ""
                self._worker_phase = ""
            self._log("Checklist: purga iniciada")
        else:
            with self._lock:
                self._set_failed_step_locked("feeder_can_purge")
            self.set_step_error("worker_failed")
        self._notify()
        return res

    def _action_purge_step(self, host: Any, *, confirm: bool) -> dict[str, Any]:
        """Botones de la purga dentro del checklist (Alimentar / Continuar).

        Sin esto, las confirmaciones de la purga solo existen en la pantalla
        principal, tapadas por este modal: la purga nunca avanzaba y el paso
        no se podía ni completar ni reintentar.
        """
        with self._lock:
            if not self._session or not self._session.get("stage2Ok"):
                return {"ok": False, "error": "stage2_pending"}
        res = (
            host.cmd_cycle_refill_confirm(True)
            if confirm
            else host.cmd_cycle_refill_retry()
        )
        if not res.get("ok"):
            with self._lock:
                self._set_failed_step_locked("feeder_can_purge")
            self.set_step_error("worker_failed")
        self._notify()
        return res

    def _set_phase(self, phase: str) -> None:
        with self._lock:
            self._worker_phase = phase
        self._notify()
