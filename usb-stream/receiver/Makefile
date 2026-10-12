HEAP_SIZE      = 8388208
STACK_SIZE     = 61800

PRODUCT = PDAudioMonitorReceiver.pdx

SDK = ${PLAYDATE_SDK_PATH}
ifeq ($(SDK),)
	SDK = $(shell egrep '^\s*SDKRoot' ~/.Playdate/config | head -n 1 | cut -c9-)
endif
ifeq ($(SDK),)
$(error SDK path not found; set ENV value PLAYDATE_SDK_PATH)
endif

# Source/pdxinfo holds the version for everything: the app title, the host's window and its log. A header
# keeps main.c's dependency tracking honest when it changes.
APP_VERSION = $(shell sed -n 's/^version=//p' Source/pdxinfo)
$(shell mkdir -p build && printf '\043define APP_VERSION "%s"\n' '$(APP_VERSION)' | cmp -s - build/version.h || printf '\043define APP_VERSION "%s"\n' '$(APP_VERSION)' > build/version.h)

# Absolute paths: common.mk puts objects at build/<source path>, and ../ would escape build/.
CODEC = $(abspath ../../shared/codec)

VPATH += src
SRC = src/main.c src/packet.c $(CODEC)/ima_adpcm.c $(CODEC)/format_chain.c $(CODEC)/format_stream.c
UINCDIR = $(CODEC) build
UASRC =
UDEFS =
UADEFS =
ULIBDIR =
ULIBS =

include $(SDK)/C_API/buildsupport/common.mk

$(OBJDIR)/pdex.$(DYLIB_EXT): $(OBJDIR)/version.h
