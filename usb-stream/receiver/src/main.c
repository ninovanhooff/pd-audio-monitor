#include <math.h>
#include <stdarg.h>
#include <string.h>

#include "packet.h"
#include "pd_api.h"

static PlaydateAPI* pd;
// Asheville Light carries Panic's button glyphs (Ⓐ Ⓑ ⬅ 🎣 ...); the Roobert fonts lack most of them.
static struct {
    LCDFont* label;
    LCDFont* value;
    LCDFont* glyph;
} fonts;

// Single producer (serial messages, on the main thread), single consumer (source callback): only the
// producer moves writePos and only the consumer moves readPos, so the ring needs no lock.
#define RING_SIZE 32768
#define RING_MASK (RING_SIZE - 1)
#define PRIME_FILL 6615 // 150 ms
#define MAX_FILL 17640  // 400 ms: beyond it the Mac's clock has run ahead, so drop back to PRIME_FILL
#define REPLAY_FILL 8820

static volatile int16_t ring[RING_SIZE];
static volatile uint32_t writePos;
static volatile uint32_t readPos;
static volatile int primed;
static volatile uint32_t underruns;
static volatile uint32_t overruns;

static PacketReceiver receiver;
// Test hook: packets that pdstream.py --out recorded, fed in as fast as they play.
static SDFile* replay;

typedef enum { OUTPUT_SPEAKER, OUTPUT_HEADPHONES, OUTPUT_BOTH, OUTPUT_COUNT } Output;
static const char* outputNames[] = {"speaker", "headphones", "speaker + headphones"};

static struct {
    Output output;
    int viaEval;
    int wantedFormat;
    unsigned int lastRequestMs;
    int peak;
    int clipped;
    float meterDb;
    float windowDb;
    float shownDb;
    unsigned int windowStartMs;
    int clipHold;
    unsigned int lastSecond;
    uint32_t lastPackets;
    uint32_t lastBytes;
    int packetsPerSecond;
    int bytesPerSecond;
    unsigned int lastPacketMs;
} ui;

static void emitSamples(void* context, const int16_t* samples, int count) {
    (void)context;
    for (int i = 0; i < count; i++) {
        int16_t sample = samples[i];
        if (sample == 32767 || sample == -32768) ui.clipped = 1;
        int magnitude = sample < 0 ? -sample : sample;
        if (magnitude > ui.peak) ui.peak = magnitude;
        ring[writePos & RING_MASK] = sample;
        writePos++;
    }
}

static void receive(const char* text) {
    if (strncmp(text, "msg ", 4) == 0) text += 4;
    if (packet_receive(&receiver, text, emitSamples, NULL)) {
        ui.lastPacketMs = pd->system->getCurrentTimeMilliseconds();
        ui.viaEval = 0;
    }
}

// pdstream_feed(packet): what the host's `eval` bytecode calls, with the packet as a raw string.
static int feed(lua_State* L) {
    (void)L;
    size_t length = 0;
    const uint8_t* packet = (const uint8_t*)pd->lua->getArgBytes(1, &length);
    if (packet && packet_receive_bytes(&receiver, packet, (int)length, emitSamples, NULL)) {
        ui.lastPacketMs = pd->system->getCurrentTimeMilliseconds();
        ui.viaEval = 1;
    }
    return 0;
}

static int sourceCallback(void* context, int16_t* left, int16_t* right, int len) {
    (void)context, (void)right;
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
        if (readPos != writePos) {
            left[i] = ring[readPos & RING_MASK];
            readPos++;
        } else {
            left[i] = 0;
            starved = 1;
        }
    }
    if (starved) {
        underruns++;
        primed = 0;
    }
    return 1;
}

static void feedReplay(void) {
    static char buffer[1024];
    static int length;
    while (replay && writePos - readPos < REPLAY_FILL) {
        char* end = memchr(buffer, '\n', (size_t)length);
        if (!end) {
            int read = length < (int)sizeof(buffer) ? pd->file->read(replay, buffer + length, sizeof(buffer) - (unsigned)length) : 0;
            if (read <= 0) {
                pd->file->close(replay);
                replay = NULL;
                return;
            }
            length += read;
            continue;
        }
        *end = 0;
        receive(buffer);
        length -= (int)(end + 1 - buffer);
        memmove(buffer, end + 1, (size_t)length);
    }
}

static void applyOutput(void) {
    pd->sound->setOutputsActive(ui.output != OUTPUT_SPEAKER, ui.output != OUTPUT_HEADPHONES);
}

// Passing a change callback stops the system from switching outputs itself, so B stays in charge.
static void headphoneChanged(int headphone, int mic) {
    (void)headphone, (void)mic;
    applyOutput();
}

static int isReceiving(void) {
    return receiver.format >= 0 && pd->system->getCurrentTimeMilliseconds() - ui.lastPacketMs < 1000;
}

// The console goes out over USB, where pdstream.py picks the request up. Repeated until the stream
// switches, since pdstream.py may not be listening yet (or anymore).
static void requestFormat(void) {
    unsigned int now = pd->system->getCurrentTimeMilliseconds();
    if (ui.wantedFormat < 0 || ui.wantedFormat == receiver.format || now - ui.lastRequestMs < 1000) return;
    pd->system->logToConsole("pdstream format %d", ui.wantedFormat);
    ui.lastRequestMs = now;
}

// The formats as a grid: sample rate rises to the right, bits per sample rise upward.
static int rateRank(int format) {
    int divisor = chain_formats[format].divisor;
    return divisor == 1 ? 2 : divisor == 2 ? 1 : 0;
}

static int codecRank(int format) {
    ChainCodec codec = chain_formats[format].codec;
    return codec == CODEC_PCM16 ? 2 : codec == CODEC_PCM8 ? 1 : 0;
}

// Not every rate has every codec: a rate step lands on the nearest codec that rate has, and a codec
// step goes to the next one the rate has. Both wrap around.
static void stepFormat(int rateStep, int codecStep) {
    int current = ui.wantedFormat >= 0 ? ui.wantedFormat : receiver.format >= 0 ? receiver.format : chain_default_format;
    int rate = (rateRank(current) + rateStep + 3) % 3, codec = codecRank(current);
    int best = -1, bestDistance = 0;
    for (int format = 0; format < chain_format_count; format++) {
        if ((format == PACKET_UNSTREAMABLE_FORMAT && !ui.viaEval) || rateRank(format) != rate) continue;
        int distance = codecRank(format) - codec;
        if (codecStep) {
            distance *= codecStep;
            if (distance <= 0) distance += 3;
        } else {
            distance = distance < 0 ? -distance * 2 : distance * 2 + 1; // on a tie, the lighter codec
        }
        if (best < 0 || distance < bestDistance) {
            best = format;
            bestDistance = distance;
        }
    }
    if (best < 0) return;
    ui.wantedFormat = best;
    ui.lastRequestMs = pd->system->getCurrentTimeMilliseconds() - 1000;
}

static void handleInput(void) {
    PDButtons pushed;
    pd->system->getButtonState(NULL, &pushed, NULL);
    if (pushed & kButtonB) {
        ui.output = (Output)((ui.output + 1) % OUTPUT_COUNT);
        applyOutput();
    }
    if (pushed & kButtonLeft) stepFormat(-1, 0);
    if (pushed & kButtonRight) stepFormat(1, 0);
    if (pushed & kButtonUp) stepFormat(0, 1);
    if (pushed & kButtonDown) stepFormat(0, -1);
}

static void updateRates(void) {
    unsigned int second = pd->system->getCurrentTimeMilliseconds() / 1000;
    if (second == ui.lastSecond) return;
    ui.packetsPerSecond = (int)(receiver.packets - ui.lastPackets) / (int)(second - ui.lastSecond);
    ui.bytesPerSecond = (int)(receiver.bytes - ui.lastBytes) / (int)(second - ui.lastSecond);
    ui.lastSecond = second;
    ui.lastPackets = receiver.packets;
    ui.lastBytes = receiver.bytes;
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

static void drawMeter(int y) {
    int framePeak = ui.peak;
    ui.peak = 0;
    float db = framePeak > 0 ? 20.0f * log10f((float)framePeak / 32768.0f) : -60.0f;
    ui.meterDb = db > ui.meterDb ? db : ui.meterDb - 1.0f;
    if (ui.meterDb < -60) ui.meterDb = -60;
    // The bar moves every frame; the number shows each window's peak so it can be read.
    if (db > ui.windowDb) ui.windowDb = db;
    unsigned int now = pd->system->getCurrentTimeMilliseconds();
    if (now - ui.windowStartMs >= 500) {
        ui.shownDb = ui.windowDb;
        ui.windowDb = -60;
        ui.windowStartMs = now;
    }
    if (ui.clipped) {
        ui.clipped = 0;
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
    feedReplay();
    handleInput();
    requestFormat();
    updateRates();

    pd->graphics->clear(kColorWhite);
    drawString(fonts.label, "USB stream", 12, 8);
    if (ui.wantedFormat >= 0 && ui.wantedFormat != receiver.format) {
        drawRow(40, "Format", "✛", "%s ...", chain_formats[ui.wantedFormat].name);
    } else {
        drawRow(40, "Format", "✛", "%s", isReceiving() ? chain_formats[receiver.format].name : "waiting for the Mac");
    }
    drawRow(64, "Output", "Ⓑ", "%s", outputNames[ui.output]);
    int kbTenths = ui.bytesPerSecond / 100;
    drawRow(88, "Link", "", "%d %s/s, %d.%d KB/s", ui.packetsPerSecond, ui.viaEval ? "eval" : "msg", kbTenths / 10, kbTenths % 10);
    drawRow(112, "Buffer", "", "%d ms", (int)((writePos - readPos) / 44));
    drawMeter(142);
    drawRow(166, "Dropouts", "", "%d lost, %d under, %d over", (int)receiver.lost, (int)underruns, (int)overruns);
    drawString(fonts.value, "Mac: route audio to BlackHole and run", 12, 196);
    drawString(fonts.value, "usb-stream/host/pdstream.py", 12, 214);
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
    // Setting up at kEventInitLua, after the Lua runtime starts: a C update callback set at
    // kEventInit keeps it from starting, and `eval` needs it.
    if (event != kEventInitLua) return 0;
    pd = playdate;

    const char* error;
    if (!pd->lua->addFunction(feed, "pdstream_feed", &error)) pd->system->logToConsole("pdstream_feed: %s", error);

    fonts.label = loadFont("/System/Fonts/Roobert-11-Bold.pft");
    fonts.value = loadFont("/System/Fonts/Roobert-11-Medium.pft");
    fonts.glyph = loadFont("/System/Fonts/Asheville-Sans-14-Light.pft");

    packet_init(&receiver);
    ui.output = OUTPUT_SPEAKER;
    ui.wantedFormat = -1;
    ui.meterDb = -60;
    ui.windowDb = -60;
    ui.shownDb = -60;
    ui.lastSecond = pd->system->getCurrentTimeMilliseconds() / 1000;

    pd->system->setAutoLockDisabled(1);
    pd->display->setRefreshRate(50);
    pd->sound->getHeadphoneState(NULL, NULL, headphoneChanged);
    applyOutput();
    pd->sound->addSource(sourceCallback, NULL, 0);
    pd->system->setSerialMessageCallback(receive);
    replay = pd->file->open("replay.txt", kFileReadData);

    pd->system->setUpdateCallback(update, NULL);
    return 0;
}
