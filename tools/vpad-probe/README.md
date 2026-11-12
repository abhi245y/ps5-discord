# Controller audio and virtual-pad probes

The native app's controller-audio worker shares its HID implementation with these
standalone diagnostic probes. Builds never deploy or join a Discord call.

## Controller audio

Settings > **Voice & audio** > **Controller audio** enables automatic USB DualSense
microphone and speaker capture during calls. It defaults to Off and is saved per
console user. Use a USB data cable and **Use USB Cable** communication mode.

The core sends bundled `assets/controller-audio.elf` to the console-local payload
loader when a call needs it. The worker:

1. Authenticates to `127.0.0.1:9045` using console-local `control.key`, and waits
   for the core's `OK` acknowledgement before touching USB.
2. Takes the physical USB controller, waits for input, then creates and binds its
   virtual replacement to the foreground console user.
3. Forwards real controls every 16 ms, sends microphone PCM to the core, and plays
   received call PCM through the controller. No microphone recording is saved.
4. Deletes the virtual pad before releasing USB when the core closes its PCM lease.
   It transitions through the unconfigured state, restores the saved configuration,
   and verifies HID driver attachment. Re-enumeration remains the fallback.

Disabling the setting, leaving the call, or quitting Discord releases the worker.
Remote Play temporarily reserves the controller and pauses automatic capture;
hardware-encoder Go Live does not reserve it. Press PS after release to wake the
physical controller; a USB replug is not normally needed.

USB PCM parks the native app's authenticated audio connection on 9042, draining
stale microphone samples while preserving S16 byte alignment. A restarted app may
replace the parked connection. The app resumes when USB releases ownership;
Bluetooth audio yields while USB owns the call. Concurrent USB owners and Remote
Play controller ownership are rejected before acknowledgement. The existing app
PCM protocol is unchanged. The key is never logged or copied to the host, and the
worker wipes its temporary key buffer.

On FW 13.00, console tests confirmed two-way Discord audio, automatic capture,
game input, PS control-center access, and physical handback after pressing PS.
Create, motion sensing, and touch coordinates are not forwarded; touchpad click
is supported. Other firmware, other titles, and the credential-retry path have
not been validated by these tests.

## Build and tests

Run builds on the designated build host, from the repository root:

```sh
make -C tools/vpad-probe service PS5_PAYLOAD_SDK=/absolute/path/to/ps5-payload-sdk
make controller-audio-test HOST_CC=clang
```

The native app's packaging script builds and requires `controller-audio.elf`.
Root `make test` includes USB report/queue sanitizer tests, PCM ownership and
engine socket tests, and per-user preference persistence tests.

Diagnostic targets are separate from the app-managed service:

| Target | Output | Purpose |
| --- | --- | --- |
| `all` | `vpad-probe.elf` | Scripted Right/Left, stick and L2 sequence; physical pad untouched |
| `game` | `vpad-game-probe.elf` | One Cross pulse to resume highlighted game, then scripted input |
| `usb` | `vpad-usb-probe.elf` | USB input forwarding and microphone counters; silent output |
| `audio` | `vpad-usb-audio-probe.elf` | USB forwarding with bounded 440/660/880 Hz speaker tones |
| `call` | `vpad-usb-call-probe.elf` | USB forwarding with authenticated live-call PCM |
| `restore` | `vpad-usb-restore-probe.elf` | Silent USB forwarding with configuration-based handback |
| `call-restore` | `vpad-usb-call-restore-probe.elf` | Live-call PCM with configuration-based handback |

Scripted probes leave the physical controller untouched. A physical pad already
active for the same user can produce an assignment prompt; test scripted variants
with it off. The `game` variant selects the highlighted item, so prepare that item
before running. USB variants forward only physical input and never run scripted
navigation. Call variants require an existing call in the app; they do not log in,
join a room, or enable Remote Play.

## Supervision and cleanup

The app worker runs for the call lifetime. A forked watchdog requests termination
after 23 seconds without main-loop progress and kills after 25 seconds without
progress. Healthy calls have no fixed time limit. Diagnostic variants retain a
23-second graceful/25-second hard deadline; USB forwarding ends at about 18 seconds.

Callbacks use bounded producer/consumer queues. Full queues drop new reports;
input older than 250 ms releases held controls, and a one-second input stall stops
forwarding. Missing MBus symbols or a failed bind prevent USB forwarding. MBus is
resolved with `dlopen`/`dlsym`; a failed bind gets at most one credential-assisted
retry, with saved credentials restored before further input or cleanup.

All variants share `/data/ps5-discord/vpad-probe.lock`, held through cleanup. The
lock inode stays on disk to prevent competing lock owners. Do not remove it while
a worker or probe runs. The core also checks this lock before starting or handing
USB to Remote Play.

Worker logs append to `/data/ps5-discord/controller-audio.log` and, when available,
`/mnt/usb0/controller-audio.log`. Diagnostic log names match their output basenames.
Each line records PID and elapsed time. Log failures stop input and trigger cleanup.
SIGINT/SIGTERM request cleanup; an idempotent `atexit` handler also deletes the
owned virtual pad and joins the USB thread before closing the lock.

A hard kill, failed delete, or process crash cannot guarantee virtual-pad removal,
USB handback, or restoration of temporary credentials. A released lock alone does
not prove cleanup succeeded. Reconnect USB if normal physical wake fails.

For assignment diagnostics, `/data/ps5-discord/vpad-probe-user` may contain an
existing console user's numeric ID, decimal or `0x` hex. Invalid or unreadable
overrides fail before creating a pad. Remove the file for foreground/initial-user
selection. This override does not sign in or create a console user.

## Reference layouts and credits

Independently written under [AGPL-3.0-or-later](../../LICENSE). ABI and routing
references are pinned here so checkout-local research files are not required:

- [FGGstore/FGG-XSense, `bfc5f9d`](https://github.com/FGGstore/FGG-XSense/tree/bfc5f9d324e2ca8b0a90fc498957699caa9fe5b0),
  GPL-3.0: `src/vpad.c` and `src/vpad.h` document HID ioctl request layouts,
  descriptor/report offsets, trigger pairing and identity quaternion.
- [AnyPad-PS5-English, `9475e9f`](https://github.com/S-Mohammad-A/AnyPad-PS5-English/tree/9475e9f88b7d06f890ca3bb3f16537bcc503eff4),
  GPL-3.0-or-later: `src/ps5_vpad.c` and `PRUEBAS.md` document user routing,
  MBus binding, SDK credential helpers and repeated held-state submission.
- [Ghostpad, `f6c7549`](https://github.com/srbraboo/Ghostpad/tree/f6c75494d00e4362052071b3fabe88c40e9b6203),
  GPL-3.0-or-later payload sources: `virtualDS5research.md` documents button
  masks, including PS5 PS bit `0x10000`. Its process injection, patching and
  physical-pad eviction are not used here.
- [FreeBSD 11 USB driver](https://github.com/freebsd/freebsd-src/blob/releng/11.0/sys/dev/usb/usb_generic.c):
  driver-detach and set-configuration behavior behind the verified handback path.

The 160-byte kernel report is not a `ScePadData` structure: buttons are at `+0x0c`,
unsigned sticks at `+0x10` (center 128), triggers at `+0x14/+0x15`, active at `+0x17`,
and quaternion W at `+0x84`. Native request sizes and pointer offsets are enforced
by static assertions; host tests check report encoding and concurrent queue use.
