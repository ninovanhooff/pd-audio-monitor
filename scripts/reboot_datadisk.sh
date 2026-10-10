#!/bin/bash

# Puts a connected Playdate into data disk mode ahead of a device install (see
# scripts/quickinstall.sh). Doesn't wait for the Data Disk volume to mount, since that takes several
# seconds and the caller can keep building meanwhile.

set -e

if [ -e /Volumes/PLAYDATE ]; then
  echo "Playdate is already in data disk mode"
  exit 0
fi

PLAYDATE_SDK_PATH="${PLAYDATE_SDK_PATH:-$HOME/Developer/PlaydateSDK}"
export PATH="$PLAYDATE_SDK_PATH/bin:$PATH"

until ls /dev/cu.usbmodemPD* >/dev/null 2>&1
do
  echo "Playdate not found. Is it connected to USB and unlocked?"
  sleep 1
done
PDUTIL_DEVICE="$(ls /dev/cu.usbmodemPD* | head -n 1)"
echo "device $PDUTIL_DEVICE"
pdutil "$PDUTIL_DEVICE" datadisk
