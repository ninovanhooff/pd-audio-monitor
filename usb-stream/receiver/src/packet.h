#ifndef PACKET_H
#define PACKET_H

#include <stdint.h>

#include "format_stream.h"

// One serial `msg` line carries base64 of: format index, sequence number, uint16 LE offset of the
// first data byte within its unit, then data bytes of the format's stream (format_stream.h).
// usb-stream/host/pdstream.py writes it; keep both in sync.
#define PACKET_HEADER_BYTES 4
#define PACKET_MAX_BYTES 192
// The bypass format needs 493 msg/s; a Playdate takes about 390.
#define PACKET_UNSTREAMABLE_FORMAT 0

typedef void (*PacketEmit)(void* context, const int16_t* samples, int count);

typedef struct {
    StreamDecoder decoder;
    int format;
    uint8_t nextSequence;
    int skip;
    uint32_t packets;
    uint32_t bytes;
    uint32_t lost;
    int16_t output[STREAM_MAX_OUTPUT];
} PacketReceiver;

void packet_init(PacketReceiver* receiver);
// Returns 0 if text isn't a packet.
int packet_receive(PacketReceiver* receiver, const char* text, PacketEmit emit, void* context);

#endif
