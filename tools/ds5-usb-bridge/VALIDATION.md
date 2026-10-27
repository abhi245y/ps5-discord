# Packaging validation

Validated on arlp-pc on 2026-10-27 using the existing PS5 payload SDK. No tools or
packages were installed. Build directories were isolated from other workspaces.

- PS5 build: passed with `-std=gnu11 -Wall -Wextra -Werror -O2`.
- Artifact: 64-bit x86-64 FreeBSD ELF, dynamically linked, PIE.
- ASan/UBSan host tests: passed. Coverage includes controller button/axis decoding,
  stereo headphone output, unity speaker downmix, zero haptic channels, gain
  saturation, descriptor bounds/index mapping, single-byte TCP fragmentation,
  bounded stereo backlog, odd-byte partial mic writes, backpressure drops,
  physical mute PCM, socket EOF and SIGPIPE-safe disconnect.
- Clang static analyzer: passed with no diagnostics for `ds5_usb.c`,
  `pcm_stream.c` and `bridge_main.c`.
- Updated future-test loopback payload: rebuilt successfully, with its existing
  sanitizer suite passing after setting speaker volume `0x64` and unity gain.

Hardware evidence is in `evidence/` and is summarized in `INTEGRATION.md`.
The packaged bridge itself was not deployed after extraction. A real Discord
call and the newly raised speaker-loopback gain remain untested, per the user's
instruction to leave speaker testing for later.
