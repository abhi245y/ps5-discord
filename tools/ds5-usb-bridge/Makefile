# SPDX-License-Identifier: AGPL-3.0-or-later
# Builds only; never deploys or starts audio.
PS5_PAYLOAD_SDK ?= ../../.deps/ps5-payload-sdk
SDK := $(abspath $(PS5_PAYLOAD_SDK))
export LLVM_CONFIG ?= $(or $(shell command -v llvm-config),$(abspath ../llvm-config))
PS5_CC := $(SDK)/bin/prospero-clang
HOST_CC ?= cc
DURATION_MS ?= 300000

HEADERS := ds5_usb.h ds5_usb_pcm.h ds5_usb_format.h pcm_stream.h
SOURCES := bridge_main.c ds5_usb.c pcm_stream.c

.PHONY: all clean test analyze
all: ds5-usb-bridge.elf

ds5-usb-bridge.elf: $(SOURCES) $(HEADERS)
	PS5_PAYLOAD_SDK=$(SDK) $(PS5_CC) -std=gnu11 -Wall -Wextra -Werror -O2 \
		-DDS5_BRIDGE_DURATION_MS=$(DURATION_MS) -o $@ $(SOURCES)

test: test_usb.c pcm_stream.c $(HEADERS)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
		-o ds5-usb-test test_usb.c pcm_stream.c
	./ds5-usb-test

analyze:
	PS5_PAYLOAD_SDK=$(SDK) $(PS5_CC) --analyze -std=gnu11 -Wall -Wextra -Werror \
		ds5_usb.c pcm_stream.c bridge_main.c

clean:
	rm -f ds5-usb-bridge.elf ds5-usb-test *.plist
