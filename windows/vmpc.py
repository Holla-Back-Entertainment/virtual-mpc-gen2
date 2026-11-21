#!/usr/bin/env python3
"""Virtual MPC on Windows: one front end for setup, build and the lab commands.

    vmpc setup [--install]        check (or install with winget) Python, QEMU, Docker, OpenSSH
    vmpc extract UPDATE.img       unpack your own MPC Gen 2 update image into firmware/extracted
    vmpc build [STEPS...]         build the kernel, bridge and disk images in Docker (once)
    vmpc start [xl|live3]         boot the MPC, open its panel in your browser; Ctrl-C shuts it down
    vmpc midi [xl|live3]          MIDI between the MPC and your DAW (loopMIDI ports, see windows/README.md)
    vmpc stop|status|firstrun|ssh|reset|preload|content|autorun [xl|live3] [ARGS...]
                                  the same lab.py command, for that model

`vmpc.cmd` next to this file runs it with whichever Python is installed. The lab itself is
lab.py; this only finds the tools, sets MODEL and adds QEMU to PATH for it.
"""
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import webbrowser

LAB = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WIN = sys.platform == "win32"
MODEL_ARGS = {"xl": "acvc", "live3": "acvg", "liveiii": "acvg", "acvc": "acvc", "acvg": "acvg"}
QEMU_DIRS = [os.path.join(os.environ.get(v, d), "qemu") for v, d in
             (("ProgramFiles", r"C:\Program Files"), ("ProgramW6432", r"C:\Program Files"))]
WINGET = {  # check name -> (winget id, what for)
    "python": ("Python.Python.3.12", "Python 3.9 or newer"),
    "qemu": ("SoftwareFreedomConservancy.QEMU", "QEMU 9 or newer (qemu-system-aarch64, qemu-img)"),
    "docker": ("Docker.DockerDesktop", "Docker Desktop (Linux containers), for `vmpc build` only"),
    "loopmidi": ("TobiasErichsen.loopMIDI", "loopMIDI, optional: MIDI ports for your DAW"),
}
# the devices lab.py gives the VM; an older or trimmed QEMU build lacks some of them
QEMU_DEVICES = ("virtio-gpu-pci", "virtio-multitouch-pci", "virtio-keyboard-pci", "nvme", "sdhci-pci",
                "sd-card", "virtio-serial-pci", "virtio-net-pci", "virtio-blk-pci")


def env_with_qemu(model=None):
    env = dict(os.environ, PYTHONUTF8="1")
    if model:
        env["MODEL"] = model
    if not shutil.which("qemu-system-aarch64"):
        for d in QEMU_DIRS:
            if os.path.exists(os.path.join(d, "qemu-system-aarch64.exe")):
                env["PATH"] = d + os.pathsep + env.get("PATH", "")
                break
    return env


def which(name, env):
    return shutil.which(name, path=env.get("PATH"))


def run_child(cmd, env=None):
    """Run cmd and wait for it. Ctrl-C reaches the child too (same console); lab.py answers it with
    a clean MPC shutdown, so wait for it instead of killing it as subprocess.call would."""
    proc = subprocess.Popen(cmd, env=env, cwd=LAB)
    while True:
        try:
            return proc.wait()
        except KeyboardInterrupt:
            continue


def lab_cmd(args, model=None):
    return run_child([sys.executable, os.path.join(LAB, "lab.py"), *args], env_with_qemu(model))


# ----------------------------------------------------------------------------- setup
def check(env):
    """[(name, ok, detail)] for everything the lab needs on this machine."""
    out = []
    v = sys.version_info
    out.append(("python", v >= (3, 9), f"Python {v.major}.{v.minor}.{v.micro} ({sys.executable})"))

    qemu = which("qemu-system-aarch64", env)
    if not qemu:
        out.append(("qemu", False, "qemu-system-aarch64 not found (PATH or Program Files\\qemu)"))
    else:
        r = subprocess.run([qemu, "--version"], capture_output=True, text=True)
        m = re.search(r"version (\d+)\.(\d+)", r.stdout)
        ver = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
        devs = subprocess.run([qemu, "-device", "help"], capture_output=True, text=True).stdout
        lacking = [d for d in QEMU_DEVICES if f'"{d}"' not in devs]
        ok = ver >= (9, 0) and not lacking and which("qemu-img", env)
        detail = f"QEMU {ver[0]}.{ver[1]} ({qemu})"
        if ver < (9, 0):
            detail += ": 9.0 or newer needed"
        if lacking:
            detail += ": this build lacks " + ", ".join(lacking)
        if not which("qemu-img", env):
            detail += ": qemu-img not found next to it"
        out.append(("qemu", bool(ok), detail))

    docker = which("docker", env)
    if not docker:
        out.append(("docker", False, "docker not found (needed for `vmpc build` only)"))
    else:
        r = subprocess.run([docker, "info", "--format", "{{.OSType}} {{.ServerVersion}}"], capture_output=True, text=True)
        os_type = r.stdout.split()[0] if r.returncode == 0 and r.stdout.strip() else ""
        out.append(("docker", os_type == "linux",
                    f"Docker {r.stdout.split()[1]} ({os_type} containers)" if os_type else
                    "docker found, but the engine is not running: start Docker Desktop"))
        if os_type and os_type != "linux":
            out[-1] = ("docker", False, "Docker is in Windows-containers mode: switch it to Linux containers")

    missing_ssh = [t for t in ("ssh", "scp") if not which(t, env)]
    out.append(("openssh", not missing_ssh, "OpenSSH client" + (": missing " + ", ".join(missing_ssh) if missing_ssh else "")))

    fw = [p for p in ("part1", "part2", "parr") if not os.path.exists(os.path.join(LAB, "firmware", "extracted", p))]
    out.append(("firmware", not fw, "firmware/extracted: " + ("ready" if not fw else "not yet extracted (vmpc extract UPDATE.img)")))
    built = [f for f in ("kernel/Image", "initramfs.cpio.gz", "emmc.base.qcow2", "lab-layer.ext4")
             if not os.path.exists(os.path.join(LAB, "out", f))]
    out.append(("build", not built, "out/: " + ("built" if not built else "not built yet (vmpc build)")))
    return out


def cmd_setup(args):
    install = "--install" in args
    env = env_with_qemu()
    results = check(env)
    for name, ok, detail in results:
        print(f"  {'ok     ' if ok else 'MISSING'}  {detail}")
    todo = [n for n, ok, _ in results if not ok and n in WINGET]
    if any(n == "openssh" and not ok for n, ok, _ in results):
        print("\nOpenSSH client: Settings > System > Optional features > OpenSSH Client, or in an admin PowerShell:\n"
              "  Add-WindowsCapability -Online -Name OpenSSH.Client~~~~0.0.1.0")
    if todo or not install:
        print("\nwinget installs (vmpc setup --install runs the missing ones):")
        for n, (pkg, what) in WINGET.items():
            print(f"  {'*' if n in todo else ' '} winget install -e --id {pkg}    # {what}")
    if install and todo:
        if not shutil.which("winget"):
            sys.exit("vmpc: winget not found (App Installer from the Microsoft Store); install the above by hand")
        for n in todo:
            pkg, what = WINGET[n]
            print(f"\nvmpc: installing {what}")
            subprocess.call(["winget", "install", "-e", "--id", pkg, "--accept-package-agreements", "--accept-source-agreements"])
        print("\nvmpc: done. Open a new terminal (PATH changes), start Docker Desktop once, then run vmpc setup again.")
    if not WIN:
        print("\n(not Windows: on macOS or Linux use lab.py directly; see README.md)")
    return 0 if all(ok for n, ok, _ in results if n in ("python", "qemu", "openssh")) else 1


# ----------------------------------------------------------------------------- start
def open_when_up(model):
    sys.path.insert(0, LAB)
    os.environ["MODEL"] = model
    import lab
    port = lab.model_ports(model)["panel"]
    for _ in range(600):
        if lab.port_open(port):
            webbrowser.open(f"http://{lab.HOST}:{port}/")
            return
        time.sleep(0.5)


def cmd_start(model, args):
    env = env_with_qemu(model)
    if not which("qemu-system-aarch64", env):
        sys.exit("vmpc: QEMU not found; run `vmpc setup`")
    if "--no-browser" in args:
        args = [x for x in args if x != "--no-browser"]
    else:
        threading.Thread(target=open_when_up, args=(model,), daemon=True).start()
    print("vmpc: booting; the panel opens in your browser. The first boot takes a few minutes under "
          "software emulation. Ctrl-C (once) shuts the MPC down cleanly.")
    return run_child([sys.executable, os.path.join(LAB, "lab.py"), "run", "--headless", *args], env)


def main(argv):
    if not argv or argv[0] in ("-h", "--help", "help"):
        print(__doc__)
        return 0
    cmd, rest = argv[0], argv[1:]
    model = os.environ.get("MODEL", "acvc")
    if rest and rest[0].lower() in MODEL_ARGS:
        model, rest = MODEL_ARGS[rest[0].lower()], rest[1:]
    if cmd == "setup":
        return cmd_setup(rest)
    if cmd == "extract":
        if not rest:
            sys.exit("usage: vmpc extract PATH\\TO\\MPC-3.10.1-Gen2-update.img")
        return run_child([sys.executable, os.path.join(LAB, "tools", "extract_update.py"), os.path.abspath(rest[0]),
                          os.path.join(LAB, "firmware", "extracted")])
    if cmd == "build":
        return lab_cmd(["build", *rest])
    if cmd in ("start", "run"):
        return cmd_start(model, rest)
    if cmd == "midi":
        return run_child([sys.executable, os.path.join(LAB, "windows", "midi_bridge.py"), "--model", model, *rest],
                         env_with_qemu(model))
    if cmd in ("stop", "status", "firstrun", "ssh", "reset", "preload", "content", "autorun", "panel", "selftest"):
        return lab_cmd([cmd, *rest], model)
    sys.exit(f"vmpc: unknown command {cmd!r} (vmpc help)")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
