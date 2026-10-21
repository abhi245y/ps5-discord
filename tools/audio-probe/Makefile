# sceAudioIn probe payload. Builds mic-probe.elf only; never deploys.
#   make PS5_PAYLOAD_SDK=<repo>/.deps/ps5-payload-sdk
PS5_PAYLOAD_SDK ?= ../../.deps/ps5-payload-sdk
SDK := $(abspath $(PS5_PAYLOAD_SDK))
export LLVM_CONFIG ?= $(or $(shell command -v llvm-config),$(abspath ../llvm-config))
PS5_CC := $(SDK)/bin/prospero-clang

.PHONY: all clean
all: mic-probe.elf

mic-probe.elf: mic_probe.c
	@echo "  CC    $@"
	@PS5_PAYLOAD_SDK=$(SDK) $(PS5_CC) -std=gnu11 -Wall -Wextra -Werror -O2 -o $@ $< -lSceUserService

clean:
	rm -f mic-probe.elf
