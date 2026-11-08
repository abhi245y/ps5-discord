# discord-capture.elf

The helper payload of the wireless Go Live backend. It borrows the console's hardware
H.264 encoder (Avcap2) through calls made inside SceRemotePlay and sends the video, over a
loopback socket, to the core (`src/capture_bc.c`; protocol in `src/capture_proto.h`). It starts no
Remote Play client session, takes no controller and changes no display setting. The game's sound
is captured by the core itself (`src/bt/game_audio.cpp`).

It is a separate process to isolate capture failures from the core and keep the GPL-3.0-only
code below out of the AGPL core binary. Socket closure requests cleanup, including during setup.
The injector restoration/supervisor issue from review finding 1 is still open: its unfinished
work is preserved on `wip/capture-inject-restore` (`3334cda`). These reliability changes do not
make a wedged or killed injector safe; console deployment remains blocked on that work.

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
  restored, detach, exit. Setup polls STOP/EOF between calls and header retries and has a shared
  absolute startup deadline. A successful OpenVideo with an unreadable handle reports uncertain
  ownership and failed cleanup instead of claiming success.
* Recovery (RECOVERY.md of the PoC): only video-read error 0x81950009; poll the output mode until
  three equal nonzero readings (100 ms, 5 s), clean teardown, one fresh session, SPS/PPS read
  again, IDR requested. A second failure ends with an error state.
* Mode guard: output mode 0x40 (the one Glint treats as 120 Hz) is refused at startup and polled
  once a second while streaming. Other mode changes are logged while reads continue, preserving
  the validated recovery path when a transition precedes ReadVideo failure.
* The encoder's SPS VUI may advertise twice the capture rate. `src/h264_timing.c` normalizes
  existing progressive timing to 30 or 60 fps, including SPS repeated in access units. Other
  NAL bytes stay unchanged. Malformed SPS, missing timing and HRD are refused explicitly.
  Saved HDR-TV recordings decode to identical images before/after normalization: 242 frames
  at 720p30 and 482 at 1080p60, checked with FFmpeg/OpenH264. Decoded output uses 30/60 fps;
  Fedora's parser-only `ffprobe r_frame_rate` reports twice that even for FFmpeg's reference
  `h264_metadata=tick_rate=60:fixed_frame_rate_flag=1` output. Live RTP/Discord negotiation
  and A/V sync still require console acceptance.
* The core uses bounded nonblocking loader uploads and one command writer. Eight concurrent
  HELLO candidates have independent deadlines; rejections never exhaust a global session budget.
  Complete PROFILE, HEADERS and IDR are required before going live. Independent first-picture,
  progress and recovery deadlines in `src/capture_proto.h` bound an authenticated silent peer.

## Build and test (on arlp-pc)

    PS5_PAYLOAD_SDK=../.deps/ps5-payload-sdk make all
    make test CC=clang

Root `make all` builds this helper, root `make test` runs its sanitizer suites, and app packaging
rebuilds it and requires both ELF and license. Root `make clean` removes its build output too.
`make deploy` still sends only the core; update the packaged app assets to update the helper.
All verification runs on arlp-pc; nothing in these commands deploys to the console.
