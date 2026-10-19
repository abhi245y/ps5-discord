#!/usr/bin/env bash
# ps5-homebrew-ui - Render every design on the host (Mesa surfaceless EGL) to PNG.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: tools/host-snapshots.sh [output dir] [design id|all] [width height]
#
# Builds the platform-neutral part of the app for the PC and runs its tour
# (see src/app/tour.hpp). HUI_STRIP=<from>,<to> writes the frames of the
# switch between two designs instead.

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$root/tools/ninja-build.sh"
cxx=$(command -v "${HOST_CXX:-clang++}")
cc=$(command -v "${HOST_CC:-clang}")
build="$root/build/host-snapshots"
ninja_begin "$build/build.ninja"

# Everything under src/ except the console platform layer and entry point.
sources=("$root/host/snapshot_main.cpp" "$root/host/manifest.cpp" "$root/host/platform_host.cpp")
while IFS= read -r -d '' source; do
    sources+=("$source")
done < <(find "$root/src" -type f \( -name '*.cpp' -o -name '*.c' \) \
    ! -path "$root/src/platform/*" ! -path "$root/src/runtime/*" ! -name main.cpp ! -name output.cpp -print0 | sort -z)

# ps5-discord: the same HarfBuzz and libwebp the console links (tools/build-deps.sh fetches them).
hb="$root/.deps/src/harfbuzz-11.2.1/src"
webp="$root/.deps/src/libwebp-1.4.0"
[[ -f $hb/harfbuzz.cc && -d $webp/src/dec ]] || { echo "HarfBuzz or libwebp missing: run make libs once" >&2; exit 1; }
objects=()
ninja_inputs=("$hb/harfbuzz.cc" "$cxx")
ninja_edge CXX "$build/obj/harfbuzz.o" "$cxx" -std=c++17 -O2 -w -fno-exceptions -fno-rtti -DHB_NO_MT \
    -MD -MF "$build/obj/harfbuzz.o.d" -c "$hb/harfbuzz.cc" -o "$build/obj/harfbuzz.o"
objects+=("$build/obj/harfbuzz.o")
while IFS= read -r -d '' source; do
    object="$build/obj/webp_$(basename "$source").o"
    ninja_inputs=("$source" "$cc")
    ninja_edge CC "$object" "$cc" -std=c11 -O2 -w -I"$webp" -MD -MF "$object.d" -c "$source" -o "$object"
    objects+=("$object")
done < <(find "$webp/src/dec" "$webp/src/dsp" "$webp/src/utils" -name '*.c' -print0 | sort -z)
for source in "${sources[@]}"; do
    relative=${source#"$root/"}
    object="$build/obj/${relative//\//_}.o"
    if [[ $source == *.c ]]; then
        ninja_inputs=("$source" "$cc")
        ninja_edge CC "$object" "${compiler_cache[@]}" "$cc" -std=c11 -O2 -w -I"$root/src" \
            -MD -MF "$object.d" -c "$source" -o "$object"
    else
        ninja_inputs=("$source" "$cxx")
        ninja_edge CXX "$object" "${compiler_cache[@]}" "$cxx" -std=c++20 -O2 -Wall -Wextra \
            -DGL_GLEXT_PROTOTYPES=1 -I"$root/src" -I"$root/host" -isystem "$hb" -isystem "$webp/src" \
            -isystem "$root/.deps/src" -MD -MF "$object.d" -c "$source" -o "$object"
    fi
    objects+=("$object")
done
ninja_inputs=("${objects[@]}")
ninja_edge LINK "$build/hui_snapshots" "$cxx" "${objects[@]}" -lEGL -lGL -lm -o "$build/hui_snapshots"
if ! ninja_run >"$build/build.log" 2>&1; then
    grep -E 'error|FAILED|warning' -A6 "$build/build.log" | head -80 >&2
    exit 1
fi
grep -E 'warning' -A4 "$build/build.log" | head -40 >&2 || true

output=${1:-"$root/build/snapshots"}
mkdir -p "$output"
EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe \
    "$build/hui_snapshots" "$root/assets" "$output" "${@:2}"
