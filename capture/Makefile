# The wireless Go Live helper (discord-capture.elf). Build-only: run on arlp-pc; nothing here
# sends anything to a console. SPDX GPL-3.0-only (see LICENSE and README.md).
#   make all                       build/discord-capture.elf (PS5_PAYLOAD_SDK=<sdk>)
#   make test CC=clang             host lifecycle/recovery tests (fake transport, loopback socket)
SDK := $(abspath $(PS5_PAYLOAD_SDK))
CC_PS5 := $(SDK)/bin/prospero-clang
export LLVM_CONFIG
CFLAGS := -std=gnu11 -Wall -Wextra -Werror -I../src -Isrc

.PHONY: all test clean
all: build/discord-capture.elf

build/discord-capture.elf: src/helper.c src/inject.c src/h264_timing.c src/h264_timing.h src/inject.h ../src/capture_proto.h
	@mkdir -p build
	$(CC_PS5) $(CFLAGS) -O2 -g -DSL_PS5 -o $@ src/helper.c src/inject.c src/h264_timing.c

build/helper-test: tests/helper_test.c src/helper.c src/h264_timing.c src/h264_timing.h src/inject.h ../src/capture_proto.h
	@mkdir -p build
	$(CC) $(CFLAGS) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread -o $@ tests/helper_test.c src/h264_timing.c

build/h264-test: tests/h264_test.c src/h264_timing.c src/h264_timing.h
	@mkdir -p build
	$(CC) $(CFLAGS) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -o $@ tests/h264_test.c src/h264_timing.c

test: build/helper-test build/h264-test
	./build/helper-test
	./build/h264-test

clean:
	rm -rf build
