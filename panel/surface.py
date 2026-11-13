"""Virtual Gen 2 control surface (MPC XL = ACVC, MPC Live III = ACVG): the protocol brain on the host.

The guest plugs a fake USB surface into the app (overlay mpclab-cs-gadget: an f_midi gadget
09e8:005a or 09e8:0058, per the device tree's product code, on the second dummy UDC, seen by the
app as a snd-usb-audio card). The host side follows MODEL (lab.py); PROFILES holds both. The guest relay
carries its MIDI here over PORTS["cs"], framed [port][len][bytes]; port = USB MIDI cable.
This module answers the app's handshake, records what the app sends (LED CCs, pad colours,
mode SysEx) and injects buttons and pads. panel/gen2.py serves it as POST /control and
GET /surface.

Handshake as observed on the running app (see Surface.on_sysex):
  app  F0 7E 00 06 01 F7                      identity request
  us   F0 7E 00 06 02 47 5A 00 ...  F7        identity reply: [6] product 0x5A (ACVC) / 0x58 (ACVG),
                                              [9..12] firmware version, [14..33] serial
  app  F0 47 00 5A 52 00 04 t0 t1 t2 t3 F7    ping with a 4-byte token
  us   F0 47 00 5A 53 00 05 t0 t1 t2 t3 01 F7 echo; [11] 1 or 2 = accepted
  then the app sets the surface mode and starts sending lights.
Inputs: buttons = ch1 notes, pads = ch10 notes (tables in panel/acvc.json / acvg.json, written by
scripts/surface_map.py from the static analysis in research/gen2-control-surface).
Q-Link n: ch1 CC 0x10+n-1, relative two's complement. The app ignores a turn unless the knob is
touched (ch1 note QLINKn_TOUCH held), so a turn on an untouched knob is wrapped in a touch here.
CC 0x10 is Q-Link 1 = bottom-left knob of the app's 4x4 Q-Link grid (XL). Live III: same CCs,
4 knobs, touch notes 84-87, Q-Link 1 = top knob of the app's 4-knob overlay; QLINK_BANK (note 0)
steps banks 1-4 (LEDs QLINK_BANK_1..4).
Pad RGB: XL one light per pad (index = pad - 1); Live III four per pad (index 4*(pad-1) .. +3),
then the step row at 0x40-0x4F. GET /surface carries "rgb_per_pad" for the Live III.
Touch strip: ch1 note TOUCH_STRIP_PRESS (65) on/off, position as ch1 CC 0x21 (MSB) then CC 0x22
(LSB), 14 bits; the app acts on the LSB and reads 0x3FFF as 1.0 (dispatcher 0x26bfaec).
Pad pressure: ch10 poly aftertouch on the pad's note while it is held.
Q-Link scaling (measured): one count = one parameter step, linear, no acceleration (+10 in one
message = ten +1 messages). The app sums the counts of one touch and rounds against the value at
touch-down: Sequence Tempo moves 0.1 BPM per count but snaps to whole BPM, so a touch worth fewer
than 5 counts changes nothing (3 -> 0, 5 -> +1, 15 -> +2, -15 -> -1); Track Velocity moves 1 per count.
XL 17th knob (the display beside the Q-Links showing the track level): touch = QLINK17_TOUCH
(note 66), turn = ch1 CC 0x65, here "QLINK 17".
Reconnect: on every (re)connect the next ping is answered with status 2, which makes the app redo
the handshake and resend all lights (resync_status). Q-Link displays are not resent (only changes
are), so Surface keeps them on disk. What the app sends is decoded in panel/surface_out.py.
"""
import json
import os
import socket
import threading
import time
from collections import deque

import surface_out
from surface_out import DISPLAY_QLINK, display_apply, display_png, display_rows, named_lights, new_display

VERSION = (1, 5, 0, 0)               # surface firmware version reported in the identity reply
# product byte (identity reply [6], = USB PID low byte), serial (20 ASCII bytes at [14..33]), Q-Links
PROFILES = {"acvc": {"name": "MPC XL", "product": 0x5A, "serial": b"MPCLAB-ACVC-00000001", "qlinks": 16,
                     "qlink17_cc": 0x65},       # the XL's 17th knob (touch note 66), lab-confirmed
            "acvg": {"name": "MPC Live III", "product": 0x58, "serial": b"MPCLAB-ACVG-00000001", "qlinks": 4,
                     "rgb_per_pad": 4,     # Live III pads: 4 RGB lights each, index 4*(pad-1)+k (seen on the app)
                     # in the table but never sent: one QLINK5_TOUCH (note 88) stops the app's surface
                     # link (no LEDs, no pings, inputs ignored) until the app restarts, 2 of 2 tries
                     "blocked": ("QLINK5_TOUCH", "QLINK6_TOUCH", "QLINK7_TOUCH", "QLINK8_TOUCH")}}


def profile(model):
    """Protocol constants + control tables for one product ("acvc" or "acvg")."""
    p = dict(PROFILES[model], model=model)
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), model + ".json")) as f:
        m = json.load(f)
    buttons = {k: v for k, v in m["buttons_ch1_note"].items() if k not in p.get("blocked", ())}
    p.update(buttons=buttons, pads=m["pads_ch10_note"], leds=m["leds_ch_cc"])
    return p


P = profile(os.environ.get("MODEL", "acvc"))    # the default for the functions below
WHEEL_CC = 0x64                      # data wheel (0x64 or 0x65 per the static analysis)
REPLY_PORT = 1                       # the app reads hw:C,D,1 (cable 1)
STRIP_MSB, STRIP_LSB = 0x21, 0x22    # touch strip position, 14-bit
TOUCH_HOLD = 1.0                     # s an auto-touch stays on after the last turn: the app sums
                                     # the counts of one touch, so a slow page drag must stay one touch


# ----------------------------------------------------------------------------- pure protocol
def frame(port, data):
    """Relay link frames: [port][len][bytes], at most 255 bytes each."""
    out = b""
    for i in range(0, len(data), 255):
        chunk = data[i:i + 255]
        out += bytes([port, len(chunk)]) + chunk
    return out


def unframe(buf):
    """-> ([(port, bytes)], rest)"""
    frames, i = [], 0
    while len(buf) - i >= 2 and len(buf) - i >= 2 + buf[i + 1]:
        frames.append((buf[i], bytes(buf[i + 2:i + 2 + buf[i + 1]])))
        i += 2 + buf[i + 1]
    return frames, buf[i:]


class MidiParser:
    """Bytes -> complete messages (running status, SysEx, realtime bytes passed through)."""
    LEN = {0x80: 3, 0x90: 3, 0xA0: 3, 0xB0: 3, 0xC0: 2, 0xD0: 2, 0xE0: 3}

    def __init__(self):
        self.msg, self.status, self.sysex = [], 0, None

    def feed(self, data):
        out = []
        for b in data:
            if b >= 0xF8:                                  # realtime: may sit anywhere
                out.append(bytes([b]))
            elif b == 0xF0:
                self.sysex = [b]
            elif b == 0xF7:
                if self.sysex is not None:
                    out.append(bytes(self.sysex + [b]))
                self.sysex = None
            elif self.sysex is not None and b < 0x80:
                self.sysex.append(b)
            elif b >= 0x80:
                self.sysex = None
                self.status, self.msg = b, [b]
                if b >= 0xF0:                              # other system common: not used here
                    self.status, self.msg = 0, []
            elif self.status:
                if not self.msg:
                    self.msg = [self.status]               # running status
                self.msg.append(b)
                if len(self.msg) == self.LEN[self.status & 0xF0]:
                    out.append(bytes(self.msg))
                    self.msg = []
        return out


def identity_reply(request, p=None):
    p = p or P
    dev = request[2] if len(request) > 2 else 0
    return bytes([0xF0, 0x7E, dev, 0x06, 0x02, 0x47, p["product"], 0x00, 0x19, *VERSION, 0x00]) + p["serial"] + b"\xF7"


def ping_reply(ping, status=1):
    """F0 47 00 pp 52 00 04 t0..t3 F7 -> F0 47 00 pp 53 00 05 t0..t3 status F7"""
    return bytes([0xF0, 0x47, ping[2], ping[3], 0x53, 0x00, 0x05, *ping[7:11], status, 0xF7])


def qlink_number(name, p=None):
    """"QLINK 3" / "qlink_3" -> 3, else None. "QLINK 17" only on products with a 17th knob."""
    p = p or P
    key = name.strip().upper().replace(" ", "_")
    if not (key.startswith("QLINK_") and key[6:].isdigit()):
        return None
    n = int(key[6:])
    return n if 1 <= n <= p["qlinks"] or (n == 17 and "qlink17_cc" in p) else None


def strip_position(value):
    """0..1 -> (MSB, LSB) of the 14-bit strip position."""
    v = round(max(0.0, min(1.0, float(value))) * 0x3FFF)
    return v >> 7, v & 0x7F


def control_message(name, action, value=127, p=None):
    """API name -> MIDI bytes. "PLAY", "play_start", "PAD 1".."PAD 16", "QLINK 1".."QLINK 16",
    "DATA_WHEEL", "TOUCH_STRIP". Actions: press/release; turn (value = signed clicks);
    pressure (pads, 0..127); move (strip, value 0..1; press also takes a position)."""
    p = p or P
    key = name.strip().upper().replace(" ", "_")
    on = action == "press"
    if key.startswith("PAD_") and key[4:].isdigit() and 1 <= int(key[4:]) <= 16:
        note = p["pads"][int(key[4:]) - 1]
        if action == "pressure":               # poly aftertouch; the app ignores 0
            return bytes([0xA9, note, max(0, min(127, int(value)))])
        return bytes([0x99, note, max(1, min(127, int(value)))]) if on else bytes([0x89, note, 0])
    if key == "TOUCH_STRIP":
        if action == "release":
            return bytes([0x80, p["buttons"]["TOUCH_STRIP_PRESS"], 0])
        msb, lsb = strip_position(value)
        pos = bytes([0xB0, STRIP_MSB, msb, 0xB0, STRIP_LSB, lsb])
        if action == "move":
            return pos
        if on:
            return bytes([0x90, p["buttons"]["TOUCH_STRIP_PRESS"], 127]) + pos
        raise KeyError(name)
    if action == "turn":                       # relative, two's complement in 7 bits; value = clicks (+/-)
        delta = max(-63, min(63, int(value))) & 0x7F
        n = qlink_number(key, p)
        if n == 17:
            return bytes([0xB0, p["qlink17_cc"], delta])
        if n:                                  # CC 0x10 + index on both, confirmed on the app (the static
            # reading "0x13 - index" for type-2 products was wrong for the Live III: CC 0x13 with
            # QLINK1_TOUCH moved nothing, CC 0x10 moved knob 1 = the top knob of the 4-knob overlay)
            return bytes([0xB0, 0x10 + n - 1, delta])
        if key == "DATA_WHEEL":
            return bytes([0xB0, WHEEL_CC, delta])
        raise KeyError(name)
    if key in p["buttons"]:
        return bytes([0x90, p["buttons"][key], 127]) if on else bytes([0x80, p["buttons"][key], 0])
    raise KeyError(name)


def resync_status(pending):
    """Ping status byte. The app's ping handler (0x26c2238): in PING_CHECK (state 3) 1 or 2 both
    pass; when ACTIVE (5/6) a 2 drops it to state 4, which redoes enquiry -> identity -> init and
    resends every light (SEND_INITIAL). So answer 2 once after a (re)connect, 1 otherwise."""
    return 2 if pending else 1


def describe(msg, p=None):
    """One app -> surface message as a log/state entry."""
    p = p or P
    s = msg[0]
    if s == 0xF0:
        cmd = msg[4] if len(msg) > 4 and msg[1] == 0x47 else None
        return {"kind": "sysex", "cmd": cmd}
    if s & 0xF0 == 0xB0:
        ch = s & 0x0F
        return {"kind": "led", "ch": ch + 1, "cc": msg[1], "value": msg[2], "name": p["leds"].get(f"{ch}:{msg[1]}")}
    return {"kind": "other"}


# ----------------------------------------------------------------------------- the surface
class Surface:
    def __init__(self, host, port, model=None, store=None):
        self.p = profile(model) if model else P
        self.addr = (host, port)
        self.sock = None
        self.lock = threading.Lock()
        self.parsers = {}
        self.state = {"model": self.p["model"], "product": self.p["name"], "controls": sorted(self.p["buttons"]),
                      "qlinks": self.p["qlinks"], "link": False, "handshake": [], "identity_requests": 0, "pings": 0,
                      "active": False, "counts": {}, "leds": {}, "rgb": {}, "sysex_last": {}, "resyncs": 0}
        self.fbs = {}                    # Q-Link display index -> framebuffer (0A/0B)
        self.display_meta = {}           # index -> {"updates", "t", "rect", "stale"}
        self.store, self.dirty = store, False      # JSON file the displays survive restarts in
        self.load_displays()
        self.resync = False              # answer the next ping with status 2 (full light resend)
        if "rgb_per_pad" in self.p:                     # Live III only; the XL state is unchanged
            self.state["rgb_per_pad"] = self.p["rgb_per_pad"]
        self.log = deque(maxlen=200)
        self.t0 = time.time()
        self.touched = set()             # touch notes held through the API (page drags)
        self.auto_touch = {}             # touch note -> release time, for turns without a touch
        threading.Thread(target=self.run, daemon=True).start()
        threading.Thread(target=self.release_touches, daemon=True).start()

    def note(self, direction, port, msg):
        self.log.append({"t": round(time.time(), 3), "dir": direction, "port": port, "hex": msg.hex(" ")})

    def send(self, msg, port=REPLY_PORT):
        with self.lock:
            if not self.sock:
                raise OSError("surface link not connected (VM or relay down)")
            self.sock.sendall(frame(port, msg))
        self.note("out", port, msg)

    def run(self):
        while True:
            try:
                s = socket.create_connection(self.addr, timeout=3)
            except OSError:
                time.sleep(1)
                continue
            s.settimeout(None)
            with self.lock:
                self.sock = s
            self.state["link"] = True
            self.t0 = time.time()
            self.resync = True           # we may have missed lights: make the app resend them all
            buf = b""
            try:
                while True:
                    data = s.recv(4096)
                    if not data:
                        break
                    frames, buf = unframe(buf + data)
                    for port, payload in frames:
                        for msg in self.parsers.setdefault(port, MidiParser()).feed(payload):
                            self.on_message(port, msg)
            except OSError:
                pass
            with self.lock:
                self.sock = None
            self.state.update(link=False, active=False)
            s.close()
            time.sleep(1)

    def on_message(self, port, msg):
        self.note("in", port, msg)
        d = describe(msg, self.p)
        st = self.state
        st["counts"][d["kind"]] = st["counts"].get(d["kind"], 0) + 1
        if d["kind"] == "led":
            st["leds"][d["name"] or f"ch{d['ch']} cc{d['cc']}"] = d["value"]
            st["active"] = True
        elif d["kind"] == "sysex":
            self.on_sysex(port, msg, d["cmd"])

    def on_sysex(self, port, msg, cmd):
        st = self.state
        if msg[:2] == b"\xF0\x7E" and len(msg) >= 6 and msg[3:5] == b"\x06\x01":
            st["identity_requests"] += 1
            self.step("identity request")
            self.send(identity_reply(msg, self.p))
        elif cmd == 0x52 and len(msg) >= 12:
            st["pings"] += 1
            status, self.resync = resync_status(self.resync), False
            st["resyncs"] += status == 2
            self.step("ping " + msg[7:11].hex() + (" -> resync" if status == 2 else ""))
            self.send(ping_reply(msg, status))
        elif cmd == 0x65 and len(msg) >= 11:              # RGB light: ... 65 00 04 index R G B F7 (0-15 = pads)
            st["rgb"][msg[7]] = list(msg[8:11])
            st["active"] = True
        elif cmd in (0x0A, 0x0B) and len(msg) >= 13:      # Q-Link display draw / fill
            fb = self.fbs.setdefault(msg[7], new_display())
            if display_apply(fb, msg):
                m = self.display_meta.setdefault(msg[7], {"updates": 0})
                m.update(updates=m["updates"] + 1, t=round(time.time(), 3), rect=list(msg[8:12]), stale=False)
                self.dirty = True
            st["sysex_last"][f"{cmd:02x}"] = msg[:12].hex(" ") + f" ... ({len(msg)} bytes)"
        elif cmd is not None:
            st["sysex_last"][f"{cmd:02x}"] = msg.hex(" ")
            self.step(f"sysex {cmd:02x} len {len(msg)}")

    def step(self, what):
        """Handshake/session log: the first 80 steps, repeats collapsed into a count."""
        h = self.state["handshake"]
        t = round(time.time() - self.t0, 2)
        if h and h[-1]["what"] == what:
            h[-1].update(n=h[-1]["n"] + 1, last=t)
        elif len(h) < 80:
            h.append({"what": what, "at": t, "n": 1, "last": t})

    def control(self, name, action, value=127):
        msg = control_message(name, action, value, self.p)
        n = qlink_number(name, self.p) if action == "turn" else None
        touch = f"QLINK{n}_TOUCH"
        if n and touch not in self.touched and touch not in self.auto_touch:
            self.send(control_message(touch, "press", p=self.p))         # the app ignores untouched turns
            self.auto_touch[touch] = 0
        self.send(msg)
        if n and touch in self.auto_touch:
            self.auto_touch[touch] = time.time() + TOUCH_HOLD
        key = name.strip().upper().replace(" ", "_")
        if key in self.p["buttons"] and action in ("press", "release"):
            (self.touched.add if action == "press" else self.touched.discard)(key)
            self.auto_touch.pop(key, None)                     # a real touch takes over

    def release_touches(self):
        """Lift auto-touches whose knob has been still for TOUCH_HOLD seconds; save displays."""
        last_save = 0
        while True:
            time.sleep(0.1)
            if self.dirty and time.time() - last_save > 1:
                last_save, self.dirty = time.time(), False
                self.save_displays()
            for touch, until in list(self.auto_touch.items()):
                if until and time.time() > until and self.auto_touch.pop(touch, None):
                    try:
                        self.send(control_message(touch, "release", p=self.p))
                    except OSError:
                        pass

    def request_resync(self):
        """Make the app resend all lights: status 2 on the next ping (pings come every 4 s)."""
        self.resync = True

    def load_displays(self):
        """Displays from the last page-server run, marked stale until the app redraws them."""
        try:
            with open(self.store) as f:
                saved = json.load(f)
            for i, hexdata in saved.items():
                fb = bytearray.fromhex(hexdata)
                if len(fb) == len(new_display()):
                    self.fbs[int(i)] = fb
                    self.display_meta[int(i)] = {"updates": 0, "stale": True}
        except (OSError, TypeError, ValueError, AttributeError):
            pass                          # no store, none saved yet, or unreadable: start empty

    def save_displays(self):
        if not self.store:
            return
        try:
            tmp = self.store + ".tmp"
            with open(tmp, "w") as f:
                json.dump({str(i): fb.hex() for i, fb in self.fbs.items()}, f)
            os.replace(tmp, self.store)
        except OSError as e:
            self.step(f"display store: {e}")

    def displays(self):
        """index -> {"qlink", "updates", "t", "rect", "stale", "lit", "rows"} for every buffer known."""
        out = {}
        for i, fb in sorted(self.fbs.items()):
            out[str(i)] = dict(self.display_meta.get(i, {}), qlink=DISPLAY_QLINK.get(i),
                               lit=sum(bin(b).count("1") for b in fb), rows=display_rows(fb))
        return out

    def display_png(self, i, scale=4):
        fb = self.fbs.get(i)
        return display_png(fb, scale) if fb is not None else None

    def snapshot(self):
        return dict(self.state, recent=list(self.log)[-40:], displays=self.displays(),
                    lights=named_lights(self.state["rgb"], self.state["leds"], self.p["leds"].values()))


def selftest():
    global P
    default, P = P, profile("acvc")              # the XL assertions below hold whatever MODEL is
    try:
        selftest_xl()
    finally:
        P = default
    selftest_models()
    surface_out.selftest()
    print("surface selftest ok")


def selftest_xl():
    assert control_message("QLINK 3", "turn", -1) == bytes([0xB0, 0x12, 0x7F])
    assert control_message("DATA_WHEEL", "turn", 2) == bytes([0xB0, WHEEL_CC, 2])
    assert unframe(frame(1, b"\x90\x52\x7f") + b"\x00") == ([(1, b"\x90\x52\x7f")], b"\x00")
    assert len(unframe(frame(0, bytes(300)))[0]) == 2
    p = MidiParser()
    assert p.feed(b"\xb0\x52\x03\x31\x01\xf8\xf0\x47\x7f") == [b"\xb0\x52\x03", b"\xb0\x31\x01", b"\xf8"]
    assert p.feed(b"\x5a\x65\xf7") == [b"\xf0\x47\x7f\x5a\x65\xf7"]
    r = identity_reply(b"\xF0\x7E\x00\x06\x01\xF7")
    assert r[6] == 0x5A and r[9:13] == bytes(VERSION) and r[14:34] == b"MPCLAB-ACVC-00000001" and r[-1] == 0xF7 and len(r) == 35
    assert r == bytes.fromhex("f0 7e 00 06 02 47 5a 00 19 01 05 00 00 00") + b"MPCLAB-ACVC-00000001\xf7"   # as before profiles
    ping = bytes.fromhex("f0 47 00 5a 52 00 04 01 02 03 04 f7")
    assert ping_reply(ping) == bytes.fromhex("f0 47 00 5a 53 00 05 01 02 03 04 01 f7")
    assert control_message("play", "press") == bytes([0x90, 82, 127])
    assert control_message("PAD 1", "press", 100) == bytes([0x99, 37, 100])
    assert control_message("Pad 16", "release") == bytes([0x89, 53, 0])
    assert describe(b"\xb0\x52\x03")["name"] == "PLAY"
    assert qlink_number("QLINK 16") == 16 and qlink_number("qlink_0") is None and qlink_number("QLINK1_TOUCH") is None
    assert control_message("QLINK 16", "turn", 200) == bytes([0xB0, 0x1F, 63])
    assert qlink_number("QLINK 17") == 17 and control_message("QLINK 17", "turn", -2) == bytes([0xB0, 0x65, 0x7E])
    assert control_message("QLINK17_TOUCH", "press") == bytes([0x90, 66, 127])
    assert resync_status(True) == 2 and resync_status(False) == 1
    assert control_message("QLINK1_TOUCH", "press") == bytes([0x90, 14, 127])
    assert strip_position(0) == (0, 0) and strip_position(1) == (0x7F, 0x7F) and strip_position(2) == (0x7F, 0x7F)
    assert strip_position(0.5) == (0x40, 0x00)
    assert control_message("TOUCH_STRIP", "press", 1) == bytes([0x90, 65, 127, 0xB0, 0x21, 0x7F, 0xB0, 0x22, 0x7F])
    assert control_message("touch strip", "move", 0) == bytes([0xB0, 0x21, 0, 0xB0, 0x22, 0])
    assert control_message("TOUCH_STRIP", "release") == bytes([0x80, 65, 0])
    assert control_message("PAD 1", "pressure", 300) == bytes([0xA9, 37, 127])
    try:
        control_message("NOPE", "press")
        raise AssertionError("unknown control accepted")
    except KeyError:
        pass


def selftest_models():
    xl, l3 = profile("acvc"), profile("acvg")
    req = b"\xF0\x7E\x00\x06\x01\xF7"
    r = identity_reply(req, l3)
    assert r[6] == 0x58 and r[14:34] == b"MPCLAB-ACVG-00000001" and len(r) == 35
    assert identity_reply(req, xl)[6] == 0x5A
    assert all(len(p["serial"]) == 20 for p in PROFILES.values())
    ping = bytes.fromhex("f0 47 00 58 52 00 04 0a 0b 0c 0d f7")
    assert ping_reply(ping) == bytes.fromhex("f0 47 00 58 53 00 05 0a 0b 0c 0d 01 f7")
    # same note for PLAY/STOP on both, different LED CCs (static analysis: PLAY cc82 XL, cc86 Live III)
    assert control_message("PLAY", "press", p=l3) == bytes([0x90, 82, 127]) == control_message("PLAY", "press", p=xl)
    assert control_message("STOP", "press", p=l3) == bytes([0x90, 81, 127])
    assert describe(b"\xb0\x56\x03", l3)["name"] == "PLAY" and describe(b"\xb0\x55\x00", l3)["name"] == "STOP"
    assert describe(b"\xb0\x52\x03", xl)["name"] == "PLAY" and describe(b"\xb0\x52\x03", l3)["name"] != "PLAY"
    assert control_message("PAD 1", "press", 90, l3) == bytes([0x99, 37, 90])
    assert control_message("QLINK 1", "turn", 1, l3) == bytes([0xB0, 0x10, 1])      # not reversed (seen on the app)
    assert control_message("QLINK 4", "turn", -1, l3) == bytes([0xB0, 0x13, 0x7F])
    assert control_message("QLINK1_TOUCH", "press", p=l3) == bytes([0x90, 84, 127])     # XL: note 14
    assert control_message("QLINK_BANK", "press", p=l3) == bytes([0x90, 0, 127])
    assert control_message("QLINK4_TOUCH", "press", p=l3) == bytes([0x90, 87, 127])
    assert control_message("STEP1", "press", p=l3) == bytes([0x90, 92, 127])
    assert describe(b"\xb0\x60\x03", l3)["name"] == "STEP_PREV" and describe(b"\xb0\x61\x01", l3)["name"] == "STEP_NEXT"
    assert xl["buttons"] == profile("acvc")["buttons"] and "QLINK5_TOUCH" in xl["buttons"]
    assert qlink_number("QLINK 17", l3) is None      # the 17th knob is XL only
    for p, absent in ((l3, ("QLINK 5", "turn", 1)), (l3, ("QLINK5_TOUCH", "press")), (l3, ("QLINK8_TOUCH", "release")), (l3, ("NUMERIC_1", "press")), (l3, ("CURSOR_LEFT", "press")),
                      (xl, ("WINDOW", "press"))):
        try:
            control_message(*absent, p=p)
            raise AssertionError(f"{p['model']} accepted {absent}")
        except KeyError:
            pass
    assert control_message("WINDOW", "press", p=l3) == bytes([0x90, 51, 127])
    assert control_message("GRID", "press", p=xl) == bytes([0x90, 51, 127])     # note 51 is a different key per product
