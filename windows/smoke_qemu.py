#!/usr/bin/env python3
"""CI smoke test: does this host's QEMU take lab.py's whole VM command line?

Writes stand-in build outputs (a dummy kernel, initramfs, eMMC and lab layer: no firmware, no
Docker), runs `lab.py run --headless --no-panel`, checks over QMP that the VM is up with every
device lab.py asks for, then quits it. It boots nothing real. Run it in a fresh checkout only:
it refuses to touch an out/ that already holds a build.

    python windows/smoke_qemu.py [--model acvc|acvg]
"""
import argparse
import os
import subprocess
import sys
import time

LAB = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STANDINS = ("kernel/Image", "initramfs.cpio.gz", "emmc.base.qcow2", "lab-layer.ext4", "fw/cmdline.txt")
MARK = os.path.join("out", ".smoke-standins")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", default="acvc")
    a = ap.parse_args()
    os.environ.update(MODEL=a.model, PYTHONUTF8="1")
    sys.path.insert(0, LAB)
    sys.path.insert(0, os.path.join(LAB, "windows"))
    import lab
    from vmpc import env_with_qemu
    env = env_with_qemu(a.model)
    out = os.path.join(LAB, "out")
    if any(os.path.exists(os.path.join(out, f)) for f in STANDINS) and not os.path.exists(os.path.join(LAB, MARK)):
        sys.exit("smoke_qemu: out/ holds a real build; run this in a fresh checkout")
    os.makedirs(os.path.join(out, "kernel"), exist_ok=True)
    os.makedirs(os.path.join(out, "fw"), exist_ok=True)
    open(os.path.join(LAB, MARK), "w").close()
    with open(os.path.join(out, "kernel", "Image"), "wb") as f:
        f.write(b"\0" * 4096)                     # not a kernel: the vCPUs spin on it, which is fine
    with open(os.path.join(out, "initramfs.cpio.gz"), "wb") as f:
        f.write(b"\x1f\x8b" + b"\0" * 64)
    with open(os.path.join(out, "lab-layer.ext4"), "wb") as f:
        f.truncate(1 << 20)
    with open(os.path.join(out, "fw", "cmdline.txt"), "w") as f:
        f.write("root=/dev/dm-0 rootwait=5 console=ttyS0\n")
    qimg = lab.shutil.which("qemu-img", path=env["PATH"])
    subprocess.run([qimg, "create", "-q", "-f", "qcow2", os.path.join(out, "emmc.base.qcow2"), "64M"], check=True)
    # the SD card path too: a file in sdcard/ makes lab.py build a FAT card with QEMU's vvfat
    os.makedirs(lab.SDCARD, exist_ok=True)
    with open(os.path.join(lab.SDCARD, "smoke é.txt"), "w", encoding="utf-8") as f:
        f.write("smoke\n")

    proc = subprocess.Popen([sys.executable, os.path.join(LAB, "lab.py"), "run", "--headless", "--no-panel"], env=env, cwd=LAB)
    ports = lab.model_ports(a.model)
    deadline = time.monotonic() + 90
    while not lab.port_open(ports["qmp"]):
        if proc.poll() is not None:
            sys.exit(f"smoke_qemu: lab.py run exited with {proc.returncode} before QMP came up")
        if time.monotonic() > deadline:
            proc.kill()
            sys.exit("smoke_qemu: no QMP after 90 s")
        time.sleep(0.5)
    status = lab.qmp("query-status", ports["qmp"])["return"]["status"]
    pci = lab.qmp("query-pci", ports["qmp"])["return"]
    ids = {(d["id"]["vendor"], d["id"]["device"]) for bus in pci for d in bus["devices"]}
    # PCI ids: blk, net and serial are transitional virtio devices (legacy 0x1000-0x1003, or 0x1040 + type
    # when modern-only); gpu and input are modern-only; then QEMU's NVMe and SDHCI
    want = {"virtio-blk": {0x1001, 0x1042}, "virtio-net": {0x1000, 0x1041}, "virtio-serial": {0x1003, 0x1043},
            "virtio-gpu": {0x1050}, "virtio-input": {0x1052}}
    lacking = [k for k, v in want.items() if not any((0x1af4, d) in ids for d in v)]
    lacking += [k for k, v in {"nvme": (0x1b36, 0x0010), "sdhci": (0x1b36, 0x0007)}.items() if v not in ids]
    blocks = {b["device"]: b.get("inserted", {}).get("file", "") for b in lab.qmp("query-block", ports["qmp"])["return"]}
    try:
        lab.qmp("quit", ports["qmp"])
    except OSError:
        pass                                  # QEMU may close the socket before it answers "quit"
    rc = proc.wait(30)
    print(f"smoke_qemu: VM status {status}, {sum(len(b['devices']) for b in pci)} PCI devices, lab.py exited {rc}")
    print("  drives: " + ", ".join(f"{k}={os.path.basename(v) or '-'}" for k, v in sorted(blocks.items())))
    problems = []
    if status != "running":
        problems.append(f"VM status {status}")
    if lacking:
        problems.append("missing PCI devices: " + ", ".join(lacking))
    if not blocks.get("sdcard", "").endswith("sdcard.img"):
        problems.append("SD card image not inserted")
    if not os.path.exists(os.path.join(lab.run_dir(a.model), "console.log")):
        problems.append("no console.log")
    cmdline = open(os.path.join(lab.run_dir(a.model), "cmdline.txt")).read()
    if (sys.platform == "win32") != ("systemd.mask=lab.mount" in cmdline):
        problems.append("lab.mount mask does not match the platform")
    if problems:
        sys.exit("smoke_qemu: " + "; ".join(problems))
    print("smoke_qemu ok")


if __name__ == "__main__":
    main()
