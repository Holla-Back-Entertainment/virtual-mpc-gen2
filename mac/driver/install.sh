#!/bin/bash
# Installs the built driver into the system HAL folder and restarts coreaudiod. Needs sudo.
set -euo pipefail
cd "$(dirname "$0")"
SRC="../build/VirtualMPCXL.driver"
DST="/Library/Audio/Plug-Ins/HAL/VirtualMPCXL.driver"
[[ -d "$SRC" ]] || { echo "not built: run driver/build.sh first" >&2; exit 1; }
sudo rm -rf "$DST"
sudo cp -R "$SRC" "$DST"
sudo chown -R root:wheel "$DST"
sudo killall coreaudiod   # kickstart is refused under SIP; launchd restarts it
echo "installed: $DST (coreaudiod restarted)"
