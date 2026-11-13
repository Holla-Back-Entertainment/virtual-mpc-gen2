#!/usr/bin/env python3
"""Extract boot/recovery/rootfs from an inMusic AZ0x update image (MPC Gen 2, 3.10.0).

Usage: python3 tools/extract_update.py Update.img [out_dir]

Header (little endian): strtab at u32@0x10, device table at u32@0x14, records at u32@0x18,
records end at u32@0x1c. Device entry: u32 usb id, u32 name. Record (64 bytes):
tag[4] u32 flags, u64 offset, u64 size, u32 name, u32 device mask, sha256[32].
BOOT records are per-device bootloaders (skipped); PARR/PART payloads are xz and the
sha256 covers the compressed bytes. Read-only on the input.
"""
import hashlib, lzma, os, struct, sys

# same names the Sample lab uses, so build-images.sh mounts stay the same
OUT_NAMES = {"boot": "part1", "rootfs": "part2", "recovery": "parr"}


def parse(img):
    if img[:4] != b"AZ0x":
        sys.exit("not an AZ0x image")
    st, dev, rec, rec_end = struct.unpack_from("<4I", img, 0x10)

    def s(off):
        return img[st + off:img.index(b"\0", st + off)].decode()

    devices = [(hex(i), s(n)) for i, n in struct.iter_unpack("<II", img[dev:rec])]
    records = []
    for o in range(rec, rec_end, 64):
        tag, flags, off, size, name, mask = struct.unpack_from("<4sIQQII", img, o)
        if hashlib.sha256(img[off:off + size]).digest() != img[o + 32:o + 64]:
            sys.exit(f"record at {o:#x}: hash mismatch")
        records.append((tag.decode(), s(name) if tag != b"BOOT" else f"boot-mask{mask:x}", off, size))
    return devices, records


def main():
    img = open(sys.argv[1], "rb").read()
    out = sys.argv[2] if len(sys.argv) > 2 else "firmware/extracted"
    devices, records = parse(img)
    print("devices:", ", ".join(f"{n} ({i})" for i, n in devices))
    os.makedirs(out, exist_ok=True)
    for tag, name, off, size in records:
        if name not in OUT_NAMES:
            continue
        with open(os.path.join(out, OUT_NAMES[name]), "wb") as f:
            f.write(lzma.decompress(img[off:off + size]))
        print(f"{OUT_NAMES[name]}: {tag} {name} ok")
    assert len([r for r in records if r[1] in OUT_NAMES]) == 3, "expected boot, rootfs, recovery"


if __name__ == "__main__":
    main()
