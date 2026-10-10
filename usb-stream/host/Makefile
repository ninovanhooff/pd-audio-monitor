CODEC = ../../shared/codec
ifeq ($(shell uname),Darwin)
LIB = libpdstream.dylib
else
LIB = libpdstream.so
endif

$(LIB): encoder.c $(CODEC)/format_stream.c $(CODEC)/format_chain.c $(CODEC)/ima_adpcm.c $(wildcard $(CODEC)/*.h)
	$(CC) -std=c11 -O2 -Wall -Wextra -shared -fPIC -I$(CODEC) -o $@ $(filter %.c,$^) -lm

PYTHON ?= python3

# One executable with Python, numpy, PortAudio and $(LIB) inside; needs `pip install pyinstaller`.
bundle: dist/pdstream

dist/pdstream: pdstream.py $(LIB)
	$(PYTHON) -m PyInstaller --onefile --noconfirm --log-level WARN --name pdstream \
		--specpath build --workpath build --distpath dist --add-binary $(abspath $(LIB)):. pdstream.py

clean:
	rm -rf libpdstream.dylib libpdstream.so build dist

.PHONY: bundle clean
