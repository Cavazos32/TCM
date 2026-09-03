"""Enlace TCP compartido hacia módulos ESP32 — verificación, heartbeat y reconexión."""

from __future__ import annotations

import json
import socket
import sys
import threading
import time
from abc import ABC, abstractmethod
from typing import Any, Callable, Optional

CONNECT_TIMEOUT = 2.0
RECONNECT_SEC = 3.0
HEARTBEAT_INTERVAL_SEC = 2.0
HEARTBEAT_STALE_SEC = 5.0
VERIFY_TIMEOUT_SEC = 3.0
RX_TIMEOUT_SEC = 0.5


def enable_tcp_keepalive(sock: socket.socket) -> None:
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
    if sys.platform == "win32":
        try:
            sock.ioctl(socket.SIO_KEEPALIVE_VALS, (1, 3000, 1000))
        except (AttributeError, OSError):
            pass
    elif sys.platform == "linux":
        for opt, val in (
            (socket.TCP_KEEPIDLE, 3),
            (socket.TCP_KEEPINTVL, 1),
            (socket.TCP_KEEPCNT, 3),
        ):
            try:
                sock.setsockopt(socket.IPPROTO_TCP, opt, val)
            except OSError:
                pass


class ModuleTcpClient(ABC):
    """Cliente TCP con verificación al conectar y detección de enlace muerto."""

    def __init__(
        self,
        host: str,
        port: int,
        on_message: Optional[Callable[[dict], None]] = None,
        on_connection: Optional[Callable[[bool], None]] = None,
    ) -> None:
        self._host = host
        self._port = port
        self._sock: Optional[socket.socket] = None
        self._rx_thread: Optional[threading.Thread] = None
        self._bg_thread: Optional[threading.Thread] = None
        self._stop_rx = threading.Event()
        self._stop_bg = threading.Event()
        self._lock = threading.Lock()
        self._on_message = on_message
        self._on_connection = on_connection
        self._connected = False
        self._last_rx_mono = 0.0
        self._last_probe_mono = 0.0

    @property
    def connected(self) -> bool:
        return self._connected

    @abstractmethod
    def _send_probe(self) -> bool:
        """Envía comando de sondeo (status/poll) al módulo."""

    def start_background(self) -> None:
        if self._bg_thread and self._bg_thread.is_alive():
            return
        self._stop_bg.clear()
        self._bg_thread = threading.Thread(target=self._bg_loop, daemon=True)
        self._bg_thread.start()

    def stop_background(self) -> None:
        self._stop_bg.set()
        self.disconnect(silent=True)

    def reconnect(self, silent: bool = True) -> bool:
        was = self._connected
        self.disconnect(silent=True)
        if was and self._on_connection:
            self._on_connection(False)
        return self.connect(timeout=CONNECT_TIMEOUT, silent=silent)

    def connect(self, timeout: float = CONNECT_TIMEOUT, silent: bool = False) -> bool:
        self.disconnect(silent=True)
        try:
            sock = socket.create_connection((self._host, self._port), timeout=timeout)
            sock.settimeout(RX_TIMEOUT_SEC)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            enable_tcp_keepalive(sock)
            self._sock = sock
            self._last_rx_mono = 0.0
            self._last_probe_mono = 0.0
            # Antes del hilo RX: hello/status del PLC/Motion al aceptar TCP
            # deben contar para la verificación (no solo respuesta al poll).
            session_start = time.monotonic()
            self._stop_rx.clear()
            self._rx_thread = threading.Thread(target=self._rx_loop, daemon=True)
            self._rx_thread.start()

            self._send_probe()
            if not self._wait_verified(session_start):
                if not silent:
                    self._emit_error(
                        f"Sin respuesta de {self._host}:{self._port} tras conectar"
                    )
                self.disconnect(silent=True)
                return False

            self._connected = True
            self._last_probe_mono = time.monotonic()
            if self._on_connection:
                self._on_connection(True)
            return True
        except OSError as exc:
            self._connected = False
            if not silent:
                self._emit_error(f"Conexion fallida: {exc}")
            return False

    def disconnect(self, silent: bool = False) -> None:
        self._stop_rx.set()
        with self._lock:
            if self._sock:
                try:
                    self._sock.close()
                except OSError:
                    pass
                self._sock = None
        if self._rx_thread and self._rx_thread.is_alive():
            self._rx_thread.join(timeout=0.05)
        self._rx_thread = None
        was = self._connected
        self._connected = False
        if was and self._on_connection and not silent:
            self._on_connection(False)

    def send_command(self, **fields: Any) -> bool:
        payload = {"type": "command", **fields}
        return self._send_json(payload)

    def _wait_verified(self, since: float) -> bool:
        deadline = time.monotonic() + VERIFY_TIMEOUT_SEC
        while time.monotonic() < deadline:
            if self._last_rx_mono >= since - 0.001:
                return True
            time.sleep(0.05)
        return False

    def _bg_loop(self) -> None:
        while not self._stop_bg.is_set():
            if not self._connected:
                self.connect(silent=True)
                self._stop_bg.wait(RECONNECT_SEC)
                continue

            now = time.monotonic()
            if self._last_rx_mono and now - self._last_rx_mono > HEARTBEAT_STALE_SEC:
                self.disconnect(silent=True)
                continue

            if now - self._last_probe_mono >= HEARTBEAT_INTERVAL_SEC:
                self._last_probe_mono = now
                if not self._send_probe():
                    self.disconnect(silent=True)
                    continue

            self._stop_bg.wait(0.5)

    def _send_json(self, payload: dict) -> bool:
        line = json.dumps(payload, separators=(",", ":")) + "\n"
        data = line.encode("utf-8")
        with self._lock:
            if not self._sock:
                self._emit_error("Sin conexion TCP")
                return False
            try:
                self._sock.sendall(data)
                return True
            except OSError as exc:
                self._emit_error(f"Envio fallido: {exc}")
                self.disconnect()
                return False

    def _rx_loop(self) -> None:
        buf = ""
        sock = self._sock
        if not sock:
            return
        while not self._stop_rx.is_set():
            try:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                self._last_rx_mono = time.monotonic()
                buf += chunk.decode("utf-8", errors="replace")
                while "\n" in buf:
                    line, buf = buf.split("\n", 1)
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        msg = json.loads(line)
                    except json.JSONDecodeError:
                        self._emit_error(f"JSON invalido: {line[:120]}")
                        continue
                    if self._on_message:
                        self._on_message(msg)
            except socket.timeout:
                continue
            except OSError:
                break
        was = self._connected
        self._connected = False
        if was and self._on_connection:
            self._on_connection(False)

    def _emit_error(self, text: str) -> None:
        if self._on_message:
            self._on_message({"type": "local_error", "message": text})
