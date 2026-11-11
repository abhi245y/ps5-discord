# Build-only. Run on arlp-pc. No target sends anything to a console.
SDK := $(abspath $(PS5_PAYLOAD_SDK))
CXX_PS5 := $(SDK)/bin/prospero-clang++
CC_PS5 := $(SDK)/bin/prospero-clang
export LLVM_CONFIG
HASH_SOURCES := src/core/worker.cpp src/core/protocol.hpp src/core/hook.hpp src/core/parent.cpp \
             src/app/overlay_ui.hpp src/app/overlay_app.cpp src/app/discord_app.inc vendor/shellui-loader/prologue.hpp
LAB_SOURCES := $(filter-out src/app/discord_app.inc,$(HASH_SOURCES)) src/app/pill_lab.inc src/app/pill_lab_model.hpp
LAB_BUILD_ID := $(shell cat $(LAB_SOURCES) | sha256sum | cut -c1-16)
BUILD_ID := $(shell cat $(HASH_SOURCES) | sha256sum | cut -c1-16)
FLAGS := -O2 -g -march=znver2 -fPIC -std=c++20 -fno-exceptions -fno-rtti -fno-threadsafe-statics -Wall -Wextra -Werror -fno-omit-frame-pointer -DOVERLAY_BUILD=0x$(BUILD_ID)ULL -Isrc -Isrc/core -Isrc/app -Ivendor/shellui-loader
LAB_FLAGS := $(filter-out -DOVERLAY_BUILD=%,$(FLAGS)) -DOVERLAY_BUILD=0x$(LAB_BUILD_ID)ULL -DOVERLAY_APP_LAB
VENDOR := vendor/shellui-loader
OBJECTS := build/pt.o build/loader.o build/injector.o build/remote-call.o build/blob.o
.PHONY: all test inspect clean preview pill-lab lab-preview
all: build/overlay-resolve.elf build/overlay-observe.elf build/overlay-widget.elf build/overlay-input.elf build/overlay-restart.elf build/overlay-service.elf inspect
# The build id is baked into both halves (worker and loader): any change to it rebuilds both,
# or the loader's request carries an id the embedded worker refuses.
build/overlay-worker.elf: $(HASH_SOURCES) Makefile
	@mkdir -p build
	$(CXX_PS5) $(FLAGS) -nostdlib++ -shared -Wl,-e,elf_main -Wl,-Bsymbolic -Wl,--hash-style=sysv src/core/worker.cpp -lkernel_sys -lSceLibcInternal -o $@
build/blob.c: build/overlay-worker.elf tools/embed.py
	python3 tools/embed.py $< $@
build/blob.o: build/blob.c
	$(CC_PS5) -O2 -fPIC -c $< -o $@
build/pt.o: $(VENDOR)/pt.c $(VENDOR)/pt.h
	@mkdir -p build
	$(CC_PS5) -O2 -g -fPIC -Wall -Wextra -Werror -I$(VENDOR) -c $< -o $@
build/loader.o: $(VENDOR)/stable_elfldr_bridge.c $(VENDOR)/elfldr.c
	@mkdir -p build
	$(CC_PS5) -O2 -g -fPIC -I$(VENDOR) -c $< -o $@
build/injector.o: $(VENDOR)/stable_shellui_injector.cpp
	@mkdir -p build
	$(CXX_PS5) $(FLAGS) -c $< -o $@
build/remote-call.o: $(VENDOR)/remote_call.cpp
	@mkdir -p build
	$(CXX_PS5) $(FLAGS) -c $< -o $@
build/parent-resolve.o: src/core/parent.cpp src/core/protocol.hpp $(HASH_SOURCES) Makefile
	@mkdir -p build
	$(CXX_PS5) $(FLAGS) -DOVERLAY_MODE=0 -c $< -o $@
build/parent-observe.o: src/core/parent.cpp src/core/protocol.hpp $(HASH_SOURCES) Makefile
	@mkdir -p build
	$(CXX_PS5) $(FLAGS) -DOVERLAY_MODE=1 -c $< -o $@
build/parent-widget.o: src/core/parent.cpp src/core/protocol.hpp $(HASH_SOURCES) Makefile
	@mkdir -p build
	$(CXX_PS5) $(FLAGS) -DOVERLAY_MODE=2 -c $< -o $@
build/parent-input.o: src/core/parent.cpp src/core/protocol.hpp $(HASH_SOURCES) Makefile
	@mkdir -p build
	$(CXX_PS5) $(FLAGS) -DOVERLAY_MODE=3 -c $< -o $@
build/overlay-resolve.elf: build/parent-resolve.o $(OBJECTS)
	$(CXX_PS5) $(FLAGS) $^ -o $@
build/overlay-observe.elf: build/parent-observe.o $(OBJECTS)
	$(CXX_PS5) $(FLAGS) $^ -o $@
build/overlay-widget.elf: build/parent-widget.o $(OBJECTS)
	$(CXX_PS5) $(FLAGS) $^ -o $@
build/overlay-input.elf: build/parent-input.o $(OBJECTS)
	$(CXX_PS5) $(FLAGS) $^ -o $@
build/parent-restart.o: src/core/parent.cpp src/core/protocol.hpp $(HASH_SOURCES) Makefile
	@mkdir -p build
	$(CXX_PS5) $(FLAGS) -DOVERLAY_MODE=4 -c $< -o $@
build/overlay-restart.elf: build/parent-restart.o $(OBJECTS)
	$(CXX_PS5) $(FLAGS) $^ -o $@
build/parent-service.o: src/core/parent.cpp src/core/protocol.hpp $(HASH_SOURCES) Makefile
	@mkdir -p build
	$(CXX_PS5) $(FLAGS) -DOVERLAY_MODE=5 -c $< -o $@
build/overlay-service.elf: build/parent-service.o $(OBJECTS)
	$(CXX_PS5) $(FLAGS) $^ -o $@
	printf '%s' $(BUILD_ID) > build/overlay.build # the core compares it with the running one's
# Standalone lab: explicit opt-in, never linked into the app package or normal blobs.
pill-lab: build/pill-lab.elf
	python3 tools/check_elf.py build/pill-lab-worker.elf
build/pill-lab-worker.elf: $(LAB_SOURCES) Makefile
	@mkdir -p build
	$(CXX_PS5) $(LAB_FLAGS) -nostdlib++ -shared -Wl,-e,elf_main -Wl,-Bsymbolic -Wl,--hash-style=sysv src/core/worker.cpp -lkernel_sys -lSceLibcInternal -o $@
build/pill-lab-blob.c: build/pill-lab-worker.elf tools/embed.py
	python3 tools/embed.py $< $@
build/pill-lab-blob.o: build/pill-lab-blob.c
	$(CC_PS5) -O2 -fPIC -c $< -o $@
build/parent-lab.o: $(LAB_SOURCES) Makefile
	@mkdir -p build
	$(CXX_PS5) $(LAB_FLAGS) -DOVERLAY_MODE=5 -c src/core/parent.cpp -o $@
build/pill-lab.elf: build/parent-lab.o $(filter-out build/blob.o,$(OBJECTS)) build/pill-lab-blob.o
	$(CXX_PS5) $(LAB_FLAGS) $^ -o $@
inspect: build/overlay-worker.elf
	python3 tools/check_elf.py $<
build/safety-test: tests/safety.cpp src/core/protocol.hpp src/core/hook.hpp
	@mkdir -p build
	$(CXX) -std=c++20 -g -O1 -Wall -Wextra -Werror -Wno-misleading-indentation -fsanitize=address,undefined -fno-sanitize=function -fno-omit-frame-pointer tests/safety.cpp -o $@
build/worker-test: tests/worker.cpp src/core/worker.cpp src/core/protocol.hpp src/core/hook.hpp src/app/demo_app.inc
	@mkdir -p build
	$(CXX) -std=c++20 -g -O1 -Wall -Wextra -Werror -Wno-misleading-indentation -fsanitize=address,undefined -fno-sanitize=function -fno-omit-frame-pointer -DOVERLAY_APP_DEMO tests/worker.cpp -o $@
build/discord-app-test: tests/discord_app.cpp src/app/discord_app.inc src/app/overlay_ui.hpp
	@mkdir -p build
	$(CXX) -std=c++20 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer tests/discord_app.cpp -o $@
# The overlay's layout as pictures (tools/preview.py), for a look before a console run.
build/preview: tests/preview.cpp src/app/discord_app.inc src/app/overlay_ui.hpp
	@mkdir -p build
	$(CXX) -std=c++20 -g -O1 -Wall -Wextra -Werror tests/preview.cpp -o $@
preview: build/preview
	@mkdir -p build/preview-out
	./build/preview build/preview-out
	python3 tools/preview.py build/preview-out
build/lab-raster-test: tests/lab_raster.cpp src/app/pill_lab_model.hpp
	@mkdir -p build
	$(CXX) -std=c++20 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer tests/lab_raster.cpp -lz -o $@
build/lab-app-test: tests/lab_app.cpp src/app/pill_lab.inc src/app/pill_lab_model.hpp src/app/overlay_ui.hpp tests/preview.cpp
	@mkdir -p build
	$(CXX) -std=c++20 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer tests/lab_app.cpp -o $@
build/lab-preview: tests/preview.cpp src/app/pill_lab.inc src/app/pill_lab_model.hpp src/app/overlay_ui.hpp
	@mkdir -p build
	$(CXX) -std=c++20 -g -O1 -Wall -Wextra -Werror -DOVERLAY_APP_LAB tests/preview.cpp -o $@
lab-preview: build/lab-preview
	@mkdir -p build/lab-preview-out
	./build/lab-preview build/lab-preview-out
	python3 tools/preview.py build/lab-preview-out
test: build/safety-test build/worker-test build/discord-app-test build/lab-raster-test build/lab-app-test
	./build/safety-test
	./build/worker-test
	./build/discord-app-test
	./build/lab-raster-test
	./build/lab-app-test
clean:
	rm -rf build
