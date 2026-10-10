#!/usr/bin/env bash
# Headless Linux check: feeds a test signal into the Simulator's mic through PulseAudio, records
# what LineInMonitor plays and compares it with chain_render on the host.
# Needs: PLAYDATE_SDK_PATH, a built LineInMonitor.pdx, Xvfb, xdotool, ImageMagick (import),
# pulseaudio + pactl + parec, ffmpeg, python3 with numpy.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
pdx="$here/../LineInMonitor.pdx"
sdk="${PLAYDATE_SDK_PATH:?set PLAYDATE_SDK_PATH}"
work="${TMPDIR:-/tmp}/line-in-e2e"
seconds=12
mkdir -p "$work"

codec="$here/../../shared/codec"
cc -std=c11 -O2 -o "$work/chain_render" "$codec/tests/chain_render.c" "$codec/format_chain.c" "$codec/ima_adpcm.c" -lm

# Skip the first-run dialogs and the mic permission prompt.
mkdir -p "$sdk/Disk/Data/nl.ninovanhooff.lineinmonitor" "$HOME/.config/Playdate Simulator"
printf 'mic+\n' > "$sdk/Disk/Data/nl.ninovanhooff.lineinmonitor/Permissions"
ini="$HOME/.config/Playdate Simulator/Playdate Simulator.ini"
grep -q ShowPerfWarning "$ini" 2>/dev/null || printf 'ShowPerfWarning=0\nShowElist=0\n' >> "$ini"

pactl info >/dev/null 2>&1 || pulseaudio --start --exit-idle-time=-1 -n \
    --load="module-null-sink sink_name=out" --load="module-native-protocol-unix"
for module in $(pactl list short modules | awk '/module-pipe-source/ && /source_name=line/ {print $1}'); do
    pactl unload-module "$module"
done
pactl load-module module-pipe-source source_name=line file="$work/line.fifo" format=s16le rate=44100 channels=1 >/dev/null
pactl set-default-source line
pactl set-default-sink out

# Sweep from 200 Hz to about 4.6 kHz: above that, the decimation phase (arbitrary per run) changes the output.
signal="aevalsrc='if(lt(t,1),0,0.25*sin(2*PI*(200+200*(t-1))*(t-1)))':s=44100:d=$seconds"
ffmpeg -v error -y -f lavfi -i "$signal" -ac 1 -f s16le "$work/input.raw"

# run <name> <clicks...>: clicks are x,y in a 1024x768 Xvfb screen (simulator window at its default spot).
run() {
    local name=$1; shift
    Xvfb :97 -screen 0 1024x768x24 >/dev/null 2>&1 & local xvfb=$!
    export DISPLAY=:97
    sleep 1
    ffmpeg -v error -re -f s16le -ar 44100 -ac 1 -i "$work/input.raw" -f s16le - > "$work/line.fifo" < /dev/null &
    timeout "$seconds" parec -d out.monitor --format=s16le --rate=44100 --channels=1 > "$work/$name.raw" &
    timeout "$seconds" "$sdk/bin/PlaydateSimulator" "$pdx" >/dev/null 2>&1 &
    sleep 3
    for click in 725,178 "$@"; do  # 725,178: simulator volume to max
        xdotool mousemove "${click%,*}" "${click#*,}" mousedown 1; sleep 0.15; xdotool mouseup 1; sleep 0.4
    done
    sleep $((seconds - 3))
    kill $xvfb 2>/dev/null || true
    wait 2>/dev/null || true
}

run bypass 643,387                 # A: compare with bypass
run pcm22 410,397 410,397          # right x2: 22.05 kHz 16-bit
"$work/chain_render" 0 < "$work/input.raw" > "$work/bypass_ref.raw"
"$work/chain_render" 5 < "$work/input.raw" > "$work/pcm22_ref.raw"

python3 - "$work" <<'PY'
import sys, numpy as np
work = sys.argv[1]
rate, window = 44100, 11025
failed = False
for name in ("bypass", "pcm22"):
    rec = np.fromfile(f"{work}/{name}.raw", "<i2").astype(float)
    ref = np.fromfile(f"{work}/{name}_ref.raw", "<i2").astype(float)
    # The app starts after the input does, so align a stretch of the recording within the reference.
    seg = rec[4 * rate:5 * rate]
    n = len(ref) + len(seg)
    corr = np.fft.irfft(np.fft.rfft(ref, n) * np.conj(np.fft.rfft(seg, n)), n)[: len(ref)]
    offset = int(np.argmax(corr)) - 4 * rate
    worst = -999.0
    for start in range(4 * rate, 8 * rate, window):
        r = rec[start:start + window]
        x = ref[start + offset:start + offset + window]
        if len(r) < window or len(x) < window or not r.any() or not x.any():
            worst = 0.0
            break
        g = (x @ r) / (x @ x)
        worst = max(worst, 10 * np.log10(((r - g * x) @ (r - g * x)) / (r @ r)))
    ok = worst < -60
    failed |= not ok
    print(f"{'PASS' if ok else 'FAIL'} {name}: worst residual vs chain_render {worst:.1f} dB over 4 s")
sys.exit(1 if failed else 0)
PY
