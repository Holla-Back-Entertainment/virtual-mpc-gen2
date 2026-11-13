#!/bin/bash
# Removes the driver from the system HAL folder and restarts coreaudiod. Needs sudo.
set -euo pipefail
DST="/Library/Audio/Plug-Ins/HAL/VirtualMPCXL.driver"
if [[ -d "$DST" ]]; then sudo rm -rf "$DST"; echo "removed: $DST"; else echo "not installed"; fi
sudo launchctl kickstart -k system/com.apple.audio.coreaudiod
