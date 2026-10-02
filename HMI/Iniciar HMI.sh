#!/usr/bin/env bash
cd "$(dirname "$0")" || exit 1
bash "./scripts/start-hmi.sh" "$@"
code=$?
if [[ $code -ne 0 ]]; then
  echo ""
  echo "El HMI se detuvo con error."
  read -r -p "Enter para cerrar "
fi
exit "$code"
