#include <math.h>
#include <stdarg.h>
#include <string.h>

#include "format_chain.h"
#include "pd_api.h"

static PlaydateAPI* pd;
static LCDFont* font;

// Single producer (mic callback), single consumer (source callback): only the producer moves
// writePos and only the consumer moves readPos, so the ring needs no lock.
#define RING_SIZE 8192
#define RING_MASK (RING_SIZE - 1)
#define PRIME_FILL 256
#define MAX_FILL 2048

static volatile int16_t ring[RING_SIZE];
static volatile uint32_t writePos;
static volatile uint32_t readPos;
static volatile int primed;
static volatile int32_t gainQ12 = 4096;
static volatile int peak;
static volatile int clipped;
static volatile uint32_t underruns;
static volatile uint32_t overruns;

// The main loop builds a chain in the idle slot, then swaps the pointer; the swap is a single store.
static FormatChain chains[2];
static FormatChain* volatile activeChain;

typedef enum { OUTPUT_SPEAKER, OUTPUT_HEADPHONES, OUTPUT_BOTH, OUTPUT_COUNT } Output;
static const char* outputNames[] = {"speaker", "headphones", "speaker + headphones"};

typedef enum { MIC_WAITING, MIC_RUNNING, MIC_DENIED, MIC_FAILED } MicState;

static struct {
    int format;
    int compare;
    float gainDb;
    Output output;
    MicState mic;
    float meterDb;
    int clipHold;
} ui;

static int16_t clamp16(int32_t value) {
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return (int16_t)value;
}

static int micCallback(void* context, int16_t* data, int len) {
    (void)context;
    const int32_t gain = gainQ12;
    int framePeak = peak;
    for (int i = 0; i < len; i++) {
        int32_t scaled = (int32_t)(((int64_t)data[i] * gain) >> 12);
        int16_t sample = clamp16(scaled);
        if (sample != scaled) clipped = 1;
        int magnitude = sample < 0 ? -sample : sample;
        if (magnitude > framePeak) framePeak = magnitude;
        ring[writePos & RING_MASK] = sample;
        writePos++;
    }
    peak = framePeak;
    return 1;
}

static int sourceCallback(void* context, int16_t* left, int16_t* right, int len) {
    (void)context, (void)right;
    FormatChain* chain = activeChain;
    uint32_t fill = writePos - readPos;

    if (!primed) {
        if (fill < PRIME_FILL) {
            memset(left, 0, (size_t)len * sizeof(int16_t));
            return 0;
        }
        primed = 1;
    }
    if (fill > MAX_FILL) {
        readPos = writePos - PRIME_FILL;
        overruns++;
    }

    int starved = 0;
    for (int i = 0; i < len; i++) {
        int16_t input = 0;
        if (readPos != writePos) {
            input = ring[readPos & RING_MASK];
            readPos++;
        } else {
            starved = 1;
        }
        left[i] = chain_process(chain, input);
    }
    if (starved) {
        underruns++;
        primed = 0;
    }
    return 1;
}

static void applyFormat(void) {
    FormatChain* idle = activeChain == &chains[0] ? &chains[1] : &chains[0];
    int format = ui.compare ? 0 : ui.format;
    chain_init(idle, &chain_formats[format]);
    activeChain = idle;
}

static void applyGain(void) {
    if (ui.gainDb < -30) ui.gainDb = -30;
    if (ui.gainDb > 30) ui.gainDb = 30;
    gainQ12 = (int32_t)(4096.0f * powf(10.0f, ui.gainDb / 20.0f));
}

static void applyOutput(void) {
    pd->sound->setOutputsActive(ui.output != OUTPUT_SPEAKER, ui.output != OUTPUT_HEADPHONES);
}

// Passing a change callback stops the system from switching outputs itself when the splitter is
// plugged in, which would otherwise mute the speaker.
static void headphoneChanged(int headphone, int mic) {
    (void)headphone, (void)mic;
    applyOutput();
}

static void startMic(void) {
    if (pd->sound->setMicCallback(micCallback, NULL, kMicInputHeadset) == 0) {
        ui.mic = MIC_FAILED;
        pd->system->logToConsole("setMicCallback failed: %s", pd->sound->getError());
        return;
    }
    ui.mic = MIC_RUNNING;
}

static void micAccessReply(bool allowed, void* userdata) {
    (void)userdata;
    if (allowed) {
        startMic();
    } else {
        ui.mic = MIC_DENIED;
    }
}

static void handleInput(void) {
    PDButtons pushed;
    pd->system->getButtonState(NULL, &pushed, NULL);

    if (pushed & kButtonRight) ui.format = (ui.format + 1) % chain_format_count;
    if (pushed & kButtonLeft) ui.format = (ui.format + chain_format_count - 1) % chain_format_count;
    if (pushed & kButtonA) ui.compare = !ui.compare;
    if (pushed & (kButtonLeft | kButtonRight | kButtonA)) applyFormat();

    if (pushed & kButtonB) {
        ui.output = (Output)((ui.output + 1) % OUTPUT_COUNT);
        applyOutput();
    }

    float crank = pd->system->getCrankChange();
    if (pushed & kButtonUp) ui.gainDb += 1;
    if (pushed & kButtonDown) ui.gainDb -= 1;
    ui.gainDb += crank / 20.0f;
    if ((pushed & (kButtonUp | kButtonDown)) || crank != 0) applyGain();
}

static void drawLine(int y, const char* text) {
    pd->graphics->drawText(text, strlen(text), kASCIIEncoding, 12, y);
}

// pd's formatter instead of snprintf: newlib's stdio needs syscalls the device doesn't provide.
static void drawLinef(int y, const char* format, ...) {
    va_list args;
    va_start(args, format);
    char* text = NULL;
    pd->system->vaFormatString(&text, format, args);
    va_end(args);
    if (!text) return;
    drawLine(y, text);
    pd->system->realloc(text, 0);
}

static void drawMeter(int y) {
    int framePeak = peak;
    peak = 0;
    float db = framePeak > 0 ? 20.0f * log10f((float)framePeak / 32768.0f) : -60.0f;
    ui.meterDb = db > ui.meterDb ? db : ui.meterDb - 1.0f;
    if (ui.meterDb < -60) ui.meterDb = -60;
    if (clipped) {
        clipped = 0;
        ui.clipHold = 30;
    }

    const int x = 12, width = 300, height = 14;
    int filled = (int)((ui.meterDb + 60.0f) / 60.0f * (float)width);
    pd->graphics->drawRect(x, y, width, height, kColorBlack);
    pd->graphics->fillRect(x, y, filled, height, kColorBlack);
    for (int mark = -48; mark < 0; mark += 12) {
        int markX = x + (mark + 60) * width / 60;
        pd->graphics->drawLine(markX, y + height, markX, y + height + 3, 1, kColorBlack);
    }
    if (ui.clipHold > 0) {
        ui.clipHold--;
        pd->graphics->drawText("CLIP", 4, kASCIIEncoding, x + width + 10, y);
    }
}

static int update(void* userdata) {
    (void)userdata;
    handleInput();

    pd->graphics->clear(kColorWhite);
    pd->graphics->setFont(font);

    drawLine(8, "Line-in monitor");
    switch (ui.mic) {
    case MIC_WAITING:
        drawLine(32, "Waiting for microphone permission...");
        return 1;
    case MIC_DENIED:
        drawLine(32, "Microphone access denied.");
        drawLine(52, "Allow it in Settings > Permissions.");
        return 1;
    case MIC_FAILED:
        drawLine(32, "Could not open the headset input.");
        return 1;
    case MIC_RUNNING:
        break;
    }

    drawLinef(36, "< %s >", chain_formats[ui.format].name);
    drawLine(58, ui.compare ? "A: playing BYPASS (44.1 kHz 16-bit)" : "A: compare with bypass");
    drawLinef(80, "B: output %s", outputNames[ui.output]);
    int tenths = (int)lrintf(ui.gainDb * 10.0f);
    int magnitude = tenths < 0 ? -tenths : tenths;
    drawLinef(102, "Gain %c%d.%d dB (crank, up/down)", tenths < 0 ? '-' : '+', magnitude / 10, magnitude % 10);
    drawMeter(128);
    drawLinef(156, "Underruns %d  overruns %d", (int)underruns, (int)overruns);
    drawLine(196, "Mac audio out -> splitter mic plug");
    drawLine(216, "Keep peaks out of CLIP with Mac volume");
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

    ui.format = chain_default_format;
    ui.output = OUTPUT_SPEAKER;
    ui.meterDb = -60;
    applyGain();
    applyFormat();


    pd->system->setAutoLockDisabled(1);
    pd->display->setRefreshRate(30);
    pd->sound->getHeadphoneState(NULL, NULL, headphoneChanged);
    applyOutput();
    pd->sound->addSource(sourceCallback, NULL, 0);

    switch (pd->sound->requestMicAccess("Plays the headset input through the speaker", micAccessReply, NULL)) {
    case kAccessAllow:
        startMic();
        break;
    case kAccessDeny:
        ui.mic = MIC_DENIED;
        break;
    case kAccessAsk:
        ui.mic = MIC_WAITING;
        break;
    }

    pd->system->setUpdateCallback(update, NULL);
    return 0;
}
