"""Debug Trails — modo de prueba separado del ciclo de producción.

Stage 1: Servo Feeder + Encoder OM.
Al Start: Holder + Encoder (PLC) ON; no se liberan hasta fin de lote/Stop.
Secuencia por lado: Set0 → Feed → All OK (LengthOK) → Cut → Wait → Next.
No introduce tolerancias ni criterios extra; la medición es la OM real (GetMeasured).
"""
from __future__ import annotations

import csv
import io
import threading
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any, Literal, Protocol

from error_catalog import format_ui
from motion import TX_LENGTH_NG_L, TX_LENGTH_NG_R

SideSel = Literal["R", "L", "Both"]
SideOne = Literal["R", "L"]


class TrailsHost(Protocol):
    def cycle_log(self, text: str) -> None: ...
    def cycle_notify(self) -> None: ...
    def motion_connected(self) -> bool: ...
    def plc_connected(self) -> bool: ...
    def cycle_is_active(self) -> bool: ...
    def clear_motion_wait_flags(self) -> None: ...
    def wait_feed_length_ok_for(
        self,
        sides: list[SideOne],
        timeout_s: float,
        *,
        abort_event: threading.Event | None = None,
    ) -> dict[SideOne, str]: ...
    def feed_fault_for(self, side: SideOne) -> str: ...
    def cmd_motion_enc_set0_r(self) -> bool: ...
    def cmd_motion_enc_set0_l(self) -> bool: ...
    def cmd_motion_feed_r(self) -> bool: ...
    def cmd_motion_feed_l(self) -> bool: ...
    def cmd_motion_stop(self) -> bool: ...
    def cmd_motion_enc_measure_r(self) -> bool: ...
    def cmd_motion_enc_measure_l(self) -> bool: ...
    def wait_encoder_mm(self, side: SideOne, timeout_s: float) -> float | None: ...
    def cmd_plc_cutter_r(self, on: bool) -> bool: ...
    def cmd_plc_cutter_l(self, on: bool) -> bool: ...
    def cmd_plc_holder(self, on: bool) -> bool: ...
    def cmd_plc_encoder(self, on: bool) -> bool: ...
    def feed_wait_timeout_s(self) -> float: ...
    def cutter_pulse_ms(self) -> int: ...
    def set_debug_trails_active(self, on: bool) -> None: ...


@dataclass
class TrailRecord:
    test_num: int
    side: SideOne
    measure_mm: float | None
    status: str  # ok | fail | error | aborted
    error: str = ""
    timestamp: str = ""
    phase: str = ""

    def to_dict(self) -> dict[str, Any]:
        return {
            "testNum": self.test_num,
            "side": self.side,
            "measureMm": self.measure_mm,
            "status": self.status,
            "error": self.error,
            "timestamp": self.timestamp,
            "phase": self.phase,
        }


@dataclass
class TrailsState:
    active: bool = False
    stage: str = "stage1"
    side: SideSel = "Both"
    num_tests: int = 0
    wait_time_s: float = 0.0
    current_test: int = 0
    phase: str = "idle"
    last_ok: bool = False
    fault: str = ""
    records: list[TrailRecord] = field(default_factory=list)


def _iso_now() -> str:
    return datetime.now(timezone.utc).astimezone().isoformat(timespec="seconds")


class DebugTrailsRunner:
    """Orquestador Debug Trails (hilo propio; no toca Communication Core)."""

    def __init__(self, host: TrailsHost) -> None:
        self._host = host
        self._lock = threading.RLock()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._state = TrailsState()

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            return {
                "active": self._state.active,
                "stage": self._state.stage,
                "side": self._state.side,
                "numTests": self._state.num_tests,
                "waitTimeS": self._state.wait_time_s,
                "currentTest": self._state.current_test,
                "phase": self._state.phase,
                "lastOk": self._state.last_ok,
                "fault": self._state.fault,
                "records": [r.to_dict() for r in self._state.records],
                "recordCount": len(self._state.records),
            }

    def is_active(self) -> bool:
        with self._lock:
            return self._state.active

    def request_start(
        self,
        side: str = "Both",
        num_tests: int = 1,
        wait_time_s: float = 1.0,
    ) -> dict[str, Any]:
        side_n = str(side or "Both").strip().capitalize()
        if side_n == "Both":
            sel: SideSel = "Both"
        elif side_n in ("R", "L"):
            sel = side_n  # type: ignore[assignment]
        else:
            return {"ok": False, "error": "Side inválido (R | L | Both)"}
        try:
            n = int(num_tests)
            wait_s = float(wait_time_s)
        except (TypeError, ValueError):
            return {"ok": False, "error": "Parámetros inválidos"}
        if n < 1:
            return {"ok": False, "error": "Number of tests debe ser ≥ 1"}
        if wait_s < 0:
            return {"ok": False, "error": "Wait time inválido"}

        with self._lock:
            if self._state.active:
                return {"ok": False, "error": "Debug Trails ya activo"}
            if self._thread and self._thread.is_alive():
                self._thread.join(timeout=0.2)
                if self._thread.is_alive():
                    return {"ok": False, "error": "Debug Trails ocupado"}

        if self._host.cycle_is_active():
            return {"ok": False, "error": "Ciclo de producción activo"}
        if not self._host.motion_connected():
            return {"ok": False, "error": "Motion sin enlace"}
        if not self._host.plc_connected():
            return {"ok": False, "error": "PLC sin enlace"}

        self._stop.clear()
        with self._lock:
            self._state = TrailsState(
                active=True,
                stage="stage1",
                side=sel,
                num_tests=n,
                wait_time_s=wait_s,
                current_test=0,
                phase="starting",
                last_ok=False,
                fault="",
                records=[],
            )
        self._host.set_debug_trails_active(True)
        self._host.cycle_log(
            f"Debug Trails Stage1 START side={sel} tests={n} wait={wait_s:.2f}s"
        )
        self._host.cycle_notify()
        self._thread = threading.Thread(
            target=self._run_stage1,
            args=(sel, n, wait_s),
            name="debug-trails-stage1",
            daemon=True,
        )
        self._thread.start()
        return {"ok": True, "trails": self.snapshot()}

    def request_stop(self) -> dict[str, Any]:
        with self._lock:
            if not self._state.active and not (self._thread and self._thread.is_alive()):
                return {"ok": True, "trails": self.snapshot()}
        self._stop.set()
        self._host.cycle_log("Debug Trails STOP solicitado")
        # Cortar feed físico + liberar waits de LengthOK (no solo cutters).
        try:
            self._host.cmd_motion_stop()
        except Exception:
            pass
        try:
            self._host.clear_motion_wait_flags()
        except Exception:
            pass
        try:
            self._host.cmd_plc_cutter_r(False)
            self._host.cmd_plc_cutter_l(False)
        except Exception:
            pass
        try:
            self._host.cmd_plc_holder(False)
            self._host.cmd_plc_encoder(False)
        except Exception:
            pass
        self._host.cycle_notify()
        return {"ok": True, "trails": self.snapshot()}

    def clear_records(self) -> dict[str, Any]:
        with self._lock:
            if self._state.active:
                return {"ok": False, "error": "No borrar registros con Trails activo"}
            self._state.records.clear()
        self._host.cycle_notify()
        return {"ok": True, "trails": self.snapshot()}

    def export_csv(self) -> tuple[str, str]:
        """Devuelve (filename, csv_text) compatible con Excel (UTF-8 BOM)."""
        with self._lock:
            rows = [r.to_dict() for r in self._state.records]
        buf = io.StringIO()
        writer = csv.writer(buf, delimiter=",", lineterminator="\r\n", quoting=csv.QUOTE_MINIMAL)
        writer.writerow(
            ["test_num", "side", "measure_mm", "status", "error", "phase", "timestamp"]
        )
        for r in rows:
            mm = r["measureMm"]
            writer.writerow(
                [
                    r["testNum"],
                    r["side"],
                    "" if mm is None else f"{float(mm):.2f}",
                    r["status"],
                    r["error"],
                    r["phase"],
                    r["timestamp"],
                ]
            )
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        filename = f"debug_trails_stage1_{stamp}.csv"
        return filename, "\ufeff" + buf.getvalue()

    def _set_phase(self, phase: str, test: int | None = None) -> None:
        with self._lock:
            self._state.phase = phase
            if test is not None:
                self._state.current_test = test
        self._host.cycle_notify()

    def _add_record(self, rec: TrailRecord) -> None:
        with self._lock:
            self._state.records.append(rec)
        self._host.cycle_notify()

    def _should_abort(self) -> bool:
        return self._stop.is_set()

    def _pausable_delay(self, sec: float) -> bool:
        remaining = max(0.0, float(sec))
        while remaining > 0:
            if self._should_abort():
                return True
            slice_s = min(0.05, remaining)
            time.sleep(slice_s)
            remaining -= slice_s
        return self._should_abort()

    def _finish(self, ok: bool, fault: str = "") -> None:
        with self._lock:
            self._state.active = False
            self._state.phase = "idle" if ok else "stopped"
            self._state.last_ok = ok
            if fault:
                self._state.fault = fault
        self._host.set_debug_trails_active(False)
        try:
            self._host.cmd_plc_cutter_r(False)
            self._host.cmd_plc_cutter_l(False)
        except Exception:
            pass
        try:
            self._host.cmd_plc_holder(False)
            self._host.cmd_plc_encoder(False)
        except Exception:
            pass
        self._host.cycle_log(
            f"Debug Trails Stage1 FIN {'OK' if ok else 'STOP'}"
            + (f" — {fault}" if fault else "")
        )
        self._host.cycle_notify()

    def _cut_pulse(self, sides: list[SideOne]) -> bool:
        pulse_ms = max(0, int(self._host.cutter_pulse_ms()))
        ok = True
        for s in sides:
            if s == "R":
                ok = self._host.cmd_plc_cutter_r(True) and ok
            else:
                ok = self._host.cmd_plc_cutter_l(True) and ok
        if pulse_ms > 0 and self._pausable_delay(pulse_ms / 1000.0):
            self._host.cmd_plc_cutter_r(False)
            self._host.cmd_plc_cutter_l(False)
            return False
        for s in sides:
            if s == "R":
                self._host.cmd_plc_cutter_r(False)
            else:
                self._host.cmd_plc_cutter_l(False)
        return ok

    def _measure_side(self, side: SideOne) -> float | None:
        if side == "R":
            if not self._host.cmd_motion_enc_measure_r():
                return None
        else:
            if not self._host.cmd_motion_enc_measure_l():
                return None
        return self._host.wait_encoder_mm(side, timeout_s=2.0)

    def _ng_error_text(self, side: SideOne, mm: float | None) -> str:
        # Dar tiempo a que llegue el detalle (láser E004/E005) tras LengthNG.
        time.sleep(0.12)
        fault = (self._host.feed_fault_for(side) or "").strip()
        if fault:
            return fault
        # Sin detalle aún: LengthNG del lado (E002 L / E003 R).
        return format_ui(TX_LENGTH_NG_R if side == "R" else TX_LENGTH_NG_L)

    def _run_one_side_feed(self, side: SideOne, test_num: int) -> TrailRecord:
        ts = _iso_now()
        self._set_phase(f"set0_{side}", test_num)
        set0 = (
            self._host.cmd_motion_enc_set0_r()
            if side == "R"
            else self._host.cmd_motion_enc_set0_l()
        )
        if not set0:
            return TrailRecord(
                test_num=test_num,
                side=side,
                measure_mm=None,
                status="error",
                error="Set0 comando falló",
                timestamp=ts,
                phase="set0",
            )
        if self._should_abort():
            return TrailRecord(
                test_num=test_num,
                side=side,
                measure_mm=None,
                status="aborted",
                error="Stop",
                timestamp=ts,
                phase="set0",
            )

        self._set_phase(f"feed_{side}", test_num)
        self._host.clear_motion_wait_flags()
        fed = (
            self._host.cmd_motion_feed_r()
            if side == "R"
            else self._host.cmd_motion_feed_l()
        )
        if not fed:
            return TrailRecord(
                test_num=test_num,
                side=side,
                measure_mm=None,
                status="error",
                error="Feed comando falló",
                timestamp=ts,
                phase="feed",
            )

        self._set_phase(f"wait_ok_{side}", test_num)
        timeout = self._host.feed_wait_timeout_s()
        results = self._host.wait_feed_length_ok_for(
            [side], timeout, abort_event=self._stop
        )
        side_res = results.get(side, "timeout")
        if self._should_abort() or side_res == "aborted":
            mm = self._measure_side(side)
            return TrailRecord(
                test_num=test_num,
                side=side,
                measure_mm=mm,
                status="aborted",
                error="Stop durante Feed",
                timestamp=_iso_now(),
                phase="feed",
            )

        self._set_phase(f"measure_{side}", test_num)
        mm = self._measure_side(side)
        if side_res != "ok":
            err = (
                self._ng_error_text(side, mm)
                if side_res == "ng"
                else "Timeout esperando LengthOK"
            )
            return TrailRecord(
                test_num=test_num,
                side=side,
                measure_mm=mm,
                status="fail",
                error=err,
                timestamp=_iso_now(),
                phase="all_ok",
            )
        if mm is None:
            return TrailRecord(
                test_num=test_num,
                side=side,
                measure_mm=None,
                status="fail",
                error="Sin medición OM válida tras Feed OK",
                timestamp=_iso_now(),
                phase="measure",
            )
        return TrailRecord(
            test_num=test_num,
            side=side,
            measure_mm=mm,
            status="ok",
            error="",
            timestamp=_iso_now(),
            phase="all_ok",
        )

    def _run_both_feed(self, test_num: int) -> list[TrailRecord]:
        ts = _iso_now()
        sides: list[SideOne] = ["R", "L"]
        self._set_phase("set0_both", test_num)
        ok_set0_r = self._host.cmd_motion_enc_set0_r()
        ok_set0_l = self._host.cmd_motion_enc_set0_l()
        if not ok_set0_r and not ok_set0_l:
            return [
                TrailRecord(
                    test_num=test_num,
                    side=s,
                    measure_mm=None,
                    status="error",
                    error="Set0 R+L falló",
                    timestamp=ts,
                    phase="set0",
                )
                for s in sides
            ]
        records: list[TrailRecord] = []
        if not ok_set0_r:
            records.append(
                TrailRecord(
                    test_num=test_num,
                    side="R",
                    measure_mm=None,
                    status="error",
                    error="Set0 R falló",
                    timestamp=ts,
                    phase="set0",
                )
            )
        if not ok_set0_l:
            records.append(
                TrailRecord(
                    test_num=test_num,
                    side="L",
                    measure_mm=None,
                    status="error",
                    error="Set0 L falló",
                    timestamp=ts,
                    phase="set0",
                )
            )
        feed_sides: list[SideOne] = []
        if ok_set0_r:
            feed_sides.append("R")
        if ok_set0_l:
            feed_sides.append("L")
        if not feed_sides:
            return records

        if self._should_abort():
            for s in feed_sides:
                records.append(
                    TrailRecord(
                        test_num=test_num,
                        side=s,
                        measure_mm=None,
                        status="aborted",
                        error="Stop",
                        timestamp=_iso_now(),
                        phase="set0",
                    )
                )
            return records

        self._set_phase("feed_both", test_num)
        self._host.clear_motion_wait_flags()
        armed: list[SideOne] = []
        for s in feed_sides:
            sent = (
                self._host.cmd_motion_feed_r()
                if s == "R"
                else self._host.cmd_motion_feed_l()
            )
            if sent:
                armed.append(s)
            else:
                records.append(
                    TrailRecord(
                        test_num=test_num,
                        side=s,
                        measure_mm=None,
                        status="error",
                        error=f"Feed {s} comando falló",
                        timestamp=_iso_now(),
                        phase="feed",
                    )
                )
        if not armed:
            return records

        self._set_phase("wait_ok_both", test_num)
        timeout = self._host.feed_wait_timeout_s()
        results = self._host.wait_feed_length_ok_for(
            armed, timeout, abort_event=self._stop
        )
        aborted = self._should_abort()

        for s in armed:
            self._set_phase(f"measure_{s}", test_num)
            mm = self._measure_side(s)
            side_res = results.get(s, "timeout")
            if aborted or side_res == "aborted":
                records.append(
                    TrailRecord(
                        test_num=test_num,
                        side=s,
                        measure_mm=mm,
                        status="aborted",
                        error="Stop durante Feed",
                        timestamp=_iso_now(),
                        phase="feed",
                    )
                )
                continue
            if side_res != "ok":
                err = (
                    self._ng_error_text(s, mm)
                    if side_res == "ng"
                    else "Timeout esperando LengthOK"
                )
                if mm is None:
                    err += "; sin medición OM"
                records.append(
                    TrailRecord(
                        test_num=test_num,
                        side=s,
                        measure_mm=mm,
                        status="fail",
                        error=err,
                        timestamp=_iso_now(),
                        phase="all_ok",
                    )
                )
                continue
            if mm is None:
                records.append(
                    TrailRecord(
                        test_num=test_num,
                        side=s,
                        measure_mm=None,
                        status="fail",
                        error="Sin medición OM válida tras Feed OK",
                        timestamp=_iso_now(),
                        phase="measure",
                    )
                )
            else:
                records.append(
                    TrailRecord(
                        test_num=test_num,
                        side=s,
                        measure_mm=mm,
                        status="ok",
                        error="",
                        timestamp=_iso_now(),
                        phase="all_ok",
                    )
                )
        return records

    def _arm_holder_encoder(self) -> bool:
        """Holder + Encoder ON al Start; se liberan solo al fin del lote/Stop."""
        ok_h = self._host.cmd_plc_holder(True)
        ok_e = self._host.cmd_plc_encoder(True)
        self._host.cycle_log(
            f"Debug Trails: Holder={'ON' if ok_h else 'FAIL'} "
            f"Encoder={'ON' if ok_e else 'FAIL'} (hasta fin de lote)"
        )
        return ok_h and ok_e

    def _run_stage1(self, side: SideSel, num_tests: int, wait_s: float) -> None:
        try:
            self._set_phase("arm_holder_encoder", 0)
            if not self._arm_holder_encoder():
                self._finish(False, "Holder/Encoder ON falló")
                return
            for i in range(1, num_tests + 1):
                if self._should_abort():
                    self._finish(False, "Stop")
                    return
                self._set_phase("test", i)
                self._host.cycle_log(f"Debug Trails Stage1 · prueba {i}/{num_tests}")

                if side == "Both":
                    recs = self._run_both_feed(i)
                else:
                    recs = [self._run_one_side_feed(side, i)]  # type: ignore[arg-type]

                for rec in recs:
                    self._add_record(rec)
                    self._host.cycle_log(
                        f"  #{rec.test_num} {rec.side}: "
                        f"{'—' if rec.measure_mm is None else f'{rec.measure_mm:.2f} mm'} "
                        f"[{rec.status}]"
                        + (f" — {rec.error}" if rec.error else "")
                    )

                if self._should_abort():
                    self._finish(False, "Stop")
                    return

                cut_sides: list[SideOne] = [r.side for r in recs if r.status == "ok"]
                if cut_sides:
                    self._set_phase("cut", i)
                    if not self._cut_pulse(cut_sides):
                        if self._should_abort():
                            self._finish(False, "Stop")
                            return
                        self._host.cycle_log("Debug Trails: pulso Cut falló / abortado")
                else:
                    self._host.cycle_log(
                        f"Debug Trails: sin Cut en prueba {i} (ningún lado All OK)"
                    )

                if i < num_tests:
                    self._set_phase("wait", i)
                    if self._pausable_delay(wait_s):
                        self._finish(False, "Stop")
                        return

            self._finish(True)
        except Exception as exc:
            self._finish(False, f"Excepción: {exc}")
