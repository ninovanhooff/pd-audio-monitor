CODEC = ../../shared/codec
SHARED = -fPIC
ifeq ($(OS),Windows_NT)
# MinGW-w64 exports every non-static symbol; -static-libgcc keeps libgcc_s_*.dll out of the way.
LIB = libpdstream.dll
SHARED = -static-libgcc
else ifeq ($(shell uname),Darwin)
LIB = libpdstream.dylib
else
LIB = libpdstream.so
endif

$(LIB): encoder.c $(CODEC)/format_stream.c $(CODEC)/format_chain.c $(CODEC)/ima_adpcm.c $(wildcard $(CODEC)/*.h)
	$(CC) -std=c11 -O2 -Wall -Wextra -shared $(SHARED) -I$(CODEC) -o $@ $(filter %.c,$^) -lm

PYTHON ?= python3

# One executable with Python, numpy, PortAudio and $(LIB) inside; needs `pip install pyinstaller`.
bundle: dist/pdstream

dist/pdstream: pdstream.py $(LIB)
	$(PYTHON) -m PyInstaller --onefile --noconfirm --log-level WARN --name pdstream \
		--specpath build --workpath build --distpath dist --add-binary $(abspath $(LIB)):. --add-data $(abspath ../receiver/Source/pdxinfo):. pdstream.py

# "PD Audio Monitor Host.app": the same in a window. macOS asks microphone access for the app itself.
# Make can't name a file with spaces, so a stamp stands in for it, and a missing app forces a rebuild.
app: build/app/stamp

build/app/stamp: pdstream.py pdstream_gui.py host.spec $(LIB) $(if $(wildcard dist/PD?Audio?Monitor?Host.app),,FORCE)
	$(PYTHON) -m PyInstaller --noconfirm --log-level WARN --workpath build/app --distpath dist host.spec
	touch $@

FORCE:

clean:
	rm -rf libpdstream.dylib libpdstream.so libpdstream.dll build dist

.PHONY: bundle app clean FORCE
