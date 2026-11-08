# DualSense USB probes. Builds only; never deploys.
#   make PS5_PAYLOAD_SDK=<repo>/.deps/ps5-payload-sdk
PS5_PAYLOAD_SDK ?= ../../.deps/ps5-payload-sdk
SDK := $(abspath $(PS5_PAYLOAD_SDK))
export LLVM_CONFIG ?= $(or $(shell command -v llvm-config),$(abspath ../llvm-config))
PS5_CC := $(SDK)/bin/prospero-clang

.PHONY: all clean
all: ds5-usb-probe.elf ds5-mic-probe.elf ds5-speaker-probe.elf ds5-music-probe.elf ds5-loopback-probe.elf

ds5-usb-probe.elf: ds5_usb_probe.c
	@echo "  CC    $@"
	@PS5_PAYLOAD_SDK=$(SDK) $(PS5_CC) -std=gnu11 -Wall -Wextra -Werror -O2 -o $@ $<

clean:
	rm -f ds5-usb-probe.elf ds5-mic-probe.elf ds5-speaker-probe.elf ds5-music-probe.elf ds5-loopback-probe.elf mic-format-test loopback-audio-test

ds5-mic-probe.elf: ds5_mic_probe.c ds5_mic_format.h
	@echo "  CC    $@"
	@PS5_PAYLOAD_SDK=$(SDK) $(PS5_CC) -std=gnu11 -Wall -Wextra -Werror -O2 -o $@ $<

ds5-speaker-probe.elf: ds5_speaker_probe.c ds5_mic_format.h ds5_speaker_tone.h
	@echo "  CC    $@"
	@PS5_PAYLOAD_SDK=$(SDK) $(PS5_CC) -std=gnu11 -Wall -Wextra -Werror -O2 -o $@ $<

ds5-music-probe.elf: ds5_speaker_probe.c ds5_mic_format.h ds5_speaker_tone.h
	@echo "  CC    $@"
	@PS5_PAYLOAD_SDK=$(SDK) $(PS5_CC) -std=gnu11 -Wall -Wextra -Werror -O2 -DDS5_PLAY_SONG -o $@ $<

ds5-loopback-probe.elf: ds5_loopback_probe.c ds5_mic_format.h ds5_loopback_audio.h
	@echo "  CC    $@"
	@PS5_PAYLOAD_SDK=$(SDK) $(PS5_CC) -std=gnu11 -Wall -Wextra -Werror -O2 -o $@ $<

.PHONY: test
test:
	$(CC) -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -o mic-format-test mic_format_test.c -lm
	./mic-format-test
	$(CC) -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -o loopback-audio-test loopback_audio_test.c
	./loopback-audio-test
