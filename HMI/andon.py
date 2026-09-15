"""Cliente TCP Andon — envía bytes de estado máquina (0x40–0x49).

Andon firmware: TCP pendiente (Config.h marca :8768, mismo puerto que PF).
Por defecto HMI usa :8769 para no chocar con PreFeeder Master :8768.
Si Andon no está, send es no-op silencioso.
"""

from __future__ import annotations

import os
from typing import Callable, Optional

from tcp_link import ModuleTcpClient

DEFAULT_HOST = os.environ.get("ANDON_HOST", "10.10.32.60")
DEFAULT_PORT = int(os.environ.get("ANDON_PORT", "8769"))


class AndonClient(ModuleTcpClient):
    def __init__(
        self,
        on_message: Optional[Callable[[dict], None]] = None,
        on_connection: Optional[Callable[[bool], None]] = None,
    ) -> None:
        super().__init__(DEFAULT_HOST, DEFAULT_PORT, on_message, on_connection)

    def _send_probe(self) -> bool:
        # Sin comando poll dedicado; el enlace basta.
        return True

    def send_machine_byte(self, byte: int) -> bool:
        """HMI → Andon: estado máquina (torre)."""
        if not self.connected:
            return False
        return self.send_command(byte=int(byte) & 0xFF)
