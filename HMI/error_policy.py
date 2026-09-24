"""Política de errores C1/C2/C3 — flip-flop Set / Res.

Set: llega EXXX detalle → latchea + aplica acción según clase.
Res: Reset HMI explícito, o Res observacional (módulo ya OK + ciclo inactivo).
No observacional: E06x (enlace) ni E068 / lote NO OK (el aborto no se borra porque PF quedó Idle).

Clases (Doc/TCM - D.xlsx):
  C1 — Stop inmediato a todos; recovery: reset + validar + confirm + home
       (atajo: Res observacional si módulo OK y ciclo inactivo)
  C2 — Pausar (no siguiente step); recovery: Reset → Resume → terminar pieza
       → review OK → purga → Continuar ciclo → el lote sigue (Stop = botón)
  C3 — Igual que C2 con lote activo (Pause; no auto-termina la pieza)
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing import Any, Optional

from error_catalog import (
    CLASS_C1,
    CLASS_C2,
    CLASS_C3,
    error_class,
    format_ui,
    lookup,
)


@dataclass
class ErrorLatch:
    """Flip-flop de error activo (un Set a la vez; Res limpia)."""

    active: bool = False
    code: str = ""
    byte: int = 0
    module: str = ""
    description: str = ""
    err_class: str = ""
    ui_text: str = ""
    needs_confirm: bool = False
    needs_home: bool = False
    recovery: str = ""  # "home" | "restart_from_0" | "retry_process" | ""
    confirmed: bool = False
    set_at: float = 0.0  # monotonic; Res observacional espera edad mínima

    def snapshot(self) -> dict[str, Any]:
        return {
            "active": self.active,
            "code": self.code,
            "byte": self.byte,
            "module": self.module,
            "description": self.description,
            "class": self.err_class,
            "ui": self.ui_text,
            "needsConfirm": self.needs_confirm,
            "needsHome": self.needs_home,
            "recovery": self.recovery,
            "confirmed": self.confirmed,
        }


def classify_action(cls: Optional[str]) -> dict[str, Any]:
    """Qué hacer en Set / qué pedir en Res según clase."""
    if cls == CLASS_C1:
        return {
            "set": "stop_all",
            "recovery": "home",
            "needs_confirm": True,
            "needs_home": True,
        }
    if cls == CLASS_C2:
        return {
            "set": "pause",
            "recovery": "restart_from_0",
            "needs_confirm": False,
            "needs_home": False,
        }
    if cls == CLASS_C3:
        return {
            "set": "finish_step",
            "recovery": "retry_process",
            "needs_confirm": False,
            "needs_home": False,
        }
    return {
        "set": "stop_all",
        "recovery": "home",
        "needs_confirm": True,
        "needs_home": True,
    }


class ErrorPolicy:
    """Latch + helpers de Set/Res. La aplicación a ciclo/TCP la hace HmiState."""

    def __init__(self) -> None:
        self.latch = ErrorLatch()
        self._last: Optional[dict[str, Any]] = None

    def set_error(self, code_or_byte: str | int) -> Optional[dict[str, Any]]:
        """
        Set del flip-flop. Si ya hay error activo del mismo byte, no-op.
        Retorna dict con ui/class/action o None si no es EXXX conocido.
        """
        entry = lookup(code_or_byte)
        if entry is None:
            return None
        byte = int(entry["byte"])
        if self.latch.active and self.latch.byte == byte:
            return {
                "ui": self.latch.ui_text,
                "class": self.latch.err_class,
                "action": classify_action(self.latch.err_class)["set"],
                "duplicate": True,
                **self.latch.snapshot(),
            }

        cls = entry["class"]
        plan = classify_action(cls)
        ui = format_ui(byte)
        self.latch = ErrorLatch(
            active=True,
            code=entry["code"],
            byte=byte,
            module=entry["module"],
            description=entry["description"],
            err_class=cls,
            ui_text=ui,
            needs_confirm=plan["needs_confirm"],
            needs_home=plan["needs_home"],
            recovery=plan["recovery"],
            confirmed=False,
            set_at=time.monotonic(),
        )
        self._last = self.latch.snapshot()
        return {
            "ui": ui,
            "class": cls,
            "action": plan["set"],
            "duplicate": False,
            **self.latch.snapshot(),
        }

    def apply_e050_lot_branch(self) -> None:
        """E050 + pieza/lote: no C1 confirm/home. Soft-Res + Resume."""
        if self.latch.code != "E050":
            return
        self.latch.needs_confirm = False
        self.latch.needs_home = False
        self.latch.recovery = "e050_material"
        self.latch.confirmed = True
        self._last = self.latch.snapshot()

    def confirm(self) -> bool:
        """Operador confirma ventana C1."""
        if not self.latch.active or not self.latch.needs_confirm:
            return False
        self.latch.confirmed = True
        return True

    def can_reset(self) -> tuple[bool, str]:
        if not self.latch.active:
            return True, ""
        if self.latch.needs_confirm and not self.latch.confirmed:
            return False, "Confirmar error C1 antes de Reset"
        return True, ""

    def clear(self) -> ErrorLatch:
        """Res del flip-flop. Conserva last para la UI de recuperación."""
        old = self.latch
        if old.active:
            self._last = old.snapshot()
        self.latch = ErrorLatch()
        return old

    def snapshot(self) -> dict[str, Any]:
        data = self.latch.snapshot()
        if self._last:
            data["last"] = dict(self._last)
        return data
