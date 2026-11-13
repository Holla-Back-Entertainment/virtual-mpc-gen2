#!/bin/bash
# Installs the built driver into the system HAL folder (removing the old one-device
# VirtualMPCXL.driver) and restarts coreaudiod. Needs sudo.
set -euo pipefail
cd "$(dirname "$0")"
SRC="../build/VirtualMPC.driver"
HAL="/Library/Audio/Plug-Ins/HAL"
DST="$HAL/VirtualMPC.driver"
[[ -d "$SRC" ]] || { echo "not built: run driver/build.sh first" >&2; exit 1; }
if [[ -d "$HAL/VirtualMPCXL.driver" ]]; then sudo rm -rf "$HAL/VirtualMPCXL.driver"; echo "removed old: $HAL/VirtualMPCXL.driver"; fi
sudo rm -rf "$DST"
sudo cp -R "$SRC" "$DST"
sudo chown -R root:wheel "$DST"
sudo killall coreaudiod   # kickstart is refused under SIP; launchd restarts it
echo "installed: $DST (coreaudiod restarted)"
