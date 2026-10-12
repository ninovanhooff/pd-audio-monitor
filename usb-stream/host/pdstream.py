#!/usr/bin/env python3
"""Stream Mac or Windows audio (BlackHole, VB-Cable, or a WAV file) to PD Audio Monitor Receiver on a Playdate.

Encodes in a Playdate audio format with shared/codec (built into libpdstream by `make`) and sends
it over USB serial as `eval` commands: Lua bytecode that hands each packet to the receiver, which
decodes it and plays it on the speaker.
"""
import argparse
import ctypes
import json
import os
import queue
import re
import sys
import threading
import time
import wave
from pathlib import Path

import numpy as np

RATE = 44100
BLOCK = 1024
PLAYDATE_VID = 0x1331
# Virtual devices the host captures, best first: BlackHole; VB-Cable (its input is "CABLE Output"), else
# VoiceMeeter's virtual output.
PREFERRED_DEVICES = ("CABLE Output", "VoiceMeeter Output") if sys.platform == "win32" else ("BlackHole",)
# The receiver logs this over USB when the user picks a format on the Playdate.
# Format -1 asks the host to stop sending: the Playdate is paused, and the eval stream would swamp it.
FORMAT_REQUEST = re.compile(rb"pdstream format (-?\d+)")
# Packet layout is in usb-stream/receiver/src/packet.h.
HEADER_BYTES = 4
PACKET_BYTES = 1024
# Set in the offset field of a stream's first packet: the receiver drops a partly received unit.
RESTART_FLAG = 0x8000
# pdc's compilation of `pdstream_feed("...")`: Playdate's Lua 5.4 with 4-byte integers and floats,
# stripped. The string constant comes last, so the chunk can be built around any packet.
EVAL_HEAD = bytes.fromhex("1b4c7561540019930d0a1a0a040404785600000040b9430180808000010285"
                          "4f0000000900000083800000420002014400010182048e") + b"pdstream_feed"
EVAL_TAIL = bytes.fromhex("810100008080808080")
LUA_MAX_SHORT_STRING = 40


def lua_size(n):
    # Lua 5.4 dump sizes: 7 bits a byte, most significant first, 0x80 marks the last byte.
    out = [0x80 | (n & 0x7F)]
    n >>= 7
    while n:
        out.insert(0, n & 0x7F)
        n >>= 7
    return bytes(out)


def eval_command(packet):
    tag = 0x04 if len(packet) <= LUA_MAX_SHORT_STRING else 0x14
    chunk = EVAL_HEAD + bytes([tag]) + lua_size(len(packet) + 1) + packet + EVAL_TAIL
    return b"eval %d\n" % len(chunk) + chunk


def settings_path():
    if sys.platform == "win32":
        base = Path(os.environ.get("APPDATA") or Path.home())
    elif sys.platform == "darwin":
        base = Path.home() / "Library" / "Application Support"
    else:
        base = Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config")
    return base / "PD Audio Monitor Host" / "settings.json"


SETTINGS_MAGIC = "pdstream-settings"
SETTINGS_VERSION = 2


def load_settings(path=None):
    """The remembered input and format; empty if the file is missing, unreadable or not this version."""
    try:
        values = json.loads((path or settings_path()).read_text())
    except (OSError, ValueError):
        return {}
    if not isinstance(values, dict) or values.get("magic") != SETTINGS_MAGIC or values.get("version") != SETTINGS_VERSION:
        return {}
    return {key: value for key, value in values.items() if key not in ("magic", "version")}


def save_settings(values, path=None):
    """Settings are a convenience: a folder that can't be written to must not stop the stream."""
    path = path or settings_path()
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps({"magic": SETTINGS_MAGIC, "version": SETTINGS_VERSION, **values}, indent=2))
    except OSError:
        pass


def load_encoder():
    # A PyInstaller bundle unpacks the library next to its own files, in sys._MEIPASS.
    here = Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parent))
    for name in ("libpdstream.dylib", "libpdstream.so", "libpdstream.dll"):
        if (here / name).exists():
            lib = ctypes.CDLL(str(here / name))
            break
    else:
        sys.exit(f"libpdstream not built: run `make -C {here}`")
    lib.pdstream_new.restype = ctypes.c_void_p
    lib.pdstream_new.argtypes = [ctypes.c_int]
    lib.pdstream_free.argtypes = [ctypes.c_void_p]
    lib.pdstream_encode.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p]
    lib.pdstream_format_channels.argtypes = [ctypes.c_int]
    lib.pdstream_format_name.restype = ctypes.c_char_p
    return lib


class Encoder:
    def __init__(self, lib, fmt):
        self.lib = lib
        self.handle = lib.pdstream_new(fmt)
        self.channels = lib.pdstream_format_channels(fmt)
        self.out = ctypes.create_string_buffer(2 * self.channels * BLOCK + 4096)

    def encode(self, data):
        """data: (n,) samples for a mono format, (n, 2) frames for a stereo one."""
        samples = np.ascontiguousarray(data, dtype="<i2")
        chunks = []
        for start in range(0, len(samples), BLOCK):
            part = samples[start:start + BLOCK]
            written = self.lib.pdstream_encode(self.handle, part.ctypes.data, len(part), self.out)
            chunks.append(self.out.raw[:written])
        return b"".join(chunks)


class Packetizer:
    def __init__(self, fmt, unit_bytes):
        self.fmt = fmt
        self.unit_bytes = unit_bytes
        self.pending = bytearray()
        self.position = 0
        self.sequence = 0
        self.restart = True

    def commands(self, data, flush=False):
        self.pending += data
        payload = PACKET_BYTES - HEADER_BYTES
        out = []
        while len(self.pending) >= payload or (flush and self.pending):
            chunk = bytes(self.pending[:payload])
            del self.pending[:payload]
            offset = self.position % self.unit_bytes
            if self.restart:
                offset |= RESTART_FLAG
                self.restart = False
            header = bytes([self.fmt, self.sequence, offset & 0xFF, offset >> 8])
            out.append(eval_command(header + chunk))
            self.position += len(chunk)
            self.sequence = (self.sequence + 1) & 0xFF
        return out


class Codec:
    """Encoder and packetizer for the current format. switch() keeps the sequence numbers running, so
    the receiver sees a format change rather than lost packets."""

    def __init__(self, lib, fmt):
        self.lib = lib
        self.paused = False
        self.encoder = self.packetizer = None
        self.switch(fmt)

    def switch(self, fmt):
        sequence = self.packetizer.sequence if self.packetizer else 0
        if self.encoder:
            self.lib.pdstream_free(self.encoder.handle)
        self.format = fmt
        self.encoder = Encoder(self.lib, fmt)
        self.packetizer = Packetizer(fmt, self.lib.pdstream_unit_bytes(fmt))
        self.packetizer.sequence = sequence

    def commands(self, frames, flush=False):
        if self.encoder.channels == 1:
            frames = (frames.astype(np.int32).sum(axis=1) // 2).astype(np.int16)
        return self.packetizer.commands(self.encoder.encode(frames), flush)


class Console:
    """Where progress goes: log lines, and a status line rewritten in place. pdstream_gui replaces it."""

    def __init__(self):
        self.status_shown = False

    def log(self, text):
        sys.stderr.write(("\n" if self.status_shown else "") + text + "\n")
        self.status_shown = False

    def status(self, text):
        sys.stderr.write(f"\r{text} ")
        sys.stderr.flush()
        self.status_shown = True

    def finish(self):
        if self.status_shown:
            sys.stderr.write("\n")


console = Console()


def to_frames(samples):
    """(n, channels) int16 as (n, 2) frames: a single channel goes to both sides. Always a copy: PortAudio
    reuses the buffer it passes to the callback."""
    samples = np.array(samples, dtype=np.int16)
    if samples.ndim == 1:
        samples = samples[:, None]
    if samples.shape[1] == 1:
        return np.repeat(samples, 2, axis=1)
    return samples[:, :2]


def playdate_ports():
    from serial.tools import list_ports

    ports = [p.device for p in list_ports.comports() if p.vid == PLAYDATE_VID]
    # macOS lists each port as tty.* and cu.*; cu.* doesn't wait for carrier detect.
    ports.sort(key=lambda device: "/cu." not in device)
    return ports


class SerialSink:
    """Drops audio while the Playdate is unplugged or locked, and opens the port when it's there."""

    def __init__(self, port, verbose):
        import serial

        self.serial = serial
        self.requested = port
        self.verbose = verbose
        self.requested_format = None
        self.port = None
        self.retry_at = 0
        if not self._reconnect():
            console.log("Waiting for a Playdate on USB: plug it in and unlock it")

    def _open(self, device):
        self.port = self.serial.Serial(device, 115200, timeout=0.2, write_timeout=5)
        self.port.write(b"echo off\n")
        threading.Thread(target=self._drain, args=(self.port,), daemon=True).start()

    def _drain(self, port):
        pending = b""
        # A read can fail while the Playdate is busy and work again afterwards; only a port this sink has
        # let go of ends the loop, or the Playdate's format requests would go unheard.
        while self.port is port:
            try:
                *complete, pending = (pending + port.read(4096)).split(b"\n")
            except (self.serial.SerialException, OSError, TypeError):
                time.sleep(0.05)
                continue
            for line in complete:
                # Each eval answers with an empty line.
                if self.verbose and line.strip():
                    console.log(line.decode(errors="replace").rstrip())
                match = FORMAT_REQUEST.search(line)
                if match:
                    self.requested_format = int(match.group(1))

    def _reconnect(self):
        now = time.monotonic()
        if now < self.retry_at:
            return False
        self.retry_at = now + 1
        ports = [self.requested] if self.requested else playdate_ports()
        if not ports:
            return False
        try:
            self._open(ports[0])
        except (self.serial.SerialException, OSError):
            return False
        console.log(f"Playdate on {ports[0]}")
        return True

    def write(self, commands):
        if self.port is None and not self._reconnect():
            return False
        try:
            self.port.write(b"".join(commands))
            return True
        except (self.serial.SerialException, OSError):
            port, self.port = self.port, None
            # A request from before the Playdate went has no say over what it gets when it returns.
            self.requested_format = None
            try:
                port.close()
            except (self.serial.SerialException, OSError):
                pass
            console.log("Playdate gone (unplugged or locked); waiting for it to come back")
            return False

    def close(self):
        if self.port is not None:
            try:
                self.port.close()
            except (self.serial.SerialException, OSError):
                pass
            self.port = None


class FileSink:
    requested_format = None

    def __init__(self, path):
        self.file = open(path, "wb")

    def write(self, commands):
        self.file.writelines(commands)
        return True


class Stats:
    def __init__(self, name):
        self.name = name
        self.commands = self.bytes = self.peak = 0
        self.last = time.monotonic()

    def add(self, frames, commands):
        self.commands += len(commands)
        self.bytes += sum(len(command) for command in commands)
        if len(frames):
            self.peak = max(self.peak, int(np.abs(frames.astype(np.int32)).max()))
        now = time.monotonic()
        if now - self.last >= 1:
            seconds = now - self.last
            peak = 20 * np.log10(self.peak / 32768) if self.peak else -99
            console.status(f"{self.name}: {self.commands / seconds:4.0f} eval/s  "
                           f"{self.bytes / seconds / 1000:5.1f} KB/s on the wire  peak {peak:6.1f} dBFS")
            self.commands = self.bytes = self.peak = 0
            self.last = now


def follow_playdate(codec, sink, stats, names):
    """Applies the Playdate's latest format request; returns whether the stream is paused."""
    fmt = sink.requested_format
    if fmt == -1:
        if not codec.paused:
            codec.paused = True
            console.log("Playdate paused: not sending until it asks for a format")
        # Nothing else is written while paused, so this is how a Playdate that has gone is noticed.
        sink.write([b"\n"])
    elif fmt is None:
        codec.paused = False
    elif fmt is not None and 0 <= fmt < len(names) and (fmt != codec.format or codec.paused):
        codec.switch(fmt)
        codec.paused = False
        stats.name = names[fmt]
        console.log(f"Playdate picked format {fmt}: {names[fmt]}")
    return codec.paused


def read_wav(path):
    with wave.open(str(path)) as wav:
        if wav.getsampwidth() != 2 or wav.getframerate() != RATE:
            sys.exit(f"{path}: need 16-bit {RATE} Hz; convert with "
                     f"ffmpeg -i {path} -ar {RATE} -c:a pcm_s16le out.wav")
        frames = np.frombuffer(wav.readframes(wav.getnframes()), dtype="<i2")
        return to_frames(frames.reshape(-1, wav.getnchannels()))


def stream_file(path, codec, sink, stats, names, paced):
    frames = read_wav(path)
    start = time.monotonic()
    for offset in range(0, len(frames), BLOCK):
        part = frames[offset:offset + BLOCK]
        last = offset + BLOCK >= len(frames)
        paused = follow_playdate(codec, sink, stats, names)
        commands = [] if paused else codec.commands(part, flush=last)
        sent = bool(commands) and sink.write(commands)
        stats.add(part, commands if sent else [])
        if paced:
            delay = start + (offset + BLOCK) / RATE - time.monotonic()
            if delay > 0:
                time.sleep(delay)


def microphone_app():
    """The app macOS asks microphone access for: "PD Audio Monitor Host.app" itself, else whatever started this."""
    if ".app/Contents/" in sys.executable:
        return "PD Audio Monitor Host"
    return "the app running this (Terminal, iTerm, ...)"


class _Block(ctypes.Structure):
    # An Objective-C block literal, for AVFoundation's completion handler.
    _fields_ = [("isa", ctypes.c_void_p), ("flags", ctypes.c_int), ("reserved", ctypes.c_int),
                ("invoke", ctypes.c_void_p), ("descriptor", ctypes.c_void_p)]


def _avcapture():
    objc = ctypes.cdll.LoadLibrary("/usr/lib/libobjc.A.dylib")
    avfoundation = ctypes.cdll.LoadLibrary("/System/Library/Frameworks/AVFoundation.framework/AVFoundation")
    objc.objc_getClass.restype = objc.sel_registerName.restype = ctypes.c_void_p
    return objc, objc.objc_getClass(b"AVCaptureDevice"), ctypes.c_void_p.in_dll(avfoundation, "AVMediaTypeAudio")


# The answer to this process's prompt: the status macOS reports keeps its value from launch.
_microphone_answer = None


def microphone_access():
    """macOS microphone access of microphone_app(): "not determined", "restricted", "denied",
    "authorized", or None off macOS."""
    if sys.platform != "darwin":
        return None
    if _microphone_answer:
        return _microphone_answer
    try:
        objc, device, audio = _avcapture()
        send = ctypes.CFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)(("objc_msgSend", objc))
        return ["not determined", "restricted", "denied", "authorized"][
            send(device, objc.sel_registerName(b"authorizationStatusForMediaType:"), audio)]
    except (OSError, AttributeError, IndexError, ValueError):
        return None


def request_microphone_access(stop=None):
    """Shows macOS's prompt and waits for the answer: True when granted. Asking through AVFoundation
    rather than by opening the capture keeps microphone_access() current, and a capture opened before
    the answer stays silent."""
    try:
        objc, device, audio = _avcapture()
        global_block = ctypes.c_void_p.in_dll(ctypes.CDLL(None), "_NSConcreteGlobalBlock")
    except (OSError, ValueError):
        return False
    answered = threading.Event()
    granted = []
    invoke = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_bool)(
        lambda block, allowed: (granted.append(allowed), answered.set()))
    descriptor = (ctypes.c_ulong * 2)(0, ctypes.sizeof(_Block))
    block = _Block(ctypes.addressof(global_block), 1 << 28, 0,  # BLOCK_IS_GLOBAL: never copied or freed
                   ctypes.cast(invoke, ctypes.c_void_p), ctypes.addressof(descriptor))
    send = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)(("objc_msgSend", objc))
    send(device, objc.sel_registerName(b"requestAccessForMediaType:completionHandler:"), audio, ctypes.addressof(block))
    global _microphone_answer
    while not answered.wait(0.5):
        if stop is not None and stop.is_set():
            return False
    _microphone_answer = "authorized" if granted[0] else "denied"
    return granted[0]


def input_devices():
    """Input devices, once each: on Windows PortAudio lists every device under several host APIs, and
    only WASAPI is used here."""
    import sounddevice as sd

    devices = [d for d in sd.query_devices() if d["max_input_channels"] > 0]
    if sys.platform == "win32":
        wasapi = [i for i, api in enumerate(sd.query_hostapis()) if api["name"] == "Windows WASAPI"]
        devices = [d for d in devices if d["hostapi"] in wasapi] or devices
    return devices


def find_input(name):
    wanted = name.lower()
    return next((d for d in input_devices() if wanted in d["name"].lower()), None)


def preferred_input():
    return next((d for d in map(find_input, PREFERRED_DEVICES) if d), None)


def stream_device(name, codec, sink, stats, names, stop=None):
    import sounddevice as sd

    # Without access, macOS hands the capture silence instead of an error.
    access = microphone_access()
    if access == "not determined":
        console.log(f"macOS asks for microphone access for {microphone_app()}: allow it.")
        if request_microphone_access(stop):
            access = "authorized"
        elif stop is not None and stop.is_set():
            return
        else:
            access = "denied"
    if access in ("denied", "restricted"):
        sys.exit(f"Microphone access is {access}, so the capture would be silent. Allow {microphone_app()} under "
                 "System Settings → Privacy & Security → Microphone, then restart it.")
    if sys.platform == "win32":
        # WASAPI can't open a stream on a different thread than the one that initialised PortAudio, which
        # the window's device list did; this also picks up devices added since.
        sd._terminate()
        sd._initialize()
    info = find_input(name)
    if info is None:
        sys.exit(f"No input device matches {name!r}.\nInput devices:\n" + "\n".join(
            f"  {d['name']}" for d in input_devices()))
    blocks = queue.Queue()

    def callback(frames, count, timing, status):
        blocks.put(to_frames(frames))

    channels = min(2, info["max_input_channels"])
    # WASAPI shared mode only offers the mixer's rate; auto_convert resamples to RATE.
    extra = sd.WasapiSettings(auto_convert=True) if sys.platform == "win32" else None
    with sd.InputStream(device=info["index"], channels=channels, samplerate=RATE, dtype="int16",
                        blocksize=BLOCK, callback=callback, extra_settings=extra):
        console.log(f"Capturing from {info['name']}")
        while stop is None or not stop.is_set():
            try:
                frames = blocks.get(timeout=0.5)
            except queue.Empty:
                continue
            paused = follow_playdate(codec, sink, stats, names)
            commands = [] if paused else codec.commands(frames)
            sent = bool(commands) and sink.write(commands)
            stats.add(frames, commands if sent else [])
            if blocks.qsize() > 20:
                console.log("Falling behind: the serial link is slower than this format needs.")


def main():
    lib = load_encoder()
    formats = [lib.pdstream_format_name(i).decode() for i in range(lib.pdstream_format_count())]
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog="formats:\n" + "\n".join(f"  {i}  {n}" for i, n in enumerate(formats)))
    parser.add_argument("--format", type=int, default=lib.pdstream_default_format(),
                        help="format number, see below; the Playdate's d-pad switches it while streaming")
    parser.add_argument("--device", help="input device name or part of it (default: the first of "
                                         + ", ".join(PREFERRED_DEVICES) + " found)")
    parser.add_argument("--input", type=Path, help="stream a 16-bit 44.1 kHz WAV file instead of a device")
    parser.add_argument("--port", help="serial port (default: the first Playdate found)")
    parser.add_argument("--out", type=Path, help="write what would go to the Playdate to a file")
    parser.add_argument("--verbose", action="store_true", help="show what the Playdate prints")
    args = parser.parse_args()
    if not 0 <= args.format < len(formats):
        parser.error(f"--format must be 0 to {len(formats) - 1}")

    codec = Codec(lib, args.format)
    sink = FileSink(args.out) if args.out else SerialSink(args.port, args.verbose)
    stats = Stats(formats[args.format])
    try:
        if args.input:
            stream_file(args.input, codec, sink, stats, formats, paced=not args.out)
        else:
            found = None if args.device else preferred_input()
            stream_device(args.device or (found["name"] if found else PREFERRED_DEVICES[0]), codec, sink, stats, formats)
    except KeyboardInterrupt:
        pass
    console.finish()


if __name__ == "__main__":
    main()
