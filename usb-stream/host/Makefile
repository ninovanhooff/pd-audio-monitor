CODEC = ../../shared/codec
ifeq ($(shell uname),Darwin)
LIB = libpdstream.dylib
else
LIB = libpdstream.so
endif

$(LIB): encoder.c $(CODEC)/format_stream.c $(CODEC)/format_chain.c $(CODEC)/ima_adpcm.c $(wildcard $(CODEC)/*.h)
	$(CC) -std=c11 -O2 -Wall -Wextra -shared -fPIC -I$(CODEC) -o $@ $(filter %.c,$^) -lm

clean:
	rm -f libpdstream.dylib libpdstream.so
