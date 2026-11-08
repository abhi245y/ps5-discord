# discord-capture.elf

The helper payload of the wireless Go Live backend. It borrows the console's hardware
H.264 encoder (Avcap2) through calls made inside SceRemotePlay and sends the video, over a
loopback socket, to the core (`src/capture_bc.c`; protocol in `src/capture_proto.h`). It starts no
Remote Play client session, takes no controller and changes no display setting. The game's sound
is captured by the core itself (`src/bt/game_audio.cpp`).

It is a separate process so that a wedged remote call or a crash never takes the core down, a
crashed core never leaves SceRemotePlay ptrace-attached, and the GPL-3.0-only code below stays out
of the AGPL core binary.

## License

GPL-3.0-only (`LICENSE`). `src/inject.[ch]` derive from Glint
(MiladRTX/Glint, commit e555d237f40901814949dd6132891ea870b5460a, GPL-3.0) with the PoC's local
transport safety changes. `src/helper.c` follows the hardware-tested sequence of the encoder
proof of concept (ps5-encoder-poc, `tools/encoder-probe/capture_record.c`, same license). The
only file shared with the core is `src/capture_proto.h` (MIT).

## Shape

* `main`: a single-instance lock (`/data/ps5-discord/capture.lock`), then a fork. The parent is a
  supervisor with an idle watchdog: 15 s without progress asks the worker to clean up, 25 s
  kills only the worker. There is no total duration.
* The worker (2 MiB thread stack) reads `/data/ps5-discord/capture.conn` (port and token the core
  wrote, mode 0600), connects to the core on 127.0.0.1, presents the token, waits for
  `CAP_START`, then runs the PoC sequence: Initialize, wait for a stable output mode, own
  ResourceArbitrator callback, EnableScanInEncode, OpenVideo, bitrate (before Start, never
  changed), Start, real SPS/PPS, keyframe, read.
* Socket closed either way (or `CAP_STOP`): Stop, Close, free, ClearCallback, Terminate, scratch
  restored, detach, exit.
* Recovery (RECOVERY.md of the PoC): only video-read error 0x81950009; poll the output mode until
  three equal nonzero readings (100 ms, 5 s), clean teardown, one fresh session, SPS/PPS read
  again, IDR requested. A second failure ends with an error state.
* Mode guard: output mode 0x40 (the one Glint treats as 120 Hz) is refused, as in the PoC's
  baseline; no capture was ever shown to work in it. Mode 0 (a transition) is waited out.
* The encoder's SPS VUI may claim 60000/1001 at 720p30. It is not rewritten; see `SPS_TIMING_NOTE`
  in `src/helper.c`.

## Build and test (on arlp-pc)

    PS5_PAYLOAD_SDK=../.deps/ps5-payload-sdk make all
    make test CC=clang
