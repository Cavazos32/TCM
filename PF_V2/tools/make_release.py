#!/usr/bin/env python3
"""Prepara assets de GitHub Release para OTA por descarga (PF_V2).

Uso:
  python PF_V2/tools/make_release.py RUTA/PF_LR.ino.bin --product pf_lr_l
  python PF_V2/tools/make_release.py RUTA/PF_LR.ino.bin --product pf_lr_r
  python PF_V2/tools/make_release.py RUTA/PreFeeder_Master.ino.bin --product pf_master

Genera en --out (por defecto PF_V2/release/<product>/):
  <bin>.bin
  <manifest>.json   (version, url, md5, sha256, size)

Cada ESP32 apunta a su manifiesto en releases/latest/download/
(p. ej. pf_lr_L_version.json). Sube bin + json de cada producto al mismo Release.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_REPO = "Cavazos32/TCM"

PRODUCTS = {
    "pf_lr_l": {
        "config": ROOT / "PF_LR" / "Config.h",
        "bin_name": "PF_LR_L.bin",
        "manifest_name": "pf_lr_L_version.json",
    },
    "pf_lr_r": {
        "config": ROOT / "PF_LR" / "Config.h",
        "bin_name": "PF_LR_R.bin",
        "manifest_name": "pf_lr_R_version.json",
    },
    "pf_master": {
        "config": ROOT / "PreFeeder_Master" / "Config.h",
        "bin_name": "PF_Master.bin",
        "manifest_name": "pf_master_version.json",
    },
}


def read_define(config: Path, name: str) -> str | None:
    if not config.exists():
        return None
    m = re.search(rf'^\s*#define\s+{name}\s+"([^"]*)"', config.read_text(encoding="utf-8"), re.M)
    return m.group(1) if m else None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("bin", type=Path, help="binario compilado (.ino.bin)")
    ap.add_argument(
        "--product",
        required=True,
        choices=sorted(PRODUCTS.keys()),
        help="variante de firmware (L/R/Master)",
    )
    ap.add_argument("--version", help="versión (por defecto FW_VERSION del Config.h del producto)")
    ap.add_argument("--tag", help="tag del release (por defecto v<version>)")
    ap.add_argument("--repo", default=DEFAULT_REPO, help=f"usuario/repo (por defecto {DEFAULT_REPO})")
    ap.add_argument(
        "--lan-base",
        help="URL base LAN para el .bin en el manifiesto (p. ej. http://10.10.32.100/ota). "
        "Si se omite, usa GitHub Releases.",
    )
    ap.add_argument("--out", type=Path, help="carpeta de salida")
    ap.add_argument(
        "--allow-secrets",
        action="store_true",
        help="no abortar si el .bin contiene WIFI_PASS / OTA_PASSWORD (riesgo en repo público)",
    )
    args = ap.parse_args()

    spec = PRODUCTS[args.product]
    config = spec["config"]
    bin_name = spec["bin_name"]
    manifest_name = spec["manifest_name"]
    out_dir = args.out or (ROOT / "release" / args.product)

    data = args.bin.read_bytes()
    if len(data) < 1024 or data[0] != 0xE9:
        print(f"ERROR: {args.bin} no parece imagen de app ESP32 (byte mágico 0xE9).", file=sys.stderr)
        return 1

    version = args.version or read_define(config, "FW_VERSION")
    if not version:
        print(f"ERROR: no encontré FW_VERSION en {config}; usa --version.", file=sys.stderr)
        return 1
    version = version.lstrip("vV")
    if not re.fullmatch(r"\d+(\.\d+){0,3}", version):
        print(f"ERROR: versión inválida '{version}' (usa 1.2.3).", file=sys.stderr)
        return 1
    if version.encode() not in data:
        print(
            f"AVISO: el .bin no contiene el texto '{version}'. "
            f"¿Compilaste con FW_VERSION=\"{version}\"?",
            file=sys.stderr,
        )

    leaked: list[str] = []
    for name in ("WIFI_PASS", "WIFI_PASSWORD", "PF_MASTER_WIFI_PASS", "OTA_PASSWORD"):
        v = read_define(config, name)
        if v and len(v) >= 4 and v.encode() in data:
            leaked.append(name)
    if leaked and not args.allow_secrets:
        print(
            "ERROR: el .bin contiene " + ", ".join(leaked) + " del Config (valores no mostrados).\n"
            "Un release público expone el .bin. Usa --allow-secrets solo bajo tu riesgo.",
            file=sys.stderr,
        )
        return 2

    tag = args.tag or f"v{version}"
    if args.lan_base:
        url = f"{args.lan_base.rstrip('/')}/{bin_name}"
    else:
        url = f"https://github.com/{args.repo}/releases/download/{tag}/{bin_name}"
    manifest = {
        "version": version,
        "url": url,
        "md5": hashlib.md5(data).hexdigest(),
        "sha256": hashlib.sha256(data).hexdigest(),
        "size": len(data),
        "product": args.product,
        "built": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    }

    out_dir.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.bin, out_dir / bin_name)
    (out_dir / manifest_name).write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    print(f"Listo en {out_dir}:")
    print(f"  {bin_name}  ({len(data)} bytes)")
    print(f"  {manifest_name}")
    print(json.dumps(manifest, indent=2))
    print(f"\nRelease tag {tag}: sube {bin_name} + {manifest_name} como assets (no pre-release).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
