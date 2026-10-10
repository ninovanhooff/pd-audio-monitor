#include "packet.h"

#include <string.h>

void packet_init(PacketReceiver* receiver) {
    receiver->format = -1;
    receiver->packets = receiver->bytes = receiver->lost = 0;
}

int packet_receive(PacketReceiver* receiver, const uint8_t* packet, int length, PacketEmit emit, void* context) {
    if (length < PACKET_HEADER_BYTES || packet[0] >= chain_format_count) return 0;

    const ChainFormat* format = &chain_formats[packet[0]];
    const int unit = stream_unit_bytes(format);
    const int offset = packet[2] | packet[3] << 8;
    if (offset >= unit) return 0;

    if (packet[0] != receiver->format) {
        receiver->format = packet[0];
        stream_decoder_init(&receiver->decoder, format);
        receiver->skip = (unit - offset) % unit;
    } else if (packet[1] != receiver->nextSequence) {
        receiver->lost += (uint8_t)(packet[1] - receiver->nextSequence);
        stream_decoder_resync(&receiver->decoder);
        receiver->skip = (unit - offset) % unit;
    }
    receiver->nextSequence = (uint8_t)(packet[1] + 1);
    receiver->packets++;
    receiver->bytes += (uint32_t)length;

    for (int i = PACKET_HEADER_BYTES; i < length; i++) {
        if (receiver->skip > 0) {
            receiver->skip--;
            continue;
        }
        int count = stream_decode(&receiver->decoder, packet[i], receiver->output);
        if (count > 0) emit(context, receiver->output, count);
    }
    return 1;
}

// The packet is the chunk's last string constant (EVAL_HEAD in pdstream.py), followed by a fixed
// 9-byte tail. Before it: a string tag, then its length plus one as a Lua dump size.
int packet_from_eval(const uint8_t* chunk, int length, const uint8_t** packet) {
    static const uint8_t tail[] = {0x81, 0x01, 0x00, 0x00, 0x80, 0x80, 0x80, 0x80, 0x80};
    static const char name[] = "pdstream_feed";
    const int nameLength = (int)sizeof(name) - 1, tailLength = (int)sizeof(tail);
    if (length < tailLength) return -1;
    const uint8_t* at = NULL;
    for (int i = 0; i + nameLength <= length && !at; i++)
        if (memcmp(chunk + i, name, (size_t)nameLength) == 0) at = chunk + i + nameLength;
    const uint8_t* end = chunk + length - tailLength;
    if (!at || memcmp(end, tail, (size_t)tailLength) != 0) return -1;
    if (at >= end || (*at != 0x04 && *at != 0x14)) return -1;
    at++;
    int size = 0;
    while (at < end && !(*at & 0x80)) size = size << 7 | *at++;
    if (at >= end) return -1;
    size = size << 7 | (*at++ & 0x7F);
    *packet = at;
    return at + size - 1 == end ? size - 1 : -1;
}
