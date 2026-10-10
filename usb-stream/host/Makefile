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

# PDStream.app: the same in a window. macOS asks microphone access for the app itself. Its build
# files get other names than pdstream's: macOS file names ignore case.
app: dist/PDStream.app

dist/PDStream.app: pdstream.py pdstream_gui.py PDStream.spec $(LIB)
	$(PYTHON) -m PyInstaller --noconfirm --log-level WARN --workpath build/app --distpath dist PDStream.spec

clean:
	rm -rf libpdstream.dylib libpdstream.so build dist

.PHONY: bundle app clean
