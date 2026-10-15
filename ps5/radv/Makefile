# RADV as libvulkan.so for Wine's payloads: win32u dlopens it by its console
# path (SONAME_LIBVULKAN, ps5/build-wine.sh) and takes vkGetInstanceProcAddr
# and vkGetDeviceProcAddr (vulkan_entry.c). The archive comes from
# ps5/build-radv-fork.sh; the link is ps5/probes/radvpay's, as a shared
# object (see that Makefile for why each runtime input and wrap is there).
#
#   make -C ps5/radv          # -> ../build/radv-release/lib/libvulkan.so

PS5_PAYLOAD_SDK ?= $(HOME)/ps5-payload-sdk-platform
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
# paths.sh is shell: make would read its $TITLE_ID as $T.
WINE_ROOT := $(shell . ../paths.sh && printf '%s' "$$WINE_ROOT")

RADV_ROOT ?= $(abspath ../../../../PS5_Vulkan)
RADV_RELEASE ?= $(abspath ../build/radv-release)
RADV_ARCHIVE := $(RADV_RELEASE)/lib/libvulkan_radeon.ps5.a
CLANG_BUILTINS ?= $(shell $(CC) --print-resource-dir)/lib/linux/libclang_rt.builtins-x86_64.a
SONAME := $(WINE_ROOT)/lib/libvulkan.so

SO := $(RADV_RELEASE)/lib/libvulkan.so
BUILD := ../build/radv-so
CFLAGS := -std=c11 -Wall -Wextra -Werror -O2 -fPIC
# AGC is loaded at run time (agc_lazy.c): importing its modules would give
# every process that loads libvulkan.so a turn at the GPU's graphics pipe.
RUNTIME := $(PS5_SYSROOT)/lib/libc++.a $(PS5_SYSROOT)/lib/libc++abi.a \
           $(PS5_SYSROOT)/lib/libunwind.a $(PS5_SYSROOT)/lib/libc.a \
           $(CLANG_BUILTINS) $(PS5_SYSROOT)/lib/libps5platform.a
LDFLAGS := -shared -Wl,-soname,$(SONAME) \
           -Wl,--no-undefined,--wrap=pthread_create,--wrap=pthread_join,--wrap=pthread_detach \
           -Wl,--wrap=fclose,--wrap=fflush,--defsym=open_memstream=ps5_open_memstream \
           -Wl,--defsym=__cxa_thread_atexit_impl=ps5___cxa_thread_atexit_impl \
           -Wl,--version-script=libvulkan.map

.PHONY: all clean
all: $(SO)

$(SO): Makefile libvulkan.map $(BUILD)/vulkan_entry.o $(BUILD)/agc_lazy.o $(RADV_ARCHIVE) $(RUNTIME)
	$(CC) $(CFLAGS) -nodefaultlibs $(LDFLAGS) -o $@ $(BUILD)/vulkan_entry.o $(BUILD)/agc_lazy.o \
	    -Wl,--whole-archive $(RADV_ARCHIVE) -Wl,--no-whole-archive \
	    -Wl,--start-group $(RUNTIME) -Wl,--end-group \
	    -lkernel_web -lSceLibcInternal -lSceNet -lSceVideoOut

$(BUILD):
	mkdir -p $@

$(BUILD)/vulkan_entry.o: vulkan_entry.c | $(BUILD)
	$(CC) -I$(RADV_RELEASE)/include $(CFLAGS) -c $< -o $@

$(BUILD)/agc_lazy.o: agc_lazy.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD) $(SO)
