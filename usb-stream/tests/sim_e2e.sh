#!/usr/bin/env bash
# Headless Linux check of the receiver app: pdstream.py records eval commands for a test signal, the
# Simulator replays them through the receiver (replay.bin in the app's data folder), and what it
# plays must match chain_render. The Simulator has no serial input, so USB itself isn't covered.
# Needs: PLAYDATE_SDK_PATH, a built usb-stream/receiver/USBStream.pdx, Xvfb, xdotool, ImageMagick
# (import), pulseaudio + pactl + parec, ffmpeg, python3 with numpy.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
pdx="$here/../receiver/USBStream.pdx"
sdk="${PLAYDATE_SDK_PATH:?set PLAYDATE_SDK_PATH}"
work="${TMPDIR:-/tmp}/usb-stream-e2e"
data="$sdk/Disk/Data/nl.ninovanhooff.usbstream"
seconds=10
mkdir -p "$work" "$data" "$HOME/.config/Playdate Simulator"

make -s -C "$here/../host"
codec="$here/../../shared/codec"
cc -std=c11 -O2 -o "$work/chain_render" "$codec/tests/chain_render.c" "$codec/format_chain.c" "$codec/ima_adpcm.c" -lm

ini="$HOME/.config/Playdate Simulator/Playdate Simulator.ini"
grep -q ShowPerfWarning "$ini" 2>/dev/null || printf 'ShowPerfWarning=0\nShowElist=0\n' >> "$ini"
pactl info >/dev/null 2>&1 || pulseaudio --start --exit-idle-time=-1 -n \
    --load="module-null-sink sink_name=out" --load="module-native-protocol-unix"
pactl set-default-sink out

# Sweep up to 3.4 kHz: nearer an 11 kHz format's Nyquist, the Simulator's output path bends the interpolation images.
ffmpeg -v error -y -f lavfi -i "aevalsrc='0.25*sin(2*PI*(200+200*t)*t)':s=44100:d=8" -c:a pcm_s16le "$work/input.wav"
ffmpeg -v error -y -i "$work/input.wav" -f s16le "$work/input.raw"

# run <format>: replays the format's commands, records the output and a screenshot.
run() {
    local format=$1
    python3 "$here/../host/pdstream.py" --format "$format" --input "$work/input.wav" --out "$data/replay.bin"
    "$work/chain_render" "$format" < "$work/input.raw" > "$work/ref$format.raw"
    Xvfb :97 -screen 0 1024x768x24 >/dev/null 2>&1 & local xvfb=$!
    export DISPLAY=:97
    sleep 1
    timeout "$seconds" parec -d out.monitor --format=s16le --rate=44100 --channels=1 > "$work/out$format.raw" &
    timeout "$seconds" "$sdk/bin/PlaydateSimulator" "$pdx" >/dev/null 2>&1 &
    sleep 3
    xdotool mousemove 725 178 mousedown 1; sleep 0.15; xdotool mouseup 1  # simulator volume to max
    sleep 2
    import -window root "$work/screen$format.png"
    sleep $((seconds - 5))
    kill $xvfb 2>/dev/null || true
    wait 2>/dev/null || true
}

run 5
run 7
rm -f "$data/replay.bin"

python3 - "$work" <<'PY'
import sys, numpy as np
work = sys.argv[1]
rate, window = 44100, 11025
failed = False
for fmt in (5, 7):
    rec = np.fromfile(f"{work}/out{fmt}.raw", "<i2").astype(float)
    ref = np.fromfile(f"{work}/ref{fmt}.raw", "<i2").astype(float)
    # Align a stretch of the recording within the reference, then compare 1 s windows.
    nonzero = np.flatnonzero(np.abs(rec) > 100)
    if len(nonzero) == 0:
        print(f"FAIL format {fmt}: recording is silent")
        failed = True
        continue
    first = nonzero[0] + rate
    seg = rec[first:first + rate]
    n = len(ref) + len(seg)
    corr = np.fft.irfft(np.fft.rfft(ref, n) * np.conj(np.fft.rfft(seg, n)), n)[: len(ref)]
    offset = int(np.argmax(corr)) - first
    worst = -999.0
    for start in range(first, first + 4 * rate, window):
        r = rec[start:start + window]
        x = ref[start + offset:start + offset + window]
        if len(r) < window or len(x) < window or not r.any() or not x.any():
            worst = 0.0
            break
        g = (x @ r) / (x @ x)
        worst = max(worst, 10 * np.log10(((r - g * x) @ (r - g * x)) / (r @ r)))
    ok = worst < -60
    failed |= not ok
    print(f"{'PASS' if ok else 'FAIL'} format {fmt}: worst residual vs chain_render {worst:.1f} dB over 4 s")
sys.exit(1 if failed else 0)
PY
