#ifndef FORMAT_CHAIN_H
#define FORMAT_CHAIN_H

#include <stdint.h>

#include "ima_adpcm.h"

// Turns 44.1 kHz mono input into what the same audio sounds like after shipping in a Playdate
// format: decimate to the format's rate, quantize with its codec, interpolate back to 44.1 kHz.
// Shift-add ADPCM rounding and linear interpolation are what probes/playback-probe measured.

typedef enum { CODEC_PCM16, CODEC_PCM8, CODEC_ADPCM } ChainCodec;

typedef struct {
    const char* name;
    int divisor;
    ChainCodec codec;
    int samplesPerBlock;
} ChainFormat;

extern const ChainFormat chain_formats[];
extern const int chain_format_count;
extern const int chain_default_format;

#define CHAIN_MAX_TAPS 127
#define CHAIN_HISTORY 128

typedef struct {
    const ChainFormat* format;
    int taps;
    int16_t fir[CHAIN_MAX_TAPS];
    int16_t history[CHAIN_HISTORY];
    int historyPos;
    int phase;
    int16_t previous;
    int16_t current;
    ImaAdpcmState adpcm;
    int blockPos;
} FormatChain;

void chain_init(FormatChain* chain, const ChainFormat* format);
int16_t chain_process(FormatChain* chain, int16_t input);

#endif
