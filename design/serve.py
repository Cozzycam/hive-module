#!/usr/bin/env python3
"""Sprite editor server for Hive Module.

Parses firmware sprite/palette headers and serves a web-based pixel editor.
Run from anywhere:
    python design/serve.py [port]

Opens on 0.0.0.0:8080 — accessible from LAN for phone editing.
"""

import http.server
import json
import os
import re
import socket
import sys
from pathlib import Path
from urllib.parse import urlparse

SCRIPT_DIR = Path(__file__).parent.resolve()
REPO_ROOT = SCRIPT_DIR.parent
DESIGN_DIR = SCRIPT_DIR
PALETTE_H = REPO_ROOT / "firmware" / "include" / "palette.h"
SPRITES_H = REPO_ROOT / "firmware" / "include" / "sprites.h"
SAVE_DIR = DESIGN_DIR / "sprites"

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080


def parse_palette():
    colors = []
    for line in PALETTE_H.read_text().splitlines():
        m = re.match(
            r"#define\s+PAL_(\w+)\s+0x([0-9A-Fa-f]{4})\s+//\s*rgb\((\d+),\s*(\d+),\s*(\d+)\)",
            line,
        )
        if m:
            colors.append(
                {
                    "name": m.group(1),
                    "hex565": m.group(2).upper(),
                    "r": int(m.group(3)),
                    "g": int(m.group(4)),
                    "b": int(m.group(5)),
                }
            )
    return colors


def parse_sprites():
    sprites = []
    content = SPRITES_H.read_text()
    pattern = r"#define\s+(\w+)_W\s+(\d+)\s*\n#define\s+\1_H\s+(\d+)\s*\n.*?\{(.*?)\};"
    for m in re.finditer(pattern, content, re.DOTALL):
        name, w, h = m.group(1), int(m.group(2)), int(m.group(3))
        hexvals = re.findall(r"0x([0-9A-Fa-f]{4})", m.group(4))
        sprites.append(
            {"name": name, "width": w, "height": h, "pixels": [v.upper() for v in hexvals]}
        )
    return sprites


def get_saved_sprites():
    if not SAVE_DIR.exists():
        return []
    out = []
    for f in sorted(SAVE_DIR.glob("*.json")):
        try:
            out.append(json.loads(f.read_text()))
        except Exception:
            pass
    return out


def get_lan_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(DESIGN_DIR), **kwargs)

    def do_GET(self):
        path = urlparse(self.path).path
        if path in ("/", "/index.html"):
            self.path = "/sprite-editor.html"
            return super().do_GET()
        if path == "/api/data":
            return self._json_response(
                {"palette": parse_palette(), "sprites": parse_sprites(), "saved": get_saved_sprites()}
            )
        return super().do_GET()

    def do_POST(self):
        path = urlparse(self.path).path
        if path == "/api/save":
            length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(length)
            try:
                sprite = json.loads(body)
                name = re.sub(r"[^a-zA-Z0-9_]", "_", sprite.get("name", "unnamed"))
                SAVE_DIR.mkdir(exist_ok=True)
                (SAVE_DIR / f"{name}.json").write_text(json.dumps(sprite, indent=2))
                self._json_response({"ok": True, "name": name})
            except Exception as e:
                self._json_response({"ok": False, "error": str(e)}, 400)
            return

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def _json_response(self, data, code=200):
        body = json.dumps(data).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", len(body))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        if args and "/api/" in str(args[0]):
            return
        super().log_message(fmt, *args)


if __name__ == "__main__":
    ip = get_lan_ip()
    print(f"\n  Hive Module Sprite Editor")
    print(f"  ========================")
    print(f"  Local:   http://localhost:{PORT}")
    print(f"  Network: http://{ip}:{PORT}")
    print(f"\n  Open either URL in your browser or phone.\n")

    server = http.server.HTTPServer(("0.0.0.0", PORT), Handler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")
        server.server_close()
