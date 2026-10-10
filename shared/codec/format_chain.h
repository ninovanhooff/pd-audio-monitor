#ifndef FORMAT_CHAIN_H
#define FORMAT_CHAIN_H

#include <stdint.h>

#include "ima_adpcm.h"

// Turns 44.1 kHz mono input into what the same audio sounds like after shipping in a Playdate
// format: decimate to the format's rate, quantize with its codec, interpolate back to 44.1 kHz.
// Playdate rounds ADPCM by shift-add and upsamples by linear interpolation (measured in the
// Simulator by probes/playback-probe).

typedef enum { CODEC_PCM16, CODEC_PCM8, CODEC_ADPCM } ChainCodec;

// name is for the host, deviceName for the Playdate's small screen. A stereo format runs one chain per
// channel; chain_process and the rest of this header handle a single channel.
typedef struct {
    const char* name;
    const char* deviceName;
    int divisor;
    ChainCodec codec;
    int samplesPerBlock;
    int channels;
} ChainFormat;

// Mono formats come first. No 44.1 kHz 16-bit stereo: it needs twice the USB link's capacity.
extern const ChainFormat chain_formats[];
extern const int chain_format_count;
extern const int chain_mono_format_count;
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
// The low-pass and decimation step alone: returns 1 when *output holds a new sample at the format's rate.
int chain_decimate(FormatChain* chain, int16_t input, int16_t* output);

#endif
