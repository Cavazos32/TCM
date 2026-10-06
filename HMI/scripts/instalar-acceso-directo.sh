#!/usr/bin/env bash
# Instala acceso directo en el Escritorio y en ~/bin.
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HMI_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
LAUNCHER="$HMI_ROOT/scripts/reiniciar-hmi-chrome.sh"
BIN_DIR="${HOME}/bin"
DESKTOP_DIR=""

for d in "$HOME/Escritorio" "$HOME/Desktop"; do
  if [[ -d "$d" ]]; then
    DESKTOP_DIR="$d"
    break
  fi
done

if [[ -z "$DESKTOP_DIR" ]]; then
  DESKTOP_DIR="$HOME/Desktop"
  mkdir -p "$DESKTOP_DIR"
fi

chmod +x "$LAUNCHER"
mkdir -p "$BIN_DIR"
ln -sf "$LAUNCHER" "$BIN_DIR/tcm-hmi-chrome"
ln -sf "$LAUNCHER" "$BIN_DIR/abrir-hmi.sh"

DESKTOP_FILE="$DESKTOP_DIR/TCM-HMI.desktop"
cat >"$DESKTOP_FILE" <<EOF
[Desktop Entry]
Version=1.0
Type=Application
Name=TCM HMI
Comment=Reinicia y abre la HMI en Google Chrome
Exec=$BIN_DIR/tcm-hmi-chrome
Icon=applications-engineering
Terminal=false
Categories=Utility;Development;
StartupNotify=true
EOF

chmod +x "$DESKTOP_FILE"
if command -v gio >/dev/null 2>&1; then
  gio set "$DESKTOP_FILE" metadata::trusted true 2>/dev/null || true
fi

echo "Acceso directo: $DESKTOP_FILE"
echo "Comando: $BIN_DIR/tcm-hmi-chrome"
