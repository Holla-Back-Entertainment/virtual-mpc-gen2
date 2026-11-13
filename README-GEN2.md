# Virtual MPC Gen 2 (MPC XL / MPC Live III)

A QEMU lab that boots the stock Akai MPC Gen 2 firmware 3.10.0 (rootfs verified block by
block with its own dm-verity table) on a virtual arm64 machine, posing as an MPC XL
(`MODEL=acvc`, default) or an MPC Live III (`MODEL=acvg`). It is a fork of the MPC Sample
lab (`README-SAMPLE.md` describes that one; most of `docs/` is still about the Sample). Use it to
try mods safely: nothing here writes to a real device.

No firmware is in this repository. `firmware/extracted/{part1,part2,parr}` come from your own
update image and are checked against `inputs.sha256`.

## Quick start

```bash
python3 lab.py build                  # Docker: kernel, bridge, disk images (once)
python3 lab.py run --headless         # VM + panel page; Ctrl-C shuts the MPC down cleanly
python3 lab.py firstrun               # tap past the welcome wizard and first dialogs
python3 lab.py ssh 'systemctl status acvs'
python3 lab.py stop                   # power key + SHUT DOWN, wait for power-off
python3 lab.py status                 # both models: on/off, run directory, ports
```

Every command, `scripts/*.py` and `panel/gen2.py` act on the model in `MODEL` (default
`acvc`, the XL). The two models have separate ports and run directories, so an XL and a Live III
run side by side from one copy:

```bash
python3 lab.py run --headless &                  # MPC XL,       http://127.0.0.1:8865/
MODEL=acvg python3 lab.py run --headless &       # MPC Live III, http://127.0.0.1:8875/
MODEL=acvg python3 lab.py stop                   # stops the Live III only
```

The app's welcome wizard (NEXT, GOT IT, LET'S GO) appears whenever the app starts, because
`StartupWizard.Enabled=1` stays in `/data/Settings/MPC/MPC.settings` (finishing the wizard
does not clear it); it is not a sign of an unclean shutdown. `lab.py firstrun` taps through it
and what follows: Select Project (NEW PROJECT), then the app's info dialogs (OK; REMIND ME
LATER where there are two buttons, never DON'T SHOW AGAIN). Before every tap it takes a
screendump and reads a few pixels (wizard pages are one flat colour, a dialog dims the app,
Select Project and the app have the grey header) and taps only a screen it recognises; the
dialogs it answered are saved as `out/run-<model>/firstrun-dialog-N.png`. The same key set to
0 (with the app stopped: `systemctl stop acvs`, edit, `systemctl start acvs`) keeps the wizard
away, also after a clean reboot (seen on the XL). It is the app's own key, but no menu sets it,
so whether inMusic supports changing it is unknown; the lab leaves it as the device has it.

**On a Mac with Ableton Live:** `mac/README.md` covers the Virtual MPC app, the
Virtual MPC XL and Live III audio devices and the Live templates (`mac/live/`).

Screen taps: `python3 scripts/tap.py X Y` (1280x800). Screenshot:
`python3 scripts/qmp.py screendump out/shots/x.png`. Buttons: `POST /control
{"name":"PLAY","action":"press"|"release"}` on the panel server, `GET /surface` lists them.

## Ports and run directories

All on 127.0.0.1. The Live III's ports are the XL's + 10. `MPCLAB_PORT_OFFSET=N` adds N to
every port of both models, so several copies of the lab can run at once; space copies 20
apart (an offset of 10 lands on the other model's ports). Example: `MPCLAB_PORT_OFFSET=40`
puts the XL's page on 8905 and the Live III's on 8915.

| name | XL (acvc) | Live III (acvg) | what |
|---|---|---|---|
| panel | 8865 | 8875 | panel page (screen, controls, sound) |
| ssh | 2122 | 2132 | root shell (`lab.py ssh`) |
| usb_ssh | 2322 | 2332 | the device's USB network address 192.168.155.1:2222 |
| qmp / qmp_panel | 5654 / 5655 | 5664 / 5665 | QEMU monitor (lab.py / panel server) |
| cs | 5651 | 5661 | virtual USB control surface (guest relay <-> panel) |
| ext_midi | 5652 | 5662 | "External MIDI" serial port (raw MIDI bytes; a Gen 1 leftover, kept) |
| midi_in1 / midi_in2 | 5751 / 5752 | 5761 / 5762 | rear MIDI In 1, 2: host -> app (raw MIDI bytes) |
| midi_outa .. midi_outd | 5753 .. 5756 | 5763 .. 5766 | rear MIDI Out A-D (Live III: A, B; its outc/outd are unused): app -> host (raw MIDI bytes) |
| console | 5653 | 5663 | guest console (also `out/run-<model>/console.log`) |
| audio / pcm | 5656 / 5657 | 5666 / 5667 | audio bridge / PCM WebSocket for the page |
| vnc | 6010 | 6020 | the screen |

Each model keeps its eMMC overlay (`emmc.qcow2`: /data, settings, projects), SD image, device
tree, console log and screen files in `out/run-acvc` or `out/run-acvg`; the kernel, initramfs,
eMMC base image and lab layer in `out/` are shared and opened read-only. `lab.py reset` clears
the eMMC of `MODEL` only. A lab from before this split kept one XL in `out/run`: the next XL
`run` moves it to `out/run-acvc` (state kept) and leaves a link `out/run -> run-acvc`. It does
not move it while a QEMU still runs from it (the eMMC overlay's write lock tells): that VM
keeps working, and `status`, `stop`, `ssh` and the panel go on using `out/run` until it stops.

## What works (verified on the XL and the Live III, 13 Nov 2026)

- **Clean shutdown.** The app reads the power key itself (gpio-keys,
  `/dev/input/by-path/platform-gpio-keys-event`; the stock `az01-pwrbtn` daemon on the same
  device is only the emergency path). QMP `system_powerdown` makes the app ask "Do you want to
  shut down your MPC XL?" (Live III: dialog SHUT DOWN MPC LIVE III, "... your MPC Live III?",
  SHUT DOWN at the same place); `lab.py` taps SHUT DOWN at (532, 770) and waits for power-off
  (about 5 s). `lab.py stop` and Ctrl-C in `lab.py run` both do this; after 60 s without
  power-off they quit QEMU and say so. Next boot has no "not properly shut down" dialog, and
  a project saved before the shutdown is listed under OPEN A RECENT PROJECT (checked on the XL).
- **`lab.py preload LIB.so | --off`.** Adds a `/run` drop-in with `LD_PRELOAD` to
  `acvs.service` and restarts it; checks that the library is in `/proc/<pid>/maps` of `MPC`.
  Test library: `tools/preload-hello.c` (built by `lab.py build tools` into
  `share/bin/libpreload-hello.so`) logs from its constructor to the journal and to
  `/run/mpclab-preload/hello.log`. Note that `az01-launch-MPC` is a shell script, so the
  library is also loaded into it, `dfu-util`, `grep`, `setarch` and the app's child processes
  (crashpad). The restart stops the app with SIGTERM; no unclean-shutdown dialog followed.
- **SD card.** Files in `sdcard/` become a FAT16 card (label `MPC XL`, `MPC LIVE3` for
  acvg) at the next `run` when `out/run-<model>/sdcard.img` does not exist yet (delete it to
  rebuild). It mounts at `/media/MPC XL` (`/media/MPC LIVE3`), shows under Places in the
  browser with that name, and the app can save projects to it (saving checked on the XL).
- **Internal NVMe ("User" storage).** `lab.py run` adds a QEMU NVMe drive,
  `out/run-<model>/nvme.qcow2` (32 GB sparse qcow2, made blank on the first run, kept across runs;
  `lab.py reset` keeps it, `lab.py reset --nvme` deletes it too). Device tree: `chosen` is set
  as in the FIT's own `rk3588-az04-acvc` / `-acvg` trees, `inmusic,nvme-fitted` and no
  `inmusic,emmc-content`. The stock units then do the rest:
  - lab udev rule `61-mpclab-nvme.rules` (NVMe serial `mpclab-nvme`) gives the drive the
    device's `ID_PATH` and by-path names `platform-a40000000.pcie-pci-0000:01:00.0-nvme-1[-partN]`;
    stock `65-nvme.rules` then pulls in `nvme.mount` + `synths.mount`. No stock unit is changed.
  - No stock unit partitions the drive (`acvg-nvme-update.sh` only unpacks onto `/synths`;
    `az0x-format-drive` would make a single exFAT partition). On a blank drive the lab's
    `mpclab-nvme-prepare.service` (before the mounts) runs the firmware's own `fdisk`,
    `mkfs.ext4`, `mkfs.exfat`: GPT, part1 8 GiB Linux filesystem, ext4, label `synths`;
    part2 the rest, Microsoft basic data, exFAT, label `MPC User`. Types, sizes and labels
    are lab choices (the factory layout is not in the update image; the label follows
    `setup-acvs-user-partition.sh` on eMMC-only units). It does nothing once part1 exists.
  - Result (XL and Live III): `/dev/nvme0n1p2 on /nvme type exfat (rw)`, `/dev/nvme0n1p1 on
    /synths type ext4 (ro)`, 0 failed units. On first boot stock `acvg-nvme-update.service`
    unpacks `/usr/share/Akai/Content/acvg-nvme-update-image.tar.gz` (in the rootfs, about 210 MB
    unpacked: `Demos/`, `Expansions/`, `Synths/`) onto `/synths` and writes `.version` = 7; later
    boots skip it. The app creates `/nvme/MPC Documents/`.
  - In the app: the browser's Places list `User` > `MPC Documents` plus the 8 expansions
    from `/synths`; Select Project shows 5 project templates and 2 demos. A project saved with
    SAVE > Project lands in `[User] MPC Documents/Projects` = `/nvme/MPC Documents/Projects/`,
    survives `lab.py stop` + `run`, is listed under OPEN A RECENT PROJECT and opens. Same on
    `MODEL=acvg` (drive prepared, mounted, save works; `lab.py stop` powered it off cleanly).
- **Boot splash**: the stock `mpc3.png` from the FIT's initramfs, shown until the app starts.
- `az0x-info` and `az0x-loadpin-trustpoints` are skipped by lab drop-ins
  (`overlay/lab-layer/etc/systemd/system/*.service.d/mpclab.conf` say why); no failed units.

## Audio (verified 13 Nov 2026)

The app opens the lab's snd-aloop card as 32 ch S32_LE 44.1 kHz each way. That is the app's own
choice, not a lab limit: it logs "Audio 44100Hz; 56-in; 56-out" (its logical channels) and then
"inefficient input/output path: hardware=56 filter=32", and calls `set_channels_near(32)` on both
streams (seen with an LD_PRELOAD trace, 14 Nov 2026) although a test kernel with aloop's cap raised to 64
offered it 1-64 (that patch was dropped as useless); the "Int32ToFloat unsupported channel count: 32" warning stays either way. So logical
channels 33-56 never reach the card.

`mpclab-audio-bridge` in the VM sends app outputs 1-2 to the host as S16 stereo over the
virtio-serial port `mpclab.pcm` (WebSocket on the `pcm` port; the page plays it, **Sound** button)
and feeds app inputs 1-2 from the source picked with the page's **In** button or the API:

    GET  /audio   {"in", "gain", "tone_hz", "width", "in_width", "bridge": linked, "levels": {"out":
                  [peak per app output channel], "in": [peak per app input channel], "drop", "under"},
                  "levels_age": s}
    POST /audio   {"in": "off"|"tone"|"mic", "gain": 0..100, "tone_hz": Hz,
                   "width": 2|8|32, "in_width": 2|4|28|32}   (any subset)

Channel map of the app's 32-channel card (XL; measured 14 Nov 2026 unless marked):

| card channels | playback (app out) | capture (app in) |
|---|---|---|
| 1-2 | Out 1,2 = Main | Input 1,2 (AUDIO_IN_1/2 meters) |
| 3-4 | Out 3,4 | Input 3,4 (AUDIO_IN_3_4 meters) |
| 5-6 | Out 5,6 | USB In 1,2 (USB_IN_LEFT/RIGHT meters) |
| 7-8 | Out 7,8 | USB In 3,4 |
| 9-28 | USB Out 1-20 (known, not re-measured) | USB In 5-24 |
| 29-32 | USB Out 21-24 (known, not re-measured) | "Input 29,30 <none>", "Input 31,32 <none>": only with 32 INPUTS/OUTPUTS on (below) |
| (33-56) | not on the card: the app's other logical channels (CV/Gate and the rest) stay inside the app | - |

Capture 7-28 = USB In 3-24, measured 14 Nov 2026 (XL): the Sampler set to each of USB Input 3,4 ..
23,24 showed a -12 dBFS tone on capture k+4,k+5 (USB Input k,k+1) and nothing with the tone on the
next pair up (tone written straight into the aloop feed with mpclab-audio stopped).

**No setting opens more than 32 card channels.** Preferences > Audio Device has two: **32
INPUTS/OUTPUTS** (key `TrackMixerUsingFixedMaximumIO` in `/data/Settings/MPC/MPC.settings`, 0 by
default; the app restarts on a change: "28 inputs and 32 outputs" off, "32 inputs and 32 outputs"
on) and **USB AUDIO CHANNEL COUNT** (`USBAudioIOConfiguration`, 24 = "24 in, 24 out", the other
choice 8). With 32 I/O on the app still logs "56-in; 56-out" and "hardware=56 filter=32"; it only
adds the inputs "Input 29,30 <none>", "Input 31,32 <none>" (a tone on capture 29,30 then shows on
the first). The output list is Out 1-8 + USB Out 1-24 either way. A **CV track** has no audio
output in the mixer; it picks CV PORT / GATE PORT from CV 1-16 (default CV 1 / CV 2), and notes
played into it put nothing on any of the 32 card channels: CV/Gate stays among the logical
channels 33-56, so an aloop/bridge widening to 56 or 64 would not expose it unless the app opened
the card wider, which it does not. Tested on the XL with the setting restored afterwards.

A tone on capture pair k,k+1 (in_width 32) lit AUDIO_IN_1/2 for 1-2, AUDIO_IN_3_4 for 3-4,
USB_IN for 5-6 and no meter for 7-32 (the XL has meters for USB In 1,2 only).

**Multichannel** (for a native client; the page keeps the defaults). The `pcm` WebSocket carries
S16LE 44.1 kHz interleaved. `width` 2 (default) = main out 1-2, exactly as before; `width` 8 = app
playback channels 1-8 = **Out 1-8** (Main = 1,2), ~705 KB/s; `width` 32 = all 32 (Out 1-8 + USB
Out 1-24), ~2.8 MB/s. `in_width` 2 (default) = the host stream feeds Input 1,2; `in_width` 4 = it
feeds **Input 1,2 and 3,4** (capture channels 1-4); 28 = capture 1-28 (Input 1-4 + USB In 1-24);
32 = all capture channels. Gain applies to every fed channel; `tone` stays on 1-2. Every write to the host is one whole 256-frame chunk of one width,
and a width change drops the partial chunk, so a client counting frames from the stream's first
byte stays aligned across a change (an in_width change drops the partial input frame; the client
should switch on a frame boundary). Measured 14 Nov 2026 (XL): with pad 1 routed to Out 3,4 in the
mixer's I/O tab, width=8 put 0.78 on channels 3,4 and 0 elsewhere, also when switched 2 -> 8
mid-stream; 706 KB/s and `drop` +0 over 12 s with a client reading; width=2 176 KB/s, +0. Width 32 (14 Nov, stdlib
WebSocket client): 2815 KB/s, `drop` +0 over 28 s with pads playing; one 16 KiB write per
256-frame chunk stays under virtio-serial's 32 KiB per write, so a chunk is never split. A
56-wide stream is not offered: the card has 32 channels (above). A tone
sent on host channels 3-4 with in_width=4 gives `levels.in` [0, 0, 0.25, 0.25] and lights the XL's
`AUDIO_IN_3_4_LEFT/RIGHT` meters (4 segments): capture 3-4 is Input 3,4.

`tone` is a sine from the bridge (0.5 x gain/100, so -12 dBFS at the default 50); `mic` is the
page's microphone (the browser asks; the page streams it back over the same WebSocket, and only
while the page's sound is on). One client at a time can hold the `pcm` WebSocket.

- **Pads sound.** A fresh project loads "Trap-Kit-MPCe Default Fm", whose samples ship in the
  rootfs (`/usr/share/Akai/SME0/Factory Tracks/Trap-Kit-MPCe Default Fm_[TrackData]/*.wav`,
  19 files). Pad peaks at the host socket (velocity 110): pad 1 0.78, 2 0.67, 3 0.47, 4 0.19.
  Metronome: 0.3 on beats, 1.0 on the downbeat (XL and Live III).
- **Sampling.** With `in=tone` the Sampler (control SAMPLER) shows Input 1,2 at -12 dB; a
  recording is a 440 Hz sine at peak 0.25 and plays back at 0.25 at the host. With the internal NVMe the app
  records to `/nvme/MPC Documents/Temporary Files` and recording works on both models (ARM,
  record the tone, stop: the KEEP OR DISCARD SAMPLE dialog appears). A `/data` created before
  the NVMe existed keeps `temporaryFileLocation` = `/sdcard/...`, an empty read-only directory,
  and the Sampler then says DISK SPACE LOW: run `lab.py reset` once (it keeps the NVMe).
- **Meter LEDs** move with signal on the XL: `VU_LEFT/RIGHT` and `MAIN_OUTPUT_LEFT/RIGHT` (0-5
  segments on pad hits), `AUDIO_IN_1/2` (4 segments at -12 dB). `AUDIO_IN_3_4_*` move with
  `in_width` 4 (above); `USB_IN_*` stay 0 (nothing feeds them). Live III: no meter CCs at all during playback.
- **Live III**: main out is app channels 1-2 as on the XL; the Sampler lists Input 1,2 then USB
  Input 1,2 ... (no separate built-in mic entry).
- **CPU** (guest, schedstat, 10 s): bridge 1.5% of one CPU with no page connected, 3.5-7% while a
  client is connected and the tone or metronome runs; the app 12-19%. The bridge moves 256 frames
  per wake-up (avail_min), about 170 wake-ups/s per thread.

## Rear MIDI ports

The stock XL has no serial MIDI for its DIN ports; they are USB MIDI behind its internal hub.
The lab plays them with a second f_midi gadget (`mpclab-midi-gadget`, 1d6b:0104 "MPC XL Rear",
in_ports=2, out_ports=4) on a third dummy UDC (`dummy_hcd.num=3`); snd-usb-audio gives the app an
ordinary USB MIDI card. `mpclab-midi-relay` joins each gadget port to its own virtio-serial port
and TCP port (`midi_in1`.. above), raw MIDI bytes, no framing. Kernel patch 0005 fixes f_midi's
swapped substream counts (with 2 in / 4 out, Out C and D were dropped). The Live III gets the same
with Out A, B only (below). The Mac app makes CoreMIDI
destinations "Virtual MPC XL MIDI In 1", "... In 2" and sources "Virtual MPC XL MIDI Out A".."Out D";
"Virtual MPC XL" (both ways) stays External MIDI.

The gadget names its USB MIDI jacks (iJack strings, kernel patch 0007, configfs `in_names` /
`out_names`), and snd-usb-audio names the ports "MPC XL Rear <jack name>". The app lists ALSA
sequencer ports, and the sequencer makes port N of a card one port for both directions, named
after its output, so In 1 and Out A share a name, as do In 2 and Out B. The ports read
**MPC XL Rear MIDI In 1 / Out A**, **MPC XL Rear MIDI In 2 / Out B** (MIDI IN and MIDI OUT lists),
**MPC XL Rear MIDI Out C**, **MPC XL Rear MIDI Out D** (MIDI OUT only); the app's own sequencer ports
mirror these names (`/proc/asound/seq/clients`, 14 Nov 2026). Before patch 0007 they read "MPC XL
Rear MIDI 1".."4". Separate names per direction would need two USB MIDI interfaces (two rawmidi
devices); not done. The gadget's own end has no sequencer client (`no_seq`, kernel patch 0006),
otherwise the app listed its ports `f_midi-2`, `f_midi-3` as inputs. snd-usb-audio claims the
device, so `az01-unsupp-dev` logs "skipping supported device" and no UNRECOGNIZED USB DEVICE
dialog appears. Verified 14 Nov 2026 (XL, Mac app): notes sent to In 1 play pad 1 (levels.out 0.78)
with the drum track's MIDI IN on All Ports, on Rear MIDI 1 (In 2 then gives 0) and on Rear MIDI 2
(In 1 then gives 0); a MIDI track with MIDI OUT Rear MIDI 1..4 echoed In 1's notes to Out A..D
only (5 of 5 note-ons, 0 on the other three each time). External MIDI still works both ways.

### Live III (verified 14 Nov 2026, TCP level)

Akai's Live III guide lists MIDI In 1, 2 and MIDI Out A, B. The same script makes a gadget with
in_ports=2, out_ports=2, product **MPC Live III Rear** (card `L3RearMIDI`; the relay runs with 2
Out ports, on `midi_in1`, `_in2`, `_outa`, `_outb` = 5761-5764; 5765/5766 stay unused). How the real
Live III wires its DIN ports inside is not known; this mirrors the XL. The jack names leave out
"MIDI " because a sequencer port name holds 31 characters ("MPC Live III Rear MIDI In 1 / Out A"
was cut to "... MIDI In 1 / O"). The app lists **MPC Live III Rear In 1 / Out A** and **MPC Live III
Rear In 2 / Out B** in both MIDI IN and MIDI OUT, next to its own "MPC Live III MIDI 1", "... 2".
Notes sent to 5761 and 5762 play pad 1 (levels.out 0.75/0.78) with the drum track on All Ports;
on Rear In 1 only 5761 plays, on Rear In 2 only 5762. A MIDI track with MIDI OUT Rear In 1 / Out A
sent 5 of 5 note-ons to 5763 and 0 to 5764; with Out B, 0 and 5. The Mac app makes no Live III
ports yet.

## What does not work

- **SD autorun.** MPC 3.10 on Gen 2 lists `autorun/` on a card for `*.scriptpkg` files only
  and checks each with `az01-signed-fs` against `/usr/lib/az0x/pubkeys/akai-1.pub`. A plain
  `autorun/autorun.json` + `autorun.sh` (+ `autorun.check`) package, as used on the Sample,
  is never read; an unsigned `.scriptpkg` is rejected ("invalid image file: bad header").
  `lab.py autorun` still builds and inserts the card and reports what the app logged, but
  only an inMusic-signed package would run. `--yes` taps the left bottom button: unverified,
  since no script question has been seen.
- **Factory content and plugins.** Only what the update image carries is installed (see
  above). A new project still reports "1 plug-in(s) failed to load: AIR Reverb Pro", and the
  app says new plugin instruments need their content files; nothing named Reverb Pro exists in
  the rootfs or on `/synths`, so where its files belong is not known.
- `tools/flow_test.py` and `tools/stock_walk.py` still drive the MPC Sample.

## Factory content (where a real unit's content would go)

On the XL / Live III (`inmusic,nvme-fitted`) the stock mounts are:

| mount | source | options | what |
|---|---|---|---|
| `/nvme` | NVMe part2 (`nvme.mount`) | rw | user storage, app's `User`; `MPC Documents/` |
| `/synths` | NVMe part1 (`synths.mount`, written by `az04-synths-generator`) | ro | factory content: `Demos/`, `Expansions/`, `Synths/<vendor - MPC - plugin>/` |
| `/content` | not mounted (`content.mount` needs `chosen/inmusic,emmc-content`, absent on these models) | | empty rootfs directory |

So content copied from a real unit's `/synths` goes onto lab NVMe partition 1 (ext4 here,
`/dev/nvme0n1p1`, files owned by root), keeping its paths, e.g. by hand in the running VM:
`mount -o remount,rw /synths`, copy into `/synths/`, `mount -o remount,ro /synths`. Keep
`/synths/.version` at 7 or higher, or `acvg-nvme-update.sh` unpacks its tarball over it on the
next boot. Expansions already on a real unit's user drive go under `/nvme/MPC Documents/`.
`python3 lab.py content FOLDER` copies a folder laid out like a real unit's `/synths` into the running VM
(it remounts `/synths` read-write and back); `--user` copies into `/nvme/MPC Documents/` instead.
Restart the app afterwards (`lab.py ssh systemctl restart acvs`). The content must be your own
(from your unit or downloads you own); none of it belongs in this repository.
`lab.py build content` (the Sample's eMMC `/content` step) is skipped on Gen 2, since nothing
mounts that partition on these models. Not tried with real content.

## Unverified

- Saving to the SD card and the recent-project list after a reboot were checked on the XL
  only; everything else above was run on both models.
- The real NVMe partition sizes, filesystems and labels (see Internal NVMe above), and whether
  the app's USB computer mode, which names the NVMe by the same platform path, works here.
