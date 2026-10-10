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


passthrough = tone(1000, 0.2)
check("format 0 (bypass) is the identity", render(0, passthrough) == passthrough)
for fmt, name, nyquist in [(2, "22.05 kHz ADPCM", 11025), (4, "22.05 kHz 16-bit", 11025), (6, "11.025 kHz 8-bit", 5512)]:
    passband = gain_db(fmt, 1000)
    check(f"{name}: 1 kHz passes", abs(passband) < 1.0, f"{passband:+.2f} dB")
    stop = gain_db(fmt, nyquist * 1.3)
    check(f"{name}: {nyquist * 1.3:.0f} Hz is rejected", stop < -30, f"{stop:+.1f} dB")

sys.exit(1 if failures else 0)
