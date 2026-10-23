PS5_PAYLOAD_SDK ?= $(HOME)/ps5-payload-sdk-platform
PS5_HOST ?= 10.0.1.41
PS5_PORT ?= 9021
OUT := ../build/addon-worker

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

ELF := $(OUT)/addon-worker.elf
CXXFLAGS := -std=c++20 -Wall -Wextra -Werror -O2 -fno-exceptions -fno-rtti \
	-I. -DFOREVER_ADDON_WORKER_CONSOLE -pthread
CFLAGS := -std=c11 -O2 -w -Ithird_party/miniz
LDFLAGS := -Wl,--build-id=none -pthread
MINIZ := miniz miniz_tinfl miniz_tdef miniz_zip
OBJECTS := $(OUT)/main.o $(OUT)/addon-worker.o $(addprefix $(OUT)/,$(addsuffix .o,$(MINIZ)))
HEADERS := addon-worker.hpp protocol.h ../probes/psmkit-wine/sha256.h
.PHONY: all verify send clean
all: verify $(ELF)

verify: verify-sources.py third_party/miniz/SOURCE.json
	python3 verify-sources.py

$(OUT)/main.o: main.cpp $(HEADERS) Makefile
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(OUT)/addon-worker.o: addon-worker.cpp $(HEADERS) $(wildcard third_party/miniz/*.h) Makefile
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(OUT)/%.o: third_party/miniz/%.c $(wildcard third_party/miniz/*.h) Makefile
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -c -o $@ $<

$(ELF): $(OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $^

send: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $(ELF)

clean:
	rm -rf $(OUT)
