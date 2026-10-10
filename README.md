# pd-audio-monitor

Tools for Playdate game composers to hear their music on the **Playdate's own speaker**, as it
would sound in a shipped audio format (default 22.05 kHz mono IMA ADPCM), while they work on a Mac.

| Folder | What | Status |
| --- | --- | --- |
| [`line-in-monitor/`](line-in-monitor/) | Playdate app: headset mic input → format emulation → speaker. Plug the Mac's audio out in with a splitter. | Builds; verified in the Simulator; untested on hardware |
| [`probes/playback-probe/`](probes/playback-probe/) | Playdate app that measures the system ADPCM rounding and upsampling the emulation copies | Simulator results in; run on device |
| [`shared/codec/`](shared/codec/) | C: IMA ADPCM (WAV and `.pda` layouts) and the format-emulation chain, plus host tests against ffmpeg, `audioop` and pdc | Tested |
| [`docs/approaches.md`](docs/approaches.md) | Analog line-in vs digital streaming (virtual device / process tap, USB serial `msg`/`eval`, Wi-Fi), with what we've measured | |

Planned: a serial receiver (C+Lua) and a macOS companion app/virtual output device. See the docs.

## Quick start

```bash
export PLAYDATE_SDK_PATH=~/Developer/PlaydateSDK
make -C line-in-monitor                  # LineInMonitor.pdx
shared/codec/tests/run_tests.sh          # needs ffmpeg; audioop / PLAYDATE_SDK_PATH enable extra checks

# Preview a mix on the Mac as the monitor would play it (format numbers: run chain_render with no args)
ffmpeg -i mix.wav -ac 1 -ar 44100 -f s16le - | shared/codec/tests/build/chain_render 2 \
  | ffmpeg -f s16le -ar 44100 -ac 1 -i - preview.wav
```
