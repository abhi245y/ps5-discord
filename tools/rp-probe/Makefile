# Remote Play loopback probe payload. Builds rp-probe.elf only; never deploys.
#   make PS5_PAYLOAD_SDK=<repo>/.deps/ps5-payload-sdk
PS5_PAYLOAD_SDK ?= ../../.deps/ps5-payload-sdk
SDK := $(abspath $(PS5_PAYLOAD_SDK))
export LLVM_CONFIG ?= $(or $(shell command -v llvm-config),$(abspath ../llvm-config))
PS5_CC := $(SDK)/bin/prospero-clang

.PHONY: all clean
all: rp-probe.elf

rp-probe.elf: rp_probe.c
	@echo "  CC    $@"
	@PS5_PAYLOAD_SDK=$(SDK) $(PS5_CC) -std=gnu11 -Wall -Wextra -Werror -O2 -o $@ $<

clean:
	rm -f rp-probe.elf
