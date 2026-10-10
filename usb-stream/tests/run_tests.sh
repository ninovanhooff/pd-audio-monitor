#!/usr/bin/env bash
# Host checks: pdstream.py's msg lines, decoded by the receiver's own packet code, must equal
# chain_render's output for every format, and the receiver must recover from lost messages.
# Needs: cc, ffmpeg, python3 with numpy.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
codec="$here/../../shared/codec"
build="$here/build"
mkdir -p "$build"
flags="-std=c11 -O2 -Wall -Wextra -Werror -I$codec -I$here/../receiver/src"
make -s -C "$here/../host"
cc $flags -o "$build/receive_lines" "$here/receive_lines.c" "$here/../receiver/src/packet.c" \
    "$codec/format_stream.c" "$codec/format_chain.c" "$codec/ima_adpcm.c" -lm
cc $flags -o "$build/chain_render" "$codec/tests/chain_render.c" "$codec/format_chain.c" "$codec/ima_adpcm.c" -lm

ffmpeg -v error -y -f lavfi -i "aevalsrc='0.4*sin(2*PI*(100+3000*t)*t)+0.1*(random(0)-0.5)':s=44100:d=3" \
    -ac 2 -c:a pcm_s16le "$build/input.wav"

python3 - "$here" "$build" <<'PY'
import subprocess, sys, wave
import numpy as np
here, build = sys.argv[1], sys.argv[2]
divisors = [1, 1, 2, 4, 2, 2, 4]
failed = False

def check(name, ok, detail):
    global failed
    failed |= not ok
    print(f"{'PASS' if ok else 'FAIL'} {name} ({detail})")

def receive(lines):
    run = subprocess.run([f"{build}/receive_lines"], input=b"".join(lines), capture_output=True, check=True)
    return np.frombuffer(run.stdout, "<i2"), run.stderr.decode().strip()

# pdstream mixes stereo to mono as (left + right) // 2; ffmpeg's -ac 1 averages with rounding, so
# compare against the mix pdstream makes.
with wave.open(f"{build}/input.wav") as wav:
    stereo = np.frombuffer(wav.readframes(wav.getnframes()), "<i2").reshape(-1, 2).astype(np.int32)
mono = (stereo.sum(axis=1) // 2).astype("<i2")

for fmt, divisor in enumerate(divisors):
    lines_path = f"{build}/format{fmt}.txt"
    subprocess.run([sys.executable, f"{here}/../host/pdstream.py", "--format", str(fmt),
                    "--input", f"{build}/input.wav", "--out", lines_path], check=True)
    lines = open(lines_path, "rb").readlines()
    longest = max(len(line) - 1 for line in lines)
    check(f"format {fmt}: lines fit a device msg", longest <= 248, f"{len(lines)} lines, longest {longest} chars")

    ref = np.frombuffer(subprocess.run([f"{build}/chain_render", str(fmt)], input=mono.tobytes(),
                                       capture_output=True, check=True).stdout, "<i2")
    out, counts = receive(lines)
    delay = divisor - 1
    n = min(len(out), len(ref) - delay)
    check(f"format {fmt}: received audio equals chain_render", n > 44100 * 2.9 and
          np.array_equal(out[:n], ref[delay:delay + n]), f"{n} samples, {counts}")

    # Drop two messages mid-stream: the receiver counts them and resumes at the next unit, after
    # which its output equals the loss-free decode checked above.
    dropped, counts = receive(lines[:40] + lines[42:])
    tail = 20000
    ok = "lost 2" in counts and len(dropped) > tail and np.array_equal(dropped[-tail:], out[-tail:])
    check(f"format {fmt}: recovers from 2 lost messages", ok, f"{counts}, {len(out) - len(dropped)} samples skipped")
sys.exit(1 if failed else 0)
PY
