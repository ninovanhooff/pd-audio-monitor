#include "format_chain.h"

#include <math.h>
#include <string.h>

// samplesPerBlock follows libsndfile (and so Audacity): 256/512/1024-byte blocks below 12 kHz,
// below 23 kHz and above.
const ChainFormat chain_formats[] = {
    {"44.1 kHz 16-bit (bypass)", 1, CODEC_PCM16, 0},
    {"44.1 kHz 8-bit", 1, CODEC_PCM8, 0},
    {"44.1 kHz IMA ADPCM", 1, CODEC_ADPCM, 2041},
    {"22.05 kHz 16-bit", 2, CODEC_PCM16, 0},
    {"22.05 kHz 8-bit", 2, CODEC_PCM8, 0},
    {"22.05 kHz IMA ADPCM", 2, CODEC_ADPCM, 1017},
    {"11.025 kHz 16-bit", 4, CODEC_PCM16, 0},
    {"11.025 kHz 8-bit", 4, CODEC_PCM8, 0},
    {"11.025 kHz IMA ADPCM", 4, CODEC_ADPCM, 505},
};
const int chain_format_count = sizeof(chain_formats) / sizeof(chain_formats[0]);
const int chain_default_format = 5;

static void design_lowpass(FormatChain* chain, int divisor) {
    chain->taps = divisor == 1 ? 0 : 32 * divisor - 1;
    if (chain->taps > CHAIN_MAX_TAPS) chain->taps = CHAIN_MAX_TAPS;
    if (chain->taps == 0) return;

    const float pi = 3.14159265f;
    const float cutoff = 0.45f / (float)divisor;
    const int middle = chain->taps / 2;
    float coefficients[CHAIN_MAX_TAPS];
    float sum = 0;
    for (int n = 0; n < chain->taps; n++) {
        float x = (float)(n - middle);
        float sinc = x == 0 ? 2 * cutoff : sinf(2 * pi * cutoff * x) / (pi * x);
        float phase = 2 * pi * (float)n / (float)(chain->taps - 1);
        float blackman = 0.42f - 0.5f * cosf(phase) + 0.08f * cosf(2 * phase);
        coefficients[n] = sinc * blackman;
        sum += coefficients[n];
    }
    for (int n = 0; n < chain->taps; n++) {
        chain->fir[n] = (int16_t)lrintf(coefficients[n] / sum * 32768.0f);
    }
}

void chain_init(FormatChain* chain, const ChainFormat* format) {
    memset(chain, 0, sizeof(*chain));
    chain->format = format;
    chain->adpcm.rounding = IMA_ROUNDING_SHIFT_ADD;
    design_lowpass(chain, format->divisor);
}

static int16_t clamp16(int32_t value) {
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return (int16_t)value;
}

static int16_t filtered(const FormatChain* chain) {
    int64_t sum = 0;
    int pos = chain->historyPos;
    for (int n = 0; n < chain->taps; n++) {
        pos = (pos - 1) & (CHAIN_HISTORY - 1);
        sum += (int32_t)chain->fir[n] * chain->history[pos];
    }
    return clamp16((int32_t)((sum + 16384) >> 15));
}

static int16_t quantize(FormatChain* chain, int16_t sample) {
    switch (chain->format->codec) {
    case CODEC_PCM16:
        return sample;
    case CODEC_PCM8:
        return (int16_t)(clamp16(sample + 128) >> 8 << 8);
    case CODEC_ADPCM:
        if (chain->blockPos == 0) {
            chain->adpcm.predictor = sample;
        } else {
            ima_adpcm_encode(&chain->adpcm, sample);
            sample = chain->adpcm.predictor;
        }
        if (++chain->blockPos == chain->format->samplesPerBlock) chain->blockPos = 0;
        return sample;
    }
    return sample;
}

int chain_decimate(FormatChain* chain, int16_t input, int16_t* output) {
    const int divisor = chain->format->divisor;
    if (divisor == 1) {
        *output = input;
        return 1;
    }
    chain->history[chain->historyPos] = input;
    chain->historyPos = (chain->historyPos + 1) & (CHAIN_HISTORY - 1);
    if (++chain->phase < divisor) return 0;
    chain->phase = 0;
    *output = filtered(chain);
    return 1;
}

int16_t chain_process(FormatChain* chain, int16_t input) {
    int16_t decimated;
    if (chain_decimate(chain, input, &decimated)) {
        chain->previous = chain->current;
        chain->current = quantize(chain, decimated);
    }
    const int divisor = chain->format->divisor;
    if (divisor == 1) return chain->current;
    int32_t step = chain->current - chain->previous;
    return (int16_t)(chain->previous + step * chain->phase / divisor);
}
