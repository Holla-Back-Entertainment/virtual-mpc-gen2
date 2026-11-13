#!/bin/bash
# Builds mac/build/Virtual MPC.app from mac/Sources/*.swift (no Xcode project), ad-hoc signed.
set -euo pipefail
cd "$(dirname "$0")"
APP="build/Virtual MPC.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
swiftc -swift-version 5 -O -target "$(uname -m)-apple-macos14.0" \
    -framework AppKit -framework WebKit -framework CoreAudio -framework AudioToolbox \
    -framework AVFoundation -framework CoreMIDI \
    Sources/*.swift -o "$APP/Contents/MacOS/VirtualMPC"
cp Info.plist "$APP/Contents/Info.plist"
codesign -s - --force --deep "$APP"
echo "built: $PWD/$APP"
