#include <math.h>
#include <stdarg.h>
#include <string.h>

#include "format_chain.h"
#include "pd_api.h"

static PlaydateAPI* pd;
// Asheville Light carries Panic's button glyphs (Ⓐ Ⓑ ⬅ 🎣 ...); the Roobert fonts lack most of them.
static struct {
    LCDFont* label;
    LCDFont* value;
    LCDFont* glyph;
    LCDFont* tiny;
} fonts;

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
    float windowDb;
    float shownDb;
    unsigned int windowStartMs;
    unsigned int lastFrameMs;
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

    if (pushed & kButtonRight) ui.format = (ui.format + 1) % chain_mono_format_count;
    if (pushed & kButtonLeft) ui.format = (ui.format + chain_mono_format_count - 1) % chain_mono_format_count;
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

static void drawString(LCDFont* font, const char* text, int x, int y) {
    pd->graphics->setFont(font);
    pd->graphics->drawText(text, strlen(text), kUTF8Encoding, x, y);
}

// One row: bold label, value, and the controls for it right-aligned as button glyphs.
static void drawRow(int y, const char* label, const char* glyphs, const char* format, ...) {
    va_list args;
    va_start(args, format);
    char* value = NULL;
    pd->system->vaFormatString(&value, format, args);
    va_end(args);

    drawString(fonts.label, label, 12, y);
    if (value) {
        drawString(fonts.value, value, 116, y);
        pd->system->realloc(value, 0);
    }
    int width = pd->graphics->getTextWidth(fonts.glyph, glyphs, strlen(glyphs), kUTF8Encoding, 0);
    int offset = (pd->graphics->getFontHeight(fonts.value) - pd->graphics->getFontHeight(fonts.glyph)) / 2;
    drawString(fonts.glyph, glyphs, 388 - width, y + offset);
}

#define METER_DECAY_DB_PER_SEC 20.0f

static void drawMeter(int y) {
    int framePeak = peak;
    peak = 0;
    float db = framePeak > 0 ? 20.0f * log10f((float)framePeak / 32768.0f) : -60.0f;
    unsigned int now = pd->system->getCurrentTimeMilliseconds();
    float fallen = ui.meterDb - METER_DECAY_DB_PER_SEC * (float)(now - ui.lastFrameMs) / 1000.0f;
    ui.lastFrameMs = now;
    ui.meterDb = db > fallen ? db : fallen;
    if (ui.meterDb < -60) ui.meterDb = -60;
    // The bar moves every frame; the number shows each window's peak so it can be read.
    if (db > ui.windowDb) ui.windowDb = db;
    if (now - ui.windowStartMs >= 500) {
        ui.shownDb = ui.windowDb;
        ui.windowDb = -60;
        ui.windowStartMs = now;
    }
    if (clipped) {
        clipped = 0;
        ui.clipHold = 30;
    }

    const int x = 12, width = 300, height = 14;
    int filled = (int)((ui.meterDb + 60.0f) / 60.0f * (float)width);
    pd->graphics->drawRect(x, y, width, height, kColorBlack);
    pd->graphics->fillRect(x, y, filled, height, kColorBlack);
    if (ui.shownDb > -60) {
        int holdX = x + (int)((ui.shownDb + 60.0f) / 60.0f * (float)width);
        if (holdX > x + width - 1) holdX = x + width - 1;
        pd->graphics->drawLine(holdX, y + 1, holdX, y + height - 2, 1, holdX < x + filled ? kColorWhite : kColorBlack);
    }
    for (int mark = -48; mark <= 0; mark += 12) {
        int markX = mark == 0 ? x + width - 1 : x + (mark + 60) * width / 60;
        pd->graphics->drawLine(markX, y + height, markX, y + height + 3, 1, kColorBlack);
        char* markText = NULL;
        pd->system->formatString(&markText, "%d", mark);
        if (markText) {
            int markWidth = pd->graphics->getTextWidth(fonts.tiny, markText, strlen(markText), kUTF8Encoding, 0);
            drawString(fonts.tiny, markText, markX - (markWidth - 1) / 2, y + height + 4);
            pd->system->realloc(markText, 0);
        }
    }
    char* level = NULL;
    LCDFont* font = fonts.label;
    if (ui.clipHold > 0) {
        ui.clipHold--;
        pd->system->formatString(&level, "CLIP");
    } else {
        int tenths = (int)lrintf(-ui.shownDb * 10.0f);
        pd->system->formatString(&level, "-%d.%ddB", tenths / 10, tenths % 10);
        font = fonts.value;
    }
    if (level) {
        int textWidth = pd->graphics->getTextWidth(font, level, strlen(level), kUTF8Encoding, 0);
        drawString(font, level, 388 - textWidth, y);
        pd->system->realloc(level, 0);
    }
}

static int update(void* userdata) {
    (void)userdata;
    handleInput();

    pd->graphics->clear(kColorWhite);
    drawString(fonts.label, "Line-in monitor", 12, 8);
    switch (ui.mic) {
    case MIC_WAITING:
        drawString(fonts.value, "Waiting for microphone permission...", 12, 40);
        return 1;
    case MIC_DENIED:
        drawString(fonts.value, "Microphone access denied.", 12, 40);
        drawString(fonts.value, "Allow it in Settings > Permissions.", 12, 60);
        return 1;
    case MIC_FAILED:
        drawString(fonts.value, "Could not open the headset input.", 12, 40);
        return 1;
    case MIC_RUNNING:
        break;
    }

    int tenths = (int)lrintf(ui.gainDb * 10.0f);
    int magnitude = tenths < 0 ? -tenths : tenths;
    drawRow(40, "Format", "⬅ ➡", "%s", chain_formats[ui.format].deviceName);
    drawRow(64, "Compare", "Ⓐ", "%s", ui.compare ? "playing bypass" : "off");
    drawRow(88, "Output", "Ⓑ", "%s", outputNames[ui.output]);
    drawRow(112, "Gain", "⬆ ⬇ 🎣", "%c%d.%d dB", tenths < 0 ? '-' : '+', magnitude / 10, magnitude % 10);
    drawMeter(142);
    drawRow(176, "Dropouts", "", "%d under, %d over", (int)underruns, (int)overruns);
    drawString(fonts.value, "Mac audio out -> splitter mic plug.", 12, 200);
    drawString(fonts.value, "Keep peaks out of CLIP with the Mac volume.", 12, 218);
    return 1;
}

static LCDFont* loadFont(const char* path) {
    const char* error;
    LCDFont* font = pd->graphics->loadFont(path, &error);
    if (!font) pd->system->error("Couldn't load %s: %s", path, error);
    return font;
}

#ifdef _WINDLL
__declspec(dllexport)
#endif
int eventHandler(PlaydateAPI* playdate, PDSystemEvent event, uint32_t arg) {
    (void)arg;
    if (event != kEventInit) return 0;
    pd = playdate;

    fonts.label = loadFont("/System/Fonts/Roobert-11-Bold.pft");
    fonts.value = loadFont("/System/Fonts/Roobert-11-Medium.pft");
    fonts.glyph = loadFont("/System/Fonts/Asheville-Sans-14-Light.pft");
    fonts.tiny = loadFont("/System/Fonts/Roobert-10-Bold.pft");

    ui.format = chain_default_format;
    ui.output = OUTPUT_SPEAKER;
    ui.meterDb = -60;
    ui.lastFrameMs = pd->system->getCurrentTimeMilliseconds();
    ui.windowDb = -60;
    ui.shownDb = -60;
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
