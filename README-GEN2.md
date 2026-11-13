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
| ext_midi | 5652 | 5662 | rear MIDI port (raw MIDI bytes) |
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

The app opens the lab's snd-aloop card as 32 ch S32_LE 44.1 kHz (it asks for 56).
`mpclab-audio-bridge` in the VM sends app outputs 1-2 to the host as S16 stereo over the
virtio-serial port `mpclab.pcm` (WebSocket on the `pcm` port; the page plays it, **Sound** button)
and feeds app inputs 1-2 from the source picked with the page's **In** button or the API:

    GET  /audio   {"in", "gain", "tone_hz", "bridge": linked, "levels": {"out": [peak per app
                  output channel], "in": [l, r], "drop", "under"}, "levels_age": s}
    POST /audio   {"in": "off"|"tone"|"mic", "gain": 0..100, "tone_hz": Hz}   (any subset)

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
  segments on pad hits), `AUDIO_IN_1/2` (4 segments at -12 dB). `AUDIO_IN_3_4_*` and `USB_IN_*`
  stay 0 (nothing feeds them). Live III: no meter CCs at all during playback.
- **Live III**: main out is app channels 1-2 as on the XL; the Sampler lists Input 1,2 then USB
  Input 1,2 ... (no separate built-in mic entry).
- **CPU** (guest, schedstat, 10 s): bridge 1.5% of one CPU with no page connected, 3.5-7% while a
  client is connected and the tone or metronome runs; the app 12-19%. The bridge moves 256 frames
  per wake-up (avail_min), about 170 wake-ups/s per thread.

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
`lab.py build content` (the Sample's eMMC `/content` step) is skipped on Gen 2, since nothing
mounts that partition on these models. Not tried with real content.

## Unverified

- Saving to the SD card and the recent-project list after a reboot were checked on the XL
  only; everything else above was run on both models.
- The real NVMe partition sizes, filesystems and labels (see Internal NVMe above), and whether
  the app's USB computer mode, which names the NVMe by the same platform path, works here.
