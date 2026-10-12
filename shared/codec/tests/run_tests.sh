#!/usr/bin/env bash
# Builds the host tools and checks the codecs against ffmpeg, audioop, pdc and the QOA reference, and the chain's
# frequency response.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
build="$here/build"
mkdir -p "$build"
cc=${CC:-cc}
flags="-std=c11 -O2 -Wall -Wextra -Werror"
$cc $flags -o "$build/adpcm_wav" "$here/adpcm_wav.c" "$here/../ima_adpcm.c"
$cc $flags -o "$build/qoa_file" "$here/qoa_file.c" "$here/../qoa.c"
$cc $flags -o "$build/chain_render" "$here/chain_render.c" "$here/../format_chain.c" "$here/../ima_adpcm.c" \
    "$here/../qoa.c" -lm
$cc $flags -o "$build/stream_render" "$here/stream_render.c" "$here/../format_stream.c" "$here/../format_chain.c" \
    "$here/../ima_adpcm.c" "$here/../qoa.c" -lm
python3 "$here/check.py" "$build"
