LLVM_MINGW ?= /opt/llvm-mingw
CC := $(LLVM_MINGW)/bin/x86_64-w64-mingw32-clang
OUT ?= write-buffer-bench.exe

.PHONY: all clean
all: $(OUT)

$(OUT): write-buffer-bench.c
	$(CC) -std=c11 -O2 -g -Wall -Wextra -Werror -o $@ $<

clean:
	rm -f $(OUT)
