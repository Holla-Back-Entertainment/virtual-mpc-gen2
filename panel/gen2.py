#!/usr/bin/env python3
"""Gen 2 lab page: the MPC's touchscreen in the browser.

    python3 panel/gen2.py            # http://127.0.0.1:8865/
    python3 panel/gen2.py --selftest

GET  /            the page
GET  /screen.png  the screen now (QMP screendump); 304 when nothing changed since ?v=
POST /touch       {"type": "down"|"move"|"up", "x": px, "y": px}  -> virtio-multitouch
POST /power       the power key
POST /control     {"name": "PLAY"|"PAD 1"..., "action": "press"|"release", "value": velocity}
                  {"name": "QLINK 1".."QLINK 16"|"DATA_WHEEL", "action": "turn", "value": clicks}
                  {"name": "QLINK 17", "action": "turn", ...} the XL's 17th knob (track level), CC 0x65
                  {"name": "TOUCH_STRIP", "action": "press"|"move"|"release", "value": 0..1}
                  {"name": "PAD 1"..., "action": "pressure", "value": 0..127} while the pad is held
                  -> the virtual control surface of MODEL (XL or Live III, panel/surface.py)
GET  /surface     surface handshake state, last LED / pad-colour state, recent MIDI both ways,
                  "displays" (Q-Link OLEDs, decoded) and "lights" (RGB lights and meters by name)
GET  /display/N.png  Q-Link display buffer N (0..16) as a PNG, ?scale=1..8 (default 4); 404 = not drawn yet
POST /resync      make the app resend every light and display (status 2 on its next ping, <= 4 s)
GET  /audio       {"in", "gain", "tone_hz", "bridge": linked, "levels": {"out": [peak per app output
                  channel], "in": [l, r], "drop", "under"}, "levels_age": s} (peaks over ~100 ms, 0..1)
POST /audio       {"in": "off"|"tone"|"mic", "gain": 0..100, "tone_hz": Hz} the app's inputs 1-2
                  (any subset of keys); "mic" = the page's microphone (the page asks for it)
Sound does not pass here: the page plays the bridge's PCM straight from QEMU's WebSocket
chardev "mpclab.pcm" (PORTS["pcm"]), as the Sample page does.

ponytail: the page polls screendumps (~10 fps). Port the Sample panel's VNC push
(panel/server.py, class Vnc) if that feels slow.
"""
import hashlib
import json
import os
import struct
import sys
import threading
import time
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from lab import HOST, MODEL, MODELS, PORTS, RUN, qmp  # noqa: E402
import surface  # noqa: E402

W, H = MODELS[MODEL]["screen"]
PPM = os.path.join(RUN, "gen2-screen.ppm")
LOCK = threading.Lock()          # one QMP exchange at a time on the panel's own QMP port
tid = 0                          # contact id of the finger that is down
SURFACE = None                   # surface.Surface, started in __main__


def call(command, **arguments):
    with LOCK:
        r = qmp(command, port=PORTS["qmp_panel"], **arguments)
    if "error" in r:
        raise OSError(r["error"]["desc"])
    return r


def touch_events(kind, x, y, contact):
    """QMP events for one touch step. QEMU forwards tracking-id as given: -1 lifts the finger.
    Every mtt event must carry axis and value, begin and end included."""
    def mtt(type_, tracking, axis="x", value=0):
        return {"type": "mtt", "data": {"type": type_, "slot": 0, "tracking-id": tracking, "axis": axis, "value": value}}
    if kind == "up":
        return [mtt("end", -1)]
    clamp = lambda v, n: max(0, min(n - 1, int(v))) * 32767 // (n - 1)
    pos = [mtt("data", contact, "x", clamp(x, W)), mtt("data", contact, "y", clamp(y, H))]
    return ([mtt("begin", contact)] if kind == "down" else []) + pos


def touch(kind, x=0, y=0):
    global tid
    if kind == "down":
        tid = tid % 60000 + 1
    call("input-send-event", events=touch_events(kind, x, y, tid))


def page_controls(model):
    """JSON for the page: null = show the XL-shaped layout as is (acvc); otherwise the control
    names the product's table knows and its Q-Link count, so the page hides the rest."""
    if model == "acvc":
        return "null"
    p = surface.profile(model)
    return json.dumps({"model": model.upper(), "name": p["name"], "buttons": sorted(p["buttons"]), "qlinks": p["qlinks"]})


def display_request(path):
    """"/display/3.png?scale=2" -> (3, 2); anything else -> (None, None)."""
    route, _, query = path.partition("?")
    if not (route.startswith("/display/") and route.endswith(".png") and route[9:-4].isdigit()):
        return None, None
    scale = 4
    for kv in query.split("&"):
        k, _, v = kv.partition("=")
        if k == "scale" and v.isdigit():
            scale = max(1, min(8, int(v)))
    return int(route[9:-4]), scale


class Audio:
    """The app's audio inputs 1-2 and the bridge's levels, over the virtio-serial port
    "mpclab.audio" (QEMU TCP chardev PORTS["audio"]) to mpclab-audio-bridge in the VM, as the
    Sample panel's Rear class does. Sends "routing ..." lines, receives "levels ..." ~10x/s.
    in: "off" (silence), "tone" (sine from the bridge), "mic" (the page's microphone, which the
    page streams to the bridge over its PCM WebSocket; nothing arrives unless a page has it)."""
    SOURCES = {"off": "silence", "tone": "tone", "mic": "host"}

    def __init__(self):
        self.lock = threading.Lock()
        self.state = {"in": "off", "gain": 50, "tone_hz": 440}
        self.sock, self.buf = None, b""
        self.lv, self.lv_time = {}, None

    def loop(self):
        import socket
        while True:
            try:
                s = socket.create_connection((HOST, PORTS["audio"]))
                with self.lock:
                    self.sock, self.buf = s, b""
                self.send()
                while chunk := s.recv(4096):
                    self.feed(chunk)
            except OSError:
                pass
            with self.lock:
                self.sock = None
            time.sleep(1)

    def feed(self, chunk):
        self.buf += chunk
        *lines, self.buf = self.buf.split(b"\n")
        for ln in lines:
            lv = parse_levels(ln.decode(errors="replace"))
            if lv:
                with self.lock:
                    self.lv, self.lv_time = lv, time.time()

    def set(self, body):
        with self.lock:
            st = dict(self.state)
        if "in" in body:
            if body["in"] not in self.SOURCES:
                raise ValueError(f"in: one of {', '.join(self.SOURCES)}")
            st["in"] = body["in"]
        if "gain" in body:
            st["gain"] = max(0, min(100, int(body["gain"])))
        if "tone_hz" in body:
            st["tone_hz"] = max(20, min(20000, float(body["tone_hz"])))
        with self.lock:
            self.state = st
        self.send()

    def send(self):
        with self.lock:
            line, s = routing_line(self.state), self.sock
        if s is not None:
            try:
                s.sendall(line.encode())
            except OSError:
                pass

    def snapshot(self):
        with self.lock:
            age = None if self.lv_time is None else round(time.time() - self.lv_time, 2)
            return dict(self.state, bridge=self.sock is not None and age is not None and age < 2, levels=self.lv, levels_age=age)


def routing_line(st):
    """Audio state -> the bridge's routing line (out stays on: main out 1-2 to the host)."""
    return f"routing out=on pair=0 in={Audio.SOURCES[st['in']]} gain={st['gain']} tone_hz={st['tone_hz']:g}\n"


def parse_levels(line):
    """"levels out <peak per channel> in <l> <r> drop <n> under <n>" -> dict, or None."""
    w = line.split()
    if w[:2] != ["levels", "out"] or "in" not in w:
        return None
    i = w.index("in")
    try:
        lv = {"out": [float(x) for x in w[2:i]], "in": [float(x) for x in w[i + 1:i + 3]]}
        lv.update({k: int(v) for k, v in zip(w[i + 3::2], w[i + 4::2])})
    except ValueError:
        return None
    return lv


AUDIO = None                     # Audio, started in __main__


def screen():
    """PPM screendump (~1 ms in QEMU's main loop) encoded to PNG here. A PNG screendump took
    45-85 ms under QEMU's lock at ~10 fps, which stalled the vCPUs: app audio xruns, clicks."""
    os.makedirs(RUN, exist_ok=True)
    call("screendump", filename=PPM, format="ppm")
    with open(PPM, "rb") as f:
        _, w, h, _, rgb = f.read().split(maxsplit=4)
    w, h = int(w), int(h)
    row = w * 3
    raw = b"".join(b"\0" + rgb[y * row:(y + 1) * row] for y in range(h))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 1)) + chunk(b"IEND", b""))


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def reply(self, code, body=b"", ctype="text/plain", **headers):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in headers.items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        try:
            if self.path in ("/", "/index.html"):
                with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "live3.html" if MODEL == "acvg" else "gen2.html"), "rb") as f:
                    page = f.read().replace(b"__CONTROLS__", page_controls(MODEL).encode()).replace(b"__MODEL_SHORT__", MODELS[MODEL]["model"].replace("Akai Professional ", "").encode()).replace(b"__MODEL__", MODELS[MODEL]["model"].encode()).replace(b"__W__", str(W).encode()).replace(b"__H__", str(H).encode()).replace(b"__PCM__", str(PORTS["pcm"]).encode())
                return self.reply(200, page, "text/html; charset=utf-8")
            if self.path.startswith("/screen.png"):
                png = screen()
                v = hashlib.md5(png).hexdigest()
                if self.path.endswith("v=" + v):
                    return self.reply(304)
                return self.reply(200, png, "image/png", **{"X-Frame": v})
            if self.path == "/audio" and AUDIO:
                return self.reply(200, json.dumps(AUDIO.snapshot()).encode(), "application/json")
            if self.path == "/surface" and SURFACE:
                return self.reply(200, json.dumps(SURFACE.snapshot()).encode(), "application/json")
            n, scale = display_request(self.path)
            if n is not None and SURFACE:
                png = SURFACE.display_png(n, scale)
                return self.reply(200, png, "image/png") if png else self.reply(404)
            self.reply(404)
        except OSError as e:                      # VM not running, QMP busy
            self.reply(503, str(e).encode())

    def do_POST(self):
        try:
            body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0)) or 0) or b"{}")
            if self.path == "/touch" and body.get("type") in ("down", "move", "up"):
                touch(body["type"], body.get("x", 0), body.get("y", 0))
            elif self.path == "/resync" and SURFACE:
                SURFACE.request_resync()
            elif self.path == "/audio" and AUDIO:
                AUDIO.set(body)
                return self.reply(200, json.dumps(AUDIO.snapshot()).encode(), "application/json")
            elif self.path == "/power":
                call("system_powerdown")
            elif self.path == "/control" and SURFACE and body.get("action") in ("press", "release", "turn", "move", "pressure"):
                SURFACE.control(str(body.get("name", "")), body["action"], body.get("value", 127))
            else:
                return self.reply(404)
            self.reply(200, b"ok")
        except KeyError as e:
            self.reply(400, f"unknown control {e}".encode())
        except (OSError, ValueError, TypeError) as e:
            self.reply(503 if isinstance(e, OSError) else 400, str(e).encode())


def selftest():
    down = touch_events("down", W - 1, 0, 7)
    assert [e["data"]["type"] for e in down] == ["begin", "data", "data"]
    assert down[1]["data"] == {"type": "data", "slot": 0, "tracking-id": 7, "axis": "x", "value": 32767}
    assert down[2]["data"]["value"] == 0 and down[2]["data"]["axis"] == "y"
    assert touch_events("move", -50, H * 2, 7)[1]["data"]["value"] == 32767      # clamped to the screen
    assert touch_events("up", 0, 0, 7) == [{"type": "mtt", "data": {"type": "end", "slot": 0, "tracking-id": -1, "axis": "x", "value": 0}}]
    assert page_controls("acvc") == "null"
    assert display_request("/display/16.png") == (16, 4) and display_request("/display/2.png?scale=99") == (2, 8)
    assert display_request("/display/x.png") == (None, None) and display_request("/surface") == (None, None)
    c = json.loads(page_controls("acvg"))
    assert c["model"] == "ACVG" and c["qlinks"] == 4 and "PLAY" in c["buttons"] and "NUMERIC_1" not in c["buttons"]
    assert routing_line({"in": "mic", "gain": 50, "tone_hz": 440.0}) == "routing out=on pair=0 in=host gain=50 tone_hz=440\n"
    lv = parse_levels("levels out 0.5000 0.2500 in 0.2500 0.2500 drop 3 under 0")
    assert lv == {"out": [0.5, 0.25], "in": [0.25, 0.25], "drop": 3, "under": 0}
    assert parse_levels("bridge: hello") is None
    a = Audio()
    a.set({"in": "tone", "gain": 300})
    assert a.snapshot()["in"] == "tone" and a.snapshot()["gain"] == 100 and a.snapshot()["bridge"] is False
    try:
        a.set({"in": "line"})
        raise AssertionError("bad source accepted")
    except ValueError:
        pass
    print("gen2 panel selftest ok")
    surface.selftest()


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        selftest()
    else:
        print(f"gen2 panel: {MODELS[MODEL]['model']} at http://{HOST}:{PORTS['panel']}/")
        SURFACE = surface.Surface(HOST, PORTS["cs"], MODEL, store=os.path.join(RUN, "gen2-displays.json"))
        AUDIO = Audio()
        threading.Thread(target=AUDIO.loop, daemon=True).start()
        ThreadingHTTPServer((HOST, PORTS["panel"]), Handler).serve_forever()
