#!/bin/bash
# Builds mac/build/VirtualMPCXL.driver (AudioServerPlugIn), universal, ad-hoc signed.
# Usage: driver/build.sh [test]   ("test" also builds and runs the offline self-check)
set -euo pipefail
cd "$(dirname "$0")"
OUT="../build/VirtualMPCXL.driver"
CFLAGS=(-std=c11 -O2 -Wall -Wextra -Werror -mmacosx-version-min=11.0 -fvisibility=hidden)
rm -rf "$OUT"
mkdir -p "$OUT/Contents/MacOS"
xcrun clang "${CFLAGS[@]}" -arch arm64 -arch x86_64 -bundle \
    -framework CoreAudio -framework CoreFoundation \
    VirtualMPCXL.c -o "$OUT/Contents/MacOS/VirtualMPCXL"
cp Info.plist "$OUT/Contents/Info.plist"
plutil -lint "$OUT/Contents/Info.plist"
codesign -s - --force "$OUT"
echo "built: $(cd "$(dirname "$OUT")" && pwd)/VirtualMPCXL.driver"
if [[ "${1:-}" == "test" ]]; then
    xcrun clang "${CFLAGS[@]}" -framework CoreAudio -framework CoreFoundation selftest.c -o ../build/vmpcxl-selftest
    ../build/vmpcxl-selftest
fi
