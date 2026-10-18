LLVM_MINGW ?= /opt/llvm-mingw
CC := $(LLVM_MINGW)/bin/x86_64-w64-mingw32-clang
OUT ?= scan-bench.exe
WORKSPACE ?= 33554432
WORKERS ?= 4

.PHONY: all clean
all: $(OUT)

$(OUT): scan-bench.c
	$(CC) -std=c11 -O2 -g -Wall -Wextra -Werror -DAGENT_WORKSPACE=$(WORKSPACE) -DSCAN_BENCH_WORKERS=$(WORKERS) -o $@ $< -lbcrypt -luser32

clean:
	rm -f $(OUT)
