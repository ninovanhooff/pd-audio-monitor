# USB Stream

Plays Mac audio on the **Playdate's speaker** over the USB cable, encoded in a Playdate audio
format. The signal stays digital end to end, so there's none of the analog noise of
[`line-in-monitor`](../line-in-monitor/).

```
DAW / any app ──► BlackHole 2ch ──► pdstream.py ──USB serial `eval`──────► USB Stream app ──► speaker
                  (virtual output)  decimate + encode                       decode + interpolate
```

The two halves are the format chain split in two (`shared/codec/format_stream.c`): what plays
equals `chain_render` (and the line-in monitor's emulation) sample for sample.

## Setup (macOS)

1. Install [BlackHole](https://existential.audio/blackhole/) 2ch: `brew install blackhole-2ch`.
2. In **Audio MIDI Setup**, set BlackHole 2ch to **44.1 kHz**.
   To also hear the Mac, create a *Multi-Output Device* with BlackHole and your
   headphones. The Playdate plays a few hundred ms later.
3. Send audio to BlackHole (or the Multi-Output Device), either as the system output or as your
   DAW's output device.
4. Build the encoder and install the Python packages (needs the Xcode command line tools):

   ```bash
   make -C usb-stream/host
   python3 -m venv ~/.venvs/pdstream
   ~/.venvs/pdstream/bin/pip install numpy pyserial sounddevice
   ```

   Or bundle it into one executable, `usb-stream/host/dist/pdstream`, that runs on Macs with the
   same CPU without Python or the packages (each start unpacks it first, which takes about 3 s):

   ```bash
   ~/.venvs/pdstream/bin/pip install pyinstaller
   make -C usb-stream/host bundle PYTHON=~/.venvs/pdstream/bin/python
   ```

   Or build `usb-stream/host/dist/PDStream.app`, which does the same in a window, with status, the
   log, and these setup and troubleshooting steps. Opened from Finder, it asks microphone access for
   itself instead of for a terminal. Each build has a new signature, so after a rebuild you may
   need to allow it again (see its Troubleshooting tab):

   ```bash
   make -C usb-stream/host app PYTHON=~/.venvs/pdstream/bin/python   # needs pyinstaller and Tk (brew install python-tk)
   open usb-stream/host/dist/PDStream.app
   ```

5. Build `USBStream.pdx` (`make -C usb-stream/receiver`, with `PLAYDATE_SDK_PATH` set) and
   sideload it. Start it on the Playdate, which stays connected over USB.
6. Run the streamer:

   ```bash
   ~/.venvs/pdstream/bin/python usb-stream/host/pdstream.py          # default 22.05 kHz IMA ADPCM
   ~/.venvs/pdstream/bin/python usb-stream/host/pdstream.py --help   # formats, --device, --port, --input file.wav
   ~/.venvs/pdstream/bin/python usb-stream/host/pdstream.py --format 0             # bypass
   usb-stream/host/dist/pdstream                                     # the bundle takes the same options
   ```

   If the serial port is busy, quit the Playdate Simulator; it also talks to the device over USB.
   Without microphone access, macOS hands the capture silence. `pdstream.py` checks at start and
   stops if access is denied: allow the app running it (Terminal, iTerm, ...; the bundle runs under
   it too) under **System Settings → Privacy & Security → Microphone**, then restart that app.

## On the Playdate

| Row | Shows |
| --- | --- |
| Format ✛ | Format of the incoming stream. ⬅/➡ pick the sample rate, ⬆/⬇ the codec (more bits up: 16-bit, 8-bit, ADPCM); the Playdate asks `pdstream.py` to switch and shows `...` until it does. Both wrap around. `--format` sets the starting format, and the PDStream window's Format menu switches it too |
| Output Ⓑ | Speaker / headphones / both |
| Link | `eval` packets and bytes arriving per second |
| Buffer | Audio queued. Playback starts at 150 ms; above 400 ms it drops back to 150 ms (clock drift). |
| meter | Peak level, or `CLIP` when the stream hits full scale |
| Dropouts | Lost packets (sequence gaps), buffer underruns and overruns |

## Formats and link load

| # | Format | `eval`/s | KB/s on the wire |
| --- | --- | --- | --- |
| 0 | 44.1 kHz 16-bit (bypass) | 87 | 96 |
| 1 | 44.1 kHz 8-bit | 44 | 48 |
| 2 | 44.1 kHz IMA ADPCM | 22 | 24 |
| 3 | 22.05 kHz 16-bit | 44 | 48 |
| 4 | 22.05 kHz 8-bit | 22 | 24 |
| **5** | **22.05 kHz IMA ADPCM** (default) | **11** | **12** |
| 6 | 11.025 kHz 16-bit | 22 | 24 |
| 7 | 11.025 kHz 8-bit | 11 | 12 |
| 8 | 11.025 kHz IMA ADPCM | 6 | 6.1 |

A Playdate takes about 86 `eval`s a second (measured with the bypass), so every format plays.

## Protocol

Each packet is up to 1024 raw bytes inside `eval <length>\n<Lua bytecode>`. The bytecode is a call
`pdstream_feed("<packet>")`, built around the packet from a template that equals pdc's output
(`EVAL_HEAD` in `pdstream.py`; the tests compare it with pdc). The receiver's empty `main.lua`
starts the Lua runtime that `eval` runs in; C registers `pdstream_feed` and sets its update callback
once that runtime is up (`kEventInitLua`). `eval` doesn't work in apps from Catalog, so the receiver
has to be sideloaded.

`pdstream.py` sends `echo off` when it opens the port. A packet holds:

| Bytes | Field |
| --- | --- |
| 0 | Format number (`chain_formats` index) |
| 1 | Sequence number, wrapping at 256 |
| 2–3 | Offset of the first data byte within its unit (uint16 LE) |
| 4… | Stream bytes: 16-bit LE or 8-bit signed samples at the format's rate, or WAV-layout ADPCM blocks |

The other way, the receiver logs `pdstream format <n>` to its console, which goes out over USB, once
a second until the stream arrives in that format. `pdstream.py` switches its encoder and keeps the
sequence numbers running.

After a gap in the sequence numbers, the receiver skips to the next unit boundary and goes on.
For ADPCM that is the next block. A new format number restarts the decoder.

Why C: Lua can't feed PCM to the audio engine, so the `eval` packets go straight to a C function.

## Test

- `tests/run_tests.sh`: on the host, `pdstream.py` writes its `eval` commands for a test signal and
  the receiver's `packet.c` decodes them. Every format must equal `chain_render` bit for bit,
  decoding must recover after two dropped packets, and, with pdc on the PATH, the bytecode must
  equal pdc's.
- `tests/sim_e2e.sh` (headless Linux): the Simulator has no serial input, so the receiver replays
  recorded commands from `replay.bin` in its data folder, taking the packets out of the bytecode.
  The check records its output and compares it with `chain_render` (−82 dB residual).

## Not yet verified

- CPU load on the Playdate.
