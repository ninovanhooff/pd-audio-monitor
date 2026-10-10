#ifndef FORMAT_STREAM_H
#define FORMAT_STREAM_H

#include <stdint.h>

#include "format_chain.h"

// The format chain split across a link: the sender decimates and encodes to the format's bytes, the
// receiver decodes and interpolates back to 44.1 kHz. Together they reproduce chain_process exactly,
// delayed by divisor - 1 samples, on each channel. A unit is one sample per channel for PCM (16-bit
// little-endian, or 8-bit signed) and one WAV-layout block per channel for ADPCM; a stereo unit holds the
// left channel's bytes, then the right's. Frames hold one sample per channel, interleaved.

#define STREAM_MAX_CHANNELS 2
#define STREAM_MAX_BLOCK_SAMPLES 2041
#define STREAM_MAX_UNIT_BYTES (STREAM_MAX_CHANNELS * IMA_ADPCM_BLOCK_BYTES(STREAM_MAX_BLOCK_SAMPLES))
#define STREAM_MAX_OUTPUT 2048 // frames

int stream_unit_bytes(const ChainFormat* format);

typedef struct {
    FormatChain chains[STREAM_MAX_CHANNELS];
    int16_t block[STREAM_MAX_CHANNELS][STREAM_MAX_BLOCK_SAMPLES];
    int count;
} StreamEncoder;

void stream_encoder_init(StreamEncoder* encoder, const ChainFormat* format);
// Takes one frame (the format's channels): writes a unit to out (room for STREAM_MAX_UNIT_BYTES) when it
// completes one; returns its size or 0.
int stream_encode(StreamEncoder* encoder, const int16_t* frame, uint8_t* out);

typedef struct {
    const ChainFormat* format;
    uint8_t unit[STREAM_MAX_UNIT_BYTES];
    int fill;
    int16_t decoded[STREAM_MAX_CHANNELS][STREAM_MAX_BLOCK_SAMPLES];
    int16_t previous[STREAM_MAX_CHANNELS];
    int16_t current[STREAM_MAX_CHANNELS];
} StreamDecoder;

void stream_decoder_init(StreamDecoder* decoder, const ChainFormat* format);
// Discards a partly received unit, e.g. after lost data.
void stream_decoder_resync(StreamDecoder* decoder);
// Writes 44.1 kHz frames to out (room for STREAM_MAX_OUTPUT frames) when byte completes a unit; returns how many.
int stream_decode(StreamDecoder* decoder, uint8_t byte, int16_t* out);

#endif
