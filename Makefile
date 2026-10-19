# ps5-discord
#
#   make deps                     fetch pinned SDK + sources into .deps/
#   make                          build/ps5-discord.elf (payload)
#   make deploy PS5_HOST=<ip>     send it to elfldr
#   make host                     build/host/ps5-discord (runs on Linux with ./token.txt)
#   make test                     host self-checks

PS5_HOST ?=
ELFLDR_PORT ?= 9021
CA_PEM ?= /etc/pki/tls/certs/ca-bundle.crt
HOST_CC ?= cc

SDK := .deps/ps5-payload-sdk
MBED := .deps/src/mbedtls-3.6.4
ZLIB := .deps/src/zlib-1.3.2
CJSON := .deps/src/cJSON-1.7.18

APP_SRC := src/main.c src/gateway.c src/ws.c src/tls.c src/zstream.c
ZLIB_SRC := $(addprefix $(ZLIB)/,adler32.c crc32.c inffast.c inflate.c inftrees.c zutil.c)
DEP_SRC = $(wildcard $(MBED)/library/*.c) $(ZLIB_SRC) $(CJSON)/cJSON.c
INCLUDES := -Isrc -I$(MBED)/include -I$(ZLIB) -I$(CJSON)

PS5_CC := $(SDK)/bin/prospero-clang
PS5_CFLAGS := -std=gnu11 -O2 -Wall $(INCLUDES) -D__unix__ \
	'-DMBEDTLS_USER_CONFIG_FILE="platform/mbedtls_config_ps5.h"'
HOST_CFLAGS := -std=gnu11 -O1 -g -Wall $(INCLUDES)

PS5_OBJ = $(patsubst %.c,build/ps5/%.o,$(APP_SRC) src/platform/entropy_ps5.c build/ca_bundle.c $(DEP_SRC))
HOST_OBJ = $(patsubst %.c,build/host/%.o,$(APP_SRC) build/ca_bundle.c $(DEP_SRC))

.PHONY: all deps host test deploy clean
all: build/ps5-discord.elf

deps:
	@bash tools/deps.sh

build/ca_bundle.c: $(CA_PEM) tools/embed_ca.py
	@mkdir -p $(@D)
	python3 tools/embed_ca.py $< $@

build/ps5-discord.elf: $(PS5_OBJ)
	PS5_PAYLOAD_SDK=$(abspath $(SDK)) $(PS5_CC) -o $@ $^ -lpthread

build/ps5/%.o: %.c
	@mkdir -p $(@D)
	@PS5_PAYLOAD_SDK=$(abspath $(SDK)) $(PS5_CC) $(if $(filter .deps/%,$<),-w) $(PS5_CFLAGS) -c $< -o $@

host: build/host/ps5-discord
build/host/ps5-discord: $(HOST_OBJ)
	$(HOST_CC) -o $@ $^

build/host/%.o: %.c
	@mkdir -p $(@D)
	@$(HOST_CC) $(if $(filter .deps/%,$<),-w) $(HOST_CFLAGS) -c $< -o $@

test: build/host/selftest
	./build/host/selftest
build/host/selftest: tests/selftest.c src/ws.c src/tls.c src/zstream.c $(ZLIB_SRC) \
		$(ZLIB)/deflate.c $(ZLIB)/trees.c $(wildcard $(MBED)/library/*.c) build/ca_bundle.c
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) -w -o $@ $^

deploy: build/ps5-discord.elf
	@test -n "$(PS5_HOST)" || { echo "usage: make deploy PS5_HOST=<ip>"; exit 2; }
	bash -c 'cat $< > /dev/tcp/$(PS5_HOST)/$(ELFLDR_PORT)'

clean:
	rm -rf build
