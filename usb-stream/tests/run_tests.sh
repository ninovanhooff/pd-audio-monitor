#!/usr/bin/env bash
# Host checks: pdstream.py's eval commands, decoded by the receiver's own packet code, must equal
# chain_render's output for every format, and the receiver must recover from lost packets. With pdc
# on the PATH, the eval bytecode must equal pdc's.
# Needs: cc, ffmpeg, python3 with numpy.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
codec="$here/../../shared/codec"
build="$here/build"
mkdir -p "$build"
flags="-std=c11 -O2 -Wall -Wextra -Werror -I$codec -I$here/../receiver/src"
make -s -C "$here/../host"
cc $flags -o "$build/receive_commands" "$here/receive_commands.c" "$here/../receiver/src/packet.c" \
    "$codec/format_stream.c" "$codec/format_chain.c" "$codec/ima_adpcm.c" -lm
cc $flags -o "$build/chain_render" "$codec/tests/chain_render.c" "$codec/format_chain.c" "$codec/ima_adpcm.c" -lm

ffmpeg -v error -y -f lavfi -i "aevalsrc='0.4*sin(2*PI*(100+3000*t)*t)+0.1*(random(0)-0.5)':s=44100:d=3" \
    -ac 2 -c:a pcm_s16le "$build/input.wav"

python3 - "$here" "$build" <<'PY'
import re, shutil, subprocess, sys, tempfile, wave, zlib
from pathlib import Path
import numpy as np
here, build = sys.argv[1], sys.argv[2]
sys.path.insert(0, f"{here}/../host")
import pdstream
divisors = [1, 1, 1, 2, 2, 2, 4, 4, 4, 1, 1, 1, 2, 2, 2, 4, 4, 4]
mono_formats = 9
failed = False

def check(name, ok, detail):
    global failed
    failed |= not ok
    print(f"{'PASS' if ok else 'FAIL'} {name} ({detail})")

def commands(data):
    out = []
    while data:
        match = re.match(rb"eval (\d+)\n", data)
        end = match.end() + int(match.group(1))
        out.append(data[:end])
        data = data[end:]
    return out

def pdc_chunk(packet_text):
    with tempfile.TemporaryDirectory() as tmp:
        Path(tmp, "src").mkdir()
        Path(tmp, "src/main.lua").write_text(f'pdstream_feed("{packet_text}")\n')
        subprocess.run(["pdc", "-s", f"{tmp}/src", f"{tmp}/out.pdx"], check=True, capture_output=True)
        pdz = Path(tmp, "out.pdx/main.pdz").read_bytes()
    # One compressed entry: flags and length, name padded to 4 bytes, uncompressed size, zlib data.
    name_end = pdz.index(b"\0", 20)
    return zlib.decompress(pdz[((name_end + 4) & ~3) + 4:])

def receive(commands):
    run = subprocess.run([f"{build}/receive_commands"], input=b"".join(commands), capture_output=True, check=True)
    return np.frombuffer(run.stdout, "<i2"), run.stderr.decode().strip()

def render(fmt, samples):
    run = subprocess.run([f"{build}/chain_render", str(fmt)], input=samples.astype("<i2").tobytes(),
                         capture_output=True, check=True)
    return np.frombuffer(run.stdout, "<i2")

# pdstream mixes stereo to mono as (left + right) // 2; ffmpeg's -ac 1 averages with rounding, so
# compare against the mix pdstream makes. Stereo formats render the interleaved left and right.
with wave.open(f"{build}/input.wav") as wav:
    stereo = np.frombuffer(wav.readframes(wav.getnframes()), "<i2").reshape(-1, 2).astype(np.int32)
mono = (stereo.sum(axis=1) // 2).astype("<i2")
interleaved = stereo.astype("<i2").reshape(-1)

# PortAudio reuses the buffer it hands the capture callback, so queued frames must be copies.
capture = np.zeros((pdstream.BLOCK, 2), np.int16)
check("captured frames are copied", not np.shares_memory(pdstream.to_frames(capture), capture), "")

if shutil.which("pdc"):
    for length in (1, 40, 41, 300, pdstream.PACKET_BYTES):
        text = "".join(chr(48 + i % 10) for i in range(length))
        command = pdstream.eval_command(text.encode())
        check(f"eval bytecode for {length} bytes equals pdc's", command.split(b"\n", 1)[1] == pdc_chunk(text), "")
else:
    print("SKIP eval bytecode against pdc (pdc not on the PATH)")

for fmt, divisor in enumerate(divisors):
    name = f"format {fmt}"
    path = f"{build}/format{fmt}.bin"
    subprocess.run([sys.executable, f"{here}/../host/pdstream.py", "--format", str(fmt),
                    "--input", f"{build}/input.wav", "--out", path], check=True)
    sent = commands(open(path, "rb").read())

    channels = 1 if fmt < mono_formats else 2
    ref = render(fmt, mono if channels == 1 else interleaved)
    out, counts = receive(sent)
    delay = (divisor - 1) * channels
    n = min(len(out), len(ref) - delay)
    check(f"{name}: received audio equals chain_render", n > 44100 * 2.9 * channels and
          np.array_equal(out[:n], ref[delay:delay + n]), f"{n} samples, {counts}")

    # Drop two packets mid-stream: the receiver counts them and resumes at the next unit, after
    # which its output equals the loss-free decode checked above.
    dropped, counts = receive(sent[:len(sent) // 4] + sent[len(sent) // 4 + 2:])
    tail = 20000 * channels
    ok = "lost 2" in counts and len(dropped) > tail and np.array_equal(dropped[-tail:], out[-tail:])
    check(f"{name}: recovers from 2 lost packets", ok, f"{counts}, {len(out) - len(dropped)} samples skipped")
sys.exit(1 if failed else 0)
PY
