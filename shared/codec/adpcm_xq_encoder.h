#ifndef ADPCM_XQ_ENCODER_H
#define ADPCM_XQ_ENCODER_H

#include "format_stream.h"

// ADPCM-XQ (David Bryant's look-ahead IMA ADPCM encoder, shared/codec/adpcm_xq) for a stream's ADPCM blocks.
// Host only: it allocates and uses doubles. The blocks stay standard IMA ADPCM in the WAV layout, so the
// receiver decodes them like the greedy encoder's.
extern const StreamBlockEncoder adpcm_xq_block_encoder;

#endif
