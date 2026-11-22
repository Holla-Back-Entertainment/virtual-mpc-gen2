# Virtual MPC on Windows

The same lab as on the Mac (stock MPC Gen 2 3.10.1 in QEMU, the panel in your browser), run from
Windows. `windows\vmpc.cmd` is the front end; it calls `lab.py`, which does the work on every OS.

**Status (22 Nov 2026).** The Windows host side is new and has not booted MPC on a Windows PC yet.
What has been checked, without a Windows PC or firmware:
- a Windows-style checkout (symlinks as plain files, Docker Desktop's 0777 file modes) now builds
  the same lab layer as a macOS/Linux checkout (compared file by file in Docker);
- QEMU 10.2 takes lab.py's whole VM command line for both models (`windows/smoke_qemu.py`, stand-in
  images, on Linux);
- the self-tests, including the MIDI bridge's parser and TCP plumbing.

`.github/workflows/windows.yml` runs the same checks plus QEMU for Windows and WinMM on GitHub's
Windows machines. If you boot it on Windows, please report what worked (README.md, Testers).

## What you need

- Windows 10 or 11, x64. 16 GB RAM is comfortable: the VM takes 4 GB (`MPCLAB_MEM`).
- Python 3.9+, QEMU 9+ for Windows, the OpenSSH client (built into Windows 10/11), and Docker
  Desktop with Linux containers for the one-time build. `vmpc setup` checks all of these, and
  `vmpc setup --install` installs the missing ones with winget.
- Your own MPC Gen 2 3.10.1 update image (inMusic Software Center or your unit's update). No
  firmware is in this repository.

**Speed.** Windows can't accelerate an arm64 VM on an x64 PC: Hyper-V (WHPX) only runs x64
guests. The VM runs under QEMU's software emulation (TCG), as on an Intel Mac or x86 Linux, so
boot and project loads take longer than on Apple Silicon, and heavy projects may click. The audio
buffer is already the lab's larger one (2048 frames). On **Windows on ARM**, `vmpc start xl --accel whpx`
is worth a try if your QEMU build supports WHPX there. It is untested.

## Set up (once)

From a terminal (Command Prompt or PowerShell) in the repository folder:

```bat
windows\vmpc setup --install
windows\vmpc extract C:\path\to\MPC-3.10.1-Gen2-update.img
windows\vmpc build
```

- Open a new terminal after `--install` (PATH changes) and start Docker Desktop once.
- `extract` checks the image against `inputs.sha256` and writes `firmware\extracted\`.
- `build` makes the kernel, bridge and disk images in Docker. Expect 20-40 minutes on the first run (the kernel build).
  Docker is not needed after that.
- A plain `git clone` is fine. Git for Windows checks out the repository's symlinks as small files,
  and `build` turns them back into symlinks inside the VM's layer.

## Use

Double-click **Start MPC XL.cmd** or **Start MPC Live III.cmd** in this folder, or:

```bat
windows\vmpc start xl          :: MPC XL,       http://127.0.0.1:8865/ opens in your browser
windows\vmpc start live3       :: MPC Live III, http://127.0.0.1:8875/
windows\vmpc firstrun xl       :: tap past the welcome screens (in a second terminal)
windows\vmpc stop xl           :: clean shutdown; Ctrl-C once in the start window does the same
windows\vmpc status            :: what runs, on which ports
windows\vmpc ssh xl            :: root shell in the VM
```

`vmpc preload`, `content`, `autorun` and `reset` take the same arguments as `lab.py` (README-GEN2.md).
To copy files into the VM, use `scp` with the lab's key:
`scp -P 2122 -i %USERPROFILE%\.ssh\mpclab_ed25519 FILE root@127.0.0.1:/nvme/` (Live III: port 2132).
`vmpc ssh` writes that key the first time.

## Sound and MIDI

- **Sound** plays in the browser: press **Sound** on the panel page. **In** picks the test tone or
  your microphone. To take the MPC into a DAW, one option (untested) is a virtual audio cable such as
  VB-CABLE. Set the browser's output to it in Settings > System > Sound > Volume mixer, and the
  DAW's input to the cable.
- **MIDI**: Windows can't create virtual MIDI ports without a driver, so create loopback ports once
  in [loopMIDI](https://www.tobias-erichsen.de/software/loopmidi.html), with these exact names, then
  run `windows\vmpc midi xl` (or `live3`) while the VM runs. `windows\vmpc midi xl --list` shows
  which ports exist.

| loopMIDI port | Your DAW | MPC port | XL / Live III lab port |
|---|---|---|---|
| Virtual MPC XL | sends to it | External MIDI in | 5652 / 5662 |
| Virtual MPC XL Ext Out | receives from it | External MIDI out | 5652 / 5662 |
| Virtual MPC XL MIDI In 1, In 2 | sends to them | rear MIDI In 1, 2 | 5751-5752 / 5761-5762 |
| Virtual MPC XL MIDI Out A ... Out D | receives from them | rear MIDI Out A-D (Live III: A, B) | 5753-5756 / 5763-5764 |

For the Live III, put `Live III` in place of `XL` in each name (for example `Virtual MPC Live III MIDI Out A`).
A loopback port carries one direction for two apps, so External MIDI, one port both ways on the
device, needs two ports here. `MPCLAB_PORT_OFFSET` moves the lab ports as for `lab.py`.

## Differences from macOS

- No `share\` folder in the VM (`/lab`): QEMU for Windows has no 9p share. The lab masks its
  mount at boot on Windows so it isn't a failed unit. Use `scp` or `vmpc content` instead.
- No Core Audio driver or Live templates (`mac/`). Sound goes through the browser; MIDI goes through loopMIDI (above).
- `lab.py content` uses Python's tar on Windows, so copied content gets the device's root-owned
  0755/0644 modes instead of Windows' 0666/0777.

## Trouble

- `vmpc setup` says Docker is in Windows-containers mode: Docker Desktop's tray menu > *Switch to Linux containers*.
- `vmpc setup` says the QEMU build lacks a device: install a current QEMU for Windows (9 or newer).
- Nothing on the page for minutes: under TCG, first boot is slow. `out\run-acvc\console.log` shows how far boot got.
- The other route is to run the whole lab inside WSL2 Ubuntu (`python3 lab.py ...`, Linux QEMU and
  Docker there). That is the Linux path, also untested by this project. WSL2 forwards `127.0.0.1`
  ports to Windows, so the browser on Windows still reaches the panel.
