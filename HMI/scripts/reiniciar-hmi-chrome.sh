#!/usr/bin/env bash
# Reinicia el servidor HMI (puerto 5050) y abre la interfaz en Google Chrome.
set -u

PORT="${HMI_PORT:-5050}"
URL="http://127.0.0.1:${PORT}"
SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
HMI_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TCM_ROOT="$(cd "$HMI_ROOT/.." && pwd)"
VENV_PYTHON="$TCM_ROOT/.venv/bin/python"
LOG="${HMI_LOG:-/tmp/tcm-hmi-server.log}"

kill_port() {
  if command -v fuser >/dev/null 2>&1; then
    fuser -k "${PORT}/tcp" 2>/dev/null || true
    return
  fi
  if command -v lsof >/dev/null 2>&1; then
    lsof -ti ":${PORT}" | xargs -r kill -9 2>/dev/null || true
  fi
}

wait_http() {
  local i
  for i in $(seq 1 40); do
    if curl -s -o /dev/null --max-time 1 "$URL"; then
      return 0
    fi
    sleep 0.25
  done
  return 1
}

open_chrome() {
  local chrome=""
  for c in google-chrome google-chrome-stable chromium chromium-browser; do
    if command -v "$c" >/dev/null 2>&1; then
      chrome="$c"
      break
    fi
  done
  if [[ -z "$chrome" ]]; then
    if command -v xdg-open >/dev/null 2>&1; then
      xdg-open "$URL" >/dev/null 2>&1 &
    fi
    return
  fi
  local profile="${HMI_CHROME_PROFILE:-$HOME/chrome-profile/Fork-3}"
  if [[ -d "$profile" ]]; then
    "$chrome" --new-window --no-sandbox \
      --user-data-dir="$profile" \
      --password-store=basic \
      --proxy-bypass-list='localhost;127.0.0.1;[::1]' \
      "$URL" >/dev/null 2>&1 &
  else
    "$chrome" --new-window "$URL" >/dev/null 2>&1 &
  fi
}

kill_port
sleep 1

if [[ ! -x "$VENV_PYTHON" ]]; then
  echo "No se encontró $VENV_PYTHON" >&2
  echo "Crea el entorno: cd $TCM_ROOT && python3 -m venv .venv && .venv/bin/pip install -r HMI/requirements.txt" >&2
  exit 1
fi

cd "$HMI_ROOT" || exit 1
export HMI_PORT="$PORT"
export PYTHONUNBUFFERED=1
nohup "$VENV_PYTHON" -u app.py >>"$LOG" 2>&1 &

if ! wait_http; then
  echo "El servidor HMI no respondió en $URL (ver $LOG)" >&2
  exit 1
fi

open_chrome
