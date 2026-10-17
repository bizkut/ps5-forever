PS5_PAYLOAD_SDK := $(HOME)/ps5-payload-sdk-platform
PS5_HOST ?= 10.0.1.41
PS5_PORT ?= 9021
WINE_SRC ?= $(abspath ../../../ps5-wine)
OUT := ../build/writebroker

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

ELF := $(OUT)/writebroker.elf
CFLAGS := -Wall -Wextra -Werror -O2 -I$(WINE_SRC)/include -pthread

.PHONY: all send clean
all: $(ELF)

$(ELF): writebroker.c ../probes/psmkit-wine/writebroker-menu.h \
		$(WINE_SRC)/include/wine/ps5_writebroker.h
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ writebroker.c

# start it on the console (once per boot; a second copy exits)
send: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $(ELF)

clean:
	rm -rf $(OUT)
