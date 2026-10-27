#!/usr/bin/env python3
"""Embed index.html into Index.h (index_html PROGMEM block)."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
html = root.joinpath("index.html").read_text(encoding="utf-8")
h_path = root.joinpath("Index.h")
content = h_path.read_text(encoding="utf-8")
marker = 'static const char index_html[] PROGMEM = R"HTML('
start = content.index(marker)
end = content.index(')HTML";', start)
feed_start = content.index("static const char feed_index_html[]", end)
new_content = (
    content[:start]
    + marker
    + html
    + "\n)HTML\";\n\n\n"
    + content[feed_start:]
)
h_path.write_text(new_content, encoding="utf-8")
print("OK:", len(html.encode("utf-8")), "bytes embedded")
