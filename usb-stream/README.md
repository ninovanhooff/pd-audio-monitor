# USB Stream

Plays Mac audio on the **Playdate's speaker** over the USB cable, encoded in a Playdate audio
format. The signal stays digital end to end, so there's none of the analog noise of
[`line-in-monitor`](../line-in-monitor/).

```
DAW / any app ──► BlackHole 2ch ──► pdstream.py ──USB serial `msg` lines──► USB Stream app ──► speaker
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

5. Build `USBStream.pdx` (`make -C usb-stream/receiver`, with `PLAYDATE_SDK_PATH` set) and
   sideload it. Start it on the Playdate, which stays connected over USB.
6. Run the streamer:

   ```bash
   ~/.venvs/pdstream/bin/python usb-stream/host/pdstream.py          # default 22.05 kHz IMA ADPCM
   ~/.venvs/pdstream/bin/python usb-stream/host/pdstream.py --help   # formats, --device, --port, --input file.wav
   usb-stream/host/dist/pdstream                                     # the bundle takes the same options
   ```

   If the serial port is busy, quit the Playdate Simulator; it also talks to the device over USB.
   If the peak stays at -99 dBFS, macOS is handing the capture silence: allow the app running the
   script (Terminal, iTerm, ...) under **System Settings → Privacy & Security → Microphone**, then
   restart that app.

## On the Playdate

| Row | Shows |
| --- | --- |
| Format ✛ | Format of the incoming stream. ⬅/➡ pick the sample rate, ⬆/⬇ the codec (more bits up: 16-bit, 8-bit, ADPCM); the Playdate asks `pdstream.py` to switch and shows `...` until it does. Both wrap around; a rate without the current codec gets the nearest one it has. `--format` sets the starting format |
| Output Ⓑ | Speaker / headphones / both |
| Link | Messages and bytes per second arriving |
| Buffer | Audio queued. Playback starts at 150 ms; above 400 ms it drops back to 150 ms (clock drift). |
| meter | Peak level, or `CLIP` when the stream hits full scale |
| Dropouts | Lost messages (sequence gaps), buffer underruns and overruns |

## Formats and link load

| # | Format | `msg`/s | KB/s on the wire |
| --- | --- | --- | --- |
| 0 | 44.1 kHz 16-bit (bypass): too much for the Playdate, `--out` only | 493 | 120 |
| 1 | 44.1 kHz IMA ADPCM | 122 | 30 |
| **2** | **22.05 kHz IMA ADPCM** (default) | **62** | **15** |
| 3 | 11.025 kHz IMA ADPCM | 31 | 7.6 |
| 4 | 22.05 kHz 16-bit | 247 | 60 |
| 5 | 22.05 kHz 8-bit | 123 | 30 |
| 6 | 11.025 kHz 8-bit | 62 | 15 |

A Playdate takes about 390 messages a second (measured with the bypass format), so the bypass
can't be streamed: `pdstream.py` refuses it and the d-pad skips it. The other formats play on a Playdate.

## Protocol

Each line is `msg <base64>\n`. A message's text can't carry raw bytes and is capped at about 251
characters, so each packet is at most 183 bytes, which is 244 base64 characters. A packet holds:

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

Why C and not Lua: `msg` works from C, and Lua can't feed PCM to the audio engine. A C+Lua
receiver taking `eval` payloads is the fallback if `msg` turns out too slow (`docs/approaches.md`).

## Test

- `tests/run_tests.sh`: on the host, `pdstream.py` writes lines for a test signal and the
  receiver's `packet.c` decodes them. Every format must equal `chain_render` bit for bit, line
  length must stay within the device limit, and decoding must recover after two dropped messages.
- `tests/sim_e2e.sh` (headless Linux): the Simulator has no serial input, so the receiver replays
  recorded lines from `replay.txt` in its data folder. The check records its output and compares
  it with `chain_render` (−82 dB residual).

## Not yet verified

- CPU load on the Playdate.
