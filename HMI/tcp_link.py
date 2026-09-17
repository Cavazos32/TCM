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
HEARTBEAT_INTERVAL_SEC = 4.0
HEARTBEAT_STALE_SEC = 12.0
VERIFY_TIMEOUT_SEC = 3.0
RX_TIMEOUT_SEC = 0.5
RX_JOIN_TIMEOUT_SEC = 1.0


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
        self._io_lock = threading.Lock()
        self._conn_lock = threading.Lock()
        self._on_message = on_message
        self._on_connection = on_connection
        self._connected = False
        self._session = 0
        self._last_rx_mono = 0.0
        self._last_probe_mono = 0.0
        self._connect_gate = threading.Lock()

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
        # No notificar caída aquí: un reconnect intencional no debe latchear E06x.
        was = self._drop_link(notify=False)
        ok = self.connect(timeout=CONNECT_TIMEOUT, silent=silent)
        if was and not ok:
            self._schedule_connection(False)
        return ok

    def connect(self, timeout: float = CONNECT_TIMEOUT, silent: bool = False) -> bool:
        # Serializar: bg reconnect + /api/network/reconnect no se pisan.
        with self._connect_gate:
            return self._connect_locked(timeout=timeout, silent=silent)

    def _connect_locked(self, timeout: float, silent: bool) -> bool:
        self._drop_link(notify=False)

        session = 0
        sock: Optional[socket.socket] = None
        session_start = 0.0
        try:
            sock = socket.create_connection((self._host, self._port), timeout=timeout)
            sock.settimeout(RX_TIMEOUT_SEC)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            enable_tcp_keepalive(sock)
        except OSError as exc:
            self._connected = False
            if not silent:
                self._emit_error(f"Conexion fallida: {exc}")
            return False

        with self._conn_lock:
            with self._io_lock:
                self._session += 1
                session = self._session
                self._sock = sock
                self._last_rx_mono = 0.0
                self._last_probe_mono = 0.0
                self._stop_rx.clear()
                session_start = time.monotonic()
                self._rx_thread = threading.Thread(
                    target=self._rx_loop, args=(session, sock), daemon=True
                )
                self._rx_thread.start()

        self._send_probe()
        if not self._wait_verified(session_start):
            if not silent:
                self._emit_error(
                    f"Sin respuesta de {self._host}:{self._port} tras conectar"
                )
            self._drop_link(notify=False)
            return False

        with self._conn_lock:
            if session != self._session or self._sock is not sock:
                return False
            self._connected = True
            self._last_probe_mono = time.monotonic()

        self._schedule_connection(True)
        return True

    def disconnect(self, silent: bool = False) -> None:
        was = self._drop_link(notify=False)
        if was and not silent:
            self._schedule_connection(False)

    def _drop_link(self, *, notify: bool) -> bool:
        """
        Cierra socket y RX sin callbacks en este hilo.
        Nunca espera al RX mientras otro callback pueda querer _conn_lock.
        """
        sock: Optional[socket.socket] = None
        rx: Optional[threading.Thread] = None
        was = False
        with self._conn_lock:
            was = self._connected
            self._connected = False
            self._stop_rx.set()
            self._session += 1
            with self._io_lock:
                sock = self._sock
                self._sock = None
            rx = self._rx_thread
            self._rx_thread = None

        if sock:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                sock.close()
            except OSError:
                pass

        # Join fuera de cualquier lock — el RX puede estar en callback diferido.
        if rx and rx is not threading.current_thread() and rx.is_alive():
            rx.join(timeout=RX_JOIN_TIMEOUT_SEC)

        if notify and was:
            self._schedule_connection(False)
        return was

    def _schedule_connection(self, connected: bool) -> None:
        """Callbacks diferidos; ignora eventos obsoletos tras reconnect."""
        if not self._on_connection:
            return
        gen = self._session
        want = connected

        def run() -> None:
            try:
                if want:
                    if not self._connected or self._session != gen:
                        return
                elif self._connected:
                    return
                self._on_connection(want)
            except Exception:
                pass

        threading.Thread(target=run, daemon=True).start()

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
                self.disconnect(silent=False)
                continue

            if now - self._last_probe_mono >= HEARTBEAT_INTERVAL_SEC:
                self._last_probe_mono = now
                if not self._send_probe():
                    # No disconnect síncrono desde send: el probe ya falló.
                    self.disconnect(silent=False)
                    continue

            self._stop_bg.wait(0.5)

    def _send_json(self, payload: dict) -> bool:
        line = json.dumps(payload, separators=(",", ":")) + "\n"
        data = line.encode("utf-8")
        with self._io_lock:
            # Evitar ventana "connected=True / sock muerto": no enviar si el
            # flag de enlace ya cayó o el socket fue invalidado por RX/drop.
            if not self._connected or not self._sock:
                return False
            try:
                self._sock.sendall(data)
                return True
            except OSError:
                # Invalidar socket ya; disconnect async notifica y limpia RX.
                self._sock = None
        threading.Thread(
            target=lambda: self.disconnect(silent=False), daemon=True
        ).start()
        return False

    def _rx_loop(self, session: int, sock: socket.socket) -> None:
        buf = ""
        while not self._stop_rx.is_set() and session == self._session:
            try:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                if session != self._session:
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
                        try:
                            self._on_message(msg)
                        except Exception:
                            pass
            except socket.timeout:
                continue
            except OSError:
                break

        if session != self._session:
            return

        was = False
        with self._conn_lock:
            with self._io_lock:
                if self._sock is sock:
                    self._sock = None
            was = self._connected
            self._connected = False
        try:
            sock.close()
        except OSError:
            pass

        if was:
            self._schedule_connection(False)

    def _emit_error(self, text: str) -> None:
        if self._on_message:
            try:
                self._on_message({"type": "local_error", "message": text})
            except Exception:
                pass
