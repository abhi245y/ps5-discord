# Standalone virtual-pad probe. Builds only; never deploys.
#   PS5_PAYLOAD_SDK=/absolute/path/to/ps5-payload-sdk make -C tools/vpad-probe
PS5_PAYLOAD_SDK ?= ../../.deps/ps5-payload-sdk
SDK := $(abspath $(PS5_PAYLOAD_SDK))
export LLVM_CONFIG ?= $(or $(shell command -v llvm-config),$(abspath ../llvm-config))
PS5_CC := $(SDK)/bin/prospero-clang

.PHONY: all clean
all: vpad-probe.elf

vpad-probe.elf: vpad_probe.c
	@echo "  CC    $@"
	@PS5_PAYLOAD_SDK=$(SDK) $(PS5_CC) -std=gnu11 -Wall -Wextra -Werror -O2 -o $@ $< -lkernel_sys -lSceUserService

clean:
	rm -f vpad-probe.elf
