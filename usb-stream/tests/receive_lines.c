// Host harness: msg lines on stdin -> the receiver's packet decoding -> 44.1 kHz s16le on stdout.
// Prints packet counts to stderr.
#include <stdio.h>

#include "packet.h"

static void emit(void* context, const int16_t* samples, int count) {
    (void)context;
    fwrite(samples, sizeof(int16_t), (size_t)count, stdout);
}

int main(void) {
    static PacketReceiver receiver;
    static char line[1024];
    int rejected = 0;
    packet_init(&receiver);
    while (fgets(line, sizeof(line), stdin)) {
        const char* text = line;
        if (text[0] == 'm' && text[1] == 's' && text[2] == 'g' && text[3] == ' ') text += 4;
        if (!packet_receive(&receiver, text, emit, NULL)) rejected++;
    }
    fprintf(stderr, "packets %u lost %u rejected %d\n", receiver.packets, receiver.lost, rejected);
    return 0;
}
