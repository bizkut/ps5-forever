.NOTPARALLEL:
.DELETE_ON_ERROR:
OUT := ../../build/title-psmkit-wine-descriptor
WORK := ../../build/title-psmkit-wine-descriptor-work
CHILD_OUT := ../../build/title-psmkit-wine-descriptor-child
CHILD_WORK := ../../build/title-psmkit-wine-descriptor-child-work
HOST_OUT := ../../build/psmkit-wine-host
PE_OUT := ../../build/psmkit-wine-pe
TITLE_BUILD := ../../title/build.sh
TITLE_PARAM := ../../title/sce_sys/param.json
PS5_VULKAN ?= $(abspath ../../../../../PS5_Vulkan)
NATIVE_TOOL ?= $(PS5_VULKAN)/build/host/ps5-native-tool
PATCH := ../localproc/patch-local-helper.py
IO := ../localproc/psmkit-io.c
WINE_SRC ?= $(abspath ../../../../ps5-wine)
HOST_CC ?= cc
LLVM_MINGW ?= $(HOME)/llvm-mingw
MINGW_CC ?= $(LLVM_MINGW)/bin/x86_64-w64-mingw32-clang
MINGW32_CC ?= $(LLVM_MINGW)/bin/i686-w64-mingw32-clang
WINE_BUILD ?= ../../build/wine-ps5-build
WINEBUILD ?= ../../build/wine-ps5-tools/tools/winebuild/winebuild
WRITEBROKER_ELF := ../../build/writebroker/writebroker.elf
WRITEBROKER_DIR := ../../writebroker
RUNTIME_OUT := ../../build/title-psmkit-wine-runtime
RUNTIME_QUARANTINE := ../../build/title-psmkit-wine-runtime-quarantine
RUNTIME_PARENT := $(RUNTIME_QUARANTINE)/parent-title
PLATFORM_SRC ?= $(abspath ../../../../../PS5_PayloadSDK/platform)
# Override B1_OUT for isolated publications; every intermediate follows it.
B1_OUT ?= ../../build/title-psmkit-wine-b1
B1_QUARANTINE ?= $(B1_OUT)-quarantine
B1_MAINTENANCE_OUT ?= $(B1_OUT)-maintenance
B1_GAME_OUT ?= $(B1_OUT)-game
B1_PROFILE_CONFIG ?=
B1_GAME_PROFILE_CONFIG ?= $(CURDIR)/b1-game-wow-classic-beta.json
B1_CONFIG_ARG := $(if $(B1_PROFILE_CONFIG),--profile-config "$(B1_PROFILE_CONFIG)",)
TITLE_DESKTOP := ../../title/main.c ../../title/hub.c ../../title/compositor.c \
	../../title/display.c ../../title/input.c ../../title/pointer.c ../../title/ime.c

.PHONY: all descriptor test test-startup-routing test-runtime-profile pe runtime runtime-b1 runtime-b1-dual runtime-child runtime-parent runtime-qualification test-staged
all: descriptor pe
# Intentionally not named runtime: no Wine loader is entered by this gate.
descriptor: $(OUT)/eboot.bin $(OUT)/psmkit-wine-descriptor-child.bin
pe: $(PE_OUT)/console.exe $(PE_OUT)/p1w-parent.exe $(PE_OUT)/p1w-child.exe \
		$(PE_OUT)/audio-tone.exe

runtime: runtime-qualification

# One build creates both roles with the same reviewed import/module manifest.
runtime-child:
	python3 build-runtime.py --wine-build "$(WINE_BUILD)"

runtime-parent: runtime-child

runtime-qualification: runtime-child pe $(HOST_OUT)/test-staged-runtime
	python3 inspect-runtime.py --wine-build "$(WINE_BUILD)" \
		--host-test "$(HOST_OUT)/test-staged-runtime" \
		--parent-title "$(RUNTIME_PARENT)" --publish "$(RUNTIME_OUT)" \
		--pe-dir "$(PE_OUT)"

runtime-b1: $(HOST_OUT)/test-staged-runtime build-runtime.py runtime_profiles.py \
		runtime_imports.py runtime-import-policy-b1.json native-entry.c runtime-parent.c \
		runtime-parent.h runtime-relaunch.h writebroker-autostart.h runtime-vulkan.c \
		runtime-input.c protocol.c protocol.h title-log.c title-log.h $(TITLE_DESKTOP) \
		../../title/cursor-arrow.h $(PE_OUT)/audio-tone.exe $(WRITEBROKER_ELF)
	# B1 installs Wine's PE tree (make install-lib), which runs the i686/x86_64
	# mingw compilers configure chose; they live in llvm-mingw's bin.
	PATH="$(dir $(MINGW_CC)):$$PATH" python3 build-runtime.py --wine-build "$(WINE_BUILD)" \
		--profile b1 $(B1_CONFIG_ARG) --writebroker-elf "$(WRITEBROKER_ELF)" \
		--quarantine "$(B1_QUARANTINE)"
	python3 inspect-runtime.py --wine-build "$(WINE_BUILD)" --profile b1 $(B1_CONFIG_ARG) \
		--host-test "$(HOST_OUT)/test-staged-runtime" \
		--parent-title "$(B1_QUARANTINE)/parent-title" --quarantine "$(B1_QUARANTINE)" --publish "$(B1_OUT)"

$(WRITEBROKER_ELF): $(WRITEBROKER_DIR)/writebroker.c $(WRITEBROKER_DIR)/Makefile \
		$(WINE_SRC)/include/wine/ps5_writebroker.h
	$(MAKE) -C "$(WRITEBROKER_DIR)" WINE_SRC="$(WINE_SRC)" all

# Product publication: independently build/qualify maintenance first and game
# second, then bind both receipts and both role-specific children into one tree.
runtime-b1-dual:
	rm -rf "$(B1_MAINTENANCE_OUT)" "$(B1_GAME_OUT)"
	$(MAKE) runtime-b1 B1_PROFILE_CONFIG="$(CURDIR)/b1-product.json"
	cp -R "$(B1_OUT)" "$(B1_MAINTENANCE_OUT)"
	$(MAKE) runtime-b1 B1_PROFILE_CONFIG="$(B1_GAME_PROFILE_CONFIG)"
	cp -R "$(B1_OUT)" "$(B1_GAME_OUT)"
	python3 combine-b1-runtime.py --maintenance "$(B1_MAINTENANCE_OUT)" \
		--game "$(B1_GAME_OUT)" --output "$(B1_OUT)"

.PHONY: radv-release-patched
radv-release-patched:
	python3 ../../radv/build-radv.py

# Requires the configured Wine host tools; deliberately separate from test.
.PHONY: test-winebuild-delayload
test-winebuild-delayload: test-winebuild-delayload.py $(WINEBUILD)
	python3 test-winebuild-delayload.py --winebuild "$(WINEBUILD)" --llvm-mingw "$(LLVM_MINGW)"

test-runtime-profile: test-runtime-profile.py runtime_profiles.py runtime_imports.py \
		runtime-import-policy-b1.json ../../deploy-wine.py
	python3 test-runtime-profile.py

test-staged: runtime-qualification

$(HOST_OUT)/test-staged-runtime: test-staged-runtime.c elf-runtime.c elf-runtime.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread test-staged-runtime.c elf-runtime.c -o $@

test: $(HOST_OUT)/test-protocol $(HOST_OUT)/test-elf-runtime $(HOST_OUT)/test-wine-title \
		$(HOST_OUT)/test-loader-path $(HOST_OUT)/test-runtime-imports \
		$(HOST_OUT)/test-runtime-input $(HOST_OUT)/test-title-input \
		$(HOST_OUT)/test-title-ime $(HOST_OUT)/test-runtime-relaunch \
		$(HOST_OUT)/test-battlenet-restart $(HOST_OUT)/test-audio-mixer \
		$(HOST_OUT)/test-audio-selection $(HOST_OUT)/test-fd-parking \
		$(HOST_OUT)/test-writebroker-autostart $(HOST_OUT)/test-writebroker-space $(HOST_OUT)/test-title-log \
		$(HOST_OUT)/test-forever-setup $(HOST_OUT)/test-title-icon \
		$(HOST_OUT)/test-desktop-surface \
		$(PE_OUT)/audio-tone.exe test-startup-routing test-runtime-profile \
		../../tools/test-ps5-capture.py
	$(HOST_OUT)/test-protocol
	$(HOST_OUT)/test-elf-runtime
	$(HOST_OUT)/test-wine-title
	$(HOST_OUT)/test-loader-path
	$(HOST_OUT)/test-runtime-imports
	$(HOST_OUT)/test-runtime-input
	$(HOST_OUT)/test-title-input
	$(HOST_OUT)/test-title-ime
	$(HOST_OUT)/test-runtime-relaunch
	$(HOST_OUT)/test-battlenet-restart
	$(HOST_OUT)/test-audio-mixer
	$(HOST_OUT)/test-audio-selection
	$(HOST_OUT)/test-fd-parking
	$(HOST_OUT)/test-writebroker-autostart
	$(HOST_OUT)/test-writebroker-space
	$(HOST_OUT)/test-title-log
	$(HOST_OUT)/test-forever-setup
	$(HOST_OUT)/test-title-icon
	$(HOST_OUT)/test-desktop-surface
	python3 ../../tools/test-ps5-capture.py

$(HOST_OUT)/test-fd-parking: test-fd-parking.c build-fd-parking-test.py $(WINE_SRC)/server/fd.c
	python3 build-fd-parking-test.py --wine "$(WINE_SRC)" --output "$@" --cc "$(HOST_CC)"

$(HOST_OUT)/test-audio-selection: test-audio-selection.c \
		$(WINE_SRC)/dlls/mmdevapi/ps5audio.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=c11 -O2 -g -Wall -Wextra -Werror \
		-I "$(WINE_SRC)/dlls/mmdevapi" $< -o $@

$(HOST_OUT)/test-audio-mixer: test-audio-mixer.c \
		$(WINE_SRC)/dlls/wineps5audio.drv/mixer.c \
		$(WINE_SRC)/dlls/wineps5audio.drv/mixer.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=c11 -O2 -g -Wall -Wextra -Werror \
		-I "$(WINE_SRC)/dlls/wineps5audio.drv" \
		test-audio-mixer.c $(WINE_SRC)/dlls/wineps5audio.drv/mixer.c -o $@

$(HOST_OUT)/test-runtime-relaunch: test-runtime-relaunch.c runtime-relaunch.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=c11 -O2 -g -Wall -Wextra -Werror $< -o $@

.PHONY: test-desktop-surface
test-desktop-surface: $(HOST_OUT)/test-desktop-surface
	$(HOST_OUT)/test-desktop-surface

$(HOST_OUT)/test-desktop-surface: test-desktop-surface.c build-desktop-surface-test.py \
		../../title/compositor.c ../../title/compositor.h ../../title/display.h \
		$(WINE_SRC)/dlls/wineps5.drv/window.c $(WINE_SRC)/dlls/ntdll/unix/server.c \
		$(WINE_SRC)/include/wine/ps5_desktop.h $(WINE_SRC)/include/wine/ps5_title.h \
		runtime-parent.c protocol.h
	python3 build-desktop-surface-test.py --wine "$(WINE_SRC)" --output "$@" --cc "$(HOST_CC)"

$(HOST_OUT)/test-writebroker-autostart: test-writebroker-autostart.c writebroker-autostart.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=c11 -O2 -g -Wall -Wextra -Werror $< -o $@

$(HOST_OUT)/test-writebroker-space: test-writebroker-space.c build-writebroker-space-test.py \
		$(WRITEBROKER_DIR)/writebroker.c $(WINE_SRC)/dlls/ntdll/unix/file.c \
		$(WINE_SRC)/include/wine/ps5_writebroker.h
	python3 build-writebroker-space-test.py --wine "$(WINE_SRC)" --output "$@" --cc "$(HOST_CC)"

$(HOST_OUT)/test-forever-setup: test-forever-setup.c forever-setup.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=c11 -O2 -g -Wall -Wextra -Werror $< -o $@

$(HOST_OUT)/test-title-icon: test-title-icon.c title-icon.h forever-setup.h third_party/stb/stb_image.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=c11 -O2 -g -Wall -Wextra -Werror $< -o $@ -lm

# Short tuning poll so the live log_async switch is exercised in milliseconds.
$(HOST_OUT)/test-title-log: test-title-log.c title-log.c title-log.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread -DPW_TITLE_LOG_TUNE_POLL_MS=20 \
		test-title-log.c title-log.c -o $@

$(HOST_OUT)/test-battlenet-restart: test-battlenet-restart.c \
		$(WINE_SRC)/programs/explorer/battlenet_restart.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=c11 -O2 -g -Wall -Wextra -Werror \
		-I "$(WINE_SRC)/programs/explorer" $< -o $@

$(HOST_OUT)/test-runtime-input: test-runtime-input.c runtime-input.c ../../title/pointer.c \
		../../title/input.h ../../title/pointer.h ../../title/ime.h ../../title/cursor-arrow.h \
		$(WINE_SRC)/include/wine/ps5_title_input.h $(WINE_SRC)/include/wine/ps5_desktop.h \
		$(WINE_SRC)/include/wine/ps5_title_gamepad.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread -I "$(WINE_SRC)/include" test-runtime-input.c ../../title/pointer.c -o $@

$(HOST_OUT)/test-title-input: test-title-input.c ../../title/input.c ../../title/input.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=gnu11 -O2 -g -Wall -Wextra -Werror $< -o $@

$(HOST_OUT)/test-title-ime: test-title-ime.c ../../title/ime.c ../../title/ime.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=gnu11 -O2 -g -Wall -Wextra -Werror $< -o $@

$(HOST_OUT)/test-runtime-imports: test-runtime-imports.py runtime_imports.py runtime_profiles.py runtime-import-policy.json
	mkdir -p $(HOST_OUT)
	printf '%s\n' '#!/bin/sh' 'exec python3 "$(CURDIR)/test-runtime-imports.py" --cc "$${HOST_CC:-$(HOST_CC)}"' > $@
	chmod +x $@

$(HOST_OUT)/test-protocol: test-protocol.c protocol.c protocol.h $(IO) ../localproc/psmkit-io.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=gnu11 -O2 -g -Wall -Wextra -Werror -DPW_P1C_FAULT_INJECTION test-protocol.c protocol.c $(IO) -o $@

$(HOST_OUT)/test-elf-runtime: test-elf-runtime.c elf-runtime.c elf-runtime.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread test-elf-runtime.c -o $@

$(HOST_OUT)/test-wine-title: $(WINE_SRC)/loader/tests/ps5_title.c $(WINE_SRC)/include/wine/ps5_title.h $(WINE_SRC)/include/wine/ps5_title_private.h $(WINE_SRC)/include/wine/ps5_suspend.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=c11 -O2 -g -pthread -I "$(WINE_SRC)/include" $< -o $@

$(HOST_OUT)/test-loader-path: test-loader-path.c build-path-test.py $(WINE_SRC)/loader/main.h $(PLATFORM_SRC)/src/libc.c $(PLATFORM_SRC)/include/ps5platform/libc.h
	mkdir -p $(HOST_OUT)
	python3 build-path-test.py --platform "$(PLATFORM_SRC)" --wine "$(WINE_SRC)" --output "$@" --cc "$(HOST_CC)"

# Compile complete production functions on the host, for both platform guards.
test-startup-routing: $(HOST_OUT)/test-startup-routing $(HOST_OUT)/non-ps5/test-startup-routing
	$(HOST_OUT)/test-startup-routing
	$(HOST_OUT)/non-ps5/test-startup-routing

$(HOST_OUT)/test-startup-routing: test-startup-routing.c build-startup-routing-test.py $(WINE_SRC)/dlls/ntdll/unix/loader.c $(WINE_SRC)/dlls/ntdll/unix/env.c
	python3 build-startup-routing-test.py --wine "$(WINE_SRC)" --output "$@" --cc "$(HOST_CC)"

$(HOST_OUT)/non-ps5/test-startup-routing: test-startup-routing.c build-startup-routing-test.py $(WINE_SRC)/dlls/ntdll/unix/loader.c $(WINE_SRC)/dlls/ntdll/unix/env.c
	python3 build-startup-routing-test.py --wine "$(WINE_SRC)" --output "$@" --cc "$(HOST_CC)" --non-ps5

$(OUT)/eboot.bin: parent.c protocol.c protocol.h $(IO) ../localproc/probe.h $(TITLE_BUILD) $(TITLE_PARAM)
	WINE_SRC="$(WINE_SRC)" PS5_TITLE_RADV= PS5_TITLE_WORK="$(abspath $(WORK))" \
		bash $(TITLE_BUILD) $(OUT) parent.c protocol.c $(IO)

$(CHILD_OUT)/eboot.bin: child.c protocol.c protocol.h $(IO) $(PATCH) $(TITLE_BUILD) $(TITLE_PARAM)
	WINE_SRC="$(WINE_SRC)" PS5_TITLE_RADV= PS5_TITLE_WORK="$(abspath $(CHILD_WORK))" \
		bash $(TITLE_BUILD) $(CHILD_OUT) child.c protocol.c $(IO)
	python3 $(PATCH) $(CHILD_WORK)/eboot.elf $(CHILD_WORK)/localproc.elf
	$(NATIVE_TOOL) self --sign --in $(CHILD_WORK)/localproc.elf --out $@ --magic 0x1D3D154F
	python3 $(PATCH) --container $@ $@
	$(NATIVE_TOOL) self --inspect --file $@

$(OUT)/psmkit-wine-descriptor-child.bin: $(CHILD_OUT)/eboot.bin $(OUT)/eboot.bin
	cp $(CHILD_OUT)/eboot.bin $@

$(PE_OUT)/p1w-parent.exe: p1w-parent.c
	mkdir -p $(PE_OUT)
	$(MINGW32_CC) -O2 -Wall -Wextra -Werror -ffreestanding -fno-stack-protector -nostdlib \
		-Wl,--subsystem,console -Wl,--entry,mainCRTStartup -o $@ $< -lkernel32 -luser32

$(PE_OUT)/p1w-child.exe: p1w-child.c
	mkdir -p $(PE_OUT)
	$(MINGW32_CC) -O2 -Wall -Wextra -Werror -ffreestanding -fno-stack-protector -nostdlib \
		-Wl,--subsystem,console -Wl,--entry,mainCRTStartup -o $@ $< -lkernel32 -luser32

$(PE_OUT)/audio-tone.exe: audio-tone.c
	mkdir -p $(PE_OUT)
	$(MINGW_CC) -O2 -Wall -Wextra -Werror -o $@ $< -lwinmm

$(PE_OUT)/%.exe: %.c
	mkdir -p $(PE_OUT)
	$(MINGW_CC) -O2 -Wall -Wextra -Werror -ffreestanding -fno-stack-protector -nostdlib \
		-Wl,--subsystem,console -Wl,--entry,mainCRTStartup -o $@ $< -lkernel32
