// ctypes entry points for pdstream.py, so the host encodes with the same code the chain tests cover.
#include <stdlib.h>

#include "adpcm_xq_encoder.h"
#include "format_stream.h"

// adpcm_xq: encode ADPCM formats with ADPCM-XQ; other formats ignore it.
StreamEncoder* pdstream_new(int format, int adpcm_xq) {
    if (format < 0 || format >= chain_format_count) return NULL;
    StreamEncoder* encoder = malloc(sizeof(StreamEncoder));
    if (!encoder) return NULL;
    stream_encoder_init(encoder, &chain_formats[format]);
    if (adpcm_xq && chain_formats[format].codec == CODEC_ADPCM && !stream_encoder_use_block_encoder(encoder, &adpcm_xq_block_encoder)) {
        free(encoder);
        return NULL;
    }
    return encoder;
}

void pdstream_free(StreamEncoder* encoder) {
    if (!encoder) return;
    stream_encoder_release(encoder);
    free(encoder);
}

int pdstream_is_adpcm(int format) {
    return chain_formats[format].codec == CODEC_ADPCM;
}

// samples holds count interleaved frames of the format's channels; out needs room for
// count * 2 * channels + STREAM_MAX_UNIT_BYTES bytes.
int pdstream_encode(StreamEncoder* encoder, const int16_t* samples, int count, uint8_t* out) {
    int channels = encoder->chains[0].format->channels;
    int written = 0;
    for (int i = 0; i < count; i++) written += stream_encode(encoder, samples + i * channels, out + written);
    return written;
}

int pdstream_format_channels(int format) {
    return chain_formats[format].channels;
}

int pdstream_unit_bytes(int format) {
    return stream_unit_bytes(&chain_formats[format]);
}

int pdstream_format_count(void) {
    return chain_format_count;
}

const char* pdstream_format_name(int format) {
    return chain_formats[format].name;
}

int pdstream_default_format(void) {
    return chain_default_format;
}
