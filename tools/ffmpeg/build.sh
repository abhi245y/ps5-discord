#!/usr/bin/env bash
# Builds FFmpeg 7.1.1 with one decoder, AAC (for the AirPods' AAC-ELD microphone stream),
# as static libraries for the PS5 payload SDK. LGPL 2.1 or later; no programs, no asm.
#
# Run it on arlp-pc - not on the laptop. Nothing is installed system-wide.
#
#   tools/ffmpeg/build.sh <ps5-payload-sdk dir> <output dir>
#
# Layout under <output dir>: dl/ (the tarball, sha256 checked), src/, build/, install/ps5/.
set -euo pipefail

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
SDK=$(realpath -- "${1:?usage: build.sh <sdk dir> <output dir>}")
OUT=$(realpath -m -- "${2:?usage: build.sh <sdk dir> <output dir>}")

VER=7.1.1
URL=https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz
SHA256=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1

mkdir -p "$OUT"/{dl,src,build,install/ps5}
tarball=$OUT/dl/ffmpeg-$VER.tar.xz
if [[ ! -f $tarball ]]; then
    curl -fsSL -o "$tarball.part" "$URL"
    mv "$tarball.part" "$tarball"
fi
echo "$SHA256  $tarball" | sha256sum -c --quiet
rm -rf "$OUT/src/ffmpeg-$VER" "$OUT/build/ps5"
tar -C "$OUT/src" -xf "$tarball"
mkdir -p "$OUT/build/ps5"

export PS5_PAYLOAD_SDK=$SDK PATH=$SDK/bin:$PATH
export LLVM_CONFIG=${LLVM_CONFIG:-$(command -v llvm-config || echo "$HERE/../llvm-config")}
cd "$OUT/build/ps5"
"$OUT/src/ffmpeg-$VER/configure" --prefix="$OUT/install/ps5" \
    --enable-cross-compile --arch=x86_64 --target-os=freebsd \
    --cc=prospero-clang --cxx=prospero-clang++ --ar=prospero-ar --ranlib=prospero-ranlib \
    --nm=prospero-nm --strip=prospero-strip --pkg-config=false \
    --disable-everything --enable-decoder=aac \
    --disable-programs --disable-doc --disable-avdevice --disable-avformat --disable-swscale \
    --disable-postproc --disable-avfilter --disable-swresample --disable-network \
    --disable-asm --disable-autodetect --disable-debug --enable-static --disable-shared
make -j"$(nproc)"
make install
echo "ffmpeg: $OUT/install/ps5/lib"
ls -l "$OUT/install/ps5/lib"/*.a
