#!/usr/bin/env python3
"""Copia manifiestos + .bin LAN a PreFeeder_Master/data/ota/ (LittleFS upload).

Tras make_release con --lan-base:
  py PF_V2/tools/make_release.py build_L/PF_LR.ino.bin --product pf_lr_l --lan-base http://10.10.32.100/ota --allow-secrets
  py PF_V2/tools/sync_ota_to_master.py

Luego Arduino IDE: PreFeeder_Master → Herramientas → ESP32 LittleFS → Upload Data.
"""
from __future__ import annotations

import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RELEASE = ROOT / "release"
DEST = ROOT / "PreFeeder_Master" / "data" / "ota"

PRODUCTS = ("pf_lr_l", "pf_lr_r", "pf_master")


def main() -> int:
    DEST.mkdir(parents=True, exist_ok=True)
    n = 0
    for p in PRODUCTS:
        src_dir = RELEASE / p
        if not src_dir.is_dir():
            continue
        for f in src_dir.iterdir():
            if f.suffix in (".json", ".bin"):
                shutil.copy2(f, DEST / f.name)
                print(f"  {f.name}")
                n += 1
    if not n:
        print("No hay archivos en PF_V2/release/pf_lr_* — ejecuta make_release.py primero.", file=sys.stderr)
        return 1
    print(f"\nListo en {DEST}. Sube LittleFS al Master y flashea L/R con OTA_MANIFEST_URL LAN.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
