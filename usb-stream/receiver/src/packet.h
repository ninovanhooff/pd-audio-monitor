#ifndef PACKET_H
#define PACKET_H

#include <stdint.h>

#include "format_stream.h"

// A packet is: format index, sequence number, uint16 LE offset of the first data byte within its
// unit (bit 15 set: the stream restarts here), then data bytes of the format's stream
// (format_stream.h). A serial `eval` carries it raw, as the string argument of pdstream_feed.
// usb-stream/host/pdstream.py writes it; keep them in sync.
#define PACKET_HEADER_BYTES 4
#define PACKET_RESTART 0x8000
#define PACKET_OFFSET_MASK 0x7FFF

typedef void (*PacketEmit)(void* context, const int16_t* frames, int count, int channels);

typedef struct {
    StreamDecoder decoder;
    int format;
    uint8_t nextSequence;
    int skip;
    uint32_t packets;
    uint32_t bytes;
    uint32_t lost;
    int16_t output[STREAM_MAX_OUTPUT * STREAM_MAX_CHANNELS];
} PacketReceiver;

void packet_init(PacketReceiver* receiver);
// Returns 0 if the input isn't a packet.
int packet_receive(PacketReceiver* receiver, const uint8_t* packet, int length, PacketEmit emit, void* context);
// Finds the packet in an `eval` command's bytecode, as pdstream.py builds it: its length, or -1.
int packet_from_eval(const uint8_t* chunk, int length, const uint8_t** packet);

#endif
