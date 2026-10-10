// Host tool: run 44.1 kHz mono s16le stdin through a format chain to stdout, to test the chain
// and to preview a mix on the Mac exactly as the line-in monitor degrades it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../format_chain.h"

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: chain_render <format> < in.s16le > out.s16le\n");
        for (int i = 0; i < chain_format_count; i++) fprintf(stderr, "  %d  %s\n", i, chain_formats[i].name);
        return 64;
    }
    int index = atoi(argv[1]);
    if (index < 0 || index >= chain_format_count) return 64;

    static FormatChain chain;
    chain_init(&chain, &chain_formats[index]);
    int16_t buffer[4096];
    size_t count;
    while ((count = fread(buffer, sizeof(int16_t), 4096, stdin)) > 0) {
        for (size_t i = 0; i < count; i++) buffer[i] = chain_process(&chain, buffer[i]);
        fwrite(buffer, sizeof(int16_t), count, stdout);
    }
    return 0;
}
