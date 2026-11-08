# ps5-discord
#
#   make deps                     fetch pinned SDK + sources into .deps/
#   make                          build/ps5-discord.elf (payload)
#   make deploy PS5_HOST=<ip>     stop the running copy, send the new one to elfldr
#   make stop PS5_HOST=<ip>       stop the running copy
#   control API: 127.0.0.1:9041 for the app (CORE_LAN=1: the LAN too, for make stop)
#   make host                     build/host/ps5-discord (runs on Linux, token in ./token)
#   make test                     host self-checks
#   make dave                     build libdave (DAVE E2EE) into .deps/dave with tools/dave/build.sh (run on arlp-pc)
#   make ffmpeg                   build FFmpeg's AAC decoder (AirPods mic) into .deps/ffmpeg with tools/ffmpeg/build.sh

PS5_HOST ?=
ELFLDR_PORT ?= 9021
# The console's FTP server: make stop reads the core key (src/core_key.h) through it.
PS5_FTP ?= anonymous:codex@$(PS5_HOST):2121
# The trust store built in: curl's extract of Mozilla's roots, pinned in tools/deps.sh.
CA_PEM := .deps/cacert-2026-10-20.pem
HOST_CC ?= cc

SDK := .deps/ps5-payload-sdk
MBED := .deps/src/mbedtls-3.6.4
ZLIB := .deps/src/zlib-1.3.2
CJSON := .deps/src/cJSON-1.7.18
QRGEN := .deps/src/QR-Code-generator-1.8.0/c
SQLITE := .deps/src/sqlite-amalgamation-3500400
OPUS := .deps/src/opus-1.6.1
# Opus, fixed-point C only (no asm, no float API). The source lists come from the tarball.
-include $(OPUS)/celt_sources.mk $(OPUS)/silk_sources.mk $(OPUS)/opus_sources.mk
OPUS_SRC := $(addprefix $(OPUS)/,$(CELT_SOURCES) $(SILK_SOURCES) $(SILK_SOURCES_FIXED) $(OPUS_SOURCES))
OPUS_FLAGS := -DOPUS_BUILD -DFIXED_POINT=1 -DDISABLE_FLOAT_API -DVAR_ARRAYS '-DPACKAGE_VERSION="1.6.1"'
OPUS_INCLUDES := -I$(OPUS)/include -I$(OPUS)/celt -I$(OPUS)/silk -I$(OPUS)/silk/fixed

DS5 := tools/ds5-usb-bridge
APP_SRC := src/main.c src/log.c src/status.c src/web.c src/chat.c src/gateway.c src/remote_auth.c src/browser_auth.c src/browser_storage.c src/http.c src/props.c src/ws.c src/tls.c src/zstream.c src/ratelimit.c \
	src/core_key.c src/voice.c src/voice_proto.c src/voice_audio.c src/broadcast.c src/capture_bc.c src/golive_opus.c src/rp_pair.c src/prefs.c src/notify.c src/stream.c src/playing.c src/update.c src/update_verify.c src/resample.c src/overlay.c src/png_circle.c $(DS5)/pcm_stream.c
# Controller audio without Go Live (controller_voice.c) is parked: CONTROLLER_VOICE=1 builds it.
ifneq ($(CONTROLLER_VOICE),)
APP_SRC += src/controller_voice.c
CV_FLAGS := -DCONTROLLER_VOICE
endif
# Go Live (console only): a loopback Remote Play session (vendored chiaki-ng,
# third_party/README.md) and the DualSense USB runtime from tools/ds5-usb-bridge.
CHIAKI := third_party/chiaki-ng
CHIAKI_SRC := $(wildcard $(CHIAKI)/src/*.c) $(CHIAKI)/src/remote/rudp.c $(CHIAKI)/src/remote/rudpsendbuffer.c \
	$(CHIAKI)/gen/takion.pb.c $(addprefix third_party/nanopb/,pb_common.c pb_encode.c pb_decode.c) \
	$(wildcard third_party/gf-complete/src/*.c) $(wildcard third_party/jerasure/src/*.c)
PS5_ONLY_SRC = $(CHIAKI_SRC) $(DS5)/ds5_usb.c $(SBC_SRC)
# The Bluetooth headset (console only): BlueV's stack (third_party/bluev, C++) and ours (src/bt).
BLUEV := third_party/bluev
BLUEV_CXX := $(shell find $(BLUEV)/src/core $(BLUEV)/src/platform -name '*.cpp' | sort) $(wildcard src/bt/*.cpp)
SBC_SRC := $(addprefix $(BLUEV)/third_party/sbc/sbc/,sbc.c sbc_primitives.c sbc_primitives_mmx.c sbc_primitives_sse.c)
HEADSET_INC := -I$(BLUEV)/src -I$(BLUEV)/third_party/sbc/sbc -Isrc
ZLIB_SRC := $(addprefix $(ZLIB)/,adler32.c crc32.c inffast.c inflate.c inftrees.c zutil.c)
DEP_SRC = $(wildcard $(MBED)/library/*.c) $(ZLIB_SRC) $(CJSON)/cJSON.c $(QRGEN)/qrcodegen.c $(SQLITE)/sqlite3.c $(OPUS_SRC)
INCLUDES := -Isrc -I$(MBED)/include -I$(ZLIB) -I$(CJSON) -I$(QRGEN) -I$(SQLITE) $(OPUS_INCLUDES) -I$(DS5) \
	-I$(CHIAKI)/include -I$(CHIAKI)/src -I$(CHIAKI)/gen -Ithird_party/nanopb -Ithird_party/gf-complete/include \
	-Ithird_party/jerasure/include
# chiaki-ng: mbedTLS backend, nanopb's C99 asserts, and mbedTLS 3 struct fields by name.
CHIAKI_FLAGS := -DCHIAKI_LIB_ENABLE_MBEDTLS -DPB_C99_STATIC_ASSERT -DMBEDTLS_ALLOW_PRIVATE_ACCESS
SQLITE_FLAGS := -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_TEMP_STORE=3 -DSQLITE_MAX_MMAP_SIZE=0

PS5_CC := $(SDK)/bin/prospero-clang
PS5_CXX := $(SDK)/bin/prospero-clang++

# libdave (DAVE E2EE) from `make dave`: real when its libraries are there, else dave_stub.c.
DAVE_OUT := .deps/dave
DAVE_PS5 := $(DAVE_OUT)/install/ps5
DAVE_HOST := $(DAVE_OUT)/install/host
DAVE_LIBS := libdave.a libmlspp.a libmls_ds.a libhpke.a libtls_syntax.a libbytes.a libcrypto.a
ifneq ($(wildcard $(DAVE_PS5)/lib/libdave.a),)
PS5_DAVE_INC := -I$(DAVE_PS5)/include
PS5_DAVE_LINK := -Wl,--start-group $(addprefix $(DAVE_PS5)/lib/,$(DAVE_LIBS)) -Wl,--end-group -lc++ -lc++abi -lunwind
PS5_DAVE_SRC :=
else
$(warning libdave for the PS5 is missing in $(DAVE_PS5): building the no-DAVE stub (run `make dave`))
PS5_DAVE_INC := -Isrc/dave_fallback
PS5_DAVE_LINK :=
PS5_DAVE_SRC := src/dave_stub.c
endif
ifneq ($(wildcard $(DAVE_HOST)/lib/libdave.a),)
HOST_DAVE_INC := -I$(DAVE_HOST)/include
HOST_DAVE_LINK := -Wl,--start-group $(addprefix $(DAVE_HOST)/lib/,$(DAVE_LIBS)) -Wl,--end-group -lstdc++ -lm
HOST_DAVE_SRC :=
HOST_DAVE_TEST := build/host/dave-test
else
$(warning libdave for the host is missing in $(DAVE_HOST): building the no-DAVE stub (run `make dave`))
HOST_DAVE_INC := -Isrc/dave_fallback
HOST_DAVE_LINK :=
HOST_DAVE_SRC := src/dave_stub.c
HOST_DAVE_TEST :=
endif
# FFmpeg's AAC decoder (the AirPods mic) from `make ffmpeg`; without it the AirPods play but
# their mic does not decode.
FFMPEG_PS5 := .deps/ffmpeg/install/ps5
ifneq ($(wildcard $(FFMPEG_PS5)/lib/libavcodec.a),)
FFMPEG_FLAGS := -DHAVE_FFMPEG -I$(FFMPEG_PS5)/include
FFMPEG_LINK := $(FFMPEG_PS5)/lib/libavcodec.a $(FFMPEG_PS5)/lib/libavutil.a
else
$(warning FFmpeg for the PS5 is missing in $(FFMPEG_PS5): the AirPods mic will not decode (run `make ffmpeg`))
endif
PS5_CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -fno-strict-aliasing $(HARDEN) $(HEADSET_INC) $(INCLUDES) \
	$(PS5_DAVE_INC) $(FFMPEG_FLAGS)
# The SDK wrappers find LLVM through llvm-config; use the system one if present.
export LLVM_CONFIG ?= $(or $(shell command -v llvm-config),$(abspath tools/llvm-config))
# Hardening: the SDK's libkernel has __stack_chk_fail; its libc has no _chk functions, so
# no _FORTIFY_SOURCE.
HARDEN := -fstack-protector-strong -Wformat=2 -Wformat-security
PS5_CFLAGS := -std=gnu11 -O2 -Wall -Wextra $(HARDEN) $(INCLUDES) $(OPUS_FLAGS) $(PS5_DAVE_INC) -D__unix__ $(CHIAKI_FLAGS) \
	'-DMBEDTLS_USER_CONFIG_FILE="platform/mbedtls_config_ps5.h"' $(SQLITE_FLAGS) $(CV_FLAGS)
HOST_CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra $(HARDEN) $(INCLUDES) $(OPUS_FLAGS) $(HOST_DAVE_INC) $(SQLITE_FLAGS) $(CV_FLAGS) \
	'-DMBEDTLS_USER_CONFIG_FILE="platform/mbedtls_config_core.h"'
# build/build_config.h, rewritten only when it changes (so the files using it rebuild then):
# - CORE_VERSION: VERSION, or VERSION-dev+g<commit> unless RELEASE=1 (GIT_COMMIT where the
#   tree is no checkout, as on the build PC);
# - CORE_LAN: the core answers only the app on the console; CORE_LAN=1 (development: make
#   stop, the deploy's restart) also lets the LAN read /api/state and POST /api/quit (with the key).
VERSION := $(shell cat VERSION)
GIT_COMMIT ?= $(or $(shell git rev-parse --short HEAD 2>/dev/null),unknown)
CORE_VERSION := $(if $(RELEASE),$(VERSION),$(VERSION)-dev+g$(GIT_COMMIT))
CORE_LAN ?= 0
PS5_CFLAGS += -Ibuild
HOST_CFLAGS += -Ibuild

PS5_OBJ = $(patsubst %.c,build/ps5/%.o,$(APP_SRC) $(PS5_ONLY_SRC) $(PS5_DAVE_SRC) src/platform/entropy_ps5.c build/ca_bundle.c $(DEP_SRC)) \
	$(patsubst %.cpp,build/ps5/%.o,$(BLUEV_CXX))
HOST_OBJ = $(patsubst %.c,build/host/%.o,$(APP_SRC) src/headset_stub.c $(HOST_DAVE_SRC) build/ca_bundle.c $(DEP_SRC))

.PHONY: all deps dave ffmpeg host test deploy stop clean release capture capture-test FORCE
.DEFAULT_GOAL := all

build/build_config.h: FORCE
	@mkdir -p $(@D)
	@printf '#define CORE_VERSION "%s"\n#define CORE_LAN %s\n' '$(CORE_VERSION)' '$(CORE_LAN)' > $@.new
	@cmp -s $@.new $@ && rm $@.new || mv $@.new $@
build/ps5/src/main.o build/ps5/src/status.o build/ps5/src/web.o build/host/src/main.o build/host/src/status.o \
	build/host/src/web.o: build/build_config.h

# The files of a GitHub release (ROADMAP.md), from a clean tree: the core built for release
# (no LAN, no -dev), the self-checks, the app with that core inside, and their checksums.
release:
	@test -f $(DAVE_PS5)/lib/libdave.a || { echo "release: libdave is missing (voice would fail with 4017): run make dave first"; exit 1; }
	@test -f $(FFMPEG_PS5)/lib/libavcodec.a || { echo "release: FFmpeg is missing (no AirPods mic): run make ffmpeg first"; exit 1; }
	@test -f .deps/firmware/rtl8761bu_fw.bin || { echo "release: the adapter firmware is missing: run make deps first"; exit 1; }
	$(MAKE) clean
	$(MAKE) RELEASE=1 CORE_LAN=0 all
	$(MAKE) test
	cd app && RELEASE=1 $(MAKE)
	rm -rf dist && mkdir -p dist
	cp build/ps5-discord.elf dist/ps5-discord-core-v$(VERSION).elf
	cp app/dist/PPSA99072.zip dist/ps5-discord-app-v$(VERSION).zip
	cd dist && sha256sum ps5-discord-* > SHA256SUMS && cat SHA256SUMS
all: build/ps5-discord.elf capture

capture:
	$(MAKE) -C capture PS5_PAYLOAD_SDK=$(abspath $(SDK))

capture-test:
	$(MAKE) -C capture test CC=$(HOST_CC)

deps:
	@bash tools/deps.sh

# FFmpeg's AAC decoder for the PS5 (once per machine).
ffmpeg:
	@bash tools/ffmpeg/build.sh $(abspath $(SDK)) $(abspath .deps/ffmpeg)

# Builds OpenSSL, mlspp and libdave for both targets (slow; once per machine).
dave:
	@bash tools/dave/build.sh $(abspath $(SDK)) $(abspath $(DAVE_OUT)) all

$(CA_PEM):
	@echo "$@ is missing: run make deps"; exit 1

build/ca_bundle.c: $(CA_PEM) tools/embed.py
	@mkdir -p $(@D)
	@python3 tools/embed.py $< $@ ca_bundle_pem

build/ps5-discord.elf: $(PS5_OBJ)
	@echo "  LD    $@"
	@PS5_PAYLOAD_SDK=$(abspath $(SDK)) $(PS5_CXX) -nostdlib++ -o $@ $^ $(PS5_DAVE_LINK) $(FFMPEG_LINK) -lc++ -lc++abi -lunwind \
		-lkernel_sys -lpthread -lSceSystemService -lSceUserService

build/ps5/%.o: %.c
	@mkdir -p $(@D)
	@PS5_PAYLOAD_SDK=$(abspath $(SDK)) $(PS5_CC) $(if $(filter .deps/% third_party/%,$<),-w) $(PS5_CFLAGS) -MMD -MP -c $< -o $@

build/ps5/%.o: %.cpp
	@mkdir -p $(@D)
	@PS5_PAYLOAD_SDK=$(abspath $(SDK)) $(PS5_CXX) $(if $(filter third_party/%,$<),-w) $(PS5_CXXFLAGS) -MMD -MP -c $< -o $@

# libsbc is built unmodified with BlueV's settings (SIMD paths, no run-time CPU probe).
$(patsubst %.c,build/ps5/%.o,$(SBC_SRC)): PS5_CFLAGS += -include $(BLUEV)/src/compat/config.h

host: build/host/ps5-discord
build/host/ps5-discord: $(HOST_OBJ)
	@echo "  LD    $@"
	@$(HOST_CC) -o $@ $^ $(HOST_DAVE_LINK)

build/host/%.o: %.c
	@mkdir -p $(@D)
	@$(HOST_CC) $(if $(filter .deps/%,$<),-w) $(HOST_CFLAGS) $(if $(filter src/% tests/%,$<),-Werror) -MMD -MP -c $< -o $@

test: build/host/selftest build/host/browser-storage-test build/host/client-read-state-test build/host/gif-frames-test \
		build/host/ds5-usb-test build/host/web-test build/host/gateway-test build/host/update-test $(HOST_DAVE_TEST) \
		build/host/golive-opus-test build/host/game-audio-test build/host/game-audio-tsan-test build/host/capture-test build/host/capture-io-test capture-test
	./build/host/selftest
	./build/host/golive-opus-test
	./build/host/game-audio-test
	./build/host/game-audio-tsan-test
	./build/host/capture-test
	./build/host/capture-io-test
	./build/host/ds5-usb-test
	./build/host/web-test
	./build/host/gateway-test
	./build/host/update-test
	./build/host/browser-storage-test
	./build/host/client-read-state-test
	./build/host/gif-frames-test
	$(if $(HOST_DAVE_TEST),./build/host/dave-test)

# Wireless Go Live: the Opus framing, the shared game-sound owner (against a fake Avcap2, under
# sanitizers and under TSan) and the backend with a fake helper replaying a console recording.
build/host/golive-opus-test: tests/golive_opus.c src/golive_opus.c src/golive_opus.h $(patsubst %.c,build/host/%.o,$(OPUS_SRC))
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) -Werror -o $@ tests/golive_opus.c src/golive_opus.c $(patsubst %.c,build/host/%.o,$(OPUS_SRC)) -lm
GA_TEST_SRC := tests/game_audio.cpp third_party/bluev/src/core/audio/avcap_capture.cpp
GA_TEST_FLAGS := -std=c++20 -g -O1 -Wall -Wextra -Werror -pthread -DGA_TEST -Ithird_party/bluev/src -Isrc
build/host/game-audio-test: $(GA_TEST_SRC) src/bt/game_audio.cpp src/game_audio.h
	@mkdir -p $(@D)
	$(CXX) $(GA_TEST_FLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -o $@ $(GA_TEST_SRC)
build/host/game-audio-tsan-test: $(GA_TEST_SRC) src/bt/game_audio.cpp src/game_audio.h
	@mkdir -p $(@D)
	$(CXX) $(GA_TEST_FLAGS) -fsanitize=thread -o $@ $(GA_TEST_SRC)
# broadcast.c and capture_bc.c as built for the host, with the rest of the core stubbed in the test.
build/host/capture-test: build/host/tests/capture_backend.o build/host/src/broadcast.o build/host/tests/capture_bc_fault.o \
		build/host/src/golive_opus.o $(patsubst %.c,build/host/%.o,$(OPUS_SRC))
	@mkdir -p $(@D)
	@echo "  LD    $@"
	@$(HOST_CC) -o $@ $^ -lpthread -lm

# Deterministic short writes/backpressure in the real session writer, leaving helper sends intact.
build/host/tests/capture_bc_fault.o: src/capture_bc.c src/capture_bc.h src/capture_io.h src/capture_proto.h src/game_audio.h src/golive_opus.h
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) -Werror -Dsend=capture_test_send -c $< -o $@

build/host/capture-io-test: tests/capture_io.c src/capture_io.h
	@mkdir -p $(@D)
	$(HOST_CC) -std=gnu11 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined -pthread -o $@ $<

# The external sender is libdave's test helper (C++, tests/dave): it signs the group's proposals.
# The media leg links the engine's packet code, Opus and mbedTLS (GCM) as objects.
DAVE_TEST_LIB = build/host/src/voice_audio.o build/host/src/core_key.o build/host/src/prefs.o build/host/$(DS5)/pcm_stream.o build/host/src/voice_proto.o build/host/$(CJSON)/cJSON.o \
	$(patsubst %.c,build/host/%.o,$(OPUS_SRC) $(wildcard $(MBED)/library/*.c))
build/host/dave-test: tests/dave_session.c tests/dave/external_sender.cpp tests/dave/external_sender_wrapper.cpp \
		$(DAVE_TEST_LIB)
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) -Itests -c tests/dave_session.c -o build/host/dave_session.o
	$(CXX) -std=c++17 -Wall -I$(DAVE_HOST)/include -I$(DAVE_HOST)/include/mlspp -Itests/dave -c tests/dave/external_sender.cpp -o build/host/external_sender.o
	$(CXX) -std=c++17 -Wall -I$(DAVE_HOST)/include -I$(DAVE_HOST)/include/mlspp -Itests/dave -c tests/dave/external_sender_wrapper.cpp -o build/host/external_sender_wrapper.o
	$(CXX) -o $@ build/host/dave_session.o build/host/external_sender.o build/host/external_sender_wrapper.o $(DAVE_TEST_LIB) $(HOST_DAVE_LINK)

build/host/client-read-state-test: tests/client_read_state.cpp app/src/discord/read_state.hpp
	@mkdir -p $(@D)
	$(CXX) -std=c++17 -Wall -Wextra -Iapp/src -isystem .deps/src -o $@ $<

build/host/gif-frames-test: tests/gif_frames.cpp app/src/discord/gif.hpp
	@mkdir -p $(@D)
	$(CXX) -std=c++17 -Wall -Wextra -Iapp/src -o $@ $<

build/host/browser-storage-test: tests/browser_storage.c src/browser_storage.c \
		$(patsubst %.c,build/host/%.o,$(CJSON)/cJSON.c $(SQLITE)/sqlite3.c $(wildcard $(MBED)/library/*.c))
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $^ -lm
# web.c's helpers (tests/web.c includes it): every other module of the host core, not main.
build/host/web-test: build/host/tests/web.o $(filter-out build/host/src/main.o build/host/src/web.o,$(HOST_OBJ))
	@mkdir -p $(@D)
	@echo "  LD    $@"
	@$(HOST_CC) -o $@ $^ $(HOST_DAVE_LINK)
build/host/tests/web.o: src/web.c | build/build_config.h
# gateway.c's session choices over a fake WebSocket (tests/gateway.c stands in for ws.c).
build/host/gateway-test: build/host/tests/gateway.o \
		$(filter-out build/host/src/main.o build/host/src/gateway.o build/host/src/ws.o,$(HOST_OBJ))
	@mkdir -p $(@D)
	@echo "  LD    $@"
	@$(HOST_CC) -o $@ $^ $(HOST_DAVE_LINK)
build/host/tests/gateway.o: src/gateway.c | build/build_config.h
# The updater's checks, swap, watch and restore on temporary folders (tests/update.c); its
# zips are deflated with zlib's deflate, which the core itself does not need.
build/host/update-test: build/host/tests/update.o build/host/$(ZLIB)/deflate.o build/host/$(ZLIB)/trees.o \
		$(filter-out build/host/src/main.o build/host/src/update.o build/host/src/update_verify.o,$(HOST_OBJ))
	@mkdir -p $(@D)
	@echo "  LD    $@"
	@$(HOST_CC) -o $@ $^ $(HOST_DAVE_LINK)
build/host/tests/update.o: src/update.c src/update_verify.c | build/build_config.h
# The bridge's checks (tools/ds5-usb-bridge: make test there adds sanitizers).
build/host/ds5-usb-test: $(DS5)/test_usb.c $(DS5)/pcm_stream.c $(wildcard $(DS5)/*.h)
	@mkdir -p $(@D)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -g -o $@ $(DS5)/test_usb.c $(DS5)/pcm_stream.c
# From the same objects as the host build: our sources with -Werror, the libraries quiet.
SELFTEST_SRC := tests/selftest.c src/resample.c src/overlay.c src/png_circle.c src/ws.c src/tls.c src/zstream.c src/log.c src/status.c src/chat.c src/ratelimit.c src/http.c src/props.c \
	src/core_key.c src/voice.c src/voice_proto.c src/voice_audio.c src/prefs.c src/notify.c $(DS5)/pcm_stream.c $(OPUS_SRC) \
	$(HOST_DAVE_SRC) $(CJSON)/cJSON.c $(QRGEN)/qrcodegen.c $(ZLIB_SRC) $(ZLIB)/deflate.c $(ZLIB)/trees.c \
	$(wildcard $(MBED)/library/*.c) build/ca_bundle.c
build/host/selftest: $(patsubst %.c,build/host/%.o,$(SELFTEST_SRC)) | build/build_config.h
	@mkdir -p $(@D)
	@echo "  LD    $@"
	@$(HOST_CC) -o $@ $^ $(HOST_DAVE_LINK)

# A development core (CORE_LAN=1: make stop can reach it). The console must then run this
# build: a core that kept the port (a release build answers only the console) makes the new
# one exit at once ("already running").
deploy: CORE_LAN = 1
deploy: build/ps5-discord.elf stop
	bash -c 'cat $< > /dev/tcp/$(PS5_HOST)/$(ELFLDR_PORT)'
	@for i in 1 2 3 4 5 6 7 8 9 10; do sleep 2; \
		curl -s -m 3 http://$(PS5_HOST):9041/api/state | grep -q '"version":"$(CORE_VERSION)"' && exit 0; done; \
		echo "deploy: $(PS5_HOST) does not run $(CORE_VERSION) after 20 s: another core holds port 9041 (quit it from the app)"; exit 1

stop:
	@test -n "$(PS5_HOST)" || { echo "usage: make $@ PS5_HOST=<ip>"; exit 2; }
	@key=$$(curl -s -m 3 ftp://$(PS5_FTP)/data/homebrew/PPSA99072/control.key); \
	if curl -sf -m 3 -X POST -H 'X-Discord-Native: 1' -H "X-Core-Key: $$key" http://$(PS5_HOST):9041/api/quit >/dev/null; \
	then sleep 1; else echo "stop: no core quit on $(PS5_HOST):9041 (none running, a release build that answers only the console, or no key from ftp://$(PS5_FTP))"; fi

clean:
	rm -rf build
	$(MAKE) -C capture clean

# Header dependencies written by -MMD.
-include $(PS5_OBJ:.o=.d) $(HOST_OBJ:.o=.d)
