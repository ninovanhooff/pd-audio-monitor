#ifndef PACKET_H
#define PACKET_H

#include <stdint.h>

#include "format_stream.h"

// A packet is: format index, sequence number, uint16 LE offset of the first data byte within its
// unit, then data bytes of the format's stream (format_stream.h). A serial `msg` line carries it in
// base64; an `eval` carries it raw, as the string argument of pdstream_feed.
// usb-stream/host/pdstream.py writes both; keep them in sync.
#define PACKET_HEADER_BYTES 4
#define PACKET_MAX_BYTES 192
// Over `msg`, the bypass format needs 493 msg/s; a Playdate takes about 390.
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
// Return 0 if the input isn't a packet.
int packet_receive(PacketReceiver* receiver, const char* text, PacketEmit emit, void* context);
int packet_receive_bytes(PacketReceiver* receiver, const uint8_t* packet, int length, PacketEmit emit, void* context);

#endif
