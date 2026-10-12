# CLAUDE.md

Tools for hearing music on the Playdate speaker in a shipped audio format. Start with `README.md`;
the design notes and measured facts are in `docs/approaches.md`.

## Layout

One folder per component; Playdate apps are plain C SDK projects (`Makefile` + `Source/` + `src/`)
that compile `shared/codec` sources by absolute path (the SDK's `common.mk` places objects at
`build/<source path>`, and `../` would escape `build/`).

## Build / test

```bash
export PLAYDATE_SDK_PATH=~/Developer/PlaydateSDK   # Linux SDK: download.panic.com/playdate_sdk/Linux/
make -C line-in-monitor
make -C probes/playback-probe
make -C usb-stream/receiver && make -C usb-stream/host   # Playdate app; libpdstream for pdstream.py
shared/codec/tests/run_tests.sh       # ffmpeg required; audioop (pip install audioop-lts) and pdc optional
usb-stream/tests/run_tests.sh         # ffmpeg, numpy
line-in-monitor/tests/sim_e2e.sh      # headless Linux: Xvfb, xdotool, ImageMagick, pulseaudio, numpy
usb-stream/tests/sim_e2e.sh           # same, replaying recorded packets (the Simulator has no serial input)
scripts/quickinstall.sh -i line-in-monitor   # macOS: build, copy to a USB-connected Playdate, run
```

Device builds need `arm-none-eabi-gcc` (on Ubuntu: `gcc-arm-none-eabi libnewlib-arm-none-eabi`).

## Tests

- `line-in-monitor/tests/sim_e2e.sh` is flaky and disabled: it exits early unless `RUN_FLAKY=1`. Don't
  re-enable it, and disable any other line-in-monitor test that turns out flaky.
- usb-stream tests must be deterministic. A flaky usb-stream test is not allowed: fix it or remove it
  before merging; never skip or retry around it.

## Gotchas

- No `snprintf`/stdio in device code: newlib's stdio pulls in syscalls the device doesn't provide
  (`_write`, `_sbrk`, ... fail to link). Use `pd->system->formatString`/`vaFormatString`.
- The device FPU is single precision: keep `double` out of hot paths.
- Button glyphs (Ⓐ Ⓑ ✛ ⬆ ➡ ⬇ ⬅ 🎣 ⊙ 🔒 🟨, drawn with `kUTF8Encoding`) are complete only in
  `Asheville-Sans-14-Light`. Asheville Bold has none of them, and the Roobert fonts lack most.
- The packet layout and the `eval` bytecode template live in both `usb-stream/receiver/src/packet.{h,c}` and
  `usb-stream/host/pdstream.py`.
- `requestMicAccess` (SDK 3.1) must be called before `setMicCallback`.
- Simulator first run shows dialogs and the mic prompt; `sim_e2e.sh` seeds
  `Disk/Data/<bundleID>/Permissions` and the Simulator ini to skip them.

## Style

Comments are rare and short: only a non-obvious why, an invariant, an external format, or a
failure being guarded against. Describe current behaviour, never history.
