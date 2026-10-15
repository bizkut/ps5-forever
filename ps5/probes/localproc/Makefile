OUT := ../../build/title-localproc
PSMKIT_OUT := ../../build/title-localproc-psmkit
PSMKIT_WORK := ../../build/title-localproc-psmkit-work
CHILD_OUT := ../../build/title-localproc-child
CHILD_WORK := ../../build/title-localproc-child-work
PSMKIT_CHILD_OUT := ../../build/title-localproc-psmkit-child
PSMKIT_CHILD_WORK := ../../build/title-localproc-psmkit-child-work
PSMKIT_IO_OUT := ../../build/title-localproc-psmkit-io
PSMKIT_IO_WORK := ../../build/title-localproc-psmkit-io-work
PSMKIT_IO_CHILD_OUT := ../../build/title-localproc-psmkit-io-child
PSMKIT_IO_CHILD_WORK := ../../build/title-localproc-psmkit-io-child-work
PSMKIT_DMEMSHARE_OUT := ../../build/title-localproc-psmkit-dmemshare
PSMKIT_DMEMSHARE_WORK := ../../build/title-localproc-psmkit-dmemshare-work
PSMKIT_DMEMSHARE_CHILD_OUT := ../../build/title-localproc-psmkit-dmemshare-child
PSMKIT_DMEMSHARE_CHILD_WORK := ../../build/title-localproc-psmkit-dmemshare-child-work
PSMKIT_SOCKET_OUT := ../../build/title-localproc-psmkit-socket
PSMKIT_SOCKET_WORK := ../../build/title-localproc-psmkit-socket-work
PSMKIT_SOCKET_CHILD_OUT := ../../build/title-localproc-psmkit-socket-child
PSMKIT_SOCKET_CHILD_WORK := ../../build/title-localproc-psmkit-socket-child-work
PSMKIT_DETECT_OUT := ../../build/title-localproc-psmkit-detect
PSMKIT_DETECT_WORK := ../../build/title-localproc-psmkit-detect-work
PSMKIT_DETECT_CHILD_OUT := ../../build/title-localproc-psmkit-detect-child
PSMKIT_DETECT_CHILD_WORK := ../../build/title-localproc-psmkit-detect-child-work
PSMKIT_FD3_OUT := ../../build/title-localproc-psmkit-fd3
PSMKIT_FD3_WORK := ../../build/title-localproc-psmkit-fd3-work
PSMKIT_FD3_CHILD_OUT := ../../build/title-localproc-psmkit-fd3-child
PSMKIT_FD3_CHILD_WORK := ../../build/title-localproc-psmkit-fd3-child-work
PSMKIT_TWO_OUT := ../../build/title-localproc-psmkit-two
PSMKIT_TWO_WORK := ../../build/title-localproc-psmkit-two-work
PSMKIT_TWO_CHILD_OUT := ../../build/title-localproc-psmkit-two-child
PSMKIT_TWO_CHILD_WORK := ../../build/title-localproc-psmkit-two-child-work
PSMKIT_CAPACITY_OUT := ../../build/title-localproc-psmkit-capacity
PSMKIT_CAPACITY_WORK := ../../build/title-localproc-psmkit-capacity-work
PSMKIT_CAPACITY_CHILD_OUT := ../../build/title-localproc-psmkit-capacity-child
PSMKIT_CAPACITY_CHILD_WORK := ../../build/title-localproc-psmkit-capacity-child-work
PSMKIT_NEST_OUT := ../../build/title-localproc-psmkit-nest
PSMKIT_NEST_WORK := ../../build/title-localproc-psmkit-nest-work
PSMKIT_NEST_CHILD_OUT := ../../build/title-localproc-psmkit-nest-child
PSMKIT_NEST_CHILD_WORK := ../../build/title-localproc-psmkit-nest-child-work
PSMKIT_NEST_GRANDCHILD_OUT := ../../build/title-localproc-psmkit-nest-grandchild
PSMKIT_NEST_GRANDCHILD_WORK := ../../build/title-localproc-psmkit-nest-grandchild-work
PSMKIT_MIXED_OUT := ../../build/title-localproc-psmkit-mixed
PSMKIT_MIXED_WORK := ../../build/title-localproc-psmkit-mixed-work
PROCESS_BROKER_OUT := ../../build/title-process-broker
PROCESS_BROKER_WORK := ../../build/title-process-broker-work
TITLE_BUILD := ../../title/build.sh
TITLE_PARAM := ../../title/sce_sys/param.json
NATIVE_TOOL := ../../../../../PS5_Vulkan/build/host/ps5-native-tool

.DELETE_ON_ERROR:

.PHONY: all title psmkit psmkit-io psmkit-dmemshare psmkit-socket psmkit-detect psmkit-fd3 psmkit-two
.PHONY: psmkit-capacity psmkit-mixed process-broker
.PHONY: psmkit-nest test-psmkit-nest

all: title

title: $(OUT)/eboot.bin $(OUT)/localproc-child.bin

psmkit: $(PSMKIT_OUT)/eboot.bin $(PSMKIT_OUT)/localproc-child.bin

psmkit-io: $(PSMKIT_IO_OUT)/eboot.bin $(PSMKIT_IO_OUT)/psmkit-io-child.bin

psmkit-dmemshare: $(PSMKIT_DMEMSHARE_OUT)/eboot.bin $(PSMKIT_DMEMSHARE_OUT)/psmkit-dmemshare-child.bin

psmkit-socket: $(PSMKIT_SOCKET_OUT)/eboot.bin $(PSMKIT_SOCKET_OUT)/psmkit-socket-child.bin

psmkit-detect: $(PSMKIT_DETECT_OUT)/eboot.bin $(PSMKIT_DETECT_OUT)/psmkit-detect-child.bin

psmkit-fd3: $(PSMKIT_FD3_OUT)/eboot.bin $(PSMKIT_FD3_OUT)/psmkit-fd3-child.bin

psmkit-two: $(PSMKIT_TWO_OUT)/eboot.bin $(PSMKIT_TWO_OUT)/psmkit-two-child.bin

psmkit-capacity: $(PSMKIT_CAPACITY_OUT)/eboot.bin $(PSMKIT_CAPACITY_OUT)/psmkit-capacity-child.bin
psmkit-mixed: $(PSMKIT_MIXED_OUT)/eboot.bin $(PSMKIT_MIXED_OUT)/psmkit-capacity-child.bin
process-broker: $(PROCESS_BROKER_OUT)/eboot.bin



psmkit-nest: $(PSMKIT_NEST_OUT)/eboot.bin $(PSMKIT_NEST_OUT)/psmkit-nest-child.bin $(PSMKIT_NEST_OUT)/psmkit-nest-grandchild.bin

test-psmkit-nest:
	mkdir -p $(PSMKIT_NEST_WORK)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror test-psmkit-nest.c psmkit-nest-protocol.c psmkit-capacity-protocol.c -o $(PSMKIT_NEST_WORK)/test-psmkit-nest
	$(PSMKIT_NEST_WORK)/test-psmkit-nest

$(PROCESS_BROKER_OUT)/eboot.bin: process-broker-title.c ../processbroker/client-core.c ../processbroker/client-core.h ../processbroker/broker-core.h ../processbroker/protocol.c ../processbroker/protocol.h psmkit-io.c psmkit-io.h probe.c probe.h $(TITLE_BUILD) $(TITLE_PARAM)
	PS5_TITLE_WORK="$(abspath $(PROCESS_BROKER_WORK))" \
		bash $(TITLE_BUILD) $(PROCESS_BROKER_OUT) process-broker-title.c ../processbroker/client-core.c ../processbroker/protocol.c psmkit-io.c probe.c

$(PSMKIT_NEST_OUT)/eboot.bin: psmkit-nest-parent.c psmkit-nest.c psmkit-nest-protocol.c psmkit-nest.h psmkit-capacity-protocol.c psmkit-capacity.h psmkit-io.c psmkit-io.h probe.c probe.h $(TITLE_BUILD) $(TITLE_PARAM)
	PS5_TITLE_WORK="$(abspath $(PSMKIT_NEST_WORK))" \
		bash $(TITLE_BUILD) $(PSMKIT_NEST_OUT) psmkit-nest-parent.c psmkit-nest.c psmkit-nest-protocol.c psmkit-capacity-protocol.c psmkit-io.c probe.c

$(PSMKIT_NEST_CHILD_OUT)/eboot.bin: psmkit-nest-child.c psmkit-nest.c psmkit-nest-protocol.c psmkit-nest.h psmkit-capacity-protocol.c psmkit-capacity.h psmkit-io.c psmkit-io.h probe.h patch-local-helper.py $(TITLE_BUILD) $(TITLE_PARAM)
	PS5_TITLE_WORK="$(abspath $(PSMKIT_NEST_CHILD_WORK))" \
		bash $(TITLE_BUILD) $(PSMKIT_NEST_CHILD_OUT) psmkit-nest-child.c psmkit-nest.c psmkit-nest-protocol.c psmkit-capacity-protocol.c psmkit-io.c
	python3 patch-local-helper.py $(PSMKIT_NEST_CHILD_WORK)/eboot.elf $(PSMKIT_NEST_CHILD_WORK)/localproc.elf
	$(NATIVE_TOOL) self --sign --in $(PSMKIT_NEST_CHILD_WORK)/localproc.elf --out $@ --magic 0x1D3D154F
	python3 patch-local-helper.py --container $@ $@
	$(NATIVE_TOOL) self --inspect --file $@ > /dev/null

$(PSMKIT_NEST_GRANDCHILD_OUT)/eboot.bin: psmkit-nest-grandchild.c psmkit-nest-protocol.c psmkit-nest.h psmkit-capacity-protocol.c psmkit-capacity.h psmkit-io.c psmkit-io.h patch-local-helper.py $(TITLE_BUILD) $(TITLE_PARAM)
	PS5_TITLE_WORK="$(abspath $(PSMKIT_NEST_GRANDCHILD_WORK))" \
		bash $(TITLE_BUILD) $(PSMKIT_NEST_GRANDCHILD_OUT) psmkit-nest-grandchild.c psmkit-nest-protocol.c psmkit-capacity-protocol.c psmkit-io.c
	python3 patch-local-helper.py $(PSMKIT_NEST_GRANDCHILD_WORK)/eboot.elf $(PSMKIT_NEST_GRANDCHILD_WORK)/localproc.elf
	$(NATIVE_TOOL) self --sign --in $(PSMKIT_NEST_GRANDCHILD_WORK)/localproc.elf --out $@ --magic 0x1D3D154F
	python3 patch-local-helper.py --container $@ $@
	$(NATIVE_TOOL) self --inspect --file $@ > /dev/null

$(PSMKIT_NEST_OUT)/psmkit-nest-child.bin: $(PSMKIT_NEST_CHILD_OUT)/eboot.bin $(PSMKIT_NEST_OUT)/eboot.bin
	cp $(PSMKIT_NEST_CHILD_OUT)/eboot.bin $@

$(PSMKIT_NEST_OUT)/psmkit-nest-grandchild.bin: $(PSMKIT_NEST_GRANDCHILD_OUT)/eboot.bin $(PSMKIT_NEST_OUT)/eboot.bin
	cp $(PSMKIT_NEST_GRANDCHILD_OUT)/eboot.bin $@

$(PSMKIT_MIXED_OUT)/eboot.bin: psmkit-mixed-parent.c psmkit-capacity.c psmkit-capacity-protocol.c psmkit-capacity.h psmkit-io.c psmkit-io.h probe.c probe.h ../mixedproc/mixed-protocol.h $(TITLE_BUILD) $(TITLE_PARAM)
	PS5_TITLE_WORK="$(abspath $(PSMKIT_MIXED_WORK))" \
		bash $(TITLE_BUILD) $(PSMKIT_MIXED_OUT) psmkit-mixed-parent.c psmkit-capacity.c psmkit-capacity-protocol.c psmkit-io.c probe.c

$(PSMKIT_MIXED_OUT)/psmkit-capacity-child.bin: $(PSMKIT_CAPACITY_CHILD_OUT)/eboot.bin $(PSMKIT_MIXED_OUT)/eboot.bin
	cp $(PSMKIT_CAPACITY_CHILD_OUT)/eboot.bin $@

$(PSMKIT_CAPACITY_OUT)/eboot.bin: psmkit-capacity-parent.c psmkit-capacity.c psmkit-capacity-protocol.c psmkit-capacity.h psmkit-io.c psmkit-io.h probe.c probe.h $(TITLE_BUILD) $(TITLE_PARAM)
	PS5_TITLE_WORK="$(abspath $(PSMKIT_CAPACITY_WORK))" \
		bash $(TITLE_BUILD) $(PSMKIT_CAPACITY_OUT) psmkit-capacity-parent.c psmkit-capacity.c psmkit-capacity-protocol.c psmkit-io.c probe.c

$(PSMKIT_CAPACITY_CHILD_OUT)/eboot.bin: psmkit-capacity-child.c psmkit-capacity-protocol.c psmkit-capacity.h psmkit-io.c psmkit-io.h patch-local-helper.py $(TITLE_BUILD) $(TITLE_PARAM)
	PS5_TITLE_WORK="$(abspath $(PSMKIT_CAPACITY_CHILD_WORK))" \
		bash $(TITLE_BUILD) $(PSMKIT_CAPACITY_CHILD_OUT) psmkit-capacity-child.c psmkit-capacity-protocol.c psmkit-io.c
	python3 patch-local-helper.py $(PSMKIT_CAPACITY_CHILD_WORK)/eboot.elf $(PSMKIT_CAPACITY_CHILD_WORK)/localproc.elf
	$(NATIVE_TOOL) self --sign --in $(PSMKIT_CAPACITY_CHILD_WORK)/localproc.elf --out $@ --magic 0x1D3D154F
	python3 patch-local-helper.py --container $@ $@
	$(NATIVE_TOOL) self --inspect --file $@ > /dev/null

$(PSMKIT_CAPACITY_OUT)/psmkit-capacity-child.bin: $(PSMKIT_CAPACITY_CHILD_OUT)/eboot.bin $(PSMKIT_CAPACITY_OUT)/eboot.bin
	cp $(PSMKIT_CAPACITY_CHILD_OUT)/eboot.bin $@

$(PSMKIT_TWO_OUT)/eboot.bin: psmkit-two-parent.c psmkit-two.c psmkit-two.h psmkit-io.c psmkit-io.h probe.c probe.h $(TITLE_BUILD) $(TITLE_PARAM)
	PS5_TITLE_WORK="$(abspath $(PSMKIT_TWO_WORK))" \
		bash $(TITLE_BUILD) $(PSMKIT_TWO_OUT) psmkit-two-parent.c psmkit-two.c psmkit-io.c probe.c

$(PSMKIT_TWO_CHILD_OUT)/eboot.bin: psmkit-two-child.c psmkit-two.c psmkit-two.h psmkit-io.c psmkit-io.h patch-local-helper.py $(TITLE_BUILD) $(TITLE_PARAM)
	PS5_TITLE_WORK="$(abspath $(PSMKIT_TWO_CHILD_WORK))" \
		bash $(TITLE_BUILD) $(PSMKIT_TWO_CHILD_OUT) psmkit-two-child.c psmkit-two.c psmkit-io.c
	python3 patch-local-helper.py $(PSMKIT_TWO_CHILD_WORK)/eboot.elf $(PSMKIT_TWO_CHILD_WORK)/localproc.elf
	$(NATIVE_TOOL) self --sign --in $(PSMKIT_TWO_CHILD_WORK)/localproc.elf --out $@ --magic 0x1D3D154F
	python3 patch-local-helper.py --container $@ $@
	$(NATIVE_TOOL) self --inspect --file $@ > /dev/null

$(PSMKIT_TWO_OUT)/psmkit-two-child.bin: $(PSMKIT_TWO_CHILD_OUT)/eboot.bin $(PSMKIT_TWO_OUT)/eboot.bin
	cp $(PSMKIT_TWO_CHILD_OUT)/eboot.bin $@

$(PSMKIT_FD3_OUT)/eboot.bin: psmkit-fd3-parent.c psmkit-fd3.c psmkit-fd3.h psmkit-socket.h psmkit-io.c psmkit-io.h probe.c probe.h $(TITLE_BUILD) $(TITLE_PARAM)
	PS5_TITLE_WORK="$(abspath $(PSMKIT_FD3_WORK))" \
		bash $(TITLE_BUILD) $(PSMKIT_FD3_OUT) psmkit-fd3-parent.c psmkit-fd3.c psmkit-io.c probe.c

$(PSMKIT_FD3_CHILD_OUT)/eboot.bin: psmkit-fd3-child.c psmkit-fd3.c psmkit-fd3.h psmkit-socket.h psmkit-io.c psmkit-io.h patch-local-helper.py $(TITLE_BUILD) $(TITLE_PARAM)
	PS5_TITLE_WORK="$(abspath $(PSMKIT_FD3_CHILD_WORK))" \
		bash $(TITLE_BUILD) $(PSMKIT_FD3_CHILD_OUT) psmkit-fd3-child.c psmkit-fd3.c psmkit-io.c
	python3 patch-local-helper.py $(PSMKIT_FD3_CHILD_WORK)/eboot.elf $(PSMKIT_FD3_CHILD_WORK)/localproc.elf
	$(NATIVE_TOOL) self --sign --in $(PSMKIT_FD3_CHILD_WORK)/localproc.elf --out $@ --magic 0x1D3D154F
	python3 patch-local-helper.py --container $@ $@
	$(NATIVE_TOOL) self --inspect --file $@ > /dev/null

$(PSMKIT_FD3_OUT)/psmkit-fd3-child.bin: $(PSMKIT_FD3_CHILD_OUT)/eboot.bin $(PSMKIT_FD3_OUT)/eboot.bin
	cp $(PSMKIT_FD3_CHILD_OUT)/eboot.bin $@

$(PSMKIT_DETECT_OUT)/eboot.bin: psmkit-detect-parent.c psmkit-detect.c psmkit-detect.h probe.h $(TITLE_BUILD) $(TITLE_PARAM)
	PS5_TITLE_WORK="$(abspath $(PSMKIT_DETECT_WORK))" \
		bash $(TITLE_BUILD) $(PSMKIT_DETECT_OUT) psmkit-detect-parent.c psmkit-detect.c

$(PSMKIT_DETECT_CHILD_OUT)/eboot.bin: psmkit-detect-child.c psmkit-detect.c psmkit-detect.h patch-local-helper.py $(TITLE_BUILD) $(TITLE_PARAM)
	PS5_TITLE_WORK="$(abspath $(PSMKIT_DETECT_CHILD_WORK))" \
		bash $(TITLE_BUILD) $(PSMKIT_DETECT_CHILD_OUT) psmkit-detect-child.c psmkit-detect.c
	python3 patch-local-helper.py $(PSMKIT_DETECT_CHILD_WORK)/eboot.elf $(PSMKIT_DETECT_CHILD_WORK)/localproc.elf
	$(NATIVE_TOOL) self --sign --in $(PSMKIT_DETECT_CHILD_WORK)/localproc.elf --out $@ --magic 0x1D3D154F
	python3 patch-local-helper.py --container $@ $@
	$(NATIVE_TOOL) self --inspect --file $@ > /dev/null

$(PSMKIT_DETECT_OUT)/psmkit-detect-child.bin: $(PSMKIT_DETECT_CHILD_OUT)/eboot.bin $(PSMKIT_DETECT_OUT)/eboot.bin
	cp $(PSMKIT_DETECT_CHILD_OUT)/eboot.bin $@

$(PSMKIT_SOCKET_OUT)/eboot.bin: psmkit-socket-parent.c psmkit-socket.h psmkit-io.c psmkit-io.h probe.c probe.h $(TITLE_BUILD) $(TITLE_PARAM)
	PS5_TITLE_WORK="$(abspath $(PSMKIT_SOCKET_WORK))" \
		bash $(TITLE_BUILD) $(PSMKIT_SOCKET_OUT) psmkit-socket-parent.c psmkit-io.c probe.c

$(PSMKIT_SOCKET_CHILD_OUT)/eboot.bin: psmkit-socket-child.c psmkit-socket.h psmkit-io.c psmkit-io.h patch-local-helper.py $(TITLE_BUILD) $(TITLE_PARAM)
	PS5_TITLE_WORK="$(abspath $(PSMKIT_SOCKET_CHILD_WORK))" \
		bash $(TITLE_BUILD) $(PSMKIT_SOCKET_CHILD_OUT) psmkit-socket-child.c psmkit-io.c
	python3 patch-local-helper.py $(PSMKIT_SOCKET_CHILD_WORK)/eboot.elf $(PSMKIT_SOCKET_CHILD_WORK)/localproc.elf
	$(NATIVE_TOOL) self --sign --in $(PSMKIT_SOCKET_CHILD_WORK)/localproc.elf --out $@ --magic 0x1D3D154F
	python3 patch-local-helper.py --container $@ $@
	$(NATIVE_TOOL) self --inspect --file $@ > /dev/null

$(PSMKIT_SOCKET_OUT)/psmkit-socket-child.bin: $(PSMKIT_SOCKET_CHILD_OUT)/eboot.bin $(PSMKIT_SOCKET_OUT)/eboot.bin
	cp $(PSMKIT_SOCKET_CHILD_OUT)/eboot.bin $@

$(PSMKIT_IO_OUT)/eboot.bin: psmkit-io-parent.c psmkit-io.c psmkit-io.h probe.c probe.h $(TITLE_BUILD) $(TITLE_PARAM)
	PS5_TITLE_WORK="$(abspath $(PSMKIT_IO_WORK))" \
		bash $(TITLE_BUILD) $(PSMKIT_IO_OUT) psmkit-io-parent.c psmkit-io.c probe.c

$(PSMKIT_IO_CHILD_OUT)/eboot.bin: psmkit-io-child.c psmkit-io.c psmkit-io.h patch-local-helper.py $(TITLE_BUILD) $(TITLE_PARAM)
	PS5_TITLE_WORK="$(abspath $(PSMKIT_IO_CHILD_WORK))" \
		bash $(TITLE_BUILD) $(PSMKIT_IO_CHILD_OUT) psmkit-io-child.c psmkit-io.c
	python3 patch-local-helper.py $(PSMKIT_IO_CHILD_WORK)/eboot.elf $(PSMKIT_IO_CHILD_WORK)/localproc.elf
	$(NATIVE_TOOL) self --sign --in $(PSMKIT_IO_CHILD_WORK)/localproc.elf --out $@ --magic 0x1D3D154F
	python3 patch-local-helper.py --container $@ $@
	$(NATIVE_TOOL) self --inspect --file $@ > /dev/null

$(PSMKIT_IO_OUT)/psmkit-io-child.bin: $(PSMKIT_IO_CHILD_OUT)/eboot.bin $(PSMKIT_IO_OUT)/eboot.bin
	cp $(PSMKIT_IO_CHILD_OUT)/eboot.bin $@

$(PSMKIT_DMEMSHARE_OUT)/eboot.bin: psmkit-dmemshare-parent.c psmkit-dmemshare.h psmkit-io.c psmkit-io.h probe.c probe.h $(TITLE_BUILD) $(TITLE_PARAM)
	PS5_TITLE_WORK="$(abspath $(PSMKIT_DMEMSHARE_WORK))" \
		bash $(TITLE_BUILD) $(PSMKIT_DMEMSHARE_OUT) psmkit-dmemshare-parent.c psmkit-io.c probe.c

$(PSMKIT_DMEMSHARE_CHILD_OUT)/eboot.bin: psmkit-dmemshare-child.c psmkit-dmemshare.h psmkit-io.c psmkit-io.h patch-local-helper.py $(TITLE_BUILD) $(TITLE_PARAM)
	PS5_TITLE_WORK="$(abspath $(PSMKIT_DMEMSHARE_CHILD_WORK))" \
		bash $(TITLE_BUILD) $(PSMKIT_DMEMSHARE_CHILD_OUT) psmkit-dmemshare-child.c psmkit-io.c
	python3 patch-local-helper.py $(PSMKIT_DMEMSHARE_CHILD_WORK)/eboot.elf $(PSMKIT_DMEMSHARE_CHILD_WORK)/localproc.elf
	$(NATIVE_TOOL) self --sign --in $(PSMKIT_DMEMSHARE_CHILD_WORK)/localproc.elf --out $@ --magic 0x1D3D154F
	python3 patch-local-helper.py --container $@ $@
	$(NATIVE_TOOL) self --inspect --file $@ > /dev/null

$(PSMKIT_DMEMSHARE_OUT)/psmkit-dmemshare-child.bin: $(PSMKIT_DMEMSHARE_CHILD_OUT)/eboot.bin $(PSMKIT_DMEMSHARE_OUT)/eboot.bin
	cp $(PSMKIT_DMEMSHARE_CHILD_OUT)/eboot.bin $@

$(PSMKIT_OUT)/eboot.bin: psmkit-parent.c probe.c probe.h $(TITLE_BUILD) $(TITLE_PARAM)
	PS5_TITLE_WORK="$(abspath $(PSMKIT_WORK))" \
		bash $(TITLE_BUILD) $(PSMKIT_OUT) psmkit-parent.c probe.c

$(OUT)/eboot.bin: parent.c probe.c probe.h $(TITLE_BUILD) $(TITLE_PARAM)
	PS5_TITLE_WORK="$(abspath ../../build/title-localproc-parent-work)" \
		bash $(TITLE_BUILD) $(OUT) parent.c probe.c

$(CHILD_OUT)/eboot.bin: child.c probe.c probe.h patch-local-helper.py $(TITLE_BUILD) $(TITLE_PARAM)
	PS5_TITLE_WORK="$(abspath $(CHILD_WORK))" \
		bash $(TITLE_BUILD) $(CHILD_OUT) child.c probe.c
	python3 patch-local-helper.py $(CHILD_WORK)/eboot.elf $(CHILD_WORK)/localproc.elf
	$(NATIVE_TOOL) self --sign --in $(CHILD_WORK)/localproc.elf --out $@ --magic 0x1D3D154F
	python3 patch-local-helper.py --container $@ $@
	$(NATIVE_TOOL) self --inspect --file $@ > /dev/null

$(PSMKIT_CHILD_OUT)/eboot.bin: psmkit-child.c patch-local-helper.py $(TITLE_BUILD) $(TITLE_PARAM)
	PS5_TITLE_WORK="$(abspath $(PSMKIT_CHILD_WORK))" \
		bash $(TITLE_BUILD) $(PSMKIT_CHILD_OUT) psmkit-child.c
	python3 patch-local-helper.py $(PSMKIT_CHILD_WORK)/eboot.elf $(PSMKIT_CHILD_WORK)/localproc.elf
	$(NATIVE_TOOL) self --sign --in $(PSMKIT_CHILD_WORK)/localproc.elf --out $@ --magic 0x1D3D154F
	python3 patch-local-helper.py --container $@ $@
	$(NATIVE_TOOL) self --inspect --file $@ > /dev/null

$(OUT)/localproc-child.bin: $(CHILD_OUT)/eboot.bin $(OUT)/eboot.bin
	cp $(CHILD_OUT)/eboot.bin $@

$(PSMKIT_OUT)/localproc-child.bin: $(PSMKIT_CHILD_OUT)/eboot.bin $(PSMKIT_OUT)/eboot.bin
	cp $(PSMKIT_CHILD_OUT)/eboot.bin $@
