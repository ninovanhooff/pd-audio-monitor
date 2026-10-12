// Host tool: write and read .qoa files (256 slices per frame, as the format asks) with the shared codec, so tests
// can check it against the reference encoder and an independent decoder.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../qoa.h"

#define MAX_CHANNELS 2

static unsigned char* slurp(const char* path, long* size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* data = malloc((size_t)*size);
    if (fread(data, 1, (size_t)*size, f) != (size_t)*size) *size = 0;
    fclose(f);
    return data;
}

static void put32(FILE* f, unsigned v) {
    for (int shift = 24; shift >= 0; shift -= 8) fputc((int)(v >> shift & 255), f);
}

static unsigned get(const unsigned char* p, int bytes) {
    unsigned v = 0;
    for (int i = 0; i < bytes; i++) v = v << 8 | p[i];
    return v;
}

// in.raw holds interleaved s16le frames.
static int encode(const char* rawPath, const char* qoaPath, int rate, int channels) {
    long size;
    unsigned char* raw = slurp(rawPath, &size);
    if (!raw || channels < 1 || channels > MAX_CHANNELS) return 1;
    const int count = (int)(size / 2 / channels);
    int16_t* samples[MAX_CHANNELS];
    for (int c = 0; c < channels; c++) {
        samples[c] = malloc((size_t)count * sizeof(int16_t));
        for (int i = 0; i < count; i++) {
            const unsigned char* at = raw + 2 * (i * channels + c);
            samples[c][i] = (int16_t)(at[0] | at[1] << 8);
        }
    }

    FILE* f = fopen(qoaPath, "wb");
    fwrite("qoaf", 1, 4, f);
    put32(f, (unsigned)count);
    QoaEncoder encoders[MAX_CHANNELS];
    for (int c = 0; c < channels; c++) qoa_encoder_init(&encoders[c]);
    static unsigned char frame[QOA_FRAME_BYTES(MAX_CHANNELS, QOA_MAX_SLICES)];
    for (int first = 0; first < count; first += QOA_MAX_SLICES * QOA_SLICE_SAMPLES) {
        const int frameSamples = QOA_MAX_SLICES * QOA_SLICE_SAMPLES;
        const int length = count - first < frameSamples ? count - first : frameSamples;
        const int16_t* part[MAX_CHANNELS];
        for (int c = 0; c < channels; c++) part[c] = samples[c] + first;
        fwrite(frame, 1, (size_t)qoa_encode_frame(encoders, channels, rate, part, length, frame), f);
    }
    fclose(f);
    return 0;
}

static int decode(const char* qoaPath, const char* rawPath) {
    long size;
    unsigned char* qoa = slurp(qoaPath, &size);
    if (!qoa || size < 16 || memcmp(qoa, "qoaf", 4)) return 1;
    FILE* f = fopen(rawPath, "wb");
    static int16_t out[MAX_CHANNELS][QOA_MAX_SLICES * QOA_SLICE_SAMPLES];
    for (long pos = 8; pos + 8 <= size;) {
        const int channels = (int)qoa[pos], count = (int)get(qoa + pos + 4, 2), frameSize = (int)get(qoa + pos + 6, 2);
        if (channels < 1 || channels > MAX_CHANNELS || count > (int)sizeof(out[0]) / 2 || pos + frameSize > size ||
            frameSize != QOA_FRAME_BYTES(channels, (count + QOA_SLICE_SAMPLES - 1) / QOA_SLICE_SAMPLES))
            return 2;
        for (int c = 0; c < channels; c++) qoa_decode_channel(qoa + pos, channels, c, count, out[c]);
        for (int i = 0; i < count; i++) {
            for (int c = 0; c < channels; c++) {
                fputc(out[c][i] & 255, f);
                fputc((out[c][i] >> 8) & 255, f);
            }
        }
        pos += frameSize;
    }
    fclose(f);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 6 && !strcmp(argv[1], "encode")) return encode(argv[2], argv[3], atoi(argv[4]), atoi(argv[5]));
    if (argc == 4 && !strcmp(argv[1], "decode")) return decode(argv[2], argv[3]);
    fprintf(stderr, "usage: qoa_file encode in.raw out.qoa rate channels  (in.raw: interleaved s16le)\n"
                    "       qoa_file decode in.qoa out.raw\n");
    return 64;
}
