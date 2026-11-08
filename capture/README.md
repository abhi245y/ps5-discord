# discord-capture.elf

The helper payload of the wireless Go Live backend. It borrows the console's hardware
H.264 encoder (Avcap2) through calls made inside SceRemotePlay and sends the video, over a
loopback socket, to the core (`src/capture_bc.c`; protocol in `src/capture_proto.h`). It starts no
Remote Play client session, takes no controller and changes no display setting. The game's sound
is captured by the core itself (`src/bt/game_audio.cpp`).

It is a separate process to isolate capture failures from the core and keep the GPL-3.0-only
code below out of the AGPL core binary. Socket closure requests cleanup, including during setup.
The supervisor owns tracing and the restoration journal. The worker requests whole injector
operations through a private inherited socketpair and a bounded shared transfer arena. A worker
crash or watchdog kill leaves the tracer and recovery state alive; failed restoration retains
ownership and retries instead of resuming the target with unverified state. Console acceptance
is still required before deployment.

## License

GPL-3.0-only (`LICENSE`). `src/inject.[ch]` derive from Glint
(MiladRTX/Glint, commit e555d237f40901814949dd6132891ea870b5460a, GPL-3.0) with the PoC's local
transport safety changes. `src/helper.c` follows the hardware-tested sequence of the encoder
proof of concept (ps5-encoder-poc, `tools/encoder-probe/capture_record.c`, same license). The
only file shared with the core is `src/capture_proto.h` (MIT).

## Shape

* `main`: a single-instance lock (`/data/ps5-discord/capture.lock`), then a fork. The parent is a
  supervisor with an idle watchdog: 15 s without progress asks the worker to clean up, 25 s
  kills only the worker. Supervisor and worker each use a 2 MiB thread stack. The supervisor
  reaps the worker before recovery, restores the pinned LWP registers, XSAVE x87/SSE/AVX state, return slot, trap bytes,
  scratch writes and page protection with readback checks, clears owned queued stops, then detaches. It keeps the lock and
  retries if any restoration fails. Observed target death is terminal. There is no total duration.
* The worker (2 MiB thread stack) reads `/data/ps5-discord/capture.conn` (port and token the core
  wrote, mode 0600), connects to the core on 127.0.0.1, presents the token, waits for
  `CAP_START`, then runs the PoC sequence: Initialize, wait for a stable output mode, own
  ResourceArbitrator callback, EnableScanInEncode, OpenVideo, bitrate (before Start, never
  changed), Start, real SPS/PPS, keyframe, read. Each injector operation executes entirely in
  the supervisor; the worker never becomes the target's tracer.
* Socket closed either way (or `CAP_STOP`): Stop, Close, free, ClearCallback, Terminate, scratch
  restored, detach, exit. Setup polls STOP/EOF between calls and header retries and has a shared
  absolute startup deadline. A successful OpenVideo with an unreadable handle reports uncertain
  ownership and failed cleanup instead of claiming success. Cleanup errors are reported to the
  core even after Stop, so the app can warn that a console restart may be needed. Original
  scratch bytes are saved on the first write to each range; unrelated bytes are never replayed
  from an old whole-page snapshot. Temporary tracing credentials are journaled and verified too.
* Recovery (RECOVERY.md of the PoC): only video-read error 0x81950009; poll the output mode until
  three equal nonzero readings (100 ms, 5 s), clean teardown, one fresh session, SPS/PPS read
  again, IDR requested. A second failure ends with an error state.
* Mode guard: output mode 0x40 (the one Glint treats as 120 Hz) is refused at startup and polled
  once a second while streaming. Up to two API query misses are tolerated; transport failures
  remain fatal. Other mode changes are logged while reads continue, preserving
  the validated recovery path when a transition precedes ReadVideo failure.
* The encoder's SPS VUI may advertise twice the capture rate. `src/h264_timing.c` normalizes
  existing progressive timing to 30 or 60 fps, including SPS repeated in access units. Other
  NAL bytes stay unchanged. Malformed SPS, missing timing and HRD are refused explicitly.
  Tests include SPS/PPS fixtures for both validated profiles. Saved HDR-TV recordings decode to identical images before/after normalization: 242 frames
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
All verification runs on arlp-pc; nothing in these commands deploys to the console. Hardware
acceptance still includes a 1080p60 session, a 30-minute mode-polling soak, a display-mode change,
Stop followed by restart, and confirmation that elfldr discards a reset/truncated upload.

## Recovery boundary and host evidence

`make test` includes the real injector and broker against a fake FreeBSD kernel. Cases cover
partial/failed installs, successful-but-ineffective restoration, stop/trap races, target exit,
credential rollback failures, two-LWP stop switching, asynchronous-signal requeue, AVX state,
strict held-state retry, malformed requests and real forked workers
exiting before/during/after RPCs. No test performs real ptrace or accesses a console.

Worker-crash recovery restores borrowed CPU/memory state. It cannot roll back an interrupted
encoder/library function or prove Stop/Close/Terminate ran after a forced kill; logs explicitly
state that service cleanup is unconfirmed and a console restart may be needed. The supervisor
itself must remain alive: killing it, kernel failure or machine loss is outside this recovery
contract. Do not use a blanket payload killer on a supervisor retaining an attached target.

Scratch is still a borrowed writable range from the validated capture design. Limiting cleanup
to written bytes avoids clobbering unrelated live data, but exclusive use of those exact ranges
remains a hardware assumption. Moving scratch to a private remote allocation is separate work
requiring console validation. Native FreeBSD register/signal behavior and the full capture flow
also need a controlled console smoke test before release.

Register operations use the actual LWP ID returned by PT_LWPINFO, never the process's changing
reporting-thread alias. Register restoration additionally requires that LWP itself to report a
stop: another thread stopping the process does not prove an injected syscall has returned.
Owned stops target the borrowed LWP. A different reporting thread's original signal is forwarded
in-kernel while the supervisor waits boundedly for the borrowed LWP's own boundary.
Even that LWP can report a stop from inside an interruptible kernel sleep. A stop whose RIP
immediately follows SYSCALL or INT 0x80 is resumed and interrupted again, with the targeted stop
posted after resume. Registers are borrowed/restored only after leaving that ambiguous boundary;
a deadline or failed instruction read retains the tracer while owned state remains. This also
applies during attach; a failed capability check releases a clean target without reading code.
The acquisition deadline is checked on every iteration, including already-ready stops.

A verified aborted call permits explicit cleanup RPCs while ordinary capture calls remain
quarantined. Plain asynchronous signals on the borrowed LWP are requeued after cleanup; queued,
timer, AIO and other kernel notification payloads keep the original kernel stop and are delivered
by detach with the same signal, preserving siginfo. This latter path cannot make more remote calls before
detach, so encoder cleanup is unconfirmed. Missing signal information retains the tracer until
a retry can classify that same stop, with retry logs limited to one per 30 seconds.
XSAVE must be available with a bounded x87/SSE/AVX
layout; unsupported layouts fail before call mutation. These checks need console confirmation.
If a payload-bearing signal coincides with an ambiguous syscall boundary while owned state
remains, recovery holds rather than consuming its kernel siginfo or restoring registers before
syscall return. That stop cannot advance during retries, so this liveness limit may require a
console restart; the host test verifies persistent ownership rather than claiming recovery.
On the console the opcode read at a stopped system thread fails (execute-only code, seen
2026-11-09). With nothing of ours installed the stop is then borrowed unchecked, as the PoC did;
a stop on our own trap needs no read. A call that hangs inside the target still needs the read
to restore, so it holds the tracer: that case may require a console restart.
Once the journal and scratch are clean, detach requires only a collected stop
and the original signal metadata, without a syscall-boundary check or process resume.
