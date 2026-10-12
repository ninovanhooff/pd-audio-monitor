// Host tool: 44.1 kHz s16le stdin (interleaved for stereo formats) -> stream encoder -> stream decoder -> stdout, to check that the
// split chain matches chain_render.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../adpcm_xq_encoder.h"
#include "../format_stream.h"

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2], "xq") != 0)) {
        fprintf(stderr, "usage: stream_render <format> [xq] < in.s16le > out.s16le (xq: encode ADPCM with ADPCM-XQ)\n");
        return 64;
    }
    int index = atoi(argv[1]);
    if (index < 0 || index >= chain_format_count) return 64;

    static StreamEncoder encoder;
    static StreamDecoder decoder;
    stream_encoder_init(&encoder, &chain_formats[index]);
    stream_decoder_init(&decoder, &chain_formats[index]);
    if (argc == 3 && !stream_encoder_use_block_encoder(&encoder, &adpcm_xq_block_encoder)) return 65;
    static uint8_t unit[STREAM_MAX_UNIT_BYTES];
    static int16_t out[STREAM_MAX_OUTPUT * STREAM_MAX_CHANNELS];
    int channels = chain_formats[index].channels;
    int16_t frame[STREAM_MAX_CHANNELS];
    while (fread(frame, sizeof(int16_t), (size_t)channels, stdin) == (size_t)channels) {
        int bytes = stream_encode(&encoder, frame, unit);
        for (int i = 0; i < bytes; i++) {
            int count = stream_decode(&decoder, unit[i], out);
            fwrite(out, sizeof(int16_t), (size_t)(count * channels), stdout);
        }
    }
    stream_encoder_release(&encoder);
    return 0;
}
