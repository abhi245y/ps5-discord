# Standalone virtual-pad probe. Builds only; never deploys.
#   PS5_PAYLOAD_SDK=/absolute/path/to/ps5-payload-sdk make -C tools/vpad-probe
PS5_PAYLOAD_SDK ?= ../../.deps/ps5-payload-sdk
SDK := $(abspath $(PS5_PAYLOAD_SDK))
export LLVM_CONFIG ?= $(or $(shell command -v llvm-config),$(abspath ../llvm-config))
PS5_CC := $(SDK)/bin/prospero-clang
HOST_CC ?= cc

.PHONY: all game usb audio call restore call-restore service test clean
all: vpad-probe.elf
game: vpad-game-probe.elf
usb: vpad-usb-probe.elf
audio: vpad-usb-audio-probe.elf
call: vpad-usb-call-probe.elf
restore: vpad-usb-restore-probe.elf
call-restore: vpad-usb-call-restore-probe.elf
service: controller-audio.elf

vpad-probe.elf: vpad_probe.c Makefile
	@echo "  CC    $@"
	@PS5_PAYLOAD_SDK=$(SDK) $(PS5_CC) -std=gnu11 -Wall -Wextra -Werror -O2 -o $@ $< -lkernel_sys -lSceUserService

vpad-game-probe.elf: vpad_probe.c Makefile
	@echo "  CC    $@"
	@PS5_PAYLOAD_SDK=$(SDK) $(PS5_CC) -std=gnu11 -Wall -Wextra -Werror -O2 -DPROBE_RESUME_GAME=1 -o $@ $< -lkernel_sys -lSceUserService

# USB variants share the same runtime, IPC, and virtual-input implementation.
USB_SOURCES := vpad_probe.c ../ds5-usb-bridge/ds5_usb.c
USB_HEADERS := usb_forward.inc usb_forward_report.h usb_call.inc usb_test_tone.h \
	../ds5-usb-bridge/ds5_usb.h ../ds5-usb-bridge/ds5_usb_pcm.h \
	../ds5-usb-bridge/ds5_usb_format.h ../ds5-usb-bridge/pcm_stream.h
USB_TARGETS := vpad-usb-probe.elf vpad-usb-audio-probe.elf vpad-usb-call-probe.elf \
	vpad-usb-restore-probe.elf vpad-usb-call-restore-probe.elf controller-audio.elf
USB_FLAGS := -DPROBE_USB_FORWARD=1

vpad-usb-probe.elf: PROBE_FLAGS := $(USB_FLAGS)
vpad-usb-audio-probe.elf: PROBE_FLAGS := $(USB_FLAGS) -DPROBE_USB_TONE=1
vpad-usb-call-probe.elf: PROBE_FLAGS := $(USB_FLAGS) -DPROBE_USB_CALL=1
vpad-usb-restore-probe.elf: PROBE_FLAGS := $(USB_FLAGS) -DDS5_USB_RESTORE_CONFIG=2
vpad-usb-call-restore-probe.elf: PROBE_FLAGS := $(USB_FLAGS) -DPROBE_USB_CALL=1 -DDS5_USB_RESTORE_CONFIG=2
controller-audio.elf: PROBE_FLAGS := $(USB_FLAGS) -DPROBE_USB_CALL=1 -DPROBE_USB_SERVICE=1 -DDS5_USB_RESTORE_CONFIG=2
vpad-usb-call-probe.elf vpad-usb-call-restore-probe.elf controller-audio.elf: CALL_SOURCE := ../ds5-usb-bridge/pcm_stream.c

$(USB_TARGETS): $(USB_SOURCES) $(USB_HEADERS) ../ds5-usb-bridge/pcm_stream.c Makefile
	@echo "  CC    $@"
	@PS5_PAYLOAD_SDK=$(SDK) $(PS5_CC) -std=gnu11 -Wall -Wextra -Werror -O2 $(PROBE_FLAGS) \
		-I../ds5-usb-bridge -o $@ $(USB_SOURCES) $(CALL_SOURCE) -lkernel_sys -lSceUserService -lpthread

test: test_usb_forward.c usb_forward_report.h usb_test_tone.h ../ds5-usb-bridge/ds5_usb.h ../ds5-usb-bridge/ds5_usb_pcm.h ../ds5-usb-bridge/ds5_usb_format.h
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -I../ds5-usb-bridge -o usb-forward-test test_usb_forward.c -lpthread -lm
	./usb-forward-test

clean:
	rm -f vpad-probe.elf vpad-game-probe.elf vpad-usb-probe.elf vpad-usb-audio-probe.elf vpad-usb-call-probe.elf vpad-usb-restore-probe.elf vpad-usb-call-restore-probe.elf controller-audio.elf usb-forward-test
