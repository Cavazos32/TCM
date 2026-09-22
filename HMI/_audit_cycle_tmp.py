"""One-shot audit: FLOW_STEPS vs _run_lot. Delete after use."""
from __future__ import annotations

import re
from pathlib import Path

text = Path(__file__).with_name("cycle.py").read_text(encoding="utf-8")
m = re.search(r"FLOW_STEPS.*?=.*?\[(.*?)\n\]\nPROGRESS", text, re.S)
block = m.group(1) if m else ""
flow_keys = re.findall(r'"key":\s*"([^"]+)"', block)
run = text.split("def _run_lot")[1].split("def _finish")[0]
enter = re.findall(r'_enter\(rep,\s*qty,\s*"([^"]+)"\)', run)
dowait = re.findall(r'_do_wait\(rep,\s*qty,\s*"([^"]+)"', run)
print("FLOW:", flow_keys)
print("enter:", enter)
print("do_wait:", dowait)
print("missing from flow:", [k for k in enter if k not in flow_keys])
print(
    "flow not entered:",
    [k for k in flow_keys if k not in enter and k not in dowait],
)
# Checklist log snippets expected in cycle.py
needles = [
    "Pinzas ON (cierran)",
    "Pinzas OFF (abren)",
    "Feed OK",
    "Depósito MOVE →",
    "Depósito MOVE ok",
    "trigger PreFeeder Tfeed",
    "Pieza {rep}/{qty} OK ·",
    "WIP Delivery (continuo/match-lineal)",
    "lastPieceSec",
    "WIP_BLOWER_CONTINUOUS = True",
]
for n in needles:
    ok = n.replace("{rep}/{qty}", "") in text or n in text
    # piece ok format
    if "Pieza" in n:
        ok = "_log_piece_ok" in text and "OK ·" in text
    print(("OK  " if ok else "MISS") + f"  {n}")
