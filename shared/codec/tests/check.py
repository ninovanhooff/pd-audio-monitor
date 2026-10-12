import hashlib
import math
import os
import random
import shutil
import struct
import subprocess
import sys
from pathlib import Path

build = Path(sys.argv[1])
RATE = 44100
failures = 0


def check(name, ok, detail=""):
    global failures
    print(("PASS " if ok else "FAIL ") + name + (f" ({detail})" if detail else ""))
    failures += not ok


def write_raw(path, samples):
    path.write_bytes(struct.pack(f"<{len(samples)}h", *samples))


def read_raw(path):
    data = path.read_bytes()
    return list(struct.unpack(f"<{len(data) // 2}h", data))


def tone(freq, seconds, amplitude=0.5, rate=RATE):
    return [int(amplitude * 32767 * math.sin(2 * math.pi * freq * i / rate)) for i in range(int(seconds * rate))]


def ffmpeg(*args):
    subprocess.run(["ffmpeg", "-v", "error", "-y", *args], check=True)


def rms(samples):
    return math.sqrt(sum(s * s for s in samples) / len(samples))


def db(ratio):
    return 20 * math.log10(max(ratio, 1e-9))


# A signal that exercises every step size: sweep, noise bursts, silence, full-scale square.
random.seed(1)
rate = 22050
spb = 1017
signal = []
for i in range(rate):
    f = 50 + 10000 * i / rate
    signal.append(int(16000 * math.sin(2 * math.pi * f * i / rate * 0.5)))
signal += [random.randint(-30000, 30000) for _ in range(rate // 4)]
signal += [0] * (rate // 4)
signal += [32767 if (i // 40) % 2 else -32768 for i in range(rate // 4)]
signal = signal[: len(signal) // spb * spb]
write_raw(build / "signal.raw", signal)


def sweep_snr(decoded):
    sweep = signal[:rate]
    return db(rms(sweep) / rms([x - y for x, y in zip(sweep, decoded)]))

# 1. Multiply rounding: bit-exact with ffmpeg's adpcm_ima_wav in both directions.
tool = [build / "adpcm_wav", "-m"]
subprocess.run([*tool, "encode", build / "signal.raw", build / "ours.wav", str(spb), str(rate)], check=True)
ffmpeg("-i", build / "ours.wav", "-f", "s16le", build / "ours_ffmpeg.raw")
subprocess.run([*tool, "decode", build / "ours.wav", build / "ours_ours.raw"], check=True)
a, b = read_raw(build / "ours_ffmpeg.raw"), read_raw(build / "ours_ours.raw")
check("multiply: our encoder -> ffmpeg decoder matches our decoder", a == b, f"{len(a)} samples")
ours_snr = sweep_snr(b)

ffmpeg("-f", "s16le", "-ar", str(rate), "-ac", "1", "-i", build / "signal.raw", "-c:a", "adpcm_ima_wav", build / "theirs.wav")
ffmpeg("-i", build / "theirs.wav", "-f", "s16le", build / "theirs_ffmpeg.raw")
subprocess.run([*tool, "decode", build / "theirs.wav", build / "theirs_ours.raw"], check=True)
a, b = read_raw(build / "theirs_ffmpeg.raw"), read_raw(build / "theirs_ours.raw")
check("multiply: ffmpeg encoder -> our decoder matches ffmpeg decoder", a == b, f"{len(a)} samples")
theirs_snr = sweep_snr(a)
check("multiply: our encoder is no worse than ffmpeg's", ours_snr > theirs_snr - 0.5,
      f"sweep SNR {ours_snr:.1f} dB vs {theirs_snr:.1f} dB")

# 2. Shift-add rounding (the default): bit-exact with the IMA reference in Python's audioop.
tool = [build / "adpcm_wav"]
try:
    import audioop
except ImportError:
    print("SKIP shift-add: audioop missing (Python 3.13+: pip install audioop-lts)")
else:
    raw = (build / "signal.raw").read_bytes()
    reference, _ = audioop.lin2adpcm(raw, 2, None)
    subprocess.run([*tool, "encode-stream", build / "signal.raw", build / "ours.nib"], check=True)
    check("shift-add: our encoder matches audioop's", (build / "ours.nib").read_bytes() == reference)
    decoded, _ = audioop.adpcm2lin(reference, 2, None)
    subprocess.run([*tool, "decode-stream", build / "ours.nib", build / "ours_stream.raw"], check=True)
    check("shift-add: our decoder matches audioop's", (build / "ours_stream.raw").read_bytes() == decoded)
    subprocess.run([*tool, "encode", build / "signal.raw", build / "ours.wav", str(spb), str(rate)], check=True)
    subprocess.run([*tool, "decode", build / "ours.wav", build / "ours_ours.raw"], check=True)
    snr = sweep_snr(read_raw(build / "ours_ours.raw"))
    check("shift-add: our encoder is no worse than ffmpeg's", snr > theirs_snr - 0.5, f"sweep SNR {snr:.1f} dB")

# 3. pdc keeps the WAV's blocks but swaps nibble order; needs the SDK's pdc.
sdk = os.environ.get("PLAYDATE_SDK_PATH")
if not sdk or not Path(sdk, "bin", "pdc").exists():
    print("SKIP .pda layout: set PLAYDATE_SDK_PATH")
else:
    source = build / "pdc_source"
    source.mkdir(exist_ok=True)
    (source / "main.lua").write_text("")
    (source / "pdxinfo").write_text("bundleID=com.example.codectest\n")
    shutil.copy(build / "theirs.wav", source / "clip.wav")
    subprocess.run([Path(sdk, "bin", "pdc"), "-q", "-sdkpath", sdk, source, build / "clip.pdx"], check=True)
    subprocess.run([*tool, "decode", build / "theirs.wav", build / "wav_layout.raw"], check=True)
    subprocess.run([*tool, "decode-pda", build / "clip.pdx" / "clip.pda", build / "pda_layout.raw"], check=True)
    a, b = read_raw(build / "wav_layout.raw"), read_raw(build / "pda_layout.raw")
    check(".pda decodes like its source WAV", len(b) > 0 and a[: len(b)] == b, f"{len(b)} samples in full blocks")

# 4. QOA: our encoder writes the same bytes as the reference encoder (phoboslab/qoa), and a decoder written from
# the spec agrees with ours. The digests are SHA-256 of the reference's files for the same samples.
def kat_signal(n, seed):
    # Integer arithmetic only, so every platform builds the same samples.
    x = seed
    out = []
    for i in range(n):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        noise = (x >> 8) % 6001 - 3000
        segment = i * 6 // n
        if segment == 0:
            s = abs((i * 23) % 8000 - 4000) * 4 - 8000 + noise
        elif segment == 1:
            s = (x >> 7) % 65536 - 32768
        elif segment == 2:
            s = 0
        elif segment == 3:
            s = 32767 if (i // 37) % 2 else -32768
        elif segment == 4:
            s = abs((i * i // 64) % 12000 - 6000) * 5 - 15000 + noise // 4
        else:
            s = 32767 if i % 3000 == 0 else noise // 8
        out.append(max(-32768, min(32767, s)))
    return out


def interleave(left, right):
    out = [0] * (2 * len(left))
    out[0::2] = left
    out[1::2] = right
    return out


# Full scale after a long silence: the first predictions are far off, and the reference's 32-bit arithmetic wraps
# in the scalefactor division (found by fuzzing; a 64-bit division gives different codes).
BURST = [27860, -22463, 32767, -26182, -2269, 10816, 32767, 20074, 26661, 4456, 3609, 13404, 27414, -32768, 32767,
         32767, 32767, 23809, -1550, -11001, -669, -117, -32768, 24079, 16579, 2761, 1472]
QOA_KATS = [
    ("stereo, 52931 samples a channel (11 frames, the last short, and a short last slice)",
     interleave(kat_signal(52931, 1), kat_signal(52931, 2)), 2,
     "20896386755c056909d1e8a8dfd9753feaed21d13cf571f4b7e681454dce8094"),
    ("silence, then full scale", [0] * 1033 + BURST, 1,
     "3a81593a612ad7157109d8aa9a93645ccfe9bdfba57d6bfc1a6766fc0ca2d2cc"),
]


def qoa_tables():
    scalefactors = [round((s + 1) ** 2.75) for s in range(16)]
    away = lambda v: int(math.copysign(math.floor(abs(v) + 0.5), v))
    return [[away(sf * q) for q in (0.75, -0.75, 2.5, -2.5, 4.5, -4.5, 7, -7)] for sf in scalefactors]


def qoa_decode(data):
    """Decodes a .qoa file straight from the spec, as a second implementation to check qoa.c against."""
    dequant = qoa_tables()
    assert data[:4] == b"qoaf"
    total = struct.unpack(">I", data[4:8])[0]
    pos = 8
    out = []
    while pos < len(data):
        header = int.from_bytes(data[pos:pos + 8], "big")
        channels, count, size = header >> 56, header >> 16 & 0xFFFF, header & 0xFFFF
        assert header >> 32 & 0xFFFFFF == 44100, "sample rate"
        slices = -(-count // 20)
        assert size == 8 + channels * (16 + 8 * slices), "frame size"
        assert count == 5120 or pos + size == len(data), "only the last frame may be short"
        lms = []
        for c in range(channels):
            words = struct.unpack(">8h", data[pos + 8 + 16 * c:pos + 24 + 16 * c])
            lms.append([list(words[:4]), list(words[4:])])
        frame = [[] for _ in range(channels)]
        at = pos + 8 + 16 * channels
        for first in range(0, count, 20):
            for c in range(channels):
                (bits,) = struct.unpack(">Q", data[at:at + 8])
                at += 8
                history, weights = lms[c]
                for i in range(min(20, count - first)):
                    residual = dequant[bits >> 60][bits >> (57 - 3 * i) & 7]
                    predicted = sum(w * h for w, h in zip(weights, history)) >> 13
                    sample = max(-32768, min(32767, predicted + residual))
                    weights[:] = [w + (residual >> 4 if h >= 0 else -(residual >> 4)) for w, h in zip(weights, history)]
                    history[:] = history[1:] + [sample]
                    frame[c].append(sample)
        out += interleave(*frame) if channels == 2 else frame[0]
        pos += size
    assert len(out) == total * channels
    return out


for name, samples, channels, digest in QOA_KATS:
    write_raw(build / "kat.raw", samples)
    subprocess.run([build / "qoa_file", "encode", build / "kat.raw", build / "kat.qoa", "44100", str(channels)],
                   check=True)
    encoded = (build / "kat.qoa").read_bytes()
    check(f"QOA {name}: our encoder writes the reference's bytes", hashlib.sha256(encoded).hexdigest() == digest,
          f"{len(encoded)} bytes")
    subprocess.run([build / "qoa_file", "decode", build / "kat.qoa", build / "kat_ours.raw"], check=True)
    ours = read_raw(build / "kat_ours.raw")
    check(f"QOA {name}: our decoder matches one written from the spec", ours == qoa_decode(encoded),
          f"{len(ours)} samples")

# 5. Chain: passband level, stopband rejection.
def render(fmt, samples):
    out = subprocess.run([build / "chain_render", str(fmt)], input=struct.pack(f"<{len(samples)}h", *samples),
                         capture_output=True, check=True).stdout
    return list(struct.unpack(f"<{len(out) // 2}h", out))


def gain_db(fmt, freq):
    x = tone(freq, 0.5)
    y = render(fmt, x)
    skip = 2000
    return db(rms(y[skip:]) / rms(x[skip:]))


# Divisor per format, as in format_chain.c; the stereo formats repeat the mono ones. QOA is the last three of each.
MONO_DIVISORS = [1, 1, 1, 2, 2, 2, 4, 4, 4, 1, 2, 4]
FORMATS = MONO_DIVISORS * 2
MONO_FORMATS = len(MONO_DIVISORS)
FIRST_QOA = 9
QOA_SLICE = 20


def is_qoa(fmt):
    return fmt % MONO_FORMATS >= FIRST_QOA


passthrough = tone(1000, 0.2)
check("format 0 (bypass) is the identity", render(0, passthrough) == passthrough)
check(f"format {MONO_FORMATS} (stereo bypass) is the identity", render(MONO_FORMATS, passthrough) == passthrough)
for fmt, name, nyquist in [(3, "22.05 kHz ADPCM", 11025), (5, "22.05 kHz 16-bit", 11025), (7, "11.025 kHz 8-bit", 5512),
                           (10, "22.05 kHz QOA", 11025)]:
    passband = gain_db(fmt, 1000)
    check(f"{name}: 1 kHz passes", abs(passband) < 1.0, f"{passband:+.2f} dB")
    stop = gain_db(fmt, nyquist * 1.3)
    check(f"{name}: {nyquist * 1.3:.0f} Hz is rejected", stop < -30, f"{stop:+.1f} dB")

# 6. The chain split across a link (sender encodes, receiver decodes) matches the whole chain.
random.seed(2)
mix = [int(0.6 * a + b) for a, b in zip(tone(440, 1.0), [random.randint(-6000, 6000) for _ in range(RATE)])]
other = [int(0.5 * a + b) for a, b in zip(tone(1234, 1.0), [random.randint(-4000, 4000) for _ in range(RATE)])]


def stream(fmt, samples):
    out = subprocess.run([build / "stream_render", str(fmt)], input=struct.pack(f"<{len(samples)}h", *samples),
                         capture_output=True, check=True).stdout
    return list(struct.unpack(f"<{len(out) // 2}h", out))


for fmt in range(len(FORMATS)):
    channels = 1 if fmt < MONO_FORMATS else 2
    signal_in = mix if channels == 1 else interleave(mix, other)
    whole = render(fmt, signal_in)
    split = stream(fmt, signal_in)
    # QOA's encoder holds back a slice of the chain's output.
    delay = (FORMATS[fmt] - 1 + (QOA_SLICE * FORMATS[fmt] if is_qoa(fmt) else 0)) * channels
    n = min(len(split), len(whole) - delay)
    check(f"format {fmt}: stream encode + decode matches the chain", n > RATE * 0.9 * channels and
          split[:n] == whole[delay:delay + n], f"{n} samples")

# A stream's QOA frames hold 100 slices, a file's 256; they decode alike. 44.1 kHz leaves nothing to decimate.
for fmt in (FIRST_QOA, MONO_FORMATS + FIRST_QOA):
    channels = 1 if fmt < MONO_FORMATS else 2
    signal_in = mix if channels == 1 else interleave(mix, other)
    write_raw(build / "qoa_stream.raw", signal_in)
    subprocess.run([build / "qoa_file", "encode", build / "qoa_stream.raw", build / "qoa_stream.qoa", "44100",
                    str(channels)], check=True)
    subprocess.run([build / "qoa_file", "decode", build / "qoa_stream.qoa", build / "qoa_stream_file.raw"], check=True)
    split, in_file = stream(fmt, signal_in), read_raw(build / "qoa_stream_file.raw")
    check(f"format {fmt}: stream frames decode like a .qoa file's",
          split == in_file[:len(split)] and len(split) > RATE * 0.9, f"{len(split)} samples")

# 7. Stereo channels are independent.
silence = [0] * len(mix)
for fmt in range(MONO_FORMATS, len(FORMATS)):
    for name, run in (("chain", lambda x: render(fmt, x)), ("stream", lambda x: stream(fmt, x))):
        right_silent = run(interleave(mix, silence))
        both = run(interleave(mix, other))
        left_silent = run(interleave(silence, other))
        n = min(len(right_silent), len(both), len(left_silent))
        # QOA has no code for zero: a silent channel decodes to +-1.
        quiet = 1 if is_qoa(fmt) else 0
        check(f"format {fmt}: {name} channels are independent",
              max(map(abs, right_silent[1:n:2])) <= quiet and max(map(abs, left_silent[0:n:2])) <= quiet and
              right_silent[0:n:2] == both[0:n:2] and left_silent[1:n:2] == both[1:n:2], f"{n // 2} frames")

sys.exit(1 if failures else 0)
