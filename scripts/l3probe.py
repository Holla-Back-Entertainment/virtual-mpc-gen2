#!/usr/bin/env python3
"""Probe server for the virtual Live III surface (engine work, not the page): one Surface held for
the whole session, served on the page port with the same POST /control and GET /surface as
panel/gen2.py, plus POST /raw {"hex": "b0 13 7f", "port": 1} to inject arbitrary MIDI.
Run instead of the page server:  python3 lab.py run --headless --no-panel; python3 scripts/l3probe.py
"""
import json
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(HERE, "..", "panel"))
from lab import HOST, PORTS  # noqa: E402
import surface  # noqa: E402

S = surface.Surface(HOST, PORTS["cs"])


class H(BaseHTTPRequestHandler):
    def reply(self, code, obj):
        b = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        if self.path == "/surface":
            return self.reply(200, S.snapshot())
        if self.path == "/log":
            return self.reply(200, list(S.log))
        self.reply(404, {})

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
        try:
            if self.path == "/raw":
                S.send(bytes.fromhex(body["hex"]), body.get("port", surface.REPLY_PORT))
            elif self.path == "/control":
                S.control(body["name"], body["action"], body.get("value", 127))
            else:
                return self.reply(404, {})
        except (KeyError, ValueError, OSError) as e:
            return self.reply(400, {"error": str(e)})
        self.reply(200, {"ok": True})

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    ThreadingHTTPServer((HOST, PORTS["panel"]), H).serve_forever()
