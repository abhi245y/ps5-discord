#!/usr/bin/env bash
# Fetches pinned third-party sources into .deps/ (ignored by git).
# Pins match spot5 where shared (SDK, mbedTLS, zlib).
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
deps="$root/.deps"
mkdir -p "$deps/src"

fetch() { # url sha256 out
    [[ -f $3 ]] && echo "$2  $3" | sha256sum --check --quiet && return
    curl -fsSL -o "$3.part" "$1"
    echo "$2  $3.part" | sha256sum --check --quiet
    mv "$3.part" "$3"
}

if [[ ! -x $deps/ps5-payload-sdk/bin/prospero-clang ]]; then
    fetch https://github.com/ps5-payload-dev/sdk/releases/download/v0.42/ps5-payload-sdk.zip \
        8cfbc7cd5811e719eb4f0c47eea668d3dc7b40bc8ab11c4a5031d40c23ec02da "$deps/ps5-payload-sdk.zip"
    unzip -q -o "$deps/ps5-payload-sdk.zip" -d "$deps"
fi

fetch https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.4/mbedtls-3.6.4.tar.bz2 \
    ec35b18a6c593cf98c3e30db8b98ff93e8940a8c4e690e66b41dfc011d678110 "$deps/mbedtls-3.6.4.tar.bz2"
[[ -d $deps/src/mbedtls-3.6.4 ]] || tar -xjf "$deps/mbedtls-3.6.4.tar.bz2" -C "$deps/src"

fetch https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz \
    bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16 "$deps/zlib-1.3.2.tar.gz"
[[ -d $deps/src/zlib-1.3.2 ]] || tar -xzf "$deps/zlib-1.3.2.tar.gz" -C "$deps/src"

fetch https://github.com/DaveGamble/cJSON/archive/refs/tags/v1.7.18.tar.gz \
    3aa806844a03442c00769b83e99970be70fbef03735ff898f4811dd03b9f5ee5 "$deps/cJSON-1.7.18.tar.gz"
[[ -d $deps/src/cJSON-1.7.18 ]] || tar -xzf "$deps/cJSON-1.7.18.tar.gz" -C "$deps/src"

fetch https://github.com/nayuki/QR-Code-generator/archive/refs/tags/v1.8.0.tar.gz \
    2ec0a4d33d6f521c942eeaf473d42d5fe139abcfa57d2beffe10c5cf7d34ae60 "$deps/qrcodegen-1.8.0.tar.gz"
[[ -d $deps/src/QR-Code-generator-1.8.0 ]] || tar -xzf "$deps/qrcodegen-1.8.0.tar.gz" -C "$deps/src"

fetch https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip \
    1d3049dd0f830a025a53105fc79fd2ab9431aea99e137809d064d8ee8356b032 \
    "$deps/sqlite-amalgamation-3500400.zip"
[[ -d $deps/src/sqlite-amalgamation-3500400 ]] || unzip -q "$deps/sqlite-amalgamation-3500400.zip" -d "$deps/src"

echo "deps ready in $deps"
