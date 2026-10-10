// Host harness: pdstream.py's output (msg lines or eval commands) on stdin -> the receiver's packet
// decoding -> 44.1 kHz s16le on stdout. Prints packet counts to stderr.
#include <stdio.h>
#include <string.h>

#include "packet.h"

static void emit(void* context, const int16_t* samples, int count) {
    (void)context;
    fwrite(samples, sizeof(int16_t), (size_t)count, stdout);
}

// The packet is the chunk's last string constant (see EVAL_HEAD in pdstream.py), followed by a fixed
// 9-byte tail. Before it: a string tag, then its length plus one as a Lua dump size.
static int eval_packet(const uint8_t* chunk, int length, const uint8_t** packet) {
    static const uint8_t tail[] = {0x81, 0x01, 0x00, 0x00, 0x80, 0x80, 0x80, 0x80, 0x80};
    static const char name[] = "pdstream_feed";
    const uint8_t* at = NULL;
    for (int i = 0; i + (int)sizeof(name) - 1 <= length && !at; i++)
        if (memcmp(chunk + i, name, sizeof(name) - 1) == 0) at = chunk + i;
    if (!at || length < (int)sizeof(tail) || memcmp(chunk + length - sizeof(tail), tail, sizeof(tail)) != 0) return -1;
    at += sizeof(name) - 1;
    if (*at != 0x04 && *at != 0x14) return -1;
    at++;
    int size = 0;
    while (!(*at & 0x80)) size = size << 7 | *at++;
    size = size << 7 | (*at++ & 0x7F);
    *packet = at;
    return at + size - 1 == chunk + length - sizeof(tail) ? size - 1 : -1;
}

int main(void) {
    static PacketReceiver receiver;
    static char line[1024];
    static uint8_t chunk[65536];
    int rejected = 0;
    packet_init(&receiver);
    while (fgets(line, sizeof(line), stdin)) {
        int length;
        if (sscanf(line, "eval %d", &length) == 1) {
            const uint8_t* packet;
            int size;
            if (length > (int)sizeof(chunk) || fread(chunk, 1, (size_t)length, stdin) != (size_t)length ||
                (size = eval_packet(chunk, length, &packet)) < 0 ||
                !packet_receive_bytes(&receiver, packet, size, emit, NULL))
                rejected++;
            continue;
        }
        const char* text = line;
        if (strncmp(text, "msg ", 4) == 0) text += 4;
        if (!packet_receive(&receiver, text, emit, NULL)) rejected++;
    }
    fprintf(stderr, "packets %u lost %u rejected %d\n", receiver.packets, receiver.lost, rejected);
    return 0;
}
