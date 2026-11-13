#!/bin/bash
# Builds mac/build/VirtualMPC.driver (AudioServerPlugIn), universal, ad-hoc signed.
# Usage: driver/build.sh [test]   ("test" also builds and runs the offline self-check)
set -euo pipefail
cd "$(dirname "$0")"
OUT="../build/VirtualMPC.driver"
CFLAGS=(-std=c11 -O2 -Wall -Wextra -Werror -mmacosx-version-min=11.0 -fvisibility=hidden)
rm -rf "$OUT" ../build/VirtualMPCXL.driver ../build/vmpcxl-selftest
mkdir -p "$OUT/Contents/MacOS"
xcrun clang "${CFLAGS[@]}" -arch arm64 -arch x86_64 -bundle \
    -framework CoreAudio -framework CoreFoundation \
    VirtualMPC.c -o "$OUT/Contents/MacOS/VirtualMPC"
cp Info.plist "$OUT/Contents/Info.plist"
plutil -lint "$OUT/Contents/Info.plist"
codesign -s - --force "$OUT"
echo "built: $(cd "$(dirname "$OUT")" && pwd)/VirtualMPC.driver"
if [[ "${1:-}" == "test" ]]; then
    xcrun clang "${CFLAGS[@]}" -framework CoreAudio -framework CoreFoundation selftest.c -o ../build/vmpc-selftest
    ../build/vmpc-selftest
fi
