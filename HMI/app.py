"""TCM HMI — Flask + React (frontend)."""

from __future__ import annotations

import atexit
import json
import os
import queue
from pathlib import Path

from flask import Flask, Response, jsonify, request, send_from_directory
from waitress import serve

from state import get_state

HMI_ROOT = Path(__file__).resolve().parent
DIST_DIR = HMI_ROOT / "frontend" / "dist"

app = Flask(__name__, static_folder=str(DIST_DIR), static_url_path="")
_sse_queues: list[queue.Queue[str]] = []

HMI_HOST = os.environ.get("HMI_HOST", "0.0.0.0")
HMI_PORT = int(os.environ.get("HMI_PORT", "5050"))


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
    # Cola corta: solo importa el estado más reciente (UI), no cada evento.
    q: queue.Queue[str] = queue.Queue(maxsize=4)
    _sse_queues.append(q)

    def stream():
        try:
            yield f"data: {json.dumps(get_state().snapshot())}\n\n"
            while True:
                try:
                    payload = q.get(timeout=25)
                    yield f"data: {payload}\n\n"
                except queue.Empty:
                    yield ": keepalive\n\n"
        finally:
            if q in _sse_queues:
                _sse_queues.remove(q)

    return Response(
        stream(),
        mimetype="text/event-stream",
        headers={
            "Cache-Control": "no-cache",
            "X-Accel-Buffering": "no",
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
    return jsonify(get_state().cmd_cycle_reset())


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
    get_state().start()
    addr = f"{HMI_HOST}:{HMI_PORT}"
    ui = DIST_DIR / "index.html"
    if ui.is_file():
        print(f"TCM HMI — http://{addr}  (UI: frontend/dist)")
    else:
        print(f"TCM HMI — http://{addr}  (UI no compilada — cd frontend && npm run build)")
    serve(app, listen=addr, threads=4)


if __name__ == "__main__":
    main()
