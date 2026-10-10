#include "packet.h"

static int base64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int base64_decode(const char* text, uint8_t* out, int capacity) {
    uint32_t bits = 0;
    int count = 0, written = 0;
    for (; *text && *text != '='; text++) {
        int value = base64_value(*text);
        if (value < 0) {
            if (*text == '\r' || *text == '\n' || *text == ' ') continue;
            return -1;
        }
        bits = bits << 6 | (uint32_t)value;
        if (++count == 4) {
            if (written + 3 > capacity) return -1;
            out[written++] = (uint8_t)(bits >> 16);
            out[written++] = (uint8_t)(bits >> 8);
            out[written++] = (uint8_t)bits;
            bits = 0;
            count = 0;
        }
    }
    if (count == 1) return -1;
    if (count > 1) {
        bits <<= 6 * (4 - count);
        if (written + count - 1 > capacity) return -1;
        out[written++] = (uint8_t)(bits >> 16);
        if (count == 3) out[written++] = (uint8_t)(bits >> 8);
    }
    return written;
}

void packet_init(PacketReceiver* receiver) {
    receiver->format = -1;
    receiver->packets = receiver->bytes = receiver->lost = 0;
}

int packet_receive(PacketReceiver* receiver, const char* text, PacketEmit emit, void* context) {
    uint8_t packet[PACKET_MAX_BYTES];
    int length = base64_decode(text, packet, PACKET_MAX_BYTES);
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
