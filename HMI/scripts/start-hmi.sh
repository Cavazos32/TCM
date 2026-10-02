#!/usr/bin/env bash
# TCM HMI - servidor en ESTA terminal (no cerrar) y abre el navegador
set -u

PORT=5050
NO_BROWSER=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --port)
      PORT="${2:-5050}"
      shift 2
      ;;
    --no-browser)
      NO_BROWSER=1
      shift
      ;;
    *)
      echo "Uso: $0 [--port N] [--no-browser]" >&2
      exit 1
      ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HMI_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
URL="http://127.0.0.1:${PORT}"
DIST_INDEX="$HMI_ROOT/frontend/dist/index.html"

hmi_port_open() {
  if command -v nc >/dev/null 2>&1; then
    nc -z 127.0.0.1 "$PORT" >/dev/null 2>&1
    return $?
  fi
  # Fallback sin nc: intenta conectar con bash /dev/tcp
  (echo >/dev/tcp/127.0.0.1/"$PORT") >/dev/null 2>&1
}

python_exe() {
  if [[ -x "$HMI_ROOT/.venv/bin/python" ]]; then
    echo "$HMI_ROOT/.venv/bin/python"
  elif command -v python3 >/dev/null 2>&1; then
    echo "python3"
  else
    echo "python"
  fi
}

open_browser() {
  if [[ "$NO_BROWSER" -eq 1 ]]; then
    return 0
  fi
  if command -v xdg-open >/dev/null 2>&1; then
    xdg-open "$URL" >/dev/null 2>&1 &
  elif command -v open >/dev/null 2>&1; then
    open "$URL" >/dev/null 2>&1 &
  else
    echo "Abre manualmente: $URL"
  fi
}

if hmi_port_open; then
  echo "TCM HMI ya esta en ejecucion ($URL)"
  echo "cycle.py / app.py NO se recargan: cierra la terminal del servidor y vuelve a iniciar."
  open_browser
  echo "Si la pagina esta en blanco: Ctrl+F5"
  echo "Esta terminal no es el servidor; se puede cerrar."
  read -r -p "Enter para salir "
  exit 0
fi

if [[ ! -f "$DIST_INDEX" ]]; then
  echo "Compilando UI (frontend)..."
  (
    cd "$HMI_ROOT/frontend" || exit 1
    npm run build
  )
  if [[ $? -ne 0 ]]; then
    echo "npm run build fallo" >&2
    read -r -p "Enter para cerrar "
    exit 1
  fi
fi

PYTHON="$(python_exe)"
export PYTHONUNBUFFERED=1

if [[ "$NO_BROWSER" -eq 0 ]]; then
  (
    for _ in $(seq 1 60); do
      sleep 0.5
      if hmi_port_open; then
        open_browser
        break
      fi
    done
  ) &
fi

echo "TCM HMI - $URL"
echo "Deja esta terminal abierta. Cerrarla apaga el HMI."
echo ""

cd "$HMI_ROOT" || exit 1
"$PYTHON" -u app.py
code=$?
if [[ $code -ne 0 ]]; then
  echo ""
  echo "El servidor salio con error $code"
  read -r -p "Enter para cerrar "
fi
exit "$code"
