#ifndef IMA_ADPCM_H
#define IMA_ADPCM_H

#include <stdint.h>

// IMA ADPCM in the WAV (format tag 0x11) layout: 4 bits per sample, each block starting with a
// header that holds the first sample verbatim plus the step index.
// Decoders disagree on rounding: the IMA reference (and Python's audioop) sums shifted steps, ffmpeg's
// adpcm_ima_wav multiplies. Which one Playdate uses is what probes/adpcm-rounding measures.
typedef enum { IMA_ROUNDING_SHIFT_ADD, IMA_ROUNDING_MULTIPLY } ImaRounding;

typedef struct {
    int16_t predictor;
    uint8_t index;
    ImaRounding rounding;
} ImaAdpcmState;

#define IMA_ADPCM_BLOCK_BYTES(samplesPerBlock) (4 + (samplesPerBlock) / 2)

uint8_t ima_adpcm_encode(ImaAdpcmState* state, int16_t sample);
int16_t ima_adpcm_decode(ImaAdpcmState* state, uint8_t nibble);

// Mono block: 4-byte header (first sample, step index, 0), then one nibble per remaining sample.
// pdc copies WAV blocks into .pda but swaps the nibble order; .pda data starts with a uint16 block size.
typedef enum { IMA_LAYOUT_WAV, IMA_LAYOUT_PLAYDATE } ImaLayout;

int ima_adpcm_encode_block(ImaAdpcmState* state, const int16_t* samples, int count, ImaLayout layout, uint8_t* out);
// count is how many samples the block holds.
void ima_adpcm_decode_block(const uint8_t* block, int count, ImaRounding rounding, ImaLayout layout, int16_t* out);

#endif
