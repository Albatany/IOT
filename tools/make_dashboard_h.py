#!/usr/bin/env python3
"""Ubah dashboard/dashboard.html menjadi pixel_iot_lab/dashboard.h.

Jalankan setiap kali dashboard.html diubah, lalu upload ulang sketch:

    python3 tools/make_dashboard_h.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "dashboard" / "dashboard.html"
DST = ROOT / "pixel_iot_lab" / "dashboard.h"
END = ')rawliteral"'

html = SRC.read_text(encoding="utf-8")
if END in html:
    raise SystemExit(f"dashboard.html tidak boleh mengandung teks {END}")

header = (
    "// File ini dibuat otomatis dari dashboard.html (jangan diedit tangan).\n"
    "// Isi dashboard disimpan di flash (PROGMEM) dan dikirim oleh ESP8266WebServer.\n"
    "#pragma once\n#include <pgmspace.h>\n\n"
    'const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(' + html + ')rawliteral";\n'
)
with open(DST, "w", encoding="utf-8", newline="\n") as f:
    f.write(header)

print(f"{DST.relative_to(ROOT)}: {len(header.encode('utf-8')):,} byte")
