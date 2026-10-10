// Host tool: write/read mono IMA ADPCM WAV files with the shared codec, so tests can check it
// against ffmpeg in both directions.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ima_adpcm.h"

static ImaRounding rounding = IMA_ROUNDING_SHIFT_ADD;

static void put16(FILE* f, unsigned v) { fputc(v & 255, f), fputc(v >> 8 & 255, f); }
static void put32(FILE* f, unsigned v) { put16(f, v & 0xffff), put16(f, v >> 16); }
static unsigned get16(const unsigned char* p) { return p[0] | p[1] << 8; }
static unsigned get32(const unsigned char* p) { return get16(p) | get16(p + 2) << 16; }

static unsigned char* slurp(const char* path, long* size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* data = malloc(*size);
    if (fread(data, 1, *size, f) != (size_t)*size) *size = 0;
    fclose(f);
    return data;
}

static int encode(const char* rawPath, const char* wavPath, int samplesPerBlock, int rate) {
    long size;
    unsigned char* raw = slurp(rawPath, &size);
    if (!raw) return 1;
    int count = (int)(size / 2);
    int16_t* samples = malloc(count * sizeof(int16_t));
    for (int i = 0; i < count; i++) samples[i] = (int16_t)get16(raw + 2 * i);

    int blockBytes = IMA_ADPCM_BLOCK_BYTES(samplesPerBlock);
    int blocks = count / samplesPerBlock;
    FILE* f = fopen(wavPath, "wb");
    fwrite("RIFF", 1, 4, f), put32(f, 4 + 28 + 12 + 8 + blocks * blockBytes);
    fwrite("WAVEfmt ", 1, 8, f), put32(f, 20);
    put16(f, 0x11), put16(f, 1), put32(f, rate), put32(f, rate * blockBytes / samplesPerBlock);
    put16(f, blockBytes), put16(f, 4), put16(f, 2), put16(f, samplesPerBlock);
    fwrite("fact", 1, 4, f), put32(f, 4), put32(f, blocks * samplesPerBlock);
    fwrite("data", 1, 4, f), put32(f, blocks * blockBytes);

    ImaAdpcmState state = {0, 0, rounding};
    unsigned char* block = malloc(blockBytes);
    for (int b = 0; b < blocks; b++) {
        ima_adpcm_encode_block(&state, samples + b * samplesPerBlock, samplesPerBlock, IMA_LAYOUT_WAV, block);
        fwrite(block, 1, blockBytes, f);
    }
    fclose(f);
    return 0;
}

static int decode(const char* wavPath, const char* rawPath) {
    long size;
    unsigned char* wav = slurp(wavPath, &size);
    if (!wav || size < 12 || memcmp(wav, "RIFF", 4) || memcmp(wav + 8, "WAVE", 4)) return 1;
    int blockBytes = 0, samplesPerBlock = 0;
    for (long pos = 12; pos + 8 <= size;) {
        unsigned chunkSize = get32(wav + pos + 4);
        const unsigned char* body = wav + pos + 8;
        if (!memcmp(wav + pos, "fmt ", 4)) {
            if (get16(body) != 0x11 || get16(body + 2) != 1) return 2;
            blockBytes = get16(body + 12);
            samplesPerBlock = get16(body + 18);
        } else if (!memcmp(wav + pos, "data", 4) && blockBytes) {
            FILE* f = fopen(rawPath, "wb");
            int16_t* out = malloc(samplesPerBlock * sizeof(int16_t));
            for (unsigned off = 0; off + blockBytes <= chunkSize; off += blockBytes) {
                ima_adpcm_decode_block(body + off, samplesPerBlock, rounding, IMA_LAYOUT_WAV, out);
                for (int i = 0; i < samplesPerBlock; i++) put16(f, (uint16_t)out[i]);
            }
            fclose(f);
            return 0;
        }
        pos += 8 + chunkSize + (chunkSize & 1);
    }
    return 3;
}

// .pda: "Playdate AUD", 24-bit rate, format byte (4 = ADPCM mono), then for ADPCM a uint16 block size.
static int decodePda(const char* pdaPath, const char* rawPath) {
    long size;
    unsigned char* pda = slurp(pdaPath, &size);
    if (!pda || size < 18 || memcmp(pda, "Playdate AUD", 12) || pda[15] != 4) return 1;
    int blockBytes = (int)get16(pda + 16);
    int samplesPerBlock = (blockBytes - 4) * 2 + 1;
    FILE* f = fopen(rawPath, "wb");
    int16_t* out = malloc(samplesPerBlock * sizeof(int16_t));
    for (long off = 18; off + blockBytes <= size; off += blockBytes) {
        ima_adpcm_decode_block(pda + off, samplesPerBlock, rounding, IMA_LAYOUT_PLAYDATE, out);
        for (int i = 0; i < samplesPerBlock; i++) put16(f, (uint16_t)out[i]);
    }
    fclose(f);
    return 0;
}

// Headerless nibble stream, high nibble first, as Python's audioop reads and writes it.
static int stream(const char* mode, const char* inPath, const char* outPath) {
    long size;
    unsigned char* in = slurp(inPath, &size);
    if (!in) return 1;
    FILE* f = fopen(outPath, "wb");
    ImaAdpcmState state = {0, 0, rounding};
    if (!strcmp(mode, "encode-stream")) {
        for (long i = 0; i + 3 < size; i += 4) {
            uint8_t hi = ima_adpcm_encode(&state, (int16_t)get16(in + i));
            uint8_t lo = ima_adpcm_encode(&state, (int16_t)get16(in + i + 2));
            fputc(hi << 4 | lo, f);
        }
    } else {
        for (long i = 0; i < size; i++) {
            put16(f, (uint16_t)ima_adpcm_decode(&state, in[i] >> 4));
            put16(f, (uint16_t)ima_adpcm_decode(&state, in[i] & 15));
        }
    }
    fclose(f);
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "-m")) {
        rounding = IMA_ROUNDING_MULTIPLY;
        argc--, argv++;
    }
    if (argc == 4 && (!strcmp(argv[1], "encode-stream") || !strcmp(argv[1], "decode-stream")))
        return stream(argv[1], argv[2], argv[3]);
    if (argc == 6 && !strcmp(argv[1], "encode")) return encode(argv[2], argv[3], atoi(argv[4]), atoi(argv[5]));
    if (argc == 4 && !strcmp(argv[1], "decode")) return decode(argv[2], argv[3]);
    if (argc == 4 && !strcmp(argv[1], "decode-pda")) return decodePda(argv[2], argv[3]);
    fprintf(stderr, "usage (-m: multiply rounding): adpcm_wav [-m] encode in.raw out.wav samplesPerBlock rate\n"
                    "       adpcm_wav [-m] decode in.wav out.raw\n"
                    "       adpcm_wav [-m] decode-pda in.pda out.raw  (full blocks only)\n"
                    "       adpcm_wav [-m] encode-stream|decode-stream in out\n");
    return 64;
}
