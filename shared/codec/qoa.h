#ifndef QOA_H
#define QOA_H

#include <stdint.h>

// QOA, the "Quite OK Audio" format (https://qoaformat.org), after the reference encoder and decoder by
// Dominic Szablewski (MIT: see qoa-LICENSE). A sign-sign LMS filter predicts each sample from the last four,
// and a 3-bit code quantizes the residual, scaled by one of 16 scalefactors per 20-sample slice: 3.2 bits a
// sample. A frame is an 8-byte header (channels, 24-bit rate, samples per channel, frame size), each
// channel's 16-byte LMS state, then 8-byte slices interleaved by channel, all big-endian. Files hold 256
// slices per frame (QOA_MAX_SLICES); the stream in format_stream.h uses shorter frames, which decode alike.
// Not a Playdate system format: a game would ship .qoa files and decode them itself.

#define QOA_SLICE_SAMPLES 20
#define QOA_SLICE_BYTES 8
#define QOA_LMS_BYTES 16
#define QOA_MAX_SLICES 256
#define QOA_FRAME_BYTES(channels, slices) (8 + (channels) * (QOA_LMS_BYTES + QOA_SLICE_BYTES * (slices)))

typedef struct {
    int16_t history[4];
    int32_t weights[4];
} QoaLms;

typedef struct {
    QoaLms lms;
    int scalefactor;
} QoaEncoder;

void qoa_encoder_init(QoaEncoder* encoder);
// Call before a frame's first slice, as qoa_encode_frame does.
void qoa_encoder_start_frame(QoaEncoder* encoder);
// Encodes count (1 to QOA_SLICE_SAMPLES) samples; returns the slice as a big-endian integer and writes what a
// decoder reconstructs from it to decoded.
uint64_t qoa_encode_slice(QoaEncoder* encoder, const int16_t* samples, int count, int16_t* decoded);

// samples[c] holds count samples of channel c; encoders holds one state per channel. Returns the frame's size,
// QOA_FRAME_BYTES(channels, slices), and writes it to out.
int qoa_encode_frame(QoaEncoder* encoders, int channels, int samplerate, const int16_t* const* samples, int count,
                     uint8_t* out);
// Decodes one channel of a frame that holds count samples per channel. The layout comes from the arguments, so a
// damaged header can't send it out of bounds.
void qoa_decode_channel(const uint8_t* frame, int channels, int channel, int count, int16_t* out);

#endif
