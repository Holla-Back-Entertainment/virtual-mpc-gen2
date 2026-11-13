#!/bin/sh
# Runs inside the lab container. Builds out/kernel/Image from mainline 6.12.y.
set -eu
VER=${KERNEL_VERSION:-6.12.60}
SRC=/build/linux-$VER
OUT=/lab/out/kernel
mkdir -p /build "$OUT"

if [ ! -d "$SRC" ]; then
    curl -fsSL "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$VER.tar.xz" | tar -xJ -C /build
fi

cd "$SRC"
# .lab-patches lists the patches applied to this tree (the reverse dry-run alone misjudges a
# patch once a later one has changed its context, e.g. 0006 under 0007, and applies it twice)
touch .lab-patches
for p in /lab/kernel/patches/*.patch; do
    n=${p##*/}
    if grep -qx "$n" .lab-patches; then
        echo "already applied: $n"
    elif patch -p1 -R -s -f --dry-run < "$p" >/dev/null 2>&1; then
        echo "$n" >> .lab-patches; echo "already applied: $n"
    else
        patch -p1 -s < "$p"; echo "$n" >> .lab-patches; echo "applied: $n"
    fi
done
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- defconfig
scripts/kconfig/merge_config.sh -m .config /lab/kernel/lab.config
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig

# Fail loudly if a required option did not survive olddefconfig (missing dependency).
grep -E '^CONFIG_[A-Z0-9_]+=y' /lab/kernel/lab.config | while read -r want; do
    grep -qx "$want" .config || { echo "MISSING: $want"; echo 1 > /tmp/missing; }
done
[ -f /tmp/missing ] && { rm -f /tmp/missing; exit 1; }

make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j"$(nproc)" Image
cp arch/arm64/boot/Image .config "$OUT/"
echo "built $(file -b "$OUT/Image")"
