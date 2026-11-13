#!/usr/bin/env python3
"""Touch the screen: python3 scripts/tap.py X Y [X2 Y2]  (screen pixels; two points = a drag).
Sends virtio-multitouch events over QMP, the lab's stand-in for the device's touch panel."""
import os, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from lab import MODELS, MODEL, qmp  # noqa: E402

w, h = MODELS[MODEL]["screen"]
TID = int(time.time()) % 60000 + 1     # a new contact id per touch
pts = list(zip(map(int, sys.argv[1::2]), map(int, sys.argv[2::2])))


def mtt(kind, **kw):
    # QMP wants axis and value on every mtt event, begin and end included
    return {"type": "mtt", "data": {"type": kind, "slot": 0, "tracking-id": TID, "axis": "x", "value": 0, **kw}}


def at(x, y):
    return [mtt("data", axis="x", value=x * 32767 // (w - 1)), mtt("data", axis="y", value=y * 32767 // (h - 1))]


def send(events):
    r = qmp("input-send-event", events=events)
    if "error" in r:
        sys.exit("QEMU: " + r["error"]["desc"])


send([mtt("begin")] + at(*pts[0]))
for p in pts[1:]:
    time.sleep(0.05)
    send(at(*p))
time.sleep(0.12)
send([mtt("end", **{"tracking-id": -1})])   # QEMU forwards the id as given: -1 lifts the finger
