# MPC Gen 2 control surface link (XL = ACVC, Live III = ACVG), firmware 3.10.0

13 Nov 2026. Static analysis of `/usr/bin/MPC` only; nothing run, nothing checked on hardware.
Addresses are vaddrs in the binary. `maps.json` holds the recovered tables (notemaps = note →
KeyID per product, lightmaps = LightID → {cc, channel}, names_a = KeyID names, names_b = LightID
names). `hs.txt`, `strmap.txt`, `sysex_sites.txt` are working notes.

## Discovery (confirmed)
- The app walks ALSA cards, reads the ctl components string, parses `USB%x:%x` and matches VID
  0x09E8 with PID 0x005A (ACVC) or 0x0058 (ACVG). Driver `AcvxDriverMidiPipe` at 0x26d3048,
  type table 0x7a14760. It is NOT found by a by-path name.
- The rawmidi device needs more than one subdevice: in `hw:C,D,1`, out `hw:C,D,1` and `hw:C,D,0`.
- So a serial-midi port with a udev symlink (the Sample lab's trick) cannot work here.

## Handshake (state machine confirmed, payload details partly guessed)
CONNECT_DELAY 1 s → ENQUIRY `F0 7E 00 06 01 F7` → PING_CHECK `F0 47 00 pp 52 00 04 t0..t3 F7`
→ SET_MODE (cmd 0x6C, 21 bytes; then `F0 47 7F pp 62 00 01 05 F7`) → SEND_INITIAL → ACTIVE.
- Identity reply `F0 7E xx 06 02 47 …`: byte 6 = product (0x5A / 0x58), version at [9..12],
  serial at 0x0e..0x21.
- Ping reply cmd 0x53 must echo the 4-byte token; byte [11] = 1 or 2 accepts.
- Guess: old firmware version → HALT_NEEDS_UPDATE; report a high version.

## Inputs (dispatcher 0x26bfaec)
- Ch1 notes = buttons via the per-product notemap (ACVC: 49 SHIFT, 81 STOP, 82 PLAY, 73 RECORD,
  35–38 PAD BANK A–D, 92–107 STEP 1–16, 14–29 Q-Link touch).
- Ch1 CC 0x10–0x1F = Q-Links, relative two's complement. CC 0x64/0x65 = relative encoders.
  CC 0x21/0x22 = 14-bit absolute (touch strip, guess).
- Ch10 notes + poly aftertouch = pads: notes 37,36,42,82,40,38,46,44,48,47,45,43,49,55,51,53
  → pads 0–15. Ch10 CC n = per-pad parameter for pad n/2 (3D X/Y, guess).

## Outputs
- LEDs = CC, value 0–3 (PLAY cc82 on ACVC, cc86 on ACVG; SHIFT cc49). ACVC meters, step lights
  and strip LEDs on ch2. Pad RGB `F0 47 7F pp 65 00 04 pad R G B F7`.

## Other ways in
- CLI: `--script <lua>`, `--scriptLibs`, `--terminateOnFinish`, `--skipStartupChecks`. The Lua
  `controlSurface` object has SetTypeTo, button Press/Release/Hold and pad velocity. Untested.
- "Developer > Hardware Emulation > Open ACVC/ACVG" menu exists (0x31a2ce0); gate unresolved.

## Build plan for a virtual surface
1. In the guest: dummy_hcd + configfs f_midi gadget, idVendor 0x09e8, idProduct 0x005a,
   in_ports=2, out_ports=2. Loopback gives a snd-usb-audio card `USB09e8:005a` with 2 subdevices.
   Check first for conflicts with the app's own gadget/UDC code.
2. Responder on the gadget-side card: answer enquiry, echo 0x52 as 0x53 with status 1, ACK and
   log 0x6C / 0x62 / 0x40 / 0x41 / 0x5B / 0x65, inject buttons, pads and Q-Links from maps.json.
3. Cheaper parallel try: `MPC --script test.lua --skipStartupChecks`.

## Still needs a capture from a real unit
- exact identity-reply layout (bytes 7–8, version positions, serial)
- full 0x6C and 0x5B payloads, meaning of the 0x62 mode values
- USB string descriptors and port names, which subdevice is "Public"
- which controls send CC 0x21/0x22 and 0x64/0x65, Q-Link order, meaning of the ch10 CCs
- retry timing and the firmware-version gate

## Corrections after running it (13 Nov 2026, virtual XL, lab `virtual-mpc-gen2`)
The fake surface now completes the handshake on the running stock app. What the app actually did:
- Ping reply is 13 bytes: `F0 47 00 pp 53 00 05 t0..t3 status F7`; status 1 accepted (parser also takes 2).
- Real order: enquiry → identity → ping → init (`6C`, `62` with mode 0x65, `41`) → enquiry →
  identity → active. The `62 … 01 05` form only appears after a failed ping.
- Accepted identity reply: `F0 7E 00 06 02 47 5A 00 19 01 05 00 00 00 <20-byte serial> F7`
  (version bytes [9..12] read raw for products 0x5A / 0x58).
- Pad RGB arrives on cable 0, LED CCs on cable 1. RGB indices run to 0x50, so lights other than
  the 16 pads use the same message.
- The app also sends `79` (16-char ASCII), `0A` (15 bytes) and `0B` (187–599 byte blobs,
  undecoded), and pings every 4 s once active.
- None of the surface→app commands `5B`, `51`, `6B`, `40`, `41` were needed.
- The kernel needs CONFIG_SND_USB_AUDIO; the gadget sits on a second dummy UDC (`dummy_hcd.num=2`).
- USB strings and port names in the lab are invented; a real unit's are still unknown.

## More corrections (13 Nov 2026, later): Q-Links, strip, pressure, Live III
- Q-Links: CC/channel/encoding were right; the app ignores a turn unless that knob's touch note
  (ch1 note 13+n, QLINKn_TOUCH) is held. CC 0x10 = Q-Link 1 = bottom-left of the app's 4x4 grid;
  0x1C = Q-Link 13 = top-left. Small turns barely register (one +3 showed nothing, 3 x +10 moved
  tempo 128.0 → 131.0), so there is some acceleration or threshold.
- On products with type field +0x34 == 2 (Live III, One, Key …) the index is 0x13 − CC. Static
  reading only; untested.
- Touch strip confirmed: touch = ch1 note 65; position = CC 0x21 (MSB, stored) then CC 0x22 (LSB,
  acts); 0x3FFF = 1.0, else masked with 0xFFF0. The app answers with strip LEDs on ch2 CC 57–81.
- Pad pressure confirmed: ch10 poly aftertouch while the pad is held; pressure 0 ignored.
- `0B` blobs: bytes 5–6 = 14-bit payload length; sent when a knob is touched or turned, so almost
  certainly the knob display graphics. Undecoded. `79` carries a serial-format ASCII string.
- Live III (product 0x58): handshake reaches active with the same replies. It also sends `5B 00 00`
  and `6B` (16 bytes), no `0A`/`0B`. LED CCs differ (PLAY cc86, STOP cc85). Idle keys dim (1) not
  off. Step row lit as RGB indices 0x40–0x4F. Its recovered table lacks CURSOR_LEFT (likely a gap).

## Final corrections of 13 Nov 2026 (XL finish + Live III build)
- RGB message `65`: on BOTH products each pad has 4 lights, index 4·(pad−1)+k (0x00–0x3F); steps
  1–16 are 0x40–0x4F; 0x50 is one unidentified XL-only light.
- Q-Link displays decoded: 17 displays (idx 0–15 = Q-Link 1–16, idx 16 = the XL's 17th knob),
  128x32 mono, SSD1306 page layout. `0B`: `F0 47 7F pp 0B Lh Ll idx p0 p1 c0 c1 <data> F7`,
  L = 5 + len(data), data = page bytes as an MSB-first bit stream in 7-bit bytes. `0A` fills a
  rectangle. Builder at 0x2686700. Decoder: lab `panel/surface_out.py`.
- XL 17th knob: touch note 66, turn = ch1 CC 0x65, shows current track level. Data wheel = 0x64.
- Knob turns: linear, no acceleration. The app sums counts within one touch and rounds against the
  value at touch-down (tempo 0.1 BPM per count, snapped to whole BPM).
- Ping reply status 2 while ACTIVE forces a full re-init and light resend (handler 0x26c2238).
- Meters: `_LOW` CC is a 4-segment thermometer (0/1/3/7/15), `_HIGH` adds 2 segments. Not seen
  to move in the lab. Gain/volume knobs have no KeyID or CC: analog-only on this link.
- Live III: Q-Link CCs are NOT reversed (0x10+n−1, touch notes 84–87); QLINK_BANK (note 0) steps
  banks 1–4. Sending QLINK5_TOUCH (note 88) hangs the app's surface side until the app restarts
  (2/2); the lab refuses notes 88–91. Strip LEDs come back on ch1 CC 57–81 (XL: ch2). No note
  moves the cursor left. Battery read from /sys/class/power_supply/sbs-6-000b and mains-power;
  no icon without one. Full list: lab `panel/LIVE3-CONTROLS.md`. XL key audit: `panel/XL-KEYS.md`.
- Power key: the app handles gpio-keys itself and shows SHUT DOWN at (532,770) on 1280x800.
- Autorun on stock 3.10 Gen 2 only accepts signed `*.scriptpkg` (az01-signed-fs, akai-1 key).
- Internal "User" storage is on NVMe on the real units; the lab has none, so it is read-only.

## Lab completion notes (13 Nov 2026, late)
- Internal storage: real XL / Live III keep user data on NVMe (`/nvme`, exFAT) and content on
  `/synths` (ext4); stock `acvg-nvme-update.service` unpacks ~210 MB of demos, expansions and
  synth data shipped in the rootfs. With `chosen/inmusic,nvme-fitted` the app's default paths
  are `/nvme/MPC Documents/...`; without it they fall back to `/sdcard/...`.
- The default kit's 19 WAVs ship in the rootfs, so pads sound without factory content.
  "AIR Reverb Pro" is still reported missing; its files are not in the image.
- Meters do move with signal: VU_*, MAIN_OUTPUT_* on pad hits, AUDIO_IN_1/2 with input (XL).
  None seen on the Live III.
- Live III key audit: Notes = PAD_PERFORM, Load = BROWSER, Mixer = CHANNEL_MIXER,
  Q-Link = QLINK_BANK, < > = STEP_PREV/NEXT, << >> = BAR_PREV/NEXT confirmed by behaviour
  (lab `panel/LIVE3-KEYS.md`). Still guessed: Set, Prev/Next, Note Repeat, Copy, Tap Tempo,
  and every shifted sub-label.
- `StartupWizard.Enabled=0` in MPC.settings keeps the welcome wizard off (not set by any menu).
