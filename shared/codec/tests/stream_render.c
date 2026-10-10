// Host tool: 44.1 kHz mono s16le stdin -> stream encoder -> stream decoder -> stdout, to check that the
// split chain matches chain_render.
#include <stdio.h>
#include <stdlib.h>

#include "../format_stream.h"

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: stream_render <format> < in.s16le > out.s16le\n");
        return 64;
    }
    int index = atoi(argv[1]);
    if (index < 0 || index >= chain_format_count) return 64;

    static StreamEncoder encoder;
    static StreamDecoder decoder;
    stream_encoder_init(&encoder, &chain_formats[index]);
    stream_decoder_init(&decoder, &chain_formats[index]);
    static uint8_t unit[STREAM_MAX_UNIT_BYTES];
    static int16_t out[STREAM_MAX_OUTPUT];
    int16_t sample;
    while (fread(&sample, sizeof(sample), 1, stdin) == 1) {
        int bytes = stream_encode(&encoder, sample, unit);
        for (int i = 0; i < bytes; i++) {
            int count = stream_decode(&decoder, unit[i], out);
            fwrite(out, sizeof(int16_t), (size_t)count, stdout);
        }
    }
    return 0;
}
