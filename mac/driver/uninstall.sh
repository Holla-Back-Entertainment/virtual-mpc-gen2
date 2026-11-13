#!/bin/bash
# Removes the driver (and the old VirtualMPCXL.driver) from the system HAL folder and restarts
# coreaudiod. Needs sudo.
set -euo pipefail
HAL="/Library/Audio/Plug-Ins/HAL"
for d in VirtualMPC.driver VirtualMPCXL.driver; do
    if [[ -d "$HAL/$d" ]]; then sudo rm -rf "$HAL/$d"; echo "removed: $HAL/$d"; fi
done
sudo killall coreaudiod   # kickstart is refused under SIP; launchd restarts it
