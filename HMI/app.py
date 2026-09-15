"""TCM HMI — Flask + React (frontend)."""

from __future__ import annotations

import atexit
import json
import os
import queue
import subprocess
import sys
import time
from pathlib import Path

from flask import Flask, Response, jsonify, request, send_from_directory
from waitress import serve

from state import get_state

HMI_ROOT = Path(__file__).resolve().parent
FRONTEND_DIR = HMI_ROOT / "frontend"
DIST_DIR = FRONTEND_DIR / "dist"
SRC_DIR = FRONTEND_DIR / "src"

app = Flask(__name__, static_folder=str(DIST_DIR), static_url_path="")
_sse_queues: list[queue.Queue[str | None]] = []
_SSE_MAX_CLIENTS = 3
HMI_HOST = os.environ.get("HMI_HOST", "0.0.0.0")
HMI_PORT = int(os.environ.get("HMI_PORT", "5050"))
# 1 = rebuild UI al arrancar si src > dist (default). 0 = no tocar npm.
HMI_AUTO_BUILD_UI = os.environ.get("HMI_AUTO_BUILD_UI", "1").strip() not in (
    "0",
    "false",
    "False",
    "no",
)


def _mtime(path: Path) -> float:
    try:
        return path.stat().st_mtime
    except OSError:
        return 0.0


def _newest_mtime(root: Path) -> float:
    newest = 0.0
    if root.is_file():
        return _mtime(root)
    if not root.is_dir():
        return 0.0
    for f in root.rglob("*"):
        if f.is_file():
            newest = max(newest, _mtime(f))
    return newest


def _frontend_sources_mtime() -> float:
    newest = 0.0
    for p in (
        SRC_DIR,
        FRONTEND_DIR / "index.html",
        FRONTEND_DIR / "package.json",
        FRONTEND_DIR / "vite.config.ts",
        FRONTEND_DIR / "vite.config.js",
        FRONTEND_DIR / "tailwind.config.js",
        FRONTEND_DIR / "postcss.config.js",
    ):
        newest = max(newest, _newest_mtime(p))
    return newest


def frontend_dist_is_stale() -> bool:
    """True si no hay dist o el código fuente es más reciente que dist."""
    index = DIST_DIR / "index.html"
    if not index.is_file():
        return True
    return _frontend_sources_mtime() > (_mtime(index) + 0.5)


def ensure_frontend_dist() -> bool:
    """
    Recompila frontend/dist si está ausente o desactualizado.
    Devuelve True si dist/index.html existe al final.
    """
    index = DIST_DIR / "index.html"
    if not frontend_dist_is_stale():
        return True

    if not HMI_AUTO_BUILD_UI:
        print(
            "[HMI] UI desactualizada (src > dist). "
            "Ejecuta: cd frontend && npm run build  "
            "(o HMI_AUTO_BUILD_UI=1 al arrancar)"
        )
        return index.is_file()

    if not (FRONTEND_DIR / "package.json").is_file():
        print("[HMI] No hay frontend/package.json — no se puede compilar UI")
        return index.is_file()

    print("[HMI] Compilando frontend (src más reciente que dist)…")
    t0 = time.monotonic()
    try:
        # Windows: npm.cmd; npm run build en frontend/
        npm = "npm.cmd" if sys.platform == "win32" else "npm"
        proc = subprocess.run(
            [npm, "run", "build"],
            cwd=str(FRONTEND_DIR),
            check=False,
            capture_output=True,
            text=True,
            timeout=180,
        )
    except FileNotFoundError:
        print("[HMI] npm no encontrado — no se pudo recompilar UI")
        return index.is_file()
    except subprocess.TimeoutExpired:
        print("[HMI] Timeout compilando UI (>180s)")
        return index.is_file()

    elapsed = time.monotonic() - t0
    if proc.returncode != 0:
        err = (proc.stderr or proc.stdout or "").strip()[-800:]
        print(f"[HMI] Falló npm run build ({elapsed:.1f}s):\n{err}")
        return index.is_file()

    print(f"[HMI] UI lista en frontend/dist ({elapsed:.1f}s)")
    return index.is_file()


def _broadcast_sse() -> None:
    """Empuja el snapshot más reciente. Si la cola está llena, descarta lo viejo
    y conserva el cliente — no cortar el SSE (el ciclo notifica muchas veces/s)."""
    data = json.dumps(get_state().snapshot(), separators=(",", ":"))
    for q in list(_sse_queues):
        try:
            q.put_nowait(data)
        except queue.Full:
            try:
                q.get_nowait()
            except queue.Empty:
                pass
            try:
                q.put_nowait(data)
            except queue.Full:
                pass


def _init_state() -> None:
    get_state().subscribe(_broadcast_sse)


_init_state()


@app.after_request
def _no_cache_spa_shell(resp: Response):
    """Evita que el navegador conserve index.html apuntando a JS viejo hasheado."""
    ct = resp.content_type or ""
    if request.path in ("/", "/index.html") or ct.startswith("text/html"):
        resp.headers["Cache-Control"] = "no-cache, no-store, must-revalidate"
        resp.headers["Pragma"] = "no-cache"
        resp.headers["Expires"] = "0"
    return resp


@atexit.register
def _shutdown() -> None:
    try:
        get_state().stop()
    except Exception:
        pass


@app.route("/")
def index():
    if DIST_DIR.joinpath("index.html").is_file():
        return send_from_directory(DIST_DIR, "index.html")
    return (
        "<h1>TCM HMI</h1><p>Ejecuta <code>cd frontend &amp;&amp; npm install &amp;&amp; npm run build</code></p>",
        503,
        {"Content-Type": "text/html; charset=utf-8"},
    )


@app.route("/api/state")
def api_state():
    return jsonify(get_state().snapshot())


@app.route("/api/events")
def api_events():
    # Limitar SSE: cada cliente ocupa un hilo Waitress; demasiados = HMI “congelada”.
    while len(_sse_queues) >= _SSE_MAX_CLIENTS:
        old = _sse_queues.pop(0)
        try:
            old.put_nowait(None)
        except queue.Full:
            pass

    q: queue.Queue[str | None] = queue.Queue(maxsize=4)
    _sse_queues.append(q)

    def stream():
        try:
            yield f"data: {json.dumps(get_state().snapshot())}\n\n"
            while True:
                try:
                    payload = q.get(timeout=15)
                except queue.Empty:
                    yield ": keepalive\n\n"
                    continue
                if payload is None:
                    break
                yield f"data: {payload}\n\n"
        finally:
            if q in _sse_queues:
                _sse_queues.remove(q)

    return Response(
        stream(),
        mimetype="text/event-stream",
        headers={
            "Cache-Control": "no-cache",
            "X-Accel-Buffering": "no",
            "Connection": "keep-alive",
        },
    )


@app.post("/api/network/reconnect")
def api_network_reconnect():
    return jsonify({"ok": True, "links": get_state().reconnect_all_modules()})


@app.post("/api/model")
def api_model():
    body = request.get_json(silent=True) or {}
    idx = int(body.get("index", 0))
    get_state().select_model(idx)
    return jsonify({"ok": True})


@app.post("/api/motion/mm-rpm")
def api_mm_rpm():
    body = request.get_json(silent=True) or {}
    get_state().set_mm_rpm(float(body.get("mm", -45)), float(body.get("rpm", 1200)))
    return jsonify({"ok": True})


@app.post("/api/start")
def api_start():
    body = request.get_json(silent=True) or {}
    qty = body.get("qty")
    return jsonify(get_state().cmd_start(qty=int(qty) if qty is not None else None))


@app.post("/api/stop")
def api_stop():
    return jsonify(get_state().cmd_stop())


@app.post("/api/resume")
def api_resume():
    return jsonify(get_state().cmd_resume())


@app.post("/api/cycle/pause")
def api_cycle_pause():
    return jsonify(get_state().cmd_cycle_pause())


@app.post("/api/cycle/reset")
def api_cycle_reset():
    body = request.get_json(silent=True) or {}
    return jsonify(
        get_state().cmd_error_reset(
            confirm=bool(body.get("confirm", False)),
            do_home=bool(body.get("doHome", False)),
        )
    )


@app.post("/api/error/confirm")
def api_error_confirm():
    return jsonify(get_state().cmd_error_confirm())


@app.post("/api/error/reset")
def api_error_reset():
    body = request.get_json(silent=True) or {}
    return jsonify(
        get_state().cmd_error_reset(
            confirm=bool(body.get("confirm", False)),
            do_home=bool(body.get("doHome", True)),
        )
    )


@app.post("/api/cycle/materialist")
def api_cycle_materialist():
    body = request.get_json(silent=True) or {}
    return jsonify(get_state().cmd_cycle_materialist(bool(body.get("on", True))))


@app.post("/api/cycle/step-by-step")
def api_cycle_step_by_step():
    body = request.get_json(silent=True) or {}
    return jsonify(get_state().cmd_cycle_step_by_step(bool(body.get("on", True))))


@app.post("/api/cycle/trial-mode")
def api_cycle_trial_mode():
    body = request.get_json(silent=True) or {}
    return jsonify(get_state().cmd_cycle_trial_mode(bool(body.get("on", True))))


@app.get("/api/cycle/config")
def api_cycle_config_get():
    return jsonify({"ok": True, "config": get_state().get_cycle_config()})


@app.post("/api/cycle/config")
def api_cycle_config_set():
    body = request.get_json(silent=True) or {}
    cfg = get_state().set_cycle_config(body)
    return jsonify({"ok": True, "config": cfg})


@app.post("/api/cycle/config/reload")
def api_cycle_config_reload():
    cfg = get_state().reload_cycle_config()
    return jsonify({"ok": True, "config": cfg})


@app.post("/api/motion")
def api_motion():
    body = request.get_json(silent=True) or {}
    action = body.pop("action", "")
    return jsonify(get_state().cmd_motion(action, **body))


@app.post("/api/plc")
def api_plc():
    body = request.get_json(silent=True) or {}
    action = body.pop("action", "")
    return jsonify(get_state().cmd_plc(action, **body))


@app.post("/api/prefeeder")
def api_pf():
    body = request.get_json(silent=True) or {}
    action = body.get("action", "")
    return jsonify(get_state().cmd_pf(action))


@app.post("/api/log/clear")
def api_log_clear():
    body = request.get_json(silent=True) or {}
    get_state().clear_log(body.get("target", "main"))
    return jsonify({"ok": True})


@app.post("/api/motion/enc-poll")
def api_enc_poll():
    body = request.get_json(silent=True) or {}
    if body.get("enable", True):
        get_state().start_enc_poll()
    else:
        get_state().stop_enc_poll()
    return jsonify({"ok": True})


@app.get("/api/motion/feed-offset")
def api_motion_feed_offset_get():
    return jsonify(get_state().refresh_feed_offset())


@app.post("/api/motion/feed-offset")
def api_motion_feed_offset_set():
    body = request.get_json(silent=True) or {}
    return jsonify(
        get_state().set_feed_offset(
            float(body.get("feedOffsetMmL", body.get("offsetMm", 0.0))),
            float(body.get("feedOffsetMmR", body.get("offsetMmB", 0.0))),
        )
    )


@app.route("/<path:asset_path>")
def spa_assets(asset_path: str):
    target = DIST_DIR / asset_path
    if target.is_file():
        return send_from_directory(DIST_DIR, asset_path)
    if DIST_DIR.joinpath("index.html").is_file():
        return send_from_directory(DIST_DIR, "index.html")
    return jsonify({"error": "not found"}), 404


def main() -> None:
    ensure_frontend_dist()
    get_state().start()
    addr = f"{HMI_HOST}:{HMI_PORT}"
    ui = DIST_DIR / "index.html"
    if ui.is_file():
        stale = " (src más nuevo — rebuild falló o desactivado)" if frontend_dist_is_stale() else ""
        print(f"TCM HMI — http://{addr}  (UI: frontend/dist){stale}")
    else:
        print(f"TCM HMI — http://{addr}  (UI no compilada — cd frontend && npm run build)")
    # Más hilos + timeout de canal: evita CloseWait eternos que dejan la HMI sin responder.
    serve(app, listen=addr, threads=24, channel_timeout=30)


if __name__ == "__main__":
    main()
