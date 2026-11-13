# Virtual MPC for macOS (app, audio device, Ableton Live)

Three parts turn the Gen 2 lab into an MPC you can play from Ableton Live 12:

| Part | Folder | What it does |
|---|---|---|
| **Virtual MPC.app** | `mac/Sources`, `mac/build.sh` | Window with the panel, starts or attaches to the lab VM, moves its audio and MIDI to macOS |
| **Virtual MPC** audio devices | `mac/driver` | Core Audio driver with one device per model: the MPC's outputs appear as inputs in Live, Live's outputs feed the MPC's inputs |
| **Live templates** | `mac/live/` | One Live set per model: the MPC track, a print track and one track per output pair |

The VM must be built first (`python3 lab.py build`, see `README-GEN2.md`).

## Install

```bash
mac/build.sh                         # -> mac/build/Virtual MPC.app
cp -R "mac/build/Virtual MPC.app" /Applications/
mac/driver/build.sh test             # -> mac/build/VirtualMPC.driver, runs the self-check
mac/driver/install.sh                # asks for your password, restarts Core Audio
```

Installing restarts Core Audio: every app's sound drops for a second, and Live may lose its
audio device (set it again, below). `mac/driver/uninstall.sh` removes the driver.

The app keeps the lab's repo path, model and audio choices in the defaults domain
`com.henryskillz.virtualmpc`; its log is `~/Library/Logs/VirtualMPC.log`.

## The app

- **MPC menu:** Start MPC (runs `lab.py run --headless` for the selected model), Stop MPC,
  Reload Panel. If the VM is already running the app attaches to it.
- **Model menu:** MPC XL or MPC Live III. Audio and MIDI follow the model.
- **Audio menu:** the speaker device (what you hear the MPC on), **Hear MPC Directly**
  (turn it off when you monitor the MPC through Live, or you hear it twice), and the routing
  mode line: `stereo` without the driver, `stereo (old Virtual MPC XL driver: reinstall the driver)`
  while only the old 16-ch driver is installed, `Virtual MPC XL: 32 out / 28 in, 44.1 kHz` or
  `Virtual MPC Live III: 30 out / 26 in, 48 kHz` with it.
- With the driver the app asks the VM for width 32 / in_width 28 and opens the model's hidden
  **Engine** device. The speaker plays MPC Main 1/2 (if Hear MPC Directly) + Live's Monitor 1/2.
  The pair runs at whatever rate Live sets (44.1 or 48 kHz); the app converts to the MPC's 44.1.
- **MIDI ports** (CoreMIDI, visible in Live; only the selected model's exist):

| macOS port | MPC port | Lab TCP |
|---|---|---|
| Virtual MPC XL | External MIDI (in and out) | 5652 |
| Virtual MPC XL MIDI In 1, In 2 | MIDI In 1, In 2 | 5751, 5752 |
| Virtual MPC XL MIDI Out A … Out D | MIDI Out A … D | 5753–5756 |
| Virtual MPC Live III | External MIDI (in and out) | 5662 |
| Virtual MPC Live III MIDI In 1, In 2 | MIDI In 1, In 2 | 5761, 5762 |
| Virtual MPC Live III MIDI Out A, Out B | MIDI Out A, B | 5763, 5764 |

## Channel map (Virtual MPC driver)

`VirtualMPC.driver` makes one device per model for Live, each with a hidden partner the app
uses (`… Engine`, not in device lists, never default). The Engine's outputs are the Live
device's inputs and Live's outputs are the Engine's inputs, so nothing loops back.
The XL keeps its old UID (`VirtualMPCXL_UID`), so Live keeps its setting.

**Virtual MPC XL** (32 in, 30 out)

| Live side | MPC side |
|---|---|
| Inputs 1–8 | Out 1–8 (Main = 1/2) |
| Inputs 9–32 | USB Out 1–24 |
| Outputs 1/2 | Monitor: played on the app's speaker device |
| Outputs 3/4, 5/6 | Input 1,2 and 3,4 |
| Outputs 7–30 | USB In 1–24 |

**Virtual MPC Live III** (30 in, 28 out)

| Live side | MPC side |
|---|---|
| Inputs 1–6 | Out 1–6 (Main = 1/2) |
| Inputs 7–30 | USB Out 1–24 |
| Outputs 1/2 | Monitor: played on the app's speaker device |
| Outputs 3/4 | Input 1,2 |
| Outputs 5–28 | USB In 1–24 |

Input arrives at the MPC through its input gain (50 % = −6 dB by default), like a real gain knob.
`mac/driver/install.sh` removes the old one-device `VirtualMPCXL.driver`.

## Ableton Live setup

The steps are the same for both models. Pick the device that matches the app's Model menu;
Live has one input device at a time, so switching model means switching it in Live too.

1. Live Settings > Audio: **Audio Input Device = Virtual MPC XL** (or **Virtual MPC Live III**).
   Output: your speakers or "Use System Device". Live runs both at one rate; with AirPods it
   is 48 kHz, which works (the app converts from the MPC's 44.1 kHz).
2. Input Config: switch on stereo 1/2 to 7/8 for the XL, 1/2 to 5/6 for the Live III (and the
   mono inputs if you route mono outs). USB Out pairs follow: 9/10 up on the XL, 7/8 up on the Live III.
3. Settings > Link, Tempo & MIDI: Track on for the model's ports you use.
4. Copy `mac/live/Virtual MPC XL.als` or `mac/live/Virtual MPC Live III.als` into your User
   Library `Templates` folder and open it from Live's browser.

Template tracks:

| Track | XL template | Live III template |
|---|---|---|
| MPC (MIDI, armed) | External Instrument: MIDI To Virtual MPC XL ch 1, audio from 1/2 | Same, to Virtual MPC Live III |
| MPC Print | Records the MPC track (post mixer) | Same |
| MPC Out pairs | Ext. In 3/4, 5/6, 7/8, monitor In | Ext. In 3/4, 5/6, monitor In |

Route a pad or track on the MPC to Out 3,4 and up to hear it on the matching track.

To sample from Live into the MPC, set Live's output device to the model's device and send a
track to output 3/4 (MPC Input 1,2). On the XL 5/6 is Input 3,4 and 7/8 up is USB In 1–24;
on the Live III 5/6 up is USB In 1–24.

## Checks

- `"/Applications/Virtual MPC.app/Contents/MacOS/VirtualMPC" --selftest`: jitter buffer,
  channel mapping, MIDI parser.
- `mac/driver/build.sh test`: driver loopback swap and summing, properties, timestamps.
- Verified 14 Nov 2026 on the XL in Live 12.4: a pad routed to Out 3,4 and 7,8 lit the matching
  Live tracks only; a tone on outputs 3/4 and 5/6 reached Input 1,2 and 3,4; MIDI In 1/2 and
  Out A–D each reached only their own port.
- Verified 14 Nov 2026 on the Live III: Out 1,2, 5,6 and USB Out 1,2 landed on device inputs
  1/2, 5/6 and 7/8 only; a tone on outputs 3/4, 5/6, 7/8 reached Input 1,2, USB In 1,2 and
  USB In 3,4; MIDI to the External, In 1 and In 2 ports played the pad; Out A and Out B each
  reached only their own source. The Live III template is derived from the tested XL one
  (names, MIDI target, one fewer out track) and has not been opened in Live.
