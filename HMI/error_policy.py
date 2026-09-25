"""Unified EXXX error latch for TCM.

Recovery decisions are intentionally outside this class. The catalog supplies
identity/module/display text; cycle/state owns the machine recovery sequence.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Optional

from error_catalog import format_ui, lookup


@dataclass
class ErrorLatch:
    active: bool = False
    code: str = ""
    byte: int = 0
    module: str = ""
    description: str = ""
    ui_text: str = ""

    def snapshot(self) -> dict[str, Any]:
        return {
            "active": self.active,
            "code": self.code,
            "byte": self.byte,
            "module": self.module,
            "description": self.description,
            "ui": self.ui_text,
        }


class ErrorPolicy:
    """Single EXXX latch.

    Set identifies and latches the actual fault.
    Reset/clear is explicit; callers must validate the underlying condition
    before invoking clear().
    """

    def __init__(self) -> None:
        self.latch = ErrorLatch()
        self._last: Optional[dict[str, Any]] = None

    def set_error(self, code_or_byte: str | int) -> Optional[dict[str, Any]]:
        entry = lookup(code_or_byte)
        if entry is None:
            return None

        byte = int(entry["byte"])
        if self.latch.active and self.latch.byte == byte:
            return {
                "ui": self.latch.ui_text,
                "code": self.latch.code,
                "duplicate": True,
                **self.latch.snapshot(),
            }

        ui = format_ui(byte)
        self.latch = ErrorLatch(
            active=True,
            code=str(entry["code"]),
            byte=byte,
            module=str(entry["module"]),
            description=str(entry["description"]),
            ui_text=ui,
        )
        self._last = self.latch.snapshot()
        return {
            "ui": ui,
            "code": self.latch.code,
            "duplicate": False,
            **self.latch.snapshot(),
        }

    def clear(self) -> ErrorLatch:
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
