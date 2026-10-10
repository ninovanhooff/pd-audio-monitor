#include "ima_adpcm.h"

static const int16_t stepTable[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,   253,   279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,   876,   963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

static const int8_t indexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

int16_t ima_adpcm_decode(ImaAdpcmState* state, uint8_t nibble) {
    int step = stepTable[state->index];
    int diff;
    if (state->rounding == IMA_ROUNDING_MULTIPLY) {
        diff = ((2 * (nibble & 7) + 1) * step) >> 3;
    } else {
        diff = step >> 3;
        if (nibble & 4) diff += step;
        if (nibble & 2) diff += step >> 1;
        if (nibble & 1) diff += step >> 2;
    }

    int predictor = state->predictor + ((nibble & 8) ? -diff : diff);
    if (predictor > 32767) predictor = 32767;
    if (predictor < -32768) predictor = -32768;
    state->predictor = (int16_t)predictor;

    int index = state->index + indexTable[nibble & 15];
    if (index < 0) index = 0;
    if (index > 88) index = 88;
    state->index = (uint8_t)index;
    return state->predictor;
}

uint8_t ima_adpcm_encode(ImaAdpcmState* state, int16_t sample) {
    int step = stepTable[state->index];
    int diff = sample - state->predictor;
    uint8_t nibble = 0;
    if (diff < 0) {
        nibble = 8;
        diff = -diff;
    }
    if (diff >= step) {
        nibble |= 4;
        diff -= step;
    }
    step >>= 1;
    if (diff >= step) {
        nibble |= 2;
        diff -= step;
    }
    step >>= 1;
    if (diff >= step) nibble |= 1;

    // Track the decoder's reconstruction, not the input, or encoder and decoder drift apart.
    ima_adpcm_decode(state, nibble);
    return nibble;
}

int ima_adpcm_encode_block(ImaAdpcmState* state, const int16_t* samples, int count, ImaLayout layout, uint8_t* out) {
    const int firstShift = layout == IMA_LAYOUT_WAV ? 0 : 4;
    state->predictor = samples[0];
    out[0] = (uint8_t)(samples[0] & 0xff);
    out[1] = (uint8_t)((uint16_t)samples[0] >> 8);
    out[2] = state->index;
    out[3] = 0;

    int bytes = 4;
    for (int i = 1; i < count; i += 2) {
        uint8_t first = ima_adpcm_encode(state, samples[i]);
        uint8_t second = (i + 1 < count) ? ima_adpcm_encode(state, samples[i + 1]) : 0;
        out[bytes++] = (uint8_t)(first << firstShift | second << (4 - firstShift));
    }
    return bytes;
}

void ima_adpcm_decode_block(const uint8_t* block, int count, ImaRounding rounding, ImaLayout layout, int16_t* out) {
    const int firstShift = layout == IMA_LAYOUT_WAV ? 0 : 4;
    ImaAdpcmState state = {
        .predictor = (int16_t)(block[0] | (block[1] << 8)),
        .index = block[2] > 88 ? 88 : block[2],
        .rounding = rounding,
    };
    out[0] = state.predictor;

    const uint8_t* data = block + 4;
    for (int i = 1; i < count; i += 2) {
        uint8_t byte = *data++;
        out[i] = ima_adpcm_decode(&state, byte >> firstShift & 15);
        if (i + 1 < count) out[i + 1] = ima_adpcm_decode(&state, byte >> (4 - firstShift) & 15);
    }
}
