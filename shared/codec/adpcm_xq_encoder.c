#include "adpcm_xq_encoder.h"

#include <stddef.h>

#include "adpcm_xq/adpcm-lib.h"

// The adpcm-xq command line's default depth.
#define LOOKAHEAD_DEPTH_SEARCHED 3

static void* create(const ChainFormat* format) {
    // The encoder wants 8n+1 samples a block.
    if (format->samplesPerBlock % 8 != 1) return NULL;
    return adpcm_create_context(1, 44100 / format->divisor, LOOKAHEAD_DEPTH_SEARCHED, NOISE_SHAPING_OFF);
}

static void encode(void* context, const int16_t* samples, int count, uint8_t* out) {
    size_t size = 0;
    adpcm_encode_block(context, out, &size, samples, count);
}

static void destroy(void* context) {
    adpcm_free_context(context);
}

const StreamBlockEncoder adpcm_xq_block_encoder = {create, encode, destroy};
