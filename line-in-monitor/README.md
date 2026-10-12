# Line-in Monitor

A Playdate app that plays whatever arrives on the headset jack's **mic input** through the
**speaker**, after degrading it to a chosen Playdate audio format. Plug the Mac's audio output into
the Playdate and you hear your mix on the real speaker, as it would sound shipped in that format.
No driver, serial link or Mac software is needed.

## Hookup

```
Mac headphone out (TRS) ──► splitter "mic" socket ─┐
                                                  ├─ TRRS (CTIA) plug ──► Playdate headset jack
             (optional headphones) ──► "phones" ──┘
```

- Use a **TRRS headset splitter** (CTIA pinout, the common "phone headset to separate mic + headphones"
  adapter). A plain TRRS-to-TRRS cable from the Mac won't work, because it connects mic pin to mic pin.
- The headset mic input expects **mic level**, while line/headphone out is much hotter. Start with the
  Mac volume low and raise it while watching the peak level next to the meter; it shows `CLIP`
  instead when the input clips. If it distorts even at low volume, use a line-to-mic attenuator cable.
- **Unplug the USB cable while listening.** With USB attached, the headset input picks up
  significant noise; run the Playdate on battery.
- The app forces the headset input (`kMicInputHeadset`) because the jack's detection circuit doesn't
  always notice a splitter. It also keeps the speaker on even though something is plugged into the jack.

## Controls

The screen shows each setting with its controls as Playdate button glyphs.

| Input | Action |
| --- | --- |
| ⬅ ➡ | Format (default 22.05 kHz IMA ADPCM) |
| Ⓐ | Toggle quick A/B against the 44.1 kHz 16-bit bypass |
| Ⓑ | Output: speaker / headphones / both |
| ⬆ ⬇ 🎣 | Digital input gain (−30 to +30 dB) |

Formats: every combination of 44.1 / 22.05 / 11.025 kHz with 16-bit, 8-bit, IMA ADPCM and
[QOA](https://qoaformat.org); 44.1 kHz 16-bit is the bypass. Each format low-pass filters and decimates
to its rate, quantizes with its codec (ADPCM: the shift-add rounding measured in the Simulator,
Audacity's block sizes; QOA: the reference encoder, which holds the sound back by one 20-sample slice),
then upsamples by linear interpolation like the Playdate does (see `probes/playback-probe`). QOA is not
a system format: it shows how a game that decodes `.qoa` files itself would sound. The same chain runs on
the Mac with `shared/codec/tests/chain_render`, so you can preview the effect on headphones.

## Build

```bash
export PLAYDATE_SDK_PATH=~/Developer/PlaydateSDK
make            # device + simulator -> LineInMonitor.pdx
```

Needs SDK/firmware **3.1+** (`requestMicAccess`). The system asks for microphone permission on
first launch.

## Test

- `../shared/codec/tests/run_tests.sh`: codec and chain on the host.
- `tests/sim_e2e.sh` (headless Linux): feeds a sweep into the Simulator's mic through PulseAudio,
  records the output, and checks it against `chain_render` (residual must be below −60 dB; it
  measures about −83 dB, and −75 dB for QOA).

## Not yet verified on hardware

- The analog quality of the headset mic input: noise, frequency response, any AGC or high-pass
  filter, and the DC bias it puts on the mic pin. Its noise floor might dominate 16-bit formats.
- Latency on device. The ring buffer primes at 256 samples (about 6 ms), plus the system's own
  buffers.
- CPU headroom. The 11.025 kHz formats run a 127-tap FIR at the decimated rate (about 1.4 M MAC/s).
  The QOA encoder tries 16 scalefactors for every 20-sample slice, stopping a try early once it can't win:
  on the host that is 2 to 7 trial samples per sample, so at 44.1 kHz it costs the most.
