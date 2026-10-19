#!/usr/bin/env bash
# ps5-discord app - Builds the static libraries the app links, with the app's
# PS5 compiler wrapper. Adapted from spot5 (GPL-3.0-or-later).
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Output: build/deps/libwebpdec.a, build/deps/libharfbuzz.a,
#         .deps/src/nlohmann/json.hpp (header only)
# ponytail: rebuilds everything each run (~1 min); move into the ninja graph if
# that starts to hurt.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sdk_root="$root/.deps/native/ps5-payload-sdk"
out="$root/build/deps"

# libwebp, decoder only: Discord serves avatars, icons and attachments as WebP.
# Release tarball signed by the WebP release key; the hash pins that verified file.
webp_version=1.4.0
webp_sha256=61f873ec69e3be1b99535634340d5bde750b2e4447caa1db9f61be3fd49ab1e5
webp_tar="$root/.deps/src/libwebp-$webp_version.tar.gz"
webp="$root/.deps/src/libwebp-$webp_version"

# HarfBuzz: shaping for names and messages outside ASCII. One-file build,
# single-threaded: only the draw thread shapes text.
hb_version=11.2.1
hb_sha256=093714c8548a285094685f0bdc999e202d666b59eeb3df2ff921ab68b8336a49
hb_tar="$root/.deps/src/harfbuzz-$hb_version.tar.xz"
hb="$root/.deps/src/harfbuzz-$hb_version"

# nlohmann/json: parsing the core's control API replies.
json_version=3.11.3
json_sha256=9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6
json="$root/.deps/src/nlohmann/json.hpp"

mkdir -p "$root/.deps/src" "$out" "$(dirname "$json")"

fetch() { # url sha256 file
    [[ -f $3 ]] || curl -fsSL -o "$3" "$1"
    echo "$2  $3" | sha256sum --check --quiet
}

cc() {
    env PS5_PAYLOAD_SDK="$sdk_root" PS5_CLANG="${PS5_CLANG:-clang}" USE_CCACHE=0 \
        sh "$root/tooling/prospero-clang18" "$@"
}

# Compiles $3.. into objects under $1 with the flags in the array named $2.
compile() {
    local dir=$1 flags_name=$2
    shift 2
    local -n flags=$flags_name
    rm -rf "$dir" && mkdir -p "$dir"
    local pids=() status=0
    for source in "$@"; do
        cc "${flags[@]}" -c "$source" -o "$dir/$(basename "$source").o" &
        pids+=($!)
        if (( ${#pids[@]} >= $(nproc) )); then
            wait "${pids[0]}" || status=1
            pids=("${pids[@]:1}")
        fi
    done
    for pid in "${pids[@]}"; do wait "$pid" || status=1; done
    return $status
}

fetch "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-$webp_version.tar.gz" \
    "$webp_sha256" "$webp_tar"
[[ -d $webp ]] || tar -xzf "$webp_tar" -C "$root/.deps/src"
fetch "https://github.com/harfbuzz/harfbuzz/releases/download/$hb_version/harfbuzz-$hb_version.tar.xz" \
    "$hb_sha256" "$hb_tar"
[[ -d $hb ]] || tar -xJf "$hb_tar" -C "$root/.deps/src"
fetch "https://github.com/nlohmann/json/releases/download/v$json_version/json.hpp" "$json_sha256" "$json"

# Everything under dec/dsp/utils compiles; the archive only lends the linker
# what WebPDecodeRGBA needs.
webp_flags=(-std=c11 -O2 -w -ffunction-sections -fdata-sections -I"$webp")
compile "$out/webp" webp_flags "$webp"/src/dec/*.c "$webp"/src/dsp/*.c "$webp"/src/utils/*.c
rm -f "$out/libwebpdec.a" && llvm-ar rcs "$out/libwebpdec.a" "$out"/webp/*.o

hb_flags=(-std=c++17 -O2 -w -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -DHB_NO_MT)
compile "$out/harfbuzz" hb_flags "$hb/src/harfbuzz.cc"
rm -f "$out/libharfbuzz.a" && llvm-ar rcs "$out/libharfbuzz.a" "$out"/harfbuzz/*.o

echo "deps: $out/libwebpdec.a $out/libharfbuzz.a $json"
