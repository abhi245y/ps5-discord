# ps5-discord
#
#   make deps                     fetch pinned SDK + sources into .deps/
#   make                          build/ps5-discord.elf (payload)
#   make deploy PS5_HOST=<ip>     stop the running copy, send the new one to elfldr
#   make stop PS5_HOST=<ip>       stop the running copy
#   status UI: http://<ip>:9041
#   make host                     build/host/ps5-discord (runs on Linux, token in ./token)
#   make test                     host self-checks
#   make dave                     build libdave (DAVE E2EE) into .deps/dave with tools/dave/build.sh (run on arlp-pc)

PS5_HOST ?=
ELFLDR_PORT ?= 9021
CA_PEM ?= $(firstword $(wildcard /etc/pki/tls/certs/ca-bundle.crt /etc/ssl/certs/ca-certificates.crt /etc/ssl/cert.pem))
HOST_CC ?= cc
# Test builds quit when the app closes (the app starts the core). Release: make CORE_FOLLOWS_APP=0
CORE_FOLLOWS_APP ?= 1

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
	src/voice.c src/voice_proto.c src/voice_audio.c src/controller_voice.c $(DS5)/pcm_stream.c
# Controller voice (console only): a loopback Remote Play session (vendored chiaki-ng,
# third_party/README.md) and the DualSense USB runtime from tools/ds5-usb-bridge.
CHIAKI := third_party/chiaki-ng
CHIAKI_SRC := $(wildcard $(CHIAKI)/src/*.c) $(CHIAKI)/src/remote/rudp.c $(CHIAKI)/src/remote/rudpsendbuffer.c \
	$(CHIAKI)/gen/takion.pb.c $(addprefix third_party/nanopb/,pb_common.c pb_encode.c pb_decode.c) \
	$(wildcard third_party/gf-complete/src/*.c) $(wildcard third_party/jerasure/src/*.c)
PS5_ONLY_SRC := $(CHIAKI_SRC) $(DS5)/ds5_usb.c
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
# The SDK wrappers find LLVM through llvm-config; use the system one if present.
export LLVM_CONFIG ?= $(or $(shell command -v llvm-config),$(abspath tools/llvm-config))
PS5_CFLAGS := -std=gnu11 -O2 -Wall $(INCLUDES) $(OPUS_FLAGS) $(PS5_DAVE_INC) -D__unix__ -DCORE_FOLLOWS_APP=$(CORE_FOLLOWS_APP) $(CHIAKI_FLAGS) \
	'-DMBEDTLS_USER_CONFIG_FILE="platform/mbedtls_config_ps5.h"' $(SQLITE_FLAGS)
HOST_CFLAGS := -std=gnu11 -O1 -g -Wall $(INCLUDES) $(OPUS_FLAGS) $(HOST_DAVE_INC) $(SQLITE_FLAGS)

PS5_OBJ = $(patsubst %.c,build/ps5/%.o,$(APP_SRC) $(PS5_ONLY_SRC) $(PS5_DAVE_SRC) src/platform/entropy_ps5.c build/ca_bundle.c build/web_index.c $(DEP_SRC))
HOST_OBJ = $(patsubst %.c,build/host/%.o,$(APP_SRC) $(HOST_DAVE_SRC) build/ca_bundle.c build/web_index.c $(DEP_SRC))

.PHONY: all deps dave host test deploy stop clean
all: build/ps5-discord.elf

deps:
	@bash tools/deps.sh

# Builds OpenSSL, mlspp and libdave for both targets (slow; once per machine).
dave:
	@bash tools/dave/build.sh $(abspath $(SDK)) $(abspath $(DAVE_OUT)) all

build/ca_bundle.c: $(CA_PEM) tools/embed.py
	@mkdir -p $(@D)
	@python3 tools/embed.py $< $@ ca_bundle_pem

build/web_index.c: web/index.html tools/embed.py
	@mkdir -p $(@D)
	@python3 tools/embed.py $< $@ web_index_html

build/ps5-discord.elf: $(PS5_OBJ)
	@echo "  LD    $@"
	@PS5_PAYLOAD_SDK=$(abspath $(SDK)) $(PS5_CXX) -nostdlib++ -o $@ $^ $(PS5_DAVE_LINK) -lpthread -lSceSystemService -lSceUserService

build/ps5/%.o: %.c
	@mkdir -p $(@D)
	@PS5_PAYLOAD_SDK=$(abspath $(SDK)) $(PS5_CC) $(if $(filter .deps/% third_party/%,$<),-w) $(PS5_CFLAGS) -MMD -MP -c $< -o $@

host: build/host/ps5-discord
build/host/ps5-discord: $(HOST_OBJ)
	@echo "  LD    $@"
	@$(HOST_CC) -o $@ $^ $(HOST_DAVE_LINK)

build/host/%.o: %.c
	@mkdir -p $(@D)
	@$(HOST_CC) $(if $(filter .deps/%,$<),-w) $(HOST_CFLAGS) -MMD -MP -c $< -o $@

test: build/host/selftest build/host/browser-storage-test build/host/client-read-state-test $(HOST_DAVE_TEST)
	./build/host/selftest
	./build/host/browser-storage-test
	./build/host/client-read-state-test
	$(if $(HOST_DAVE_TEST),./build/host/dave-test)

# The external sender is libdave's test helper (C++, tests/dave): it signs the group's proposals.
# The media leg links the engine's packet code, Opus and mbedTLS (GCM) as objects.
DAVE_TEST_LIB = build/host/src/voice_audio.o build/host/$(DS5)/pcm_stream.o build/host/src/voice_proto.o build/host/$(CJSON)/cJSON.o \
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
	$(CXX) -std=c++17 -Wall -Wextra -Iapp/src -isystem app/.deps/src -o $@ $<

build/host/browser-storage-test: tests/browser_storage.c src/browser_storage.c $(CJSON)/cJSON.c $(SQLITE)/sqlite3.c
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $^ -lm
build/host/selftest: tests/selftest.c src/ws.c src/tls.c src/zstream.c src/log.c src/status.c src/chat.c src/ratelimit.c \
		src/voice.c src/voice_proto.c src/voice_audio.c $(DS5)/pcm_stream.c $(OPUS_SRC) $(HOST_DAVE_SRC) \
		$(CJSON)/cJSON.c $(QRGEN)/qrcodegen.c $(ZLIB_SRC) \
		$(ZLIB)/deflate.c $(ZLIB)/trees.c $(wildcard $(MBED)/library/*.c) build/ca_bundle.c
	@mkdir -p $(@D)
	@echo "  LD    $@"
	@$(HOST_CC) $(HOST_CFLAGS) -w -o $@ $^ $(HOST_DAVE_LINK)

deploy: build/ps5-discord.elf stop
	bash -c 'cat $< > /dev/tcp/$(PS5_HOST)/$(ELFLDR_PORT)'

stop:
	@test -n "$(PS5_HOST)" || { echo "usage: make $@ PS5_HOST=<ip>"; exit 2; }
	@curl -s -m 3 -X POST http://$(PS5_HOST):9041/api/quit >/dev/null && sleep 1 || true

clean:
	rm -rf build

# Header dependencies written by -MMD.
-include $(PS5_OBJ:.o=.d) $(HOST_OBJ:.o=.d)
