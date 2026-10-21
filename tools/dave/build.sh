#!/usr/bin/env bash
# Builds OpenSSL 3.6.5 (libcrypto + libssl), nlohmann/json 3.11.3, mlspp
# (commit 1cc50a1, the commit libdave v1.2.1 pins in cpp/vcpkg-alts) and
# libdave v1.2.1/cpp as STATIC libraries, for the PS5 payload SDK and for the host.
#
# Run it on arlp-pc (Fedora, clang 21, cmake, ninja, perl) - not on the laptop.
# Nothing is installed system-wide; everything lands under <output dir>.
#
#   tools/dave/build.sh <ps5-payload-sdk dir> <output dir> [ps5|host|all]
#
# Layout under <output dir>:
#   dl/        pinned tarballs (sha256 checked)
#   src/       unpacked / cloned sources (pristine)
#   build/     out-of-tree build trees
#   install/   ps5/ and host/ prefixes (include/, lib/*.a)
#   bin/       llvm-config shim the SDK wrappers call for --bindir
set -euo pipefail

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
SDK=$(realpath -- "${1:?usage: build.sh <sdk dir> <output dir> [ps5|host|all]}")
OUT=$(realpath -m -- "${2:?usage: build.sh <sdk dir> <output dir> [ps5|host|all]}")
WHAT=${3:-all}
JOBS=$(nproc)

OSSL_VER=3.6.5
OSSL_URL=https://github.com/openssl/openssl/releases/download/openssl-$OSSL_VER/openssl-$OSSL_VER.tar.gz
OSSL_SHA256=a2157c2830efdec3788939b00c9b0638306d3f0bbb76dc4832ee503bb397df98 # == openssl.org/source/openssl-3.6.5.tar.gz.sha256
JSON_URL=https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
JSON_SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
MLSPP_REPO=https://github.com/cisco/mlspp.git
MLSPP_COMMIT=1cc50a124a3bc4e143a787ec934280dc70c1034d
DAVE_REPO=https://github.com/discord/libdave.git
DAVE_COMMIT=8de72b1f8a2ac3c5a5270755bb8091a62e3c6169 # tag v1.2.1/cpp

mkdir -p "$OUT"/{dl,src,build,bin,install/host,install/ps5}

fetch() { # url sha256 dest
    local url=$1 sha=$2 dest=$3
    if [[ ! -f $dest ]]; then
        curl -fsSL -o "$dest.part" "$url"
        mv "$dest.part" "$dest"
    fi
    echo "$sha  $dest" | sha256sum -c --quiet
}

clone() { # repo commit dir  (shallow fetch of one pinned commit)
    local repo=$1 commit=$2 dir=$3
    if [[ ! -d $dir/.git ]]; then
        git init -q "$dir"
        git -C "$dir" remote add origin "$repo"
        git -C "$dir" fetch -q --depth 1 origin "$commit"
        git -C "$dir" checkout -q FETCH_HEAD
    fi
    [[ $(git -C "$dir" rev-parse HEAD) == "$commit" ]] || { echo "wrong commit in $dir" >&2; exit 1; }
}

# The SDK wrappers call prospero-llvm-config --bindir, which needs an llvm-config.
# Fedora only ships llvm-config under llvm-devel, so use the same shim as the laptop repo.
install -m755 "$HERE/../llvm-config" "$OUT/bin/llvm-config"

ps5_env() {
    export PATH="$OUT/bin:$PATH"
    export PS5_PAYLOAD_SDK="$SDK"
    CC=$SDK/bin/prospero-clang
    CXX=$SDK/bin/prospero-clang++
    AR=$SDK/bin/prospero-ar
    RANLIB=$SDK/bin/prospero-ranlib
    export CC CXX AR RANLIB
}

host_env() {
    CC=clang
    CXX=clang++
    AR=llvm-ar
    RANLIB=llvm-ranlib
    export CC CXX AR RANLIB
}

setup_env() { # ps5|host
    if [[ $1 == ps5 ]]; then ps5_env; else host_env; fi
}

# ---- sources -------------------------------------------------------------
fetch_sources() {
    fetch "$OSSL_URL" "$OSSL_SHA256" "$OUT/dl/openssl-$OSSL_VER.tar.gz"
    if [[ ! -d $OUT/src/openssl-$OSSL_VER ]]; then
        tar -xzf "$OUT/dl/openssl-$OSSL_VER.tar.gz" -C "$OUT/src"
    fi
    fetch "$JSON_URL" "$JSON_SHA256" "$OUT/dl/json-3.11.3.tar.xz"
    if [[ ! -d $OUT/src/json ]]; then
        tar -xJf "$OUT/dl/json-3.11.3.tar.xz" -C "$OUT/src"
    fi
    clone "$MLSPP_REPO" "$MLSPP_COMMIT" "$OUT/src/mlspp"
    clone "$DAVE_REPO" "$DAVE_COMMIT" "$OUT/src/libdave"
}

# ---- OpenSSL (libcrypto + libssl, static) --------------------------------
# OpenSSL's Configure and its util/*.pl scripts need core Perl modules. The minimal perl on
# arlp-pc lacks several, and installing them needs approval, so stop with the exact command.
check_perl() {
    local m missing=()
    for m in FindBin IPC::Cmd Digest::SHA File::Compare File::Copy Test::More Pod::Html Time::Piece; do
        perl -M"$m" -e1 2>/dev/null || missing+=("$m")
    done
    if (( ${#missing[@]} )); then
        echo "missing perl modules: ${missing[*]}" >&2
        echo "install needed (Fedora): sudo dnf install perl-FindBin perl-IPC-Cmd perl-Digest-SHA perl-File-Compare perl-Test-Simple perl-Pod-Html perl-Time-Piece" >&2
        exit 1
    fi
}

build_openssl() { # ps5|host
    local t=$1 bld=$OUT/build/$1/openssl inst=$OUT/install/$1
    setup_env "$t"
    check_perl
    rm -rf "$bld"; mkdir -p "$(dirname "$bld")"
    cp -a "$OUT/src/openssl-$OSSL_VER" "$bld"   # OpenSSL builds in-tree; keep src pristine
    cd "$bld"
    local common=(no-shared no-tests no-apps no-docs no-ui-console no-dso --prefix="$inst" --libdir=lib)
    if [[ $t == ps5 ]]; then
        ./Configure BSD-x86_64 "${common[@]}" -fPIC
    else
        ./Configure linux-x86_64 "${common[@]}"
    fi
    make -j"$JOBS" build_libs
    make install_dev
}

# ---- cmake packages ------------------------------------------------------
cmake_common=()
cmake_setup() { # ps5|host
    local t=$1 inst=$OUT/install/$1
    cmake_common=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF
                  -DCMAKE_INSTALL_LIBDIR=lib
                  -DCMAKE_POSITION_INDEPENDENT_CODE=ON
                  -DCMAKE_INSTALL_PREFIX="$inst"
                  -DCMAKE_PREFIX_PATH="$inst" -DCMAKE_FIND_ROOT_PATH="$inst")
    if [[ $t == ps5 ]]; then
        cmake_common+=(-DCMAKE_TOOLCHAIN_FILE="$SDK/toolchain/prospero.cmake" -DPS5_PAYLOAD_SDK="$SDK")
    else
        cmake_common+=(-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++)
    fi
}

cmake_build() { # ps5|host src-dir name [extra cmake args...]
    local t=$1 src=$2 name=$3; shift 3
    local bld=$OUT/build/$t/$name
    rm -rf "$bld"
    cmake -S "$src" -B "$bld" "${cmake_common[@]}" "$@"
    cmake --build "$bld" -j"$JOBS"
    cmake --install "$bld"
}

build_cmake_stack() { # ps5|host
    local t=$1
    setup_env "$t"; cmake_setup "$t"
    # header-only json; its install provides nlohmann_jsonConfig.cmake
    cmake_build "$t" "$OUT/src/json" json -DJSON_BuildTests=OFF -DJSON_Install=ON
    # mlspp: same namespace and GREASE setting as libdave's vcpkg overlay
    cmake_build "$t" "$OUT/src/mlspp" mlspp -DDISABLE_GREASE=ON -DMLS_CXX_NAMESPACE=mlspp -DTESTING=OFF
    # libdave: C++17 static lib + dave.h C API
    cmake_build "$t" "$OUT/src/libdave/cpp" libdave -DTESTING=OFF
}

build_target() { # ps5|host
    build_openssl "$1"
    build_cmake_stack "$1"
    build_probe "$1"
}

# Links the C API smoke test against the static stack. PS5 output is linked only, never run.
build_probe() { # ps5|host
    local t=$1 inst=$OUT/install/$1 pdir=$OUT/probe/$1
    setup_env "$t"
    mkdir -p "$pdir"
    cp "$HERE/probe/dave_probe.c" "$pdir/"
    local libs=(-Wl,--start-group "$inst/lib/libdave.a" "$inst/lib/libmlspp.a"
                "$inst/lib/libmls_ds.a" "$inst/lib/libhpke.a" "$inst/lib/libtls_syntax.a"
                "$inst/lib/libbytes.a" "$inst/lib/libcrypto.a" -Wl,--end-group)
    "$CC" -I"$inst/include" -c "$pdir/dave_probe.c" -o "$pdir/dave_probe.o"
    if [[ $t == ps5 ]]; then
        # -nostdlib++ keeps the wrapper's libc++ out of the front of the link line;
        # the C++ runtime goes after the archives that need it.
        "$CXX" -nostdlib++ -o "$pdir/dave_probe.elf" "$pdir/dave_probe.o" "${libs[@]}" \
            -lc++ -lc++abi -lunwind -lpthread
    else
        "$CXX" -o "$pdir/dave_probe" "$pdir/dave_probe.o" "${libs[@]}" -lpthread
    fi
}

fetch_sources
case $WHAT in
    ps5)  build_target ps5 ;;
    host) build_target host ;;
    all)  build_target host; build_target ps5 ;;
    *) echo "unknown target $WHAT" >&2; exit 1 ;;
esac
echo "done: $OUT/install"
