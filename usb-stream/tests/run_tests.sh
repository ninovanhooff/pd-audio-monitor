#!/usr/bin/env bash
# Host checks: pdstream.py's eval commands, decoded by the receiver's own packet code, must equal
# chain_render's output for every format, and the receiver must recover from lost packets. With pdc
# on the PATH, the eval bytecode must equal pdc's.
# The receiver's buffer and meter arithmetic (meter.h) is checked first.
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
cc $flags -o "$build/meter_test" "$here/meter_test.c" -lm
"$build/meter_test"
cc $flags -o "$build/chain_render" "$codec/tests/chain_render.c" "$codec/format_chain.c" "$codec/ima_adpcm.c" -lm
# ADPCM-XQ is third-party code: built without -Werror.
cc -std=c11 -O2 -Wall -Wextra -c -o "$build/adpcm-lib.o" "$codec/adpcm_xq/adpcm-lib.c"
cc -std=c11 -O2 -Wall -Wextra -c -o "$build/adpcm-dns.o" "$codec/adpcm_xq/adpcm-dns.c"
cc $flags -o "$build/stream_render" "$codec/tests/stream_render.c" "$codec/format_stream.c" "$codec/format_chain.c" \
    "$codec/ima_adpcm.c" "$codec/adpcm_xq_encoder.c" "$build/adpcm-lib.o" "$build/adpcm-dns.o" -lm

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

def render_xq(fmt, samples):
    run = subprocess.run([f"{build}/stream_render", str(fmt), "xq"], input=samples.astype("<i2").tobytes(),
                         capture_output=True, check=True)
    return np.frombuffer(run.stdout, "<i2")

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

# --adpcm-xq: the host's ADPCM-XQ blocks reach the receiver as the stream encoder made them, and the greedy
# encoder stays the default.
for fmt in (2, 5, 8, 11, 14, 17):
    path = f"{build}/format{fmt}_xq.bin"
    subprocess.run([sys.executable, f"{here}/../host/pdstream.py", "--format", str(fmt), "--adpcm-xq",
                    "--input", f"{build}/input.wav", "--out", path], check=True)
    out, counts = receive(commands(open(path, "rb").read()))
    channels = 1 if fmt < mono_formats else 2
    ref = render_xq(fmt, mono if channels == 1 else interleaved)
    n = min(len(out), len(ref))
    greedy, _ = receive(commands(open(f"{build}/format{fmt}.bin", "rb").read()))
    check(f"format {fmt}: --adpcm-xq audio equals stream_render's", n > 44100 * 2.9 * channels and
          np.array_equal(out[:n], ref[:n]) and not np.array_equal(out[:n], greedy[:n]), f"{n} samples, {counts}")

lib = pdstream.load_encoder()
codec = pdstream.Codec(lib, 5)
frames = stereo.astype(np.int16)[:4410]
before = b"".join(codec.commands(frames, flush=True))
codec.adpcm_xq = True
after = b"".join(codec.commands(frames, flush=True))
check("Codec: adpcm_xq switches the encoder on the next frames", codec.encoder.adpcm_xq and before != after, "")
codec.switch(3)
check("Codec: a format without ADPCM ignores adpcm_xq", codec.encoder.adpcm_xq and bool(codec.commands(frames, flush=True)), "")

# The host's remembered settings round-trip, and a missing, corrupt or unwritable file is no error.
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp, "nested", "settings.json")
    check("settings: a missing file is empty", pdstream.load_settings(path) == {}, "")
    pdstream.save_settings({"device": "BlackHole 2ch", "format": 5}, path)
    check("settings: saved values come back", pdstream.load_settings(path) == {"device": "BlackHole 2ch", "format": 5}, "")
    path.write_text("{not json")
    check("settings: a corrupt file is empty", pdstream.load_settings(path) == {}, "")
    path.write_text("[1, 2]")
    check("settings: a non-object file is empty", pdstream.load_settings(path) == {}, "")
    path.write_text('{"magic": "pdstream-settings", "version": 0, "format": 3}')
    check("settings: another version isn't loaded", pdstream.load_settings(path) == {}, "")
    path.write_text('{"version": 1, "format": 3}')
    check("settings: a file without the magic isn't loaded", pdstream.load_settings(path) == {}, "")
    blocker = Path(tmp, "file")
    blocker.write_text("")
    try:
        pdstream.save_settings({"format": 1}, blocker / "settings.json")
        saved = True
    except OSError:
        saved = False
    check("settings: an unwritable folder is ignored", saved, "")

info = (Path(pdstream.__file__).parent.parent / "receiver/Source/pdxinfo").read_text()
check("version: the host reads the receiver's pdxinfo", f"\nversion={pdstream.app_version()}\n" in "\n" + info and re.fullmatch(r"\d+\.\d+", pdstream.app_version()), pdstream.app_version())

# The host streams only to a receiver that has spoken recently, and stops when the console complains.
now = [0.0]
watch = pdstream.ReceiverWatch(lambda: now[0])
check("watch: silent until the receiver speaks", not watch.listening(), "")
check("watch: a hello is heard", watch.hear(b"pdstream hello 2") == "hello" and watch.listening()
      and watch.version == 2, "")
now[0] = 2.9
check("watch: still listening within the timeout", watch.listening(), "")
now[0] = 3.1
check("watch: not listening after the timeout", not watch.listening(), "")
check("watch: a format request is a sign of life", watch.hear(b"pdstream format 5") == "format" and watch.listening(), "")
for noise in (b'Unknown command 0$B!b#2sC2:CPB3 (try help for a full list of commands)', b"Lua runtime is not available",
              b"help for a full list of commands)"):
    watch.hear(b"pdstream hello 2")
    check(f"watch: {noise[:30]!r} stops the stream", watch.hear(noise) == "noise" and not watch.listening(), "")
check("watch: a hello after the noise resumes it", watch.hear(b"pdstream hello 2") == "hello" and watch.listening(), "")
check("watch: other lines are ignored", watch.hear(b"some log line") is None and watch.listening(), "")
watch.reset()
check("watch: reset forgets the receiver", not watch.listening() and watch.version is None, "")

class FakeSink:
    def __init__(self):
        self.requested_format, self.heard, self.polls = None, False, 0

    def listening(self):
        return self.heard

    def poll(self):
        self.polls += 1

lib = pdstream.load_encoder()
codec, sink = pdstream.Codec(lib, 5), FakeSink()
lines = []
pdstream.console = type("Lines", (), {"log": staticmethod(lines.append)})()
check("follow_playdate: waits for a receiver", pdstream.follow_playdate(codec, sink, None, []) and sink.polls == 1, "")
check("follow_playdate: says which receiver version to run", f"version {pdstream.app_version()}" in lines[-1], lines[-1])
sink.heard = True
check("follow_playdate: streams once it has spoken", not pdstream.follow_playdate(codec, sink, None, []), "")
sink.requested_format = -1
check("follow_playdate: a pause stops it", pdstream.follow_playdate(codec, sink, None, []), "")

# A stream that restarts without a gap in the sequence (the host resumes after the Playdate's menu
# closes) starts at a unit boundary while the receiver may be partway through a unit. The restart must
# realign the decoder, with nothing counted as lost.
frames = stereo.astype(np.int16)
for fmt in range(len(divisors)):
    channels = 1 if fmt < mono_formats else 2
    codec = pdstream.Codec(lib, fmt)
    first = b"".join(codec.commands(frames[:44100]))
    codec.switch(fmt)
    second = b"".join(codec.commands(frames[44100:88200]))
    alone, _ = receive(commands(second))
    both, counts = receive(commands(first) + commands(second))
    tail = 20000 * channels
    ok = "lost 0" in counts and len(alone) > tail and np.array_equal(both[-tail:], alone[-tail:])
    check(f"format {fmt}: a restarted stream realigns the decoder", ok, counts)
sys.exit(1 if failed else 0)
PY
