#!/bin/bash

# Builds a Playdate app and installs it on a connected Playdate over USB, copying only the files
# needed on-device (skipping the simulator-only pdex.dylib) and only the ones that changed.
#
# Usage: scripts/quickinstall.sh [-i <app dir>]
# Without -i, the app is the one containing the working directory, or the only one below it.
# The app dir is the folder holding the Makefile and Source/pdxinfo, e.g. line-in-monitor.

set -e

usage() {
  echo "usage: $0 [-i <app dir>]" >&2
  exit 1
}

is_app() {
  [ -f "$1/Makefile" ] && [ -f "$1/Source/pdxinfo" ]
}

APP_DIR=""
while getopts "i:h" opt; do
  case "$opt" in
    i) APP_DIR="$OPTARG" ;;
    *) usage ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

if [ -n "$APP_DIR" ]; then
  if ! is_app "$APP_DIR" && is_app "$REPO_DIR/$APP_DIR"; then
    APP_DIR="$REPO_DIR/$APP_DIR"
  fi
else
  dir="$PWD"
  while [ "$dir" != "/" ] && ! is_app "$dir"; do
    dir="$(dirname "$dir")"
  done
  if is_app "$dir"; then
    APP_DIR="$dir"
  else
    candidates="$(find "$PWD" -maxdepth 4 -path '*/Source/pdxinfo' -not -path '*.pdx/*' | sed 's|/Source/pdxinfo$||')"
    if [ "$(echo "$candidates" | grep -c .)" = "1" ]; then
      APP_DIR="$candidates"
    else
      echo "No single Playdate app found from $PWD; pass one with -i:" >&2
      echo "$candidates" | sed "s|^$REPO_DIR/|  |" >&2
      exit 1
    fi
  fi
fi
is_app "$APP_DIR" || { echo "$APP_DIR is not a Playdate app (needs Makefile and Source/pdxinfo)" >&2; exit 1; }
APP_DIR="$(cd "$APP_DIR" && pwd)"

PLAYDATE_SDK_PATH="${PLAYDATE_SDK_PATH:-$HOME/Developer/PlaydateSDK}"
export PLAYDATE_SDK_PATH
export PATH="$PLAYDATE_SDK_PATH/bin:$PATH"

PDX="$(sed -nE 's/^PRODUCT[[:space:]]*=[[:space:]]*([^[:space:]]+).*/\1/p' "$APP_DIR/Makefile" | head -n 1)"
[ -n "$PDX" ] || { echo "no PRODUCT in $APP_DIR/Makefile" >&2; exit 1; }

# Starts the reboot in the background so the Data Disk mounts while we build, even if the Playdate
# isn't connected yet; the wait below picks up its result.
"$SCRIPT_DIR/reboot_datadisk.sh" &
REBOOT_PID=$!

echo "Building $APP_DIR ..."
make -C "$APP_DIR"

wait "$REBOOT_PID" || { echo "Couldn't put the Playdate into data disk mode" >&2; exit 1; }

echo "Waiting for Data Disk to be mounted ... "
until [ -d /Volumes/PLAYDATE/Games ]
do
  sleep 1
done
echo "Game Dir mounted"

# Only copy files which changed and are newer than the destination, excluding ._*, which macOS
# generates on the target volume: deleting them is slow and they'd be regenerated anyway.
rsync -zavrti --update --modify-window=1 --prune-empty-dirs --delete \
  --exclude='._*' --exclude='*.dylib' --exclude='*.dSYM' \
  "$APP_DIR/$PDX" "/Volumes/PLAYDATE/Games/"

# Unmount so the device drops back into USB command mode.
MOUNT_DEVICE="$(diskutil list | grep PLAYDATE | grep -oE '[^ ]+$' | head -n 1)"
diskutil unmount "$MOUNT_DEVICE"
diskutil eject PLAYDATE

echo "Waiting for USB Device to be mounted ... "
PDUTIL_DEVICE=""
until [ -e "$PDUTIL_DEVICE" ]
do
  sleep 1
  PDUTIL_DEVICE="$(ls /dev/cu.usbmodemPD* 2>/dev/null | head -n 1)"
done
echo "Usb Device Connected: $PDUTIL_DEVICE"

echo "Running $PDX"
pdutil "$PDUTIL_DEVICE" run "/Games/$PDX"
