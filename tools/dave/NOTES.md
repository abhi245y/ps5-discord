# libdave (DAVE E2EE) for the PS5 payload: build notes

Status: PS5 and host static stacks built; host probe runs; PS5 probe linked (not run).
Build folder on arlp-pc: /home/abhiram/Documents/ps5-discord-voice-deps (out/install/{ps5,host}).

## Pins (sha256 checked by build.sh)
- OpenSSL 3.6.5 (latest 3.x tag): https://github.com/openssl/openssl/releases/download/openssl-3.6.5/openssl-3.6.5.tar.gz
  sha256 a2157c2830efdec3788939b00c9b0638306d3f0bbb76dc4832ee503bb397df98 (matches openssl.org .sha256)
- nlohmann/json 3.11.3: https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  sha256 d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
- mlspp commit 1cc50a124a3bc4e143a787ec934280dc70c1034d (the commit libdave's vcpkg overlay pins)
- libdave tag v1.2.1/cpp = commit 8de72b1f8a2ac3c5a5270755bb8091a62e3c6169

## Toolchain
- SDK wrappers call `prospero-llvm-config --bindir`, which needs an llvm-config on PATH. arlp-pc has
  none, so tools/dave/llvm-config (copy of tools/llvm-config) goes in $OUT/bin.
- build.sh needs these core Perl modules (checked up front): FindBin IPC::Cmd Digest::SHA
  File::Compare File::Copy Test::More Pod::Html Time::Piece. All are installed on arlp-pc now.
- CMake packages use lib/ (CMAKE_INSTALL_LIBDIR=lib); Fedora's default is lib64.

## Static library sizes (bytes)
| lib            | ps5       | host      |
|----------------|-----------|-----------|
| libcrypto.a    | 14,222,328| 11,818,456|
| libssl.a       |  2,529,684|  2,103,748|
| libmlspp.a     |  3,086,118|  2,329,334|
| libhpke.a      |  1,296,844|  1,206,268|
| libdave.a      |    935,180|    659,896|
| libmls_vectors.a| 1,111,100|    899,112|
| libmls_ds.a    |    242,130|    123,780|
| libtls_syntax.a|     21,468|     20,956|
| libbytes.a     |     31,980|     16,630|

Probe ELF: PS5 8,645,568 bytes (linked, not run). Host 6,746,816 (run).

## PS5 link line (verified on dave_probe.c)
    prospero-clang++ -nostdlib++ -o app.elf objs... \
        -Wl,--start-group libdave.a libmlspp.a libmls_ds.a libhpke.a libtls_syntax.a libbytes.a libcrypto.a -Wl,--end-group \
        -lc++ -lc++abi -lunwind -lpthread
- Use prospero-clang++ as the link driver. libc++ is needed by libdave.
- libssl.a is not referenced by libdave/mlspp; only libcrypto.a is needed.
- Compile with -I<install>/ps5/include (dave/dave.h is C-compatible).
- The PS5 probe was linked by hand with these exact flags (build.sh has the same code in build_probe, but
  the ps5 build ran before build_probe was added). Re-running `build.sh ... ps5` re-links it.

## Patches
- None to library source. Build-system changes only: CMAKE_INSTALL_LIBDIR=lib, and the llvm-config shim.

## Observed at runtime (host probe)
- daveSessionInit calls the MLS failure callback once: "GetPersistedKeyPair: Generic failure", then falls
  back to transient keys. Expected with PERSISTENT_KEYS off. Callers should not treat it as fatal.
- Bogus external sender (4 bytes) produces "Invalid variant type label" via the callback, not a crash.
- Key package: 393 bytes, first byte 0x00.

## Not yet verified
- The PS5 probe is not run (per rules).
- OpenSSL on PS5 uses asm (BSD-x86_64, no no-asm). Entropy source on PS5 libc not checked at runtime.
