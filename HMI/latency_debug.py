"""Instrumentación de latencia HMI — desactivada por defecto.

Activar con variable de entorno ``HMI_LATENCY_DEBUG=1``.
"""

from __future__ import annotations

import os
import sys
import time
from typing import Any

_ENABLED = os.environ.get("HMI_LATENCY_DEBUG", "").strip().lower() in (
    "1",
    "true",
    "yes",
    "on",
)


def enabled() -> bool:
    return _ENABLED


def mark(tag: str, *, source: str = "", **extra: Any) -> None:
    if not _ENABLED:
        return
    now = time.monotonic()
    parts = [f"[LAT-HMI] T{tag}", f"t={now:.6f}s"]
    if source:
        parts.append(f"src={source}")
    for key, value in extra.items():
        parts.append(f"{key}={value}")
    print(" ".join(parts), file=sys.stderr, flush=True)


def mark_command(module: str, cmd: str) -> None:
    """T0/T1: comando entra a HMI y se envía por TCP."""
    mark("0", source=module, cmd=cmd)
    mark("1", source=module, cmd=cmd)
