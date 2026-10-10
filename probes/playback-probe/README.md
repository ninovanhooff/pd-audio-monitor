# Playback probe

Measures the two system behaviours that `shared/codec/format_chain.c` imitates, and shows the
results on screen and in the console:

1. **ADPCM rounding.** It loads `probe.pda` (pdc output of `Source/probe.wav`, made with ffmpeg),
   decodes its blocks with both IMA roundings, and compares the result to the system's own
   `sample->decompress()`.
2. **Upsampling.** It plays a 22.05 kHz 16-bit noise sample through a `SamplePlayer` on its own
   channel, captures the rendered 44.1 kHz output with a custom effect, and fits a zero-order-hold
   model and a linear-interpolation model to it.

## Results

| | Simulator 3.1.2 (Linux) |
| --- | --- |
| ADPCM rounding | shift-add (IMA reference): 0 mismatches; multiply (ffmpeg): 41455 of 44100 |
| 22.05 → 44.1 kHz | linear interpolation, −90 dB residual (zero-order hold −6 dB) |

The Simulator stands in for the device: it has to decode the same ADPCM files the same way. The device
can't show the results, because its console output goes over the USB link that other serial traffic
disturbs. If the device ever disagrees, change `format_chain.c` to match.

While writing the probe, a `.pda` detail turned up: pdc copies the WAV's ADPCM blocks unchanged
except that it stores the **high nibble first**. `getData()` returns the data starting at the
uint16 block size. `IMA_LAYOUT_PLAYDATE` in `shared/codec` encodes that layout, and
`run_tests.sh` checks it against pdc.

## Build

```bash
export PLAYDATE_SDK_PATH=~/Developer/PlaydateSDK
make            # -> PlaybackProbe.pdx
```
