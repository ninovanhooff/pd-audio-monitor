#include <math.h>
#include <stdarg.h>
#include <string.h>

#include "ima_adpcm.h"
#include "pd_api.h"

// Measures two things format_chain.c has to guess: which IMA ADPCM rounding the system decoder
// uses, and how a 22.05 kHz sample is upsampled to the 44.1 kHz output.

static PlaydateAPI* pd;
static LCDFont* font;

#define MAX_LINES 12
static char* lines[MAX_LINES];
static int lineCount;

static void report(const char* format, ...) {
    va_list args;
    va_start(args, format);
    char* text = NULL;
    pd->system->vaFormatString(&text, format, args);
    va_end(args);
    if (!text) return;
    pd->system->logToConsole("%s", text);
    if (lineCount < MAX_LINES) {
        lines[lineCount++] = text;
    } else {
        pd->system->realloc(text, 0);
    }
}

// MARK: ADPCM rounding

static int countMismatches(const uint8_t* adpcm, uint32_t adpcmBytes, int blockBytes, ImaRounding rounding,
                           const int16_t* expected, int expectedCount) {
    const int samplesPerBlock = (blockBytes - 4) * 2 + 1;
    int16_t* decoded = pd->system->realloc(NULL, (size_t)samplesPerBlock * sizeof(int16_t));
    int mismatches = 0, position = 0;
    for (uint32_t offset = 0; offset + (uint32_t)blockBytes <= adpcmBytes && position < expectedCount;
         offset += (uint32_t)blockBytes) {
        ima_adpcm_decode_block(adpcm + offset, samplesPerBlock, rounding, IMA_LAYOUT_PLAYDATE, decoded);
        for (int i = 0; i < samplesPerBlock && position < expectedCount; i++, position++) {
            if (decoded[i] != expected[position]) mismatches++;
        }
    }
    pd->system->realloc(decoded, 0);
    return mismatches;
}

static void probeAdpcmRounding(void) {
    AudioSample* sample = pd->sound->sample->load("probe");
    if (!sample) {
        report("ADPCM: can't load probe.pda");
        return;
    }
    uint8_t* data;
    SoundFormat format;
    uint32_t rate, bytes;
    pd->sound->sample->getData(sample, &data, &format, &rate, &bytes);
    if (format != kSoundADPCMMono || bytes < 2) {
        report("ADPCM: unexpected format %d", format);
        return;
    }
    const int blockBytes = data[0] | data[1] << 8;
    const uint32_t adpcmBytes = bytes - 2;
    uint8_t* adpcm = pd->system->realloc(NULL, adpcmBytes);
    memcpy(adpcm, data + 2, adpcmBytes);
    report("ADPCM: %d Hz, %d-byte blocks, %d bytes", (int)rate, blockBytes, (int)adpcmBytes);

    if (blockBytes > 4 && pd->sound->sample->decompress(sample)) {
        pd->sound->sample->getData(sample, &data, &format, &rate, &bytes);
        const int16_t* pcm = (const int16_t*)data;
        const int count = (int)(bytes / 2);
        int shiftAdd = countMismatches(adpcm, adpcmBytes, blockBytes, IMA_ROUNDING_SHIFT_ADD, pcm, count);
        int multiply = countMismatches(adpcm, adpcmBytes, blockBytes, IMA_ROUNDING_MULTIPLY, pcm, count);
        report("ADPCM mismatches of %d: shift-add %d, multiply %d", count, shiftAdd, multiply);
        report("ADPCM rounding: %s", shiftAdd == 0   ? "shift-add (IMA reference)"
                                     : multiply == 0 ? "multiply (ffmpeg)"
                                                     : "neither");
    } else {
        report("ADPCM: decompress failed");
    }
    pd->system->realloc(adpcm, 0);
    pd->sound->sample->freeSample(sample);
}

// MARK: Upsampling

#define SOURCE_RATE 22050
#define SOURCE_COUNT 4410
#define CAPTURE_COUNT 8192
#define FIT_WINDOW 4000
#define MAX_LAG 512

static int16_t source[SOURCE_COUNT];
static int32_t capture[CAPTURE_COUNT];
static volatile int captured;
static SamplePlayer* player;
static int framesSincePlay;

static int captureEffect(SoundEffect* effect, int32_t* left, int32_t* right, int nsamples, int bufactive) {
    (void)effect, (void)right;
    if (!bufactive && captured == 0) return 0;
    for (int i = 0; i < nsamples && captured < CAPTURE_COUNT; i++) capture[captured++] = left[i];
    return 0;
}

// Models return twice the predicted sample, so linear interpolation stays an integer.
typedef int32_t Model(int n);

static int32_t zeroOrderHold(int n) { return 2 * source[n / 2]; }

static int32_t linear(int n) {
    if (n % 2 == 0) return 2 * source[n / 2];
    return source[n / 2] + source[n / 2 + 1];
}

// Residual after the best least-squares gain, in dB relative to the capture, at the best lag. Sums are
// integer because the FPU is single precision and float sums can't resolve a -60 dB residual.
static float fitResidualDb(Model* model, int* bestLag) {
    float best = 0;
    int found = 0;
    for (int lag = 0; lag < MAX_LAG; lag++) {
        int64_t xy = 0, xx = 0, yy = 0;
        for (int n = 0; n < FIT_WINDOW; n++) {
            int64_t x = model(n), y = capture[lag + n] >> 8;
            xy += x * y, xx += x * x, yy += y * y;
        }
        if (xx == 0 || yy == 0) continue;
        double residual = (double)yy - (double)xy * (double)xy / (double)xx;
        float db = 10.0f * log10f((float)(residual / (double)yy) + 1e-12f);
        if (!found || db < best) best = db, *bestLag = lag;
        found = 1;
    }
    return best;
}

static void startUpsamplingProbe(void) {
    uint32_t seed = 12345;
    for (int i = 0; i < SOURCE_COUNT; i++) {
        seed = seed * 1664525u + 1013904223u;
        source[i] = (int16_t)((int32_t)(seed >> 16) - 32768) / 2;
    }
    AudioSample* sample = pd->sound->sample->newSampleFromData((uint8_t*)source, kSound16bitMono, SOURCE_RATE,
                                                               (int)sizeof(source), 0);
    player = pd->sound->sampleplayer->newPlayer();
    pd->sound->sampleplayer->setSample(player, sample);

    SoundChannel* channel = pd->sound->channel->newChannel();
    pd->sound->channel->addSource(channel, (SoundSource*)player);
    pd->sound->channel->addEffect(channel, pd->sound->effect->newEffect(captureEffect, NULL));
    pd->sound->addChannel(channel);
    pd->sound->sampleplayer->play(player, 1, 1.0f);
}

static void finishUpsamplingProbe(void) {
    if (captured < MAX_LAG + FIT_WINDOW) {
        report("Upsampling: only %d samples captured", captured);
        return;
    }
    int holdLag = 0, linearLag = 0;
    float hold = fitResidualDb(zeroOrderHold, &holdLag);
    float lin = fitResidualDb(linear, &linearLag);
    report("Upsampling residual (lower = closer):");
    report("  zero-order hold %d dB (lag %d)", (int)lrintf(hold), holdLag);
    report("  linear          %d dB (lag %d)", (int)lrintf(lin), linearLag);
    report("Upsampling: %s", hold < -60 ? "zero-order hold" : lin < -60 ? "linear" : "neither model");
}

static int update(void* userdata) {
    (void)userdata;
    if (player && ++framesSincePlay == 30) finishUpsamplingProbe();

    pd->graphics->clear(kColorWhite);
    pd->graphics->setFont(font);
    pd->graphics->drawText("Playback probe", 14, kASCIIEncoding, 8, 4);
    for (int i = 0; i < lineCount; i++) {
        pd->graphics->drawText(lines[i], strlen(lines[i]), kASCIIEncoding, 8, 26 + i * 18);
    }
    return 1;
}

#ifdef _WINDLL
__declspec(dllexport)
#endif
int eventHandler(PlaydateAPI* playdate, PDSystemEvent event, uint32_t arg) {
    (void)arg;
    if (event != kEventInit) return 0;
    pd = playdate;

    const char* error;
    font = pd->graphics->loadFont("/System/Fonts/Asheville-Sans-14-Bold.pft", &error);
    if (!font) pd->system->error("Couldn't load font: %s", error);

    probeAdpcmRounding();
    startUpsamplingProbe();
    pd->display->setRefreshRate(30);
    pd->system->setUpdateCallback(update, NULL);
    return 0;
}
