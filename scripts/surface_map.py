#!/usr/bin/env python3
"""Write panel/<model>.json, the control-surface tables panel/surface.py uses, from the static
analysis in research/gen2-control-surface/maps.json.

    python3 scripts/surface_map.py acvg [path/to/maps.json]

buttons_ch1_note: KeyID name -> ch1 note (notemaps[P][note] = KeyID, names_a[KeyID]; first note wins)
leds_ch_cc:       "ch:cc" -> LightID name (lightmaps[P][LightID] = [cc, ch], names_b; first wins, cc 255 = none)
pads_ch10_note:   pad 1..16 -> ch10 note, from the input dispatcher (one table, not per product)
This reproduces panel/acvc.json exactly for "acvc".
LIGHT_NAMES fills LightIDs the static analysis left as "?", from the running app.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MAPS = os.path.join(HERE, "..", "..", "..", "research", "gen2-control-surface", "maps.json")
PADS = [37, 36, 42, 82, 40, 38, 46, 44, 48, 47, 45, 43, 49, 55, 51, 53]
# LightID -> name where names_b has "?". LightIDs 19/20 sit between NO_FILTER and LOCATE, as
# STEP_PREV/STEP_NEXT do in names_a; on the virtual Live III holding STEP_PREV (note 68) sends
# cc 0x60 = 3 and releasing it cc 0x60 = 1, STEP_NEXT (note 69) the same on cc 0x61.
LIGHT_NAMES = {19: "STEP_PREV", 20: "STEP_NEXT"}


def tables(maps, model):
    p = model.upper()
    buttons, leds = {}, {}
    for note, key in enumerate(maps["notemaps"][p]):
        name = maps["names_a"].get(str(key))
        if key != 255 and name is not None and name not in buttons:
            buttons[name] = note
    for light, (cc, ch) in enumerate(maps["lightmaps"][p]):
        if cc != 255 and f"{ch}:{cc}" not in leds:
            name = maps["names_b"].get(str(light))
            leds[f"{ch}:{cc}"] = LIGHT_NAMES.get(light, name) if name == "?" else name
    return {"_source": f"research/gen2-control-surface/maps.json (static analysis of MPC 3.10, {p} tables)",
            "buttons_ch1_note": buttons, "pads_ch10_note": PADS, "leds_ch_cc": leds}


def main():
    model = sys.argv[1].lower()
    with open(sys.argv[2] if len(sys.argv) > 2 else MAPS) as f:
        t = tables(json.load(f), model)
    out = os.path.join(HERE, "..", "panel", model + ".json")
    with open(out, "w") as f:
        f.write(json.dumps(t, indent=0))
    print(f"{out}: {len(t['buttons_ch1_note'])} buttons, {len(t['leds_ch_cc'])} lights")


if __name__ == "__main__":
    main()
