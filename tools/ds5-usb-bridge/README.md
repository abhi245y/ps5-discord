# DS5 USB integration payload

`ds5-usb-bridge.elf` packages direct USB controller input, simultaneous microphone
capture/playback, headphone handoff and physical mute handling for integration
with ps5-discord. It uses the core's existing local PCM socket; it has no LAN
audio listener and requires no new credentials or PSN authentication.

Start with [INTEGRATION.md](INTEGRATION.md), which records the API, core hooks,
hardware evidence and remaining integration work for Claude.

## Files

- `ds5_usb.c`, `ds5_usb.h`: reusable single-owner USB runtime and callback API.
- `ds5_usb_format.h`: descriptor validation and interface-index discovery.
- `ds5_usb_pcm.h`: controller decoding, selective route reports and PCM mapping.
- `pcm_stream.c`, `pcm_stream.h`: bounded nonblocking core audio adapter.
- `bridge_main.c`: standalone payload wrapper with loopback IPC and watchdog.
- `test_usb.c`: portable sanitizer tests.
- `evidence/`: diagnostic logs from successful controller/audio probes.

## Build

Build on the home PC in an isolated directory, per workspace instructions:

```sh
make PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk LLVM_CONFIG=/path/to/tools/llvm-config
make test HOST_CC=clang
make analyze PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk LLVM_CONFIG=/path/to/tools/llvm-config
```

Builds never deploy, record, or play audio. Default standalone duration is five
minutes, followed by USB cleanup. To change the build-time duration, force a
rebuild, for example `make -B DURATION_MS=60000` with the same SDK arguments.
Embedded callers control their own lifecycle through `ds5_usb_run` and
`ds5_usb_request_stop`.

## Runtime contract

- Connects to `127.0.0.1:9042/TCP` before taking USB ownership. Missing core means
  an error notification and exit without taking the controller.
- Sends 48 kHz mono signed 16-bit little-endian microphone PCM to the core.
- Receives 48 kHz stereo signed 16-bit little-endian playback PCM from the core.
- Sends each full 64-byte USB controller report as one UDP datagram to
  `127.0.0.1:9043`. The core/Remote Play receiver for that port is integration work.
- Default gains: mic `0x20`, headphones `0x7f` with half-amplitude PCM, speaker
  `0x64` with unity PCM, speaker pre-gain 2. Speaker configuration matches normal
  speaker playback. The louder speaker loopback profile has **not** been retested.
- Keeps the built-in controller mic selected even when a headset is plugged in.
- Microphone audio is not locally echoed to playback. Received call audio is
  independent of the microphone callback.
- Logs to `/data/ps5-discord-probe/ds5bridge.txt` and, when available,
  `/mnt/usb0/ds5-usb-bridge.log`.

The USB runtime was extracted from hardware-verified probes. The packaged bridge
has passed cross-compilation, host tests and static analysis; it has not been
deployed or tested in a Discord call. No further speaker test was run, as requested.

All new code is `AGPL-3.0-or-later`. See the project's root `LICENSE`. USB takeover
uses the approach already proven by the Ghostcontrol-derived controller probe;
USB audio and HID field definitions are documented in the integration handoff.
