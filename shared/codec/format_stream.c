#include "format_stream.h"

#include <string.h>

int stream_unit_bytes(const ChainFormat* format) {
    switch (format->codec) {
    case CODEC_PCM16:
        return 2;
    case CODEC_PCM8:
        return 1;
    case CODEC_ADPCM:
        return IMA_ADPCM_BLOCK_BYTES(format->samplesPerBlock);
    }
    return 1;
}

static int16_t clamp16(int32_t value) {
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return (int16_t)value;
}

void stream_encoder_init(StreamEncoder* encoder, const ChainFormat* format) {
    chain_init(&encoder->chain, format);
    encoder->count = 0;
}

int stream_encode(StreamEncoder* encoder, int16_t input, uint8_t* out) {
    int16_t sample;
    if (!chain_decimate(&encoder->chain, input, &sample)) return 0;

    const ChainFormat* format = encoder->chain.format;
    switch (format->codec) {
    case CODEC_PCM16:
        out[0] = (uint8_t)(sample & 0xff);
        out[1] = (uint8_t)((uint16_t)sample >> 8);
        return 2;
    case CODEC_PCM8:
        out[0] = (uint8_t)(clamp16(sample + 128) >> 8);
        return 1;
    case CODEC_ADPCM:
        encoder->block[encoder->count++] = sample;
        if (encoder->count < format->samplesPerBlock) return 0;
        encoder->count = 0;
        return ima_adpcm_encode_block(&encoder->chain.adpcm, encoder->block, format->samplesPerBlock,
                                      IMA_LAYOUT_WAV, out);
    }
    return 0;
}

void stream_decoder_init(StreamDecoder* decoder, const ChainFormat* format) {
    memset(decoder, 0, sizeof(*decoder));
    decoder->format = format;
}

void stream_decoder_resync(StreamDecoder* decoder) {
    decoder->fill = 0;
}

static int interpolate(StreamDecoder* decoder, const int16_t* samples, int count, int16_t* out) {
    const int divisor = decoder->format->divisor;
    int written = 0;
    for (int i = 0; i < count; i++) {
        decoder->previous = decoder->current;
        decoder->current = samples[i];
        if (divisor == 1) {
            out[written++] = decoder->current;
            continue;
        }
        int32_t step = decoder->current - decoder->previous;
        for (int phase = 0; phase < divisor; phase++) {
            out[written++] = (int16_t)(decoder->previous + step * phase / divisor);
        }
    }
    return written;
}

int stream_decode(StreamDecoder* decoder, uint8_t byte, int16_t* out) {
    const ChainFormat* format = decoder->format;
    decoder->unit[decoder->fill++] = byte;
    if (decoder->fill < stream_unit_bytes(format)) return 0;
    decoder->fill = 0;

    switch (format->codec) {
    case CODEC_PCM16:
        decoder->decoded[0] = (int16_t)(decoder->unit[0] | decoder->unit[1] << 8);
        return interpolate(decoder, decoder->decoded, 1, out);
    case CODEC_PCM8:
        decoder->decoded[0] = (int16_t)((int8_t)decoder->unit[0] * 256);
        return interpolate(decoder, decoder->decoded, 1, out);
    case CODEC_ADPCM:
        ima_adpcm_decode_block(decoder->unit, format->samplesPerBlock, IMA_ROUNDING_SHIFT_ADD, IMA_LAYOUT_WAV,
                               decoder->decoded);
        return interpolate(decoder, decoder->decoded, format->samplesPerBlock, out);
    }
    return 0;
}
