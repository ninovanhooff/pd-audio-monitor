#include <math.h>
#include <stdarg.h>
#include <string.h>

#include "meter.h"
#include "packet.h"
#include "pd_api.h"

static PlaydateAPI* pd;
// Asheville Light carries Panic's button glyphs (Ⓐ Ⓑ ⬅ 🎣 ...); the Roobert fonts lack most of them.
static struct {
    LCDFont* label;
    LCDFont* value;
    LCDFont* glyph;
    LCDFont* tiny;
} fonts;

// Single producer (pdstream_feed, on the main thread), single consumer (source callback): only the
// producer moves writePos and only the consumer moves readPos, so the ring needs no lock.
#define RING_MASK (RING_SIZE - 1)
#define REPLAY_MAX_COMMAND 2048

static volatile int16_t ringLeft[RING_SIZE];
static volatile int16_t ringRight[RING_SIZE];
static volatile uint32_t writePos;
static volatile uint32_t readPos;
static volatile int primed;
static volatile uint32_t primeFill;
static volatile uint32_t underruns;
static volatile uint32_t overruns;

static PacketReceiver receiver;
static SoundSource* source;
static PDMenuItem* prefillItem;
// Set while the system menu is up: the source is removed and packets that still arrive are dropped.
static int paused;
// Test hook: `eval` commands that pdstream.py --out recorded, fed in as fast as they play.
static SDFile* replay;

typedef enum { OUTPUT_SPEAKER, OUTPUT_HEADPHONES, OUTPUT_BOTH, OUTPUT_COUNT } Output;
static const char* outputNames[] = {"speaker", "headphones", "speaker + headphones"};

static struct {
    Output output;
    int wantedFormat;
    unsigned int lastRequestMs;
    unsigned int matchedSinceMs;
    unsigned int resumedAtMs;
    int peak;
    int clipped;
    float meterDb;
    unsigned int lastMeterMs;
    float windowDb;
    float shownDb;
    int shownBufferMs;
    unsigned int windowStartMs;
    int clipHold;
    unsigned int lastSecond;
    uint32_t lastPackets;
    uint32_t lastBytes;
    int packetsPerSecond;
    int bytesPerSecond;
    unsigned int lastPacketMs;
} ui;

static void trackLevel(int16_t sample) {
    if (sample == 32767 || sample == -32768) ui.clipped = 1;
    int magnitude = sample < 0 ? -sample : sample;
    if (magnitude > ui.peak) ui.peak = magnitude;
}

static void emitFrames(void* context, const int16_t* frames, int count, int channels) {
    (void)context;
    for (int i = 0; i < count; i++) {
        int16_t left = frames[i * channels], right = frames[i * channels + channels - 1];
        trackLevel(left);
        trackLevel(right);
        ringLeft[writePos & RING_MASK] = left;
        ringRight[writePos & RING_MASK] = right;
        writePos++;
    }
}

static void receive(const uint8_t* packet, int length) {
    if (paused) return;
    if (packet_receive(&receiver, packet, length, emitFrames, NULL))
        ui.lastPacketMs = pd->system->getCurrentTimeMilliseconds();
}

// pdstream_feed(packet): what the host's `eval` bytecode calls, with the packet as a raw string.
static int feed(lua_State* L) {
    (void)L;
    size_t length = 0;
    const uint8_t* packet = (const uint8_t*)pd->lua->getArgBytes(1, &length);
    if (packet) receive(packet, (int)length);
    return 0;
}

static int sourceCallback(void* context, int16_t* left, int16_t* right, int len) {
    (void)context;
    uint32_t fill = writePos - readPos;
    uint32_t prime = primeFill;

    if (!primed) {
        if (fill < prime) {
            memset(left, 0, (size_t)len * sizeof(int16_t));
            memset(right, 0, (size_t)len * sizeof(int16_t));
            return 0;
        }
        primed = 1;
    }
    if (fill > max_fill_samples(prime)) {
        readPos = writePos - prime;
        overruns++;
    }

    int starved = 0;
    for (int i = 0; i < len; i++) {
        if (readPos != writePos) {
            left[i] = ringLeft[readPos & RING_MASK];
            right[i] = ringRight[readPos & RING_MASK];
            readPos++;
        } else {
            left[i] = right[i] = 0;
            starved = 1;
        }
    }
    if (starved) {
        underruns++;
        primed = 0;
    }
    return 1;
}

// The Simulator has no serial input: replay holds `eval <length>\n<bytecode>` commands, whose
// packets go in without running the bytecode.
static void feedReplay(void) {
    static uint8_t buffer[REPLAY_MAX_COMMAND];
    static int length;
    while (replay && writePos - readPos < replay_fill_samples(primeFill)) {
        const uint8_t* newline = memchr(buffer, '\n', (size_t)length);
        int header = newline ? (int)(newline + 1 - buffer) : 0;
        int size = header > 5 && memcmp(buffer, "eval ", 5) == 0 ? 0 : -1;
        for (int i = 5; size >= 0 && i < header - 1; i++)
            size = buffer[i] >= '0' && buffer[i] <= '9' && size < REPLAY_MAX_COMMAND ? size * 10 + buffer[i] - '0' : -1;
        if (header && (size <= 0 || header + size > REPLAY_MAX_COMMAND)) {
            pd->system->logToConsole("replay: not an eval command");
            pd->file->close(replay);
            replay = NULL;
            return;
        }
        if (!header || length < header + size) {
            int read = pd->file->read(replay, buffer + length, (unsigned)(REPLAY_MAX_COMMAND - length));
            if (read <= 0) {
                pd->file->close(replay);
                replay = NULL;
                return;
            }
            length += read;
            continue;
        }
        const uint8_t* packet;
        int packetLength = packet_from_eval(buffer + header, size, &packet);
        if (packetLength >= 0) receive(packet, packetLength);
        length -= header + size;
        memmove(buffer, buffer + header + size, (size_t)length);
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
// switches, since pdstream.py may not be listening yet (or anymore); after that the host may pick
// another format. Packets of the old format still queued on the link can flip the stream back, so the
// request stays until the stream has held the new format for a while.
static void requestFormat(void) {
    unsigned int now = pd->system->getCurrentTimeMilliseconds();
    if (ui.resumedAtMs) {
        // The host stopped when the menu opened; ask until packets flow again. Old packets still queued on
        // the link don't count: the stream must have run for a while.
        if (now - ui.resumedAtMs > 2000 && now - ui.lastPacketMs < 500) {
            ui.resumedAtMs = 0;
        } else if (now - ui.lastRequestMs >= 1000) {
            int format = ui.wantedFormat >= 0 ? ui.wantedFormat : receiver.format >= 0 ? receiver.format : chain_default_format;
            pd->system->logToConsole("pdstream format %d", format);
            ui.lastRequestMs = now;
        }
        return;
    }
    if (ui.wantedFormat >= 0 && ui.wantedFormat == receiver.format) {
        if (ui.matchedSinceMs == 0) ui.matchedSinceMs = now;
        if (now - ui.matchedSinceMs > 1500) ui.wantedFormat = -1;
    } else {
        ui.matchedSinceMs = 0;
    }
    if (ui.wantedFormat < 0 || ui.wantedFormat == receiver.format || now - ui.lastRequestMs < 1000) return;
    pd->system->logToConsole("pdstream format %d", ui.wantedFormat);
    ui.lastRequestMs = now;
}

// The formats as a grid: sample rate rises to the right, bits per sample rise upward; each cell has a
// mono and a stereo format, except 44.1 kHz 16-bit stereo.
static int rateRank(int format) {
    int divisor = chain_formats[format].divisor;
    return divisor == 1 ? 2 : divisor == 2 ? 1 : 0;
}

static int codecRank(int format) {
    ChainCodec codec = chain_formats[format].codec;
    return codec == CODEC_PCM16 ? 2 : codec == CODEC_PCM8 ? 1 : 0;
}

static int findFormat(int rate, int codec, int channels) {
    for (int format = 0; format < chain_format_count; format++) {
        if (rateRank(format) == rate && codecRank(format) == codec && chain_formats[format].channels == channels)
            return format;
    }
    return -1;
}

// Rate and codec wrap around, skipping combinations that don't exist; channelsToggle switches mono/stereo.
static void stepFormat(int rateStep, int codecStep, int channelsToggle) {
    int current = ui.wantedFormat >= 0 ? ui.wantedFormat : receiver.format >= 0 ? receiver.format : chain_default_format;
    int rate = rateRank(current), codec = codecRank(current), channels = chain_formats[current].channels;
    if (channelsToggle) channels = 3 - channels;
    int found = -1;
    for (int tries = 0; tries < 3 && found < 0; tries++) {
        rate = (rate + rateStep + 3) % 3;
        codec = (codec + codecStep + 3) % 3;
        found = findFormat(rate, codec, channels);
        if (!rateStep && !codecStep) break;
    }
    if (found < 0) return;
    ui.wantedFormat = found;
    ui.lastRequestMs = pd->system->getCurrentTimeMilliseconds() - 1000;
}

static void handleInput(void) {
    PDButtons pushed;
    pd->system->getButtonState(NULL, &pushed, NULL);
    if (pushed & kButtonB) {
        ui.output = (Output)((ui.output + 1) % OUTPUT_COUNT);
        applyOutput();
    }
    if (pushed & kButtonLeft) stepFormat(-1, 0, 0);
    if (pushed & kButtonRight) stepFormat(1, 0, 0);
    if (pushed & kButtonUp) stepFormat(0, 1, 0);
    if (pushed & kButtonA) stepFormat(0, 0, 1);
    if (pushed & kButtonDown) stepFormat(0, -1, 0);
}

// Dropouts count per stream: a stream starting after a gap in the packets, or switching format, begins from zero.
static void resetDropoutsOnStart(void) {
    static int wasReceiving, lastFormat = -1;
    int receiving = isReceiving();
    if (receiving && (!wasReceiving || receiver.format != lastFormat)) {
        receiver.lost = 0;
        underruns = 0;
        overruns = 0;
    }
    wasReceiving = receiving;
    lastFormat = receiver.format;
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
    float db = framePeak > 0 ? 20.0f * log10f((float)framePeak / 32768.0f) : METER_FLOOR_DB;
    unsigned int now = pd->system->getCurrentTimeMilliseconds();
    ui.meterDb = meter_next_db(ui.meterDb, db, now - ui.lastMeterMs);
    ui.lastMeterMs = now;
    // The bar moves every frame; the number and the buffer readout change once per window so they can be read.
    if (db > ui.windowDb) ui.windowDb = db;
    if (now - ui.windowStartMs >= 500) {
        ui.shownDb = ui.windowDb;
        ui.shownBufferMs = samples_to_ms(writePos - readPos);
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
    if (ui.shownDb > -60) {
        int holdX = x + (int)((ui.shownDb + 60.0f) / 60.0f * (float)width);
        if (holdX > x + width - 1) holdX = x + width - 1;
        pd->graphics->drawLine(holdX, y + 1, holdX, y + height - 2, 1, holdX < x + filled ? kColorWhite : kColorBlack);
    }
    static const char* markLabels[] = {"-48", "-36", "-24", "-12", "0"};
    for (int i = 0; i < 5; i++) {
        int markX = i < 4 ? x + (12 * i + 12) * width / 60 : x + width - 1;
        pd->graphics->drawLine(markX, y + height, markX, y + height + 3, 1, kColorBlack);
        int labelWidth = pd->graphics->getTextWidth(fonts.tiny, markLabels[i], strlen(markLabels[i]), kUTF8Encoding, 0);
        drawString(fonts.tiny, markLabels[i], markX - (labelWidth - 1) / 2, y + height + 4);
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

// What survives a restart, in the app's data folder: a magic and a version byte, then the format and
// the pre-fill option. A file of another version is ignored.
#define SETTINGS_FILE "settings.bin"
#define SETTINGS_MAGIC 0x5D
#define SETTINGS_VERSION 2
static int savedFormat = -1;
static int savedPrefill = PREFILL_DEFAULT_OPTION;

static void loadSettings(void) {
    SDFile* file = pd->file->open(SETTINGS_FILE, kFileReadData);
    if (!file) return;
    uint8_t bytes[4];
    if (pd->file->read(file, bytes, sizeof(bytes)) == (int)sizeof(bytes) && bytes[0] == SETTINGS_MAGIC &&
        bytes[1] == SETTINGS_VERSION) {
        if (bytes[2] < chain_format_count) savedFormat = bytes[2];
        if (bytes[3] < PREFILL_OPTIONS) savedPrefill = bytes[3];
    }
    pd->file->close(file);
}

static void saveSettings(void) {
    SDFile* file = pd->file->open(SETTINGS_FILE, kFileWrite);
    if (!file) return;
    uint8_t bytes[4] = {SETTINGS_MAGIC, SETTINGS_VERSION, (uint8_t)savedFormat, (uint8_t)savedPrefill};
    pd->file->write(file, bytes, sizeof(bytes));
    pd->file->close(file);
}

// A stream's format is kept once it has held for a while, not while a switch is still settling.
static void rememberFormat(void) {
    static int candidate = -1;
    static unsigned int sinceMs;
    unsigned int now = pd->system->getCurrentTimeMilliseconds();
    int format = isReceiving() ? receiver.format : -1;
    if (format != candidate) {
        candidate = format;
        sinceMs = now;
    } else if (format >= 0 && format != savedFormat && now - sinceMs > 3000) {
        savedFormat = format;
        saveSettings();
    }
}

static void prefillChanged(void* userdata) {
    (void)userdata;
    int option = pd->system->getMenuItemValue(prefillItem);
    primeFill = prefill_samples(option);
    if (option != savedPrefill) {
        savedPrefill = option;
        saveSettings();
    }
}

static void startSource(void) {
    // Nothing plays between the old buffer and the new stream.
    readPos = writePos;
    primed = 0;
    applyOutput();
    source = pd->sound->addSource(sourceCallback, NULL, 1);
}

static void pauseStream(void) {
    paused = 1;
    // The update callback doesn't run while paused, so this is the one chance to ask the host to stop.
    pd->system->logToConsole("pdstream format -1");
    if (!source) return;
    pd->sound->removeSource(source);
    source = NULL;
}

static void resumeStream(void) {
    paused = 0;
    unsigned int now = pd->system->getCurrentTimeMilliseconds();
    ui.resumedAtMs = now ? now : 1;
    ui.lastRequestMs = now - 1000;
    if (!source) startSource();
}

static int update(void* userdata) {
    (void)userdata;
    feedReplay();
    handleInput();
    requestFormat();
    resetDropoutsOnStart();
    rememberFormat();
    updateRates();

    pd->graphics->clear(kColorWhite);
    drawString(fonts.label, "PD Audio Monitor Receiver", 12, 8);
    if (ui.wantedFormat >= 0 && ui.wantedFormat != receiver.format) {
        drawRow(40, "Format", "✛Ⓐ", "%s ...", chain_formats[ui.wantedFormat].deviceName);
    } else {
        drawRow(40, "Format", "✛Ⓐ", "%s", isReceiving() ? chain_formats[receiver.format].deviceName : "waiting for the host");
    }
    drawRow(64, "Output", "Ⓑ", "%s", outputNames[ui.output]);
    int kbTenths = ui.bytesPerSecond / 100;
    drawRow(88, "Link", "", "%d eval/s, %d.%d KB/s", ui.packetsPerSecond, kbTenths / 10, kbTenths % 10);
    drawRow(112, "Buffer", "", "%d ms", ui.shownBufferMs);
    drawMeter(148);
    if (isReceiving()) {
        drawRow(214, "Dropouts", "", "%d lost, %d under, %d over", (int)receiver.lost, (int)underruns, (int)overruns);
    } else {
        drawString(fonts.value, "Computer: route audio to a cable, run", 12, 196);
        drawString(fonts.value, "PD Audio Monitor Host", 12, 214);
    }
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
    if (event == kEventPause || event == kEventResume) {
        if (!pd) return 0;
        if (event == kEventPause) pauseStream();
        else resumeStream();
        return 0;
    }
    // Setting up at kEventInitLua, after the Lua runtime starts: a C update callback set at
    // kEventInit keeps it from starting, and `eval` needs it.
    if (event != kEventInitLua) return 0;
    pd = playdate;

    const char* error;
    if (!pd->lua->addFunction(feed, "pdstream_feed", &error)) pd->system->logToConsole("pdstream_feed: %s", error);

    fonts.label = loadFont("/System/Fonts/Roobert-11-Bold.pft");
    fonts.value = loadFont("/System/Fonts/Roobert-11-Medium.pft");
    fonts.glyph = loadFont("/System/Fonts/Asheville-Sans-14-Light.pft");
    fonts.tiny = loadFont("/System/Fonts/Roobert-10-Bold.pft");

    packet_init(&receiver);
    ui.output = OUTPUT_SPEAKER;
    ui.wantedFormat = -1;
    ui.meterDb = METER_FLOOR_DB;
    ui.windowDb = METER_FLOOR_DB;
    ui.shownDb = METER_FLOOR_DB;
    ui.lastMeterMs = pd->system->getCurrentTimeMilliseconds();
    ui.lastSecond = pd->system->getCurrentTimeMilliseconds() / 1000;

    static const char* prefillTitles[PREFILL_OPTIONS] = {
        "10 ms", "20 ms", "30 ms", "40 ms", "50 ms", "60 ms", "70 ms", "80 ms", "90 ms", "100 ms", "110 ms",
        "120 ms", "130 ms", "140 ms", "150 ms", "160 ms", "170 ms", "180 ms", "190 ms", "200 ms", "210 ms",
        "220 ms", "230 ms", "240 ms", "250 ms", "260 ms", "270 ms", "280 ms", "290 ms", "300 ms"};
    prefillItem = pd->system->addOptionsMenuItem("Pre-fill", prefillTitles, PREFILL_OPTIONS, prefillChanged, NULL);
    loadSettings();
    pd->system->setMenuItemValue(prefillItem, savedPrefill);
    ui.wantedFormat = savedFormat;
    prefillChanged(NULL);

    pd->system->setAutoLockDisabled(1);
    pd->display->setRefreshRate(50);
    pd->sound->getHeadphoneState(NULL, NULL, headphoneChanged);
    startSource();
    replay = pd->file->open("replay.bin", kFileReadData);

    pd->system->setUpdateCallback(update, NULL);
    return 0;
}
