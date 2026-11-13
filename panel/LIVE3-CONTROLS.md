# Virtual MPC Live III (MODEL=acvg): controls

What the stock MPC 3.10.0 app does with the fake Live III surface (`panel/surface.py`,
product 0x58, USB 09e8:0058). Checked on the running app on 13 Nov 2026 unless marked
"table only". The control names are the ones `POST /control` takes; `GET /surface` lists them
under `controls`.

Message notation: `9n` = note on, ch1 = status 0x90, ch10 = 0x99. LED = a ch1 CC from the app,
value 0 off, 1 dim (idle keys on the Live III sit at 1, not 0), 3 lit, 4/5 brighter or coloured.
RGB = `F0 47 7F 58 65 00 04 index R G B F7` on cable 0, 7-bit colour.

## Differences from the XL (MODEL=acvc)

| | XL | Live III |
|---|---|---|
| Identity product byte | 0x5A | 0x58 |
| Q-Links | 16, touch notes 14-29 | 4, touch notes 84-87, plus QLINK_BANK (note 0) for banks 1-4 |
| Q-Link CC | 0x10 + n - 1 | the same: 0x10 + n - 1 (not reversed, see below) |
| Pad RGB | 1 light per pad, index pad - 1 | 4 lights per pad, index 4*(pad-1) .. 4*(pad-1)+3 |
| Step row | - | RGB index 0x40-0x4F, notes 92-107 |
| Strip LEDs | ch2 CC 57-81 | ch1 CC 57-81 |
| PLAY / STOP LED | cc82 / cc81 | cc86 / cc85 |
| Note 51 | GRID | WINDOW |
| CURSOR_LEFT | note 78 | none (see below) |
| `0A` / `0B` knob display SysEx | yes | no; sends `5B 00 00` and `6B` instead |

## Never send: QLINK5_TOUCH .. QLINK8_TOUCH (notes 88-91)

The app's Live III table has them, but one QLINK5_TOUCH (note 88) stops the app's surface
side for good: no more LEDs or pings, every later button and pad ignored, an "Audio
Processing" thread parked on a PI mutex held by the other one. The touchscreen keeps working.
Reproduced 2 of 2 on fresh boots; only restarting the app (`systemctl restart acvs`) clears it.
`surface.py` drops these four names for acvg (`PROFILES["acvg"]["blocked"]`), so the API
answers 400.

## Pads (16)

| Control | Message | Feedback |
|---|---|---|
| `PAD 1`..`PAD 16` press / release | ch10 note on/off, notes 37 36 42 82 40 38 46 44 48 47 45 43 49 55 51 53 | 4 RGB lights per pad brighten while held (pad 1: idle `3F 00 00`, held `71 00 00`) |
| `PAD n` pressure 0..127 | ch10 poly aftertouch on the pad's note, while held | the pad's 4 lights track it (0x49 → 0x53 → 0x67 → 0x7F) |

The 4-lights-per-pad layout is why "pad 1 changed pads 2-4" earlier: indices 1-3 are pad 1.
The welcome dialog calls these "MPCe expressive pads"; whether a real unit also sends a pad
position (X/Y) per pad is unknown. The lab sends velocity and pressure only.

## Q-Links (4 knobs + bank)

| Control | Message | Feedback |
|---|---|---|
| `QLINK1_TOUCH`..`QLINK4_TOUCH` | ch1 notes 84-87 | opens the 4-knob overlay at the right of the screen |
| `QLINK 1`..`QLINK 4` turn | ch1 CC 0x10-0x13, relative two's complement; the server wraps it in the touch | Q-Link 1 = top knob, 4 = bottom |
| `QLINK_BANK` | note 0 | LED cc0; bank LEDs cc90-93 (QLINK_BANK_1..4) step 1 → 2 → ...; the overlay shows the next 4 parameters |
| `QLINK_PROJECT`, `QLINK_PROGRAM`, `QLINK_PAD_SCENE`, `QLINK_PAD_PARAM`, `QLINK_SCREEN`, `QLINK_TRIGGER` | notes 59-63, 113 | table only |

Evidence: CC 0x13 with QLINK1_TOUCH moved nothing; CC 0x10 + QLINK1_TOUCH moved the top
knob (Track Level), 0x11 + touch 2 the second (Playhead), 0x12 + touch 3 the third
(Metronome Enable), 0x13 + touch 4 the fourth (Metronome Level). A turn with a different
knob's touch held is ignored. Small turns can be swallowed: use about ±20 per message.
Screenshots: `out/shots/f_qlinks_api.png` (API turns through the real page server, knobs 1-4).

## Transport and edit keys (ch1 notes; LED = ch1 CC)

Checked: `PLAY` 82 (LED cc86 → 3), `STOP` 81 (LED cc85; PLAY back to 1), `STEP_PREV` 68 (cc96
3 held / 1 released), `STEP_NEXT` 69 (cc97), `BROWSER` 41 (opens the browser; lights cc41 = 5,
named STEP_SEQ in the light table), `MAIN` 52 (cc52), `MENU` 123 (cc123), `SHIFT` 49 (cc49;
also re-lights cc95 UNDO, cc85 STOP, cc110 NEXT for their shifted functions), `DATA_PLUS` 54,
`CURSOR_RIGHT` 66, `CURSOR_DOWN` 57, `CURSOR_UP` 56, `CURSOR_CENTRE` 58 (move/enter the
selected field on the Main screen), `XYFX` 126 (opens XYFX).

`MIC_SPEAKER` 34: nothing visible, LED cc34 stays 1.

Table only (same message form, LED in brackets): PLAY_START 83 [87], RECORD 73 [101],
OVERDUB 80 [84], LOCATE 70 [98], BAR_PREV 71 [99], BAR_NEXT 72 [100], LOOP_ON 74 [102],
TAP_TEMPO 53 [53, blinks with tempo], UNDO 67 [95], ERASE 9, NOTE_REPEAT 11, SET 10, F_KEY 78,
TC 45, SAVE 50, WINDOW 51, COPY 122, DELETE_KEY 124, NEXT 108 [110], PREV 109 [111],
NEXT_SEQ 42, READ_WRITE 75, MUTE 76, SOLO 77, INSTANCE_MUTE 117, INSTANCE_SOLO 118,
TRACK_NEXT 119, TRACK_PREV 120, NUMERIC_PLUS_MINUS 110, FOOTSWITCH_2 64, LCD_F2-F4 13-15.

CURSOR_LEFT: the app's Live III note table has no CURSOR_LEFT. With the START field selected,
none of the 20 unmapped notes (1, 2, 7, 16-31, 79) nor notes 4, 55, 64, 68, 78, 109, 110 moved
the selection left; CURSOR_RIGHT (66) moved it right. So 3.10.0 gives no note for it.

## Modes and pad banks (table only unless noted)

MODE_PROGRAM_EDIT 12, MODE_PROGRAM_MIX 3, MODE_SEQUENCE_EDIT 5, MODE_SAMPLE_EDIT 6 [LED 43],
MODE_SONG 8, MIX 33, ARRANGE 112, RECORD_TO_ARRANGE 32, PAD_PERFORM 114 (also note 127),
PAD_MIXER 115, CHANNEL_MIXER 116, SOUNDS 121, SAMPLER 125, PROGRAM 46, SAMPLE 47,
NO_FILTER 48, TRACK_MUTE 44, STRIP 4, PAD_BANK_A-D 35-38 (MAIN press: A = 5, B-D = 4),
FULL_LEVEL 39, SIXTEEN_LEVEL 40. Lights with no key: EMULATE_MPC3000/MPC60/VINTAGE 114-116,
PAD_ASSIGN 112, VU meters 68-71.

## Step row

`STEP1`..`STEP16` = notes 92-107. Checked: STEP1 sets RGB 0x40 to `7F 00 00` (step on), STEP8
sets 0x47. Idle step lights are `0C 0C 0C`, beat steps `1F 1F 1F`, the playhead step green.

## Touch strip

`TOUCH_STRIP` press/move/release = the XL messages: note 65 on/off, position CC 0x21 (MSB)
then CC 0x22 (LSB). Checked: press at 0.2 lit ch1 CC 57 (`7F`), move to 0.8 lit CC 76, release
cleared it, and the meters showed sound. The light table also names CC 59-63 as QLINK_* keys,
so strip segments 3-7 and those keys share CCs on this product; which one a real unit means is
unresolved. Strip division lights (1/4 .. 1/32T) are CC 24-31.

## Data wheel

`DATA_WHEEL` turn = CC 0x64, relative. Checked: +1 changed BARS 2 → 3. CC 0x65 does nothing.

## Battery, mains, speaker

The device tree says az04, so the app uses class Az04: battery
`/sys/class/power_supply/sbs-6-000b/` (files `present`, `status` compared with "Charging",
`capacity` / 100) and mains `/sys/class/power_supply/mains-power`. The lab has neither and the
app shows no battery icon or warning. A tmpfs over `/sys/class/power_supply` with those files
(present 1, Discharging, 73, mains online 0), then an app restart, still showed no indicator,
so no fake was added. Not proven where 3.10.0 would draw it.

## Tools

`scripts/l3probe.py` stands in for the page server on the same port and adds
`POST /raw {"hex": "b0 10 6c"}` for arbitrary MIDI. Start it before `lab.py run`.
