// Derived from the QOA reference implementation, Copyright (c) 2023 Dominic Szablewski (MIT: see qoa-LICENSE).
#include "qoa.h"

#include <string.h>

// Quantized residual -8..8 to a code; zero shares code 0 with the lowest positive value.
static const int8_t quantTable[17] = {7, 7, 7, 5, 5, 3, 3, 1, 0, 0, 2, 2, 4, 4, 6, 6, 6};

// 1 / round(pow(s + 1, 2.75)) in .16 fixed point: the 16 scalefactors' reciprocals.
static const int32_t reciprocalTable[16] = {65536, 9363, 3121, 1457, 781, 475, 311, 216,
                                            156,   117,  90,   71,   57,  47,  39,  32};

// round(scalefactor * {0.75, -0.75, 2.5, -2.5, 4.5, -4.5, 7, -7}), ties away from zero.
static const int16_t dequantTable[16][8] = {
    {1, -1, 3, -3, 5, -5, 7, -7},
    {5, -5, 18, -18, 32, -32, 49, -49},
    {16, -16, 53, -53, 95, -95, 147, -147},
    {34, -34, 113, -113, 203, -203, 315, -315},
    {63, -63, 210, -210, 378, -378, 588, -588},
    {104, -104, 345, -345, 621, -621, 966, -966},
    {158, -158, 528, -528, 950, -950, 1477, -1477},
    {228, -228, 760, -760, 1368, -1368, 2128, -2128},
    {316, -316, 1053, -1053, 1895, -1895, 2947, -2947},
    {422, -422, 1405, -1405, 2529, -2529, 3934, -3934},
    {548, -548, 1828, -1828, 3290, -3290, 5117, -5117},
    {696, -696, 2320, -2320, 4176, -4176, 6496, -6496},
    {868, -868, 2893, -2893, 5207, -5207, 8099, -8099},
    {1064, -1064, 3548, -3548, 6386, -6386, 9933, -9933},
    {1286, -1286, 4288, -4288, 7718, -7718, 12005, -12005},
    {1536, -1536, 5120, -5120, 9216, -9216, 14336, -14336},
};

// Sums wrap like the reference's 32-bit ints: weights can outgrow 16 bits within a frame.
static int lms_predict(const QoaLms* lms) {
    uint32_t sum = 0;
    for (int i = 0; i < 4; i++) sum += (uint32_t)lms->weights[i] * (uint32_t)lms->history[i];
    return (int)sum >> 13;
}

static void lms_update(QoaLms* lms, int sample, int residual) {
    const int delta = residual >> 4;
    for (int i = 0; i < 4; i++) lms->weights[i] += lms->history[i] < 0 ? -delta : delta;
    for (int i = 0; i < 3; i++) lms->history[i] = lms->history[i + 1];
    lms->history[3] = (int16_t)sample;
}

static int clamp16(int value) {
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return value;
}

// Divides by the scalefactor, rounding away from zero so that no small residual quantizes to zero.
static int scale_residual(int residual, int scalefactor) {
    const int n = (int)((uint32_t)residual * (uint32_t)reciprocalTable[scalefactor] + (1 << 15)) >> 16;
    return n + ((residual > 0) - (residual < 0)) - ((n > 0) - (n < 0));
}

static void put_u64(uint8_t* out, uint64_t value) {
    for (int i = 0; i < 8; i++) out[i] = (uint8_t)(value >> (56 - 8 * i));
}

static uint64_t get_u64(const uint8_t* in) {
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) value = value << 8 | in[i];
    return value;
}

void qoa_encoder_init(QoaEncoder* encoder) {
    memset(encoder, 0, sizeof(*encoder));
    encoder->lms.weights[2] = -(1 << 13);
    encoder->lms.weights[3] = 1 << 14;
}

void qoa_encoder_start_frame(QoaEncoder* encoder) {
    // The header holds 16-bit weights: narrow them as the decoder will see them. Each frame also
    // restarts the search at scalefactor 0, as the reference encoder does; ties go to the first tried.
    for (int i = 0; i < 4; i++) encoder->lms.weights[i] = (int16_t)encoder->lms.weights[i];
    encoder->scalefactor = 0;
}

uint64_t qoa_encode_slice(QoaEncoder* encoder, const int16_t* samples, int count, int16_t* decoded) {
    uint64_t bestRank = UINT64_MAX, bestSlice = 0;
    QoaLms bestLms = encoder->lms;
    int bestScalefactor = 0;
    int16_t bestDecoded[QOA_SLICE_SAMPLES] = {0};

    for (int attempt = 0; attempt < 16; attempt++) {
        // Neighbouring slices tend to share a scalefactor, so try the last one's first.
        const int scalefactor = (attempt + encoder->scalefactor) & 15;
        QoaLms lms = encoder->lms;
        uint64_t slice = (uint64_t)scalefactor;
        uint64_t rank = 0;
        int16_t trial[QOA_SLICE_SAMPLES];

        for (int i = 0; i < count; i++) {
            const int predicted = lms_predict(&lms);
            int scaled = scale_residual(samples[i] - predicted, scalefactor);
            scaled = scaled < -8 ? -8 : scaled > 8 ? 8 : scaled;
            const int code = quantTable[scaled + 8];
            const int dequantized = dequantTable[scalefactor][code];
            const int reconstructed = clamp16(predicted + dequantized);

            // Large weights cost extra, which keeps the filter from popping on awkward input.
            uint32_t norm = 0;
            for (int k = 0; k < 4; k++) norm += (uint32_t)lms.weights[k] * (uint32_t)lms.weights[k];
            int penalty = ((int)norm >> 18) - 0x8ff;
            if (penalty < 0) penalty = 0;

            const int64_t error = samples[i] - reconstructed;
            rank += (uint64_t)(error * error) + (uint64_t)(penalty * penalty);
            if (rank > bestRank) break;

            lms_update(&lms, reconstructed, dequantized);
            slice = slice << 3 | (uint64_t)code;
            trial[i] = (int16_t)reconstructed;
        }

        if (rank < bestRank) {
            bestRank = rank;
            bestSlice = slice;
            bestLms = lms;
            bestScalefactor = scalefactor;
            memcpy(bestDecoded, trial, (size_t)count * sizeof(int16_t));
        }
    }

    encoder->lms = bestLms;
    encoder->scalefactor = bestScalefactor;
    memcpy(decoded, bestDecoded, (size_t)count * sizeof(int16_t));
    // A short slice (the end of a file) leaves its unused codes zero at the bottom.
    return bestSlice << ((QOA_SLICE_SAMPLES - count) * 3);
}

int qoa_encode_frame(QoaEncoder* encoders, int channels, int samplerate, const int16_t* const* samples, int count,
                     uint8_t* out) {
    const int slices = (count + QOA_SLICE_SAMPLES - 1) / QOA_SLICE_SAMPLES;
    const int size = QOA_FRAME_BYTES(channels, slices);
    put_u64(out, (uint64_t)channels << 56 | (uint64_t)samplerate << 32 | (uint64_t)count << 16 | (uint64_t)size);

    int pos = 8;
    for (int c = 0; c < channels; c++) {
        qoa_encoder_start_frame(&encoders[c]);
        for (int i = 0; i < 4; i++, pos += 2) {
            out[pos] = (uint8_t)((uint16_t)encoders[c].lms.history[i] >> 8);
            out[pos + 1] = (uint8_t)(encoders[c].lms.history[i] & 0xff);
        }
        for (int i = 0; i < 4; i++, pos += 2) {
            out[pos] = (uint8_t)((uint16_t)encoders[c].lms.weights[i] >> 8);
            out[pos + 1] = (uint8_t)(encoders[c].lms.weights[i] & 0xff);
        }
    }

    for (int first = 0; first < count; first += QOA_SLICE_SAMPLES) {
        const int length = count - first < QOA_SLICE_SAMPLES ? count - first : QOA_SLICE_SAMPLES;
        for (int c = 0; c < channels; c++) {
            int16_t decoded[QOA_SLICE_SAMPLES];
            put_u64(out + pos, qoa_encode_slice(&encoders[c], samples[c] + first, length, decoded));
            pos += QOA_SLICE_BYTES;
        }
    }
    return size;
}

void qoa_decode_channel(const uint8_t* frame, int channels, int channel, int count, int16_t* out) {
    QoaLms lms;
    const uint8_t* state = frame + 8 + channel * QOA_LMS_BYTES;
    for (int i = 0; i < 4; i++) {
        lms.history[i] = (int16_t)(state[2 * i] << 8 | state[2 * i + 1]);
        lms.weights[i] = (int16_t)(state[8 + 2 * i] << 8 | state[8 + 2 * i + 1]);
    }

    const uint8_t* slices = frame + 8 + channels * QOA_LMS_BYTES;
    for (int first = 0; first < count; first += QOA_SLICE_SAMPLES) {
        uint64_t bits = get_u64(slices + ((first / QOA_SLICE_SAMPLES) * channels + channel) * QOA_SLICE_BYTES);
        const int scalefactor = (int)(bits >> 60);
        bits <<= 4;
        const int end = count - first < QOA_SLICE_SAMPLES ? count : first + QOA_SLICE_SAMPLES;
        for (int i = first; i < end; i++, bits <<= 3) {
            const int predicted = lms_predict(&lms);
            const int dequantized = dequantTable[scalefactor][bits >> 61];
            out[i] = (int16_t)clamp16(predicted + dequantized);
            lms_update(&lms, out[i], dequantized);
        }
    }
}
