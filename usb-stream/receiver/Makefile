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

# Absolute paths: common.mk puts objects at build/<source path>, and ../ would escape build/.
CODEC = $(abspath ../../shared/codec)

VPATH += src
SRC = src/main.c src/packet.c $(CODEC)/ima_adpcm.c $(CODEC)/format_chain.c $(CODEC)/format_stream.c
UINCDIR = $(CODEC)
UASRC =
UDEFS =
UADEFS =
ULIBDIR =
ULIBS =

include $(SDK)/C_API/buildsupport/common.mk
