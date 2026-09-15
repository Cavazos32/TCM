"""Cliente TCP Andon — envía bytes de estado máquina (0x40–0x49).

Firmware: TCP :8769, hello role=andon, ping→pong.
Mute buzzer: command buzzerMute (Debug HMI).
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
        return self.send_command(command="ping")

    def send_machine_byte(self, byte: int) -> bool:
        """HMI → Andon: estado máquina (torre)."""
        if not self.connected:
            return False
        return self.send_command(byte=int(byte) & 0xFF)

    def set_buzzer_mute(self, mute: bool) -> bool:
        """HMI Debug → Andon: silenciar buzzer."""
        if not self.connected:
            return False
        return self.send_command(command="buzzerMute", mute=bool(mute))

    def set_output(self, out: str, on: bool) -> bool:
        """Prueba manual: green|yellow|red|buzzer."""
        if not self.connected:
            return False
        return self.send_command(command="setOut", out=str(out), on=bool(on))

    def all_off(self) -> bool:
        if not self.connected:
            return False
        return self.send_command(command="allOff")

    def resume_auto(self) -> bool:
        """Sale de modo manual y reaplica último estado máquina."""
        if not self.connected:
            return False
        return self.send_command(command="resumeAuto")
