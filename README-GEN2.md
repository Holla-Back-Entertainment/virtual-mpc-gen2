# Virtual MPC Gen 2 (MPC XL / MPC Live III)

A QEMU lab that boots the stock Akai MPC Gen 2 firmware 3.10.0 (rootfs verified block by
block with its own dm-verity table) on a virtual arm64 machine, posing as an MPC XL
(`MODEL=acvc`, default) or an MPC Live III (`MODEL=acvg`). It is a fork of the MPC Sample
lab (`README.md` describes that one; most of `docs/` is still about the Sample). Use it to
try mods safely: nothing here writes to a real device.

No firmware is in this repository. `firmware/extracted/{part1,part2,parr}` come from your own
update image and are checked against `inputs.sha256`.

## Quick start

```bash
python3 lab.py build                  # Docker: kernel, bridge, disk images (once)
python3 lab.py run --headless         # VM + panel page; Ctrl-C shuts the MPC down cleanly
python3 lab.py ssh 'systemctl status acvs'
python3 lab.py stop                   # power key + SHUT DOWN, wait for power-off
```

First boot: the app's welcome wizard (NEXT, GOT IT, LET'S GO) appears on every boot,
because the app keeps `StartupWizard.Enabled=1` in `/data/Settings/MPC/MPC.settings`; it is
not a sign of an unclean shutdown.

Screen taps: `python3 scripts/tap.py X Y` (1280x800). Screenshot:
`python3 scripts/qmp.py screendump out/shots/x.png`. Buttons: `POST /control
{"name":"PLAY","action":"press"|"release"}` on the panel server, `GET /surface` lists them.

## Ports

All on 127.0.0.1. `MPCLAB_PORT_OFFSET=N` adds N to every port, so several copies of the lab
can run at once (for example `MPCLAB_PORT_OFFSET=40` puts the page on 8905).

| name | port | what |
|---|---|---|
| panel | 8865 | panel page (screen, controls, sound) |
| ssh | 2122 | root shell (`lab.py ssh`) |
| usb_ssh | 2322 | the device's USB network address 192.168.155.1:2222 |
| qmp / qmp_panel | 5654 / 5655 | QEMU monitor (lab.py / panel server) |
| cs | 5651 | virtual USB control surface (guest relay <-> panel) |
| ext_midi | 5652 | rear MIDI port (raw MIDI bytes) |
| console | 5653 | guest console (also `out/run/console.log`) |
| audio / pcm | 5656 / 5657 | audio bridge / PCM WebSocket for the page |
| vnc | 6010 | the screen |

## What works (verified on the XL, 13 Nov 2026)

- **Clean shutdown.** The app reads the power key itself (gpio-keys,
  `/dev/input/by-path/platform-gpio-keys-event`; the stock `az01-pwrbtn` daemon on the same
  device is only the emergency path). QMP `system_powerdown` makes the app ask "Do you want to
  shut down your MPC XL?"; `lab.py` taps SHUT DOWN at (532, 770) and waits for power-off
  (about 5 s). `lab.py stop` and Ctrl-C in `lab.py run` both do this; after 60 s without
  power-off they quit QEMU and say so. Next boot has no "not properly shut down" dialog, and
  a project saved before the shutdown is listed under OPEN A RECENT PROJECT.
- **`lab.py preload LIB.so | --off`.** Adds a `/run` drop-in with `LD_PRELOAD` to
  `acvs.service` and restarts it; checks that the library is in `/proc/<pid>/maps` of `MPC`.
  Test library: `tools/preload-hello.c` (built by `lab.py build tools` into
  `share/bin/libpreload-hello.so`) logs from its constructor to the journal and to
  `/run/mpclab-preload/hello.log`. Note that `az01-launch-MPC` is a shell script, so the
  library is also loaded into it, `dfu-util`, `grep`, `setarch` and the app's child processes
  (crashpad). The restart stops the app with SIGTERM; no unclean-shutdown dialog followed.
- **SD card.** Files in `sdcard/` become a FAT16 card (label `MPC XL`, `MPC LIVE3` for
  acvg) at the next `run` when `out/run/sdcard.img` does not exist yet (delete it to rebuild).
  It mounts at `/media/MPC XL`, shows under Places in the browser, and the app can save
  projects to it.
- **Boot splash**: the stock `mpc3.png` from the FIT's initramfs, shown until the app starts.
- `az0x-info` and `az0x-loadpin-trustpoints` are skipped by lab drop-ins
  (`overlay/lab-layer/etc/systemd/system/*.service.d/mpclab.conf` say why); no failed units.

## What does not work

- **SD autorun.** MPC 3.10 on Gen 2 lists `autorun/` on a card for `*.scriptpkg` files only
  and checks each with `az01-signed-fs` against `/usr/lib/az0x/pubkeys/akai-1.pub`. A plain
  `autorun/autorun.json` + `autorun.sh` (+ `autorun.check`) package, as used on the Sample,
  is never read; an unsigned `.scriptpkg` is rejected ("invalid image file: bad header").
  `lab.py autorun` still builds and inserts the card and reports what the app logged, but
  only an inMusic-signed package would run. `--yes` taps the left bottom button: unverified,
  since no script question has been seen.
- **User storage is read-only.** The XL keeps projects on an internal NVMe drive (`/nvme`,
  device tree `chosen/inmusic,nvme-fitted`); the lab has no NVMe, so saving to "User" fails
  with "READ ONLY FILE SYSTEM". Save to the SD card instead.
- `/content` (factory library) is empty; the content build step was not tried on Gen 2.
- `tools/flow_test.py` and `tools/stock_walk.py` still drive the MPC Sample.

## Unverified

- `MODEL=acvg` (Live III): selftest passes; the shutdown button position and the rest of this
  page were checked on the XL only.
