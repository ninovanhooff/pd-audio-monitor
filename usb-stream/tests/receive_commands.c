// Host harness: pdstream.py's eval commands on stdin -> the receiver's packet decoding -> 44.1 kHz
// interleaved s16le on stdout. Prints packet counts to stderr.
#include <stdio.h>

#include "packet.h"

static void emit(void* context, const int16_t* frames, int count, int channels) {
    (void)context;
    fwrite(frames, sizeof(int16_t), (size_t)(count * channels), stdout);
}

int main(void) {
    static PacketReceiver receiver;
    static char line[64];
    static uint8_t chunk[65536];
    int rejected = 0;
    packet_init(&receiver);
    while (fgets(line, sizeof(line), stdin)) {
        int length;
        const uint8_t* packet;
        int size;
        if (sscanf(line, "eval %d", &length) != 1 || length > (int)sizeof(chunk) ||
            fread(chunk, 1, (size_t)length, stdin) != (size_t)length ||
            (size = packet_from_eval(chunk, length, &packet)) < 0 ||
            !packet_receive(&receiver, packet, size, emit, NULL))
            rejected++;
    }
    fprintf(stderr, "packets %u lost %u rejected %d\n", receiver.packets, receiver.lost, rejected);
    return 0;
}
