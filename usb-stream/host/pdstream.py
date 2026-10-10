#!/usr/bin/env python3
"""Stream Mac audio (BlackHole, or a WAV file) to the USB Stream app on a Playdate.

Encodes in a Playdate audio format with shared/codec (built into libpdstream by `make`) and sends
it as serial `msg` lines, which the receiver decodes and plays on the speaker.
"""
import argparse
import base64
import ctypes
import queue
import sys
import threading
import time
import wave
from pathlib import Path

import numpy as np

RATE = 44100
BLOCK = 1024
PLAYDATE_VID = 0x1331
# Packet layout is in usb-stream/receiver/src/packet.h. 183 bytes is 244 base64 characters, under
# the device's limit of about 251 per message.
HEADER_BYTES = 4
PACKET_BYTES = 183


def load_encoder():
    here = Path(__file__).resolve().parent
    for name in ("libpdstream.dylib", "libpdstream.so"):
        if (here / name).exists():
            lib = ctypes.CDLL(str(here / name))
            break
    else:
        sys.exit(f"libpdstream not built: run `make -C {here}`")
    lib.pdstream_new.restype = ctypes.c_void_p
    lib.pdstream_new.argtypes = [ctypes.c_int]
    lib.pdstream_free.argtypes = [ctypes.c_void_p]
    lib.pdstream_encode.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p]
    lib.pdstream_format_name.restype = ctypes.c_char_p
    return lib


class Encoder:
    def __init__(self, lib, fmt):
        self.lib = lib
        self.handle = lib.pdstream_new(fmt)
        self.out = ctypes.create_string_buffer(2 * BLOCK + 4096)

    def encode(self, mono):
        samples = np.ascontiguousarray(mono, dtype="<i2")
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

    def lines(self, data, flush=False):
        self.pending += data
        payload = PACKET_BYTES - HEADER_BYTES
        out = []
        while len(self.pending) >= payload or (flush and self.pending):
            chunk = bytes(self.pending[:payload])
            del self.pending[:payload]
            offset = self.position % self.unit_bytes
            header = bytes([self.fmt, self.sequence, offset & 0xFF, offset >> 8])
            out.append(b"msg " + base64.b64encode(header + chunk) + b"\n")
            self.position += len(chunk)
            self.sequence = (self.sequence + 1) & 0xFF
        return out


def to_mono(frames):
    frames = np.asarray(frames, dtype=np.int32)
    if frames.ndim == 1:
        return frames.astype(np.int16)
    return (frames.sum(axis=1) // frames.shape[1]).astype(np.int16)


def playdate_ports():
    from serial.tools import list_ports

    ports = [p.device for p in list_ports.comports() if p.vid == PLAYDATE_VID]
    # macOS lists each port as tty.* and cu.*; cu.* doesn't wait for carrier detect.
    ports.sort(key=lambda device: "/cu." not in device)
    return ports


def find_port():
    ports = playdate_ports()
    if not ports:
        sys.exit("No Playdate found on USB. Plug it in and unlock it, or pass --port.")
    return ports[0]


class SerialSink:
    """Drops audio while the Playdate is unplugged or locked, and reopens the port when it returns."""

    def __init__(self, port, verbose):
        import serial

        self.serial = serial
        self.requested = port
        self.verbose = verbose
        self.port = None
        self.retry_at = 0
        self._open(port)

    def _open(self, device):
        self.port = self.serial.Serial(device, 115200, timeout=0.2, write_timeout=5)
        threading.Thread(target=self._drain, args=(self.port,), daemon=True).start()

    def _drain(self, port):
        try:
            while True:
                data = port.read(4096)
                if data and self.verbose:
                    sys.stderr.write(data.decode(errors="replace"))
        except (self.serial.SerialException, OSError, TypeError):
            pass  # write() notices the lost port; closing it mid-read can raise TypeError

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
        sys.stderr.write(f"Playdate back on {ports[0]}\n")
        return True

    def write(self, lines):
        if self.port is None and not self._reconnect():
            return False
        try:
            self.port.write(b"".join(lines))
            return True
        except (self.serial.SerialException, OSError):
            port, self.port = self.port, None
            try:
                port.close()
            except (self.serial.SerialException, OSError):
                pass
            sys.stderr.write("\nPlaydate gone (unplugged or locked); waiting for it to come back\n")
            return False


class FileSink:
    def __init__(self, path):
        self.file = open(path, "wb")

    def write(self, lines):
        self.file.writelines(lines)
        return True


class Stats:
    def __init__(self, name):
        self.name = name
        self.lines = self.bytes = self.peak = 0
        self.last = time.monotonic()
        self.reported = False

    def add(self, mono, lines):
        self.lines += len(lines)
        self.bytes += sum(len(line) for line in lines)
        if len(mono):
            self.peak = max(self.peak, int(np.abs(mono.astype(np.int32)).max()))
        now = time.monotonic()
        if now - self.last >= 1:
            seconds = now - self.last
            peak = 20 * np.log10(self.peak / 32768) if self.peak else -99
            sys.stderr.write(f"\r{self.name}: {self.lines / seconds:4.0f} msg/s  "
                             f"{self.bytes / seconds / 1000:5.1f} KB/s on the wire  peak {peak:6.1f} dBFS ")
            sys.stderr.flush()
            self.reported = True
            self.lines = self.bytes = self.peak = 0
            self.last = now


def read_wav(path):
    with wave.open(str(path)) as wav:
        if wav.getsampwidth() != 2 or wav.getframerate() != RATE:
            sys.exit(f"{path}: need 16-bit {RATE} Hz; convert with "
                     f"ffmpeg -i {path} -ar {RATE} -c:a pcm_s16le out.wav")
        frames = np.frombuffer(wav.readframes(wav.getnframes()), dtype="<i2")
        return to_mono(frames.reshape(-1, wav.getnchannels()))


def stream_file(path, encoder, packetizer, sink, stats, paced):
    mono = read_wav(path)
    start = time.monotonic()
    for offset in range(0, len(mono), BLOCK):
        part = mono[offset:offset + BLOCK]
        last = offset + BLOCK >= len(mono)
        lines = packetizer.lines(encoder.encode(part), flush=last)
        sent = sink.write(lines)
        stats.add(part, lines if sent else [])
        if paced:
            delay = start + (offset + BLOCK) / RATE - time.monotonic()
            if delay > 0:
                time.sleep(delay)


def stream_device(name, encoder, packetizer, sink, stats):
    import sounddevice as sd

    try:
        info = sd.query_devices(name, "input")
    except ValueError as error:
        sys.exit(f"{error}\nInput devices:\n" + "\n".join(
            f"  {d['name']}" for d in sd.query_devices() if d["max_input_channels"] > 0))
    blocks = queue.Queue()

    def callback(frames, count, timing, status):
        blocks.put(to_mono(frames))

    channels = min(2, info["max_input_channels"])
    with sd.InputStream(device=info["index"], channels=channels, samplerate=RATE, dtype="int16",
                        blocksize=BLOCK, callback=callback):
        sys.stderr.write(f"Capturing from {info['name']}\n")
        while True:
            mono = blocks.get()
            lines = packetizer.lines(encoder.encode(mono))
            sent = sink.write(lines)
            stats.add(mono, lines if sent else [])
            if blocks.qsize() > 20:
                sys.stderr.write("\nFalling behind: the serial link is slower than this format needs.\n")


def main():
    lib = load_encoder()
    formats = [lib.pdstream_format_name(i).decode() for i in range(lib.pdstream_format_count())]
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog="formats:\n" + "\n".join(f"  {i}  {n}" for i, n in enumerate(formats)))
    parser.add_argument("--format", type=int, default=lib.pdstream_default_format(), help="format number, see below")
    parser.add_argument("--device", default="BlackHole", help="input device name or part of it (default BlackHole)")
    parser.add_argument("--input", type=Path, help="stream a 16-bit 44.1 kHz WAV file instead of a device")
    parser.add_argument("--port", help="serial port (default: the first Playdate found)")
    parser.add_argument("--out", type=Path, help="write the msg lines to a file instead of the Playdate")
    parser.add_argument("--verbose", action="store_true", help="show what the Playdate prints")
    args = parser.parse_args()
    if not 0 <= args.format < len(formats):
        parser.error(f"--format must be 0 to {len(formats) - 1}")

    encoder = Encoder(lib, args.format)
    packetizer = Packetizer(args.format, lib.pdstream_unit_bytes(args.format))
    sink = FileSink(args.out) if args.out else SerialSink(args.port or find_port(), args.verbose)
    stats = Stats(formats[args.format])
    try:
        if args.input:
            stream_file(args.input, encoder, packetizer, sink, stats, paced=not args.out)
        else:
            stream_device(args.device, encoder, packetizer, sink, stats)
    except KeyboardInterrupt:
        pass
    if stats.reported:
        sys.stderr.write("\n")


if __name__ == "__main__":
    main()
