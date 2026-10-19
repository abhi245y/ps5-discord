#!/usr/bin/env bash
# ps5-discord - Launches or closes this title on the console (development).
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Usage: PS5_HOST=<ip> tools/controller.sh <launch|close>
# Builds the controller payload (tools/controllers, from
# ps5-homebrew-dev-protocol) for this title ID and sends it to elfldr.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
mode=${1:-}
[[ $mode == launch || $mode == close ]] || { echo "usage: $0 <launch|close>" >&2; exit 2; }
host=${PS5_HOST:?set PS5_HOST}
sdk=${PS5_PAYLOAD_SDK:-$root/../.deps/ps5-payload-sdk}
export LLVM_CONFIG=${LLVM_CONFIG:-$root/tools/llvm-config} # the SDK finds clang through it
title=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$root/sce_sys/param.json")
out="$root/build/controller-$mode.elf"
mkdir -p "$root/build"
libs=(-lSceSystemService)
[[ $mode == launch ]] && libs+=(-lSceUserService)
"$sdk/bin/prospero-clang" -Wall -Werror -DBOOTSTRAP_TITLE_ID="\"$title\"" "${libs[@]}" \
    -o "$out" "$root/tools/controllers/$mode.c"
socat -u "FILE:$out" "TCP:$host:${ELF_PORT:-9021}"
echo "$mode sent for $title"
