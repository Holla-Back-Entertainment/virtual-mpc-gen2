"""What the app sends to the virtual XL besides plain LED CCs: Q-Link displays, RGB lights, meters.
Pure functions; panel/surface.py feeds them. Static addresses are vaddrs in MPC 3.10.0 /usr/bin/MPC.

Q-Link displays (decoded 13 Nov 2026, checked against the app's Q-Link overlay):
  17 OLEDs, 128x32 monochrome, SSD1306-style: 4 pages of 8 rows, one byte per page column,
  bit 0 = top row of the page, 1 = lit. Encoder at 0x2686700, dirty rectangle at 0x2686500.
  F0 47 7F pp 0B Lh Ll idx p0 p1 c0 c1 <data> F7   draw pages p0..p1 x columns c0..c1, page-major.
      L (14 bit) = 5 + len(data). data = those page bytes as one MSB-first bit stream cut into
      7-bit SysEx bytes (n bytes -> ceil(8n/7)); a full screen is 586 bytes, 599 with framing.
  F0 47 7F pp 0A 00 07 idx p0 p1 c0 c1 hi lo F7    fill the rectangle with byte (hi << 4 | lo);
      sent instead of a 0B when the new image is blank (00 03 00 7F 00 00 = clear all).
  idx 0..15 = Q-Link 1..16 (same order as the overlay: 1 = bottom-left), idx 16 = the 17th knob
  (touch note 66 QLINK17_TOUCH, turn = ch1 CC 0x65; shows the current track's level).
  The app compares each buffer with its last sent image and only sends changes (round-robin over
  the 17, 0x2686fe8), so a host that restarts sees nothing until a display changes: surface.py
  keeps the buffers on disk for that.

RGB lights (65 message, F0 47 7F pp 65 00 04 idx R G B F7, 7-bit R G B):
  0x00..0x3F  pads: idx = 4 * (pad - 1) + segment, 4 segments per pad (0x26bc5ac: pad*4 + seg,
              on products whose 0x2697780 flag is set; the app has so far always sent all four alike)
  0x40..0x4F  STEP 1..16 buttons (playhead chase and step edits seen in the lab)
  0x50        one more XL-only light (0x26bc1c0 exempts it from the range check); not identified
Meters (LED CCs on ch2): *_LOW carries a 4-segment thermometer (0, 1, 3, 7, 15), *_HIGH 2 more
  segments (0..3). The right-hand wing's gain/volume knobs send nothing here (no KeyID, no CC in
  the dispatcher 0x26bfaec): analog only, as far as this link goes.
"""
import struct
import zlib

DISPLAY_W, DISPLAY_H, DISPLAY_PAGES, DISPLAYS = 128, 32, 4, 17
DISPLAY_QLINK = {i: i + 1 for i in range(DISPLAYS)}        # display index -> Q-Link (17 = the 17th knob)
RGB_STEPS, RGB_EXTRA = 0x40, 0x50


def unpack7(data):
    """7-bit SysEx bytes -> 8-bit bytes (MSB-first bit stream; trailing pad bits dropped)."""
    acc = nbits = 0
    out = bytearray()
    for b in data:
        acc, nbits = (acc << 7) | (b & 0x7F), nbits + 7
        if nbits >= 8:
            nbits -= 8
            out.append((acc >> nbits) & 0xFF)
            acc &= (1 << nbits) - 1
    return bytes(out)


def pack7(data):
    """8-bit bytes -> 7-bit SysEx bytes, as the app's encoder does (inverse of unpack7)."""
    acc = nbits = 0
    out = bytearray()
    for b in data:
        acc, nbits = (acc << 8) | b, nbits + 8
        while nbits >= 7:
            nbits -= 7
            out.append((acc >> nbits) & 0x7F)
            acc &= (1 << nbits) - 1
    if nbits:
        out.append((acc << (7 - nbits)) & 0x7F)
    return bytes(out)


def new_display():
    return bytearray(DISPLAY_PAGES * DISPLAY_W)


def display_apply(fb, msg):
    """Apply one 0A/0B message to a framebuffer (index page * 128 + column).
    -> False (buffer untouched) for a malformed message."""
    if len(msg) < 13 or msg[4] not in (0x0A, 0x0B):
        return False
    p0, p1, c0, c1 = msg[8:12]
    if not (p0 <= p1 < DISPLAY_PAGES and c0 <= c1 < DISPLAY_W):
        return False
    cells = [(p, c) for p in range(p0, p1 + 1) for c in range(c0, c1 + 1)]
    if msg[4] == 0x0A:
        if len(msg) < 15:
            return False
        data = bytes([((msg[12] & 0x0F) << 4) | (msg[13] & 0x0F)]) * len(cells)
    else:
        data = unpack7(msg[12:-1])
        if len(data) < len(cells):
            return False
    for (p, c), v in zip(cells, data):
        fb[p * DISPLAY_W + c] = v
    return True


def display_rows(fb):
    """Framebuffer -> 32 rows of 32 hex digits (128 bits, MSB = leftmost pixel, 1 = lit)."""
    rows = []
    for y in range(DISPLAY_H):
        page, bit = divmod(y, 8)
        bits = 0
        for x in range(DISPLAY_W):
            bits = (bits << 1) | ((fb[page * DISPLAY_W + x] >> bit) & 1)
        rows.append(f"{bits:032x}")
    return rows


def display_text(fb):
    """ASCII art: '#' lit, '.' dark."""
    return "\n".join(bin(int(r, 16))[2:].zfill(DISPLAY_W).replace("1", "#").replace("0", ".") for r in display_rows(fb))


def display_png(fb, scale=4):
    """Framebuffer -> greyscale PNG (white = lit), stdlib only."""
    w, h = DISPLAY_W * scale, DISPLAY_H * scale
    raw = bytearray()
    for row in display_rows(fb):
        bits = int(row, 16)
        line = bytes(255 if (bits >> (DISPLAY_W - 1 - x // scale)) & 1 else 0 for x in range(w))
        raw += (b"\x00" + line) * scale
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def rgb_name(i):
    """RGB index -> light name."""
    if i < RGB_STEPS:
        return f"PAD{i // 4 + 1}_SEG{i % 4}"
    if i < RGB_STEPS + 16:
        return f"STEP{i - RGB_STEPS + 1}"
    return "XL_LIGHT_50" if i == RGB_EXTRA else f"RGB_{i:02X}"


def meter_segments(low, high):
    """LOW thermometer (4 bits) + HIGH (2 bits) -> lit segments 0..6."""
    return bin(low & 0x0F).count("1") + bin(high & 0x03).count("1")


def named_lights(rgb, leds, known=()):
    """rgb {index: [r, g, b]}, leds {name: value}; known = every LED name of the product, so all
    meters are listed (at 0) before the app first sends them.
    -> {"pads": {"1": [r, g, b]}, "pad_segments": {"1": [[r, g, b] x4]}, "steps": {"1": [r, g, b]},
    "rgb": {name: [r, g, b]}, "meters": {name: {"low", "high", "segments"}}}."""
    rgb = {int(k): v for k, v in rgb.items()}
    pads, segs, steps = {}, {}, {}
    for i, v in sorted(rgb.items()):
        if i < RGB_STEPS:
            segs.setdefault(str(i // 4 + 1), [None] * 4)[i % 4] = v
            if i % 4 == 0:
                pads[str(i // 4 + 1)] = v
        elif i < RGB_STEPS + 16:
            steps[str(i - RGB_STEPS + 1)] = v
    meters = {}
    for name, v in [(n, 0) for n in known if n] + list(leds.items()):
        for part in ("_LOW", "_HIGH"):
            if ("METER" in name or name.startswith("VU_")) and name.endswith(part):
                m = meters.setdefault(name[:-len(part)].replace("_METER", ""), {"low": 0, "high": 0})
                m[part[1:].lower()] = v
    for m in meters.values():
        m["segments"] = meter_segments(m["low"], m["high"])
    return {"pads": pads, "pad_segments": segs, "steps": steps,
            "rgb": {rgb_name(i): v for i, v in sorted(rgb.items())}, "meters": meters}


def selftest():
    for data in (b"", b"\x00", b"\xff" * 7, bytes(range(256)), bytes(512)):
        packed = pack7(data)
        assert all(b < 0x80 for b in packed) and unpack7(packed)[:len(data)] == data
    assert len(pack7(bytes(512))) == 586                       # full screen: 599-byte message
    # a 0B as the app builds it: pages 0..1, columns 2..3, then a 0A clear
    fb = new_display()
    data = pack7(bytes([0x01, 0x80, 0xFF, 0x00]))
    n = 5 + len(data)
    msg = bytes([0xF0, 0x47, 0x7F, 0x5A, 0x0B, n >> 7, n & 0x7F, 3, 0, 1, 2, 3]) + data + b"\xF7"
    assert display_apply(fb, msg) and fb[2] == 0x01 and fb[3] == 0x80 and fb[128 + 2] == 0xFF
    rows = display_rows(fb)
    assert rows[0] == f"{1 << 125:032x}" and rows[7] == f"{1 << 124:032x}" and rows[8] == f"{1 << 125:032x}"
    assert display_text(fb).split("\n")[0][2] == "#"
    clear = bytes.fromhex("f0 47 7f 5a 0a 00 07 03 00 03 00 7f 00 00 f7")
    assert display_apply(fb, clear) and not any(fb)
    assert display_apply(fb, bytes.fromhex("f0 47 7f 5a 0a 00 07 03 00 03 00 7f 0f 0f f7")) and all(b == 0xFF for b in fb)
    assert not display_apply(fb, bytes.fromhex("f0 47 7f 5a 0a 00 07 03 00 04 00 7f 00 00 f7"))   # page 4: no
    assert not display_apply(fb, msg[:13] + b"\xF7")                                          # short data
    png = display_png(fb, 1)
    assert png.startswith(b"\x89PNG") and struct.unpack(">II", png[16:24]) == (128, 32)
    assert rgb_name(0) == "PAD1_SEG0" and rgb_name(0x3F) == "PAD16_SEG3" and rgb_name(0x42) == "STEP3"
    assert rgb_name(0x50) == "XL_LIGHT_50"
    assert meter_segments(7, 1) == 4 and meter_segments(15, 3) == 6
    lights = named_lights({"4": [1, 2, 3], "5": [1, 2, 3], "64": [0, 127, 0]},
                          {"MAIN_OUTPUT_METER_LEFT_LOW": 15, "MAIN_OUTPUT_METER_LEFT_HIGH": 1, "PLAY": 3},
                          ["PLAY", "AUDIO_IN_1_METER_LOW", "MAIN_OUTPUT_METER_LEFT_LOW", None])
    assert lights["pads"] == {"2": [1, 2, 3]} and lights["pad_segments"]["2"][:2] == [[1, 2, 3]] * 2
    assert lights["steps"] == {"1": [0, 127, 0]}
    assert lights["meters"] == {"MAIN_OUTPUT_LEFT": {"low": 15, "high": 1, "segments": 5},
                                "AUDIO_IN_1": {"low": 0, "high": 0, "segments": 0}}
    print("surface_out selftest ok")
