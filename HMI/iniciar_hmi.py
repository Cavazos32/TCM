#!/usr/bin/env python3
"""Lanza TCM HMI usando el Python del venv (.venv).

Uso (con el Python del sistema; el script se re-lanza solo en el venv):
  python3 iniciar_hmi.py
  python3 iniciar_hmi.py --port 5050
  python3 iniciar_hmi.py --no-browser
"""

from __future__ import annotations

import argparse
import os
import socket
import subprocess
import sys
import threading
import time
import webbrowser
from pathlib import Path

HMI_ROOT = Path(__file__).resolve().parent
TCM_ROOT = HMI_ROOT.parent
PORT_DEFAULT = 5050


def _venv_python() -> Path | None:
    if sys.platform == "win32":
        candidate = TCM_ROOT / ".venv" / "Scripts" / "python.exe"
    else:
        candidate = TCM_ROOT / ".venv" / "bin" / "python"
    return candidate if candidate.is_file() else None


def _running_in_venv() -> bool:
    venv = _venv_python()
    if venv is None:
        return False
    try:
        return Path(sys.executable).resolve() == venv.resolve()
    except OSError:
        return False


def _ensure_venv_python() -> None:
    """Re-lanza este script con el Python del venv si hace falta."""
    if _running_in_venv():
        return
    venv = _venv_python()
    if venv is None:
        print(
            "No se encontro TCM/.venv (raiz del repo).\n"
            "Crea el entorno:\n"
            "  cd TCM\n"
            "  python3 -m venv .venv\n"
            "  source .venv/bin/activate   # Windows: .venv\\Scripts\\activate\n"
            "  pip install -r HMI/requirements.txt",
            file=sys.stderr,
        )
        sys.exit(1)
    os.environ["PYTHONUNBUFFERED"] = "1"
    os.execv(str(venv), [str(venv), "-u", str(Path(__file__).resolve()), *sys.argv[1:]])


def _port_open(port: int, timeout_s: float = 0.4) -> bool:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(timeout_s)
    try:
        sock.connect(("127.0.0.1", port))
        return True
    except OSError:
        return False
    finally:
        sock.close()


def _open_browser(url: str) -> None:
    try:
        webbrowser.open(url)
    except Exception:
        print(f"Abre manualmente: {url}")


def _wait_and_open_browser(url: str, port: int) -> None:
    for _ in range(60):
        time.sleep(0.5)
        if _port_open(port):
            _open_browser(url)
            return


def _build_frontend_if_needed() -> None:
    dist_index = HMI_ROOT / "frontend" / "dist" / "index.html"
    if dist_index.is_file():
        return
    print("Compilando UI (frontend)...")
    result = subprocess.run(
        ["npm", "run", "build"],
        cwd=HMI_ROOT / "frontend",
        shell=(sys.platform == "win32"),
    )
    if result.returncode != 0:
        print("npm run build fallo", file=sys.stderr)
        _pause("Enter para cerrar ")
        sys.exit(1)


def _pause(msg: str) -> None:
    try:
        input(msg)
    except EOFError:
        pass


def main() -> int:
    parser = argparse.ArgumentParser(description="Lanza TCM HMI desde el venv")
    parser.add_argument("--port", type=int, default=PORT_DEFAULT)
    parser.add_argument("--no-browser", action="store_true")
    args = parser.parse_args()

    _ensure_venv_python()

    port = args.port
    url = f"http://127.0.0.1:{port}"
    os.environ.setdefault("HMI_PORT", str(port))
    os.environ["PYTHONUNBUFFERED"] = "1"

    if _port_open(port):
        print(f"TCM HMI ya esta en ejecucion ({url})")
        print(
            "cycle.py / app.py NO se recargan: cierra la terminal del servidor y vuelve a iniciar."
        )
        if not args.no_browser:
            _open_browser(url)
        print("Si la pagina esta en blanco: Ctrl+F5")
        print("Esta terminal no es el servidor; se puede cerrar.")
        _pause("Enter para salir ")
        return 0

    _build_frontend_if_needed()

    if not args.no_browser:
        threading.Thread(
            target=_wait_and_open_browser,
            args=(url, port),
            daemon=True,
        ).start()

    print(f"TCM HMI - {url}")
    print("Deja esta terminal abierta. Cerrarla apaga el HMI.")
    print()

    result = subprocess.run(
        [sys.executable, "-u", str(HMI_ROOT / "app.py")],
        cwd=HMI_ROOT,
    )
    code = int(result.returncode or 0)
    if code != 0:
        print(f"\nEl servidor salio con error {code}")
        _pause("Enter para cerrar ")
    return code


if __name__ == "__main__":
    raise SystemExit(main())
