# pd-audio-monitor

Tools for Playdate game composers to hear their music on the **Playdate's own speaker**, as it
would sound in a shipped audio format (default 22.05 kHz mono IMA ADPCM), while they work on a Mac or Windows PC.

| Folder | What | Status |
| --- | --- | --- |
| [`usb-stream/`](usb-stream/) | Mac (BlackHole) or Windows (VB-Cable) → `pdstream.py` → USB serial → PD Audio Monitor Receiver (Playdate app) → speaker, digital end to end | Bit-exact on the host; plays on hardware in every format |
| [`line-in-monitor/`](line-in-monitor/) | Playdate app: headset mic input → format emulation → speaker. Plug the Mac's audio out in with a splitter. | Builds; verified in the Simulator; noisy input on hardware |
| [`probes/playback-probe/`](probes/playback-probe/) | Playdate app that measures the system ADPCM rounding and upsampling the emulation copies | Simulator results in; run on device |
| [`shared/codec/`](shared/codec/) | C: IMA ADPCM (WAV and `.pda` layouts), [QOA](https://qoaformat.org), the format-emulation chain and its sender/receiver split, plus host tests against ffmpeg, `audioop`, pdc and the QOA reference | Tested |
| [`docs/approaches.md`](docs/approaches.md) | Analog line-in vs digital streaming (virtual device / process tap, USB serial `msg`/`eval`, Wi-Fi), with what we've measured | |
| [`docs/choosing-a-format.md`](docs/choosing-a-format.md) | Which music each sample rate, bit depth and codec degrades, and how to hear it | |

Planned: a macOS companion app or virtual output device instead of BlackHole + a script. See the docs.

## Quick start

```bash
export PLAYDATE_SDK_PATH=~/Developer/PlaydateSDK
make -C line-in-monitor                  # LineInMonitor.pdx
shared/codec/tests/run_tests.sh          # needs ffmpeg; audioop / PLAYDATE_SDK_PATH enable extra checks

# Preview a mix on the Mac as the monitor would play it (format numbers: run chain_render with no args)
ffmpeg -i mix.wav -ac 1 -ar 44100 -f s16le - | shared/codec/tests/build/chain_render 5 \
  | ffmpeg -f s16le -ar 44100 -ac 1 -i - preview.wav
```
