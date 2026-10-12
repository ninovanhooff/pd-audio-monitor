#include "format_stream.h"

#include <string.h>

static int channel_unit_bytes(const ChainFormat* format) {
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

int stream_unit_bytes(const ChainFormat* format) {
    return format->channels * channel_unit_bytes(format);
}

static int16_t clamp16(int32_t value) {
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return (int16_t)value;
}

void stream_encoder_init(StreamEncoder* encoder, const ChainFormat* format) {
    for (int c = 0; c < format->channels; c++) chain_init(&encoder->chains[c], format);
    encoder->count = 0;
    encoder->blockEncoder = NULL;
    memset(encoder->blockContext, 0, sizeof(encoder->blockContext));
}

int stream_encoder_use_block_encoder(StreamEncoder* encoder, const StreamBlockEncoder* blockEncoder) {
    const ChainFormat* format = encoder->chains[0].format;
    if (format->codec != CODEC_ADPCM || encoder->count != 0) return 0;
    stream_encoder_release(encoder);
    for (int c = 0; c < format->channels; c++) {
        encoder->blockContext[c] = blockEncoder->create(format);
        if (!encoder->blockContext[c]) {
            encoder->blockEncoder = blockEncoder;
            stream_encoder_release(encoder);
            return 0;
        }
    }
    encoder->blockEncoder = blockEncoder;
    return 1;
}

void stream_encoder_release(StreamEncoder* encoder) {
    for (int c = 0; c < STREAM_MAX_CHANNELS; c++) {
        if (encoder->blockContext[c]) encoder->blockEncoder->destroy(encoder->blockContext[c]);
        encoder->blockContext[c] = NULL;
    }
    encoder->blockEncoder = NULL;
}

int stream_encode(StreamEncoder* encoder, const int16_t* frame, uint8_t* out) {
    const ChainFormat* format = encoder->chains[0].format;
    int16_t sample[STREAM_MAX_CHANNELS];
    int ready = 0;
    for (int c = 0; c < format->channels; c++) ready = chain_decimate(&encoder->chains[c], frame[c], &sample[c]);
    if (!ready) return 0;

    int size = 0;
    switch (format->codec) {
    case CODEC_PCM16:
        for (int c = 0; c < format->channels; c++) {
            out[size++] = (uint8_t)(sample[c] & 0xff);
            out[size++] = (uint8_t)((uint16_t)sample[c] >> 8);
        }
        return size;
    case CODEC_PCM8:
        for (int c = 0; c < format->channels; c++) out[size++] = (uint8_t)(clamp16(sample[c] + 128) >> 8);
        return size;
    case CODEC_ADPCM:
        for (int c = 0; c < format->channels; c++) encoder->block[c][encoder->count] = sample[c];
        if (++encoder->count < format->samplesPerBlock) return 0;
        encoder->count = 0;
        for (int c = 0; c < format->channels; c++) {
            if (encoder->blockEncoder) {
                encoder->blockEncoder->encode(encoder->blockContext[c], encoder->block[c], format->samplesPerBlock,
                                              out + size);
                size += IMA_ADPCM_BLOCK_BYTES(format->samplesPerBlock);
            } else {
                size += ima_adpcm_encode_block(&encoder->chains[c].adpcm, encoder->block[c], format->samplesPerBlock,
                                               IMA_LAYOUT_WAV, out + size);
            }
        }
        return size;
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

// Interpolates decoded[][0..count) up to 44.1 kHz into interleaved frames.
static int interpolate(StreamDecoder* decoder, int count, int16_t* out) {
    const int divisor = decoder->format->divisor, channels = decoder->format->channels;
    int written = 0;
    for (int i = 0; i < count; i++) {
        for (int c = 0; c < channels; c++) {
            decoder->previous[c] = decoder->current[c];
            decoder->current[c] = decoder->decoded[c][i];
        }
        if (divisor == 1) {
            for (int c = 0; c < channels; c++) out[written++] = decoder->current[c];
            continue;
        }
        for (int phase = 0; phase < divisor; phase++) {
            for (int c = 0; c < channels; c++) {
                int32_t step = decoder->current[c] - decoder->previous[c];
                out[written++] = (int16_t)(decoder->previous[c] + step * phase / divisor);
            }
        }
    }
    return written / channels;
}

int stream_decode(StreamDecoder* decoder, uint8_t byte, int16_t* out) {
    const ChainFormat* format = decoder->format;
    decoder->unit[decoder->fill++] = byte;
    if (decoder->fill < stream_unit_bytes(format)) return 0;
    decoder->fill = 0;

    int samples = 1;
    for (int c = 0; c < format->channels; c++) {
        const uint8_t* unit = decoder->unit + c * channel_unit_bytes(format);
        switch (format->codec) {
        case CODEC_PCM16:
            decoder->decoded[c][0] = (int16_t)(unit[0] | unit[1] << 8);
            break;
        case CODEC_PCM8:
            decoder->decoded[c][0] = (int16_t)((int8_t)unit[0] * 256);
            break;
        case CODEC_ADPCM:
            ima_adpcm_decode_block(unit, format->samplesPerBlock, IMA_ROUNDING_SHIFT_ADD, IMA_LAYOUT_WAV,
                                   decoder->decoded[c]);
            samples = format->samplesPerBlock;
            break;
        }
    }
    return interpolate(decoder, samples, out);
}
