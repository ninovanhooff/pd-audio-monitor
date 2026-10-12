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

# 4. Chain: passband level, stopband rejection.
def render(fmt, samples):
    out = subprocess.run([build / "chain_render", str(fmt)], input=struct.pack(f"<{len(samples)}h", *samples),
                         capture_output=True, check=True).stdout
    return list(struct.unpack(f"<{len(out) // 2}h", out))


def gain_db(fmt, freq):
    x = tone(freq, 0.5)
    y = render(fmt, x)
    skip = 2000
    return db(rms(y[skip:]) / rms(x[skip:]))


# Divisor per format, as in format_chain.c.
FORMATS = [1, 1, 1, 2, 2, 2, 4, 4, 4, 1, 1, 1, 2, 2, 2, 4, 4, 4]
MONO_FORMATS = 9

passthrough = tone(1000, 0.2)
check("format 0 (bypass) is the identity", render(0, passthrough) == passthrough)
check("format 9 (stereo bypass) is the identity", render(9, passthrough) == passthrough)
for fmt, name, nyquist in [(3, "22.05 kHz ADPCM", 11025), (5, "22.05 kHz 16-bit", 11025), (7, "11.025 kHz 8-bit", 5512)]:
    passband = gain_db(fmt, 1000)
    check(f"{name}: 1 kHz passes", abs(passband) < 1.0, f"{passband:+.2f} dB")
    stop = gain_db(fmt, nyquist * 1.3)
    check(f"{name}: {nyquist * 1.3:.0f} Hz is rejected", stop < -30, f"{stop:+.1f} dB")

# 5. The chain split across a link (sender encodes, receiver decodes) matches the whole chain.
random.seed(2)
mix = [int(0.6 * a + b) for a, b in zip(tone(440, 1.0), [random.randint(-6000, 6000) for _ in range(RATE)])]
other = [int(0.5 * a + b) for a, b in zip(tone(1234, 1.0), [random.randint(-4000, 4000) for _ in range(RATE)])]


def interleave(left, right):
    out = [0] * (2 * len(left))
    out[0::2] = left
    out[1::2] = right
    return out


def stream(fmt, samples, *options):
    out = subprocess.run([build / "stream_render", str(fmt), *options], input=struct.pack(f"<{len(samples)}h", *samples),
                         capture_output=True, check=True).stdout
    return list(struct.unpack(f"<{len(out) // 2}h", out))


for fmt in range(len(FORMATS)):
    channels = 1 if fmt < MONO_FORMATS else 2
    signal_in = mix if channels == 1 else interleave(mix, other)
    whole = render(fmt, signal_in)
    split = stream(fmt, signal_in)
    delay = (FORMATS[fmt] - 1) * channels
    n = min(len(split), len(whole) - delay)
    check(f"format {fmt}: stream encode + decode matches the chain", n > RATE * 0.9 * channels and
          split[:n] == whole[delay:delay + n], f"{n} samples")

# 6. Stereo channels are independent.
silence = [0] * len(mix)
for fmt in range(MONO_FORMATS, len(FORMATS)):
    for name, run in (("chain", lambda x: render(fmt, x)), ("stream", lambda x: stream(fmt, x))):
        right_silent = run(interleave(mix, silence))
        both = run(interleave(mix, other))
        left_silent = run(interleave(silence, other))
        n = min(len(right_silent), len(both), len(left_silent))
        check(f"format {fmt}: {name} channels are independent",
              not any(right_silent[1:n:2]) and not any(left_silent[0:n:2]) and
              right_silent[0:n:2] == both[0:n:2] and left_silent[1:n:2] == both[1:n:2], f"{n // 2} frames")

# 7. ADPCM-XQ: its blocks decode with the receiver's decoder, and quantize the filtered signal more closely
# than the greedy encoder does. The reference is the same chain without quantization (the PCM16 format of the
# same rate).
ADPCM_FORMATS = (2, 5, 8, 11, 14, 17)
PCM16_OF_DIVISOR = {1: (0, 9), 2: (3, 12), 4: (6, 15)}
for fmt, divisor in enumerate(FORMATS):
    if fmt not in ADPCM_FORMATS:
        continue
    channels = 1 if fmt < MONO_FORMATS else 2
    signal_in = mix if channels == 1 else interleave(mix, other)
    reference = render(PCM16_OF_DIVISOR[divisor][channels - 1], signal_in)
    delay = (divisor - 1) * channels
    errors = {}
    for name, options in (("greedy", ()), ("ADPCM-XQ", ("xq",))):
        decoded = stream(fmt, signal_in, *options)
        n = min(len(decoded), len(reference) - delay)
        errors[name] = rms([a - b for a, b in zip(decoded[:n], reference[delay:delay + n])])
    check(f"format {fmt}: ADPCM-XQ quantizes closer than the greedy encoder",
          errors["ADPCM-XQ"] < errors["greedy"],
          f"error {errors['ADPCM-XQ']:.1f} vs {errors['greedy']:.1f} LSB rms")

silence = [0] * len(mix)
for fmt in (11, 14, 17):
    right_silent = stream(fmt, interleave(mix, silence), "xq")
    both = stream(fmt, interleave(mix, other), "xq")
    n = min(len(right_silent), len(both))
    check(f"format {fmt}: ADPCM-XQ channels are independent",
          not any(right_silent[1:n:2]) and right_silent[0:n:2] == both[0:n:2], f"{n // 2} frames")

sys.exit(1 if failures else 0)
