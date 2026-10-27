# DS5 USB handoff for Claude

## Objective and current status

Integrate one USB owner into the main Discord/Remote Play payload so a DualSense
plugged directly into the PS5 provides controller input, built-in microphone
capture, headphone or speaker playback, automatic output handoff and mute control.
The user requested packaging for this integration and deferred further speaker
testing. No new core deployment or Discord call was performed during packaging.

The standalone artifact is `ds5-usb-bridge.elf`. The reusable API is
`ds5_usb.h` (API version 1). The existing main core source was left unchanged.

Hardware tests on the connected DualSense `054c:0ce6` established:

- USB controller input: all tested buttons/sticks/triggers, about 500 reports/s.
- Microphone capture: 48 kHz stereo S16_LE USB audio; audible recordings confirmed.
- Speaker playback: tones and a full 141.552-second public-domain vocal recording.
- Duplex loopback: 60 seconds, 2,879,208 input frames and 2,879,232 output frames,
  3,749 transfers each direction, 29,919 HID reports, four output handoffs, no ring
  underruns or overruns, and clean re-enumeration.
- Headphone playback at mic volume `0x20`, headphone volume `0x7f` and PCM gain
  1/2 was reported good. The speaker was too quiet at the tested 1/16 loopback
  gain. The user then requested normal speaker-playback settings and a later test.

The current speaker configuration is therefore volume `0x64`, pre-gain 2 and
unity PCM, matching the successful normal speaker/music path. This profile has
not been retested as microphone loopback. Treat that as pending validation.

`evidence/` contains logs, not user audio or credentials. Probe source and older
recordings remain in the project under `tools/ds5-usb-probe/` and gitignored `tmp/`.

## Integration paths

### Recommended: embed the USB runtime

Compile `ds5_usb.c` with its three headers into the core. Run `ds5_usb_run` on a
dedicated worker thread. Use callbacks to exchange PCM with the voice engine and
feed controller state to the Remote Play session. All callbacks run on the USB
thread; copy borrowed data into bounded queues and return immediately.

The core already defines its PCM contract in `src/voice_audio.h` and
`src/voice_audio.c`: 48 kHz mono capture, 48 kHz stereo playback, 20 ms voice frames.
USB arrives in roughly 16 ms grains. Do not assume every microphone callback has
exactly 768 frames: asynchronous USB packets can carry 47/48/49 samples, and the
maximum callback is 784 mono frames. Accumulate 960-sample frames for Opus as the
existing audio engine already does.

Public callback semantics:

| Callback | Meaning |
| --- | --- |
| `controller(user, state, report)` | Decoded buttons, axes and triggers plus complete raw 64-byte USB report; about 500/s. |
| `microphone(user, mono, frames, muted)` | Downmixed mono S16 PCM, already zeroed when physically muted. Copy it before returning. |
| `playback(user, stereo, frames)` | Supply up to the requested number of stereo S16 frames; return frames supplied, not sample count. Missing frames become silence. |
| `route(user, headphones, mic_muted)` | A route/mute HID report completed successfully. Initial state also generates this callback. |
| `service(user)` | Optional bounded nonblocking IPC/control work: 0 continue, 1 stop normally, -1 fail and clean up. |
| `log(user, message)` | Route diagnostics to the core logger; message storage is borrowed. |

Initialize configuration with `ds5_usb_default_config`. For an embedded call,
set `duration_ms = 0`, supply your call lifecycle/stop handling and provide outer
supervision for USB ioctls. Call `ds5_usb_request_stop` on shutdown and join the
worker before destroying callback queues or the Remote Play session. A finite
duration is supported up to one hour. The standalone wrapper supplies its own
fork watchdog; the library itself never forks.

Only one `ds5_usb_run` can own the runtime in a process; concurrent attempts return
`DS5_USB_BUSY`. It can be run again after cleanup. PS is forwarded by default;
`stop_on_ps` is off. Enabling that option is for standalone tests, not gameplay.

### Alternative: use the standalone bridge payload

Start the existing core, then launch `ds5-usb-bridge.elf`. It connects to the
already implemented `127.0.0.1:9042/TCP` raw PCM contract before taking USB:

- Payload to core: mono 48 kHz S16_LE microphone bytes, no headers.
- Core to payload: stereo interleaved 48 kHz S16_LE playback bytes, no headers.

The core only supports one PCM client and replaces the previous one. Ensure the
app-side audio client or the older LAN bridge does not keep reconnecting and
replacing this payload. The older `feat/voice-bridge` LAN adapter on port 9044 is
not required for direct USB audio.

The adapter keeps a 60 ms playback prefill and at most 200 ms of queued audio,
dropping oldest complete stereo frames when behind. It preserves 1-3 byte TCP
fragments until a whole stereo frame arrives. Microphone transmission keeps at
most one USB grain pending: it drops new grains under backpressure, never a
partially transmitted sample, and never blocks the USB thread. EOF or a socket
error releases the USB device; the wrapper does not reconnect automatically.

Standalone controller reports go to `127.0.0.1:9043/UDP`: exactly one native
64-byte USB report per datagram. This is a new optional integration endpoint;
the main core currently has no receiver for it. Add a loopback-only receiver
and use `ds5_usb_parse_controller` or the raw report to feed Remote Play.
Without a controller consumer, the standalone bridge does **not** preserve
game control while it owns the controller. The direct callback path avoids UDP.

The standalone build runs at most five minutes by default and requests graceful
termination after duration + 13 seconds, then kills/reaps a stuck worker after
duration + 15 seconds. `DURATION_MS` changes this finite build-time limit. It does
not start a call, join a channel, perform login or read Discord credentials.

## Remote Play/controller hook

The already working controller conversion is in `tmp/rp-session/rp_session.c`,
function `to_chiaki`, and its USB reader is `pad_thread`. Replace that separate
USB reader with this module's controller callback; two independent USB owners
must not compete for the HID interface.

The callback provides the original report, so the known conversion can be reused
without changing its input. Call `chiaki_session_set_controller_state` only while
the session is live, and synchronize its lifetime against the USB worker.
Do not cast the new `ds5_usb_controller.buttons` mask directly to Chiaki's mask:
d-pad, Create/Options and PS/touchpad bit positions differ. Axes and trigger values
have the same ranges as the proven conversion. The raw report also preserves
motion and touch-position data; the decoded structure currently covers buttons,
stick axes, triggers and touchpad click only.

## USB ownership and audio details

- Descriptor-selected UAC1 PCM: mic is stereo S16_LE at 48 kHz; playback is
  quad-channel S16_LE at 48 kHz. The tested layout is interfaces 2/1, alt 1,
  mic endpoint `0x82`, output endpoint `0x01`, HID IN `0x84`, HID OUT `0x03`.
- AudioControl is located through its collection descriptor. Interface/alternate
  **indices** are derived from descriptor order, rather than assumed equal to
  USB-visible numbers. The tested device uses the usual 1 ms high-speed cadence.
- Takes the audio collection and HID driver, sets both audio alternates, then
  initializes six USB_FS transfer slots. Completes/resubmits all endpoints on one
  thread using the returned completion index.
- Requested frame lengths are reset on every resubmission. Mic input uses the
  kernel-reported actual frame lengths. Output uses 384 bytes/ms: 48 frames,
  four channels, 16 bits. Maximum endpoint packet size is 392, not the PCM payload.
- Output channels 0/1 carry audio. Channels 2/3 are haptics and remain zero.
  Headphones receive original stereo; speaker output is `(left + right) / 2` on
  the right audio channel only. Headphone and speaker PCM gains are configurable
  Q15 values, with saturation at signed 16-bit limits.
- Headphone detect: USB report `0x01`, byte 54 bit 0. Changes wait for 100 ms of
  stable state, then a selective report `0x02` selects route 0 (headphones) or
  `0x30` (speaker). The forced internal mic bit stays set on both routes.
- Mic mute is initially taken from byte 54 bit 2. The mute-button edge is byte 10
  bit 2; it toggles hardware mute and its LED. Physical mute also zeros the PCM
  callback. Incoming call audio continues playing while the local mic is muted.
- The built-in mic remains selected when a headset with a mic is connected.
  Headset-mic selection is a separate, untested feature.
- Cleanup stops/closes every transfer, uninitializes USB_FS, restores both old
  audio alternates and re-enumerates. Afterward the user may need to press PS.
  A forced kill cannot perform that restoration; reconnect the controller.

The older statement in `docs/voice-audio.md` that only the app can supply audio
remains true for the tested `sceAudioIn` path, but is not true for direct USB:
these probes established payload microphone and speaker access through ugen.

HID field references:
[Sony's Linux hid-playstation driver](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-playstation.c),
[dualsensectl](https://github.com/nowrep/dualsensectl/blob/main/main.c).
USB API reference: the SDK's FreeBSD 11.4 `dev/usb/usb_ioctl.h` and
[FreeBSD USB_FS implementation](https://github.com/freebsd/freebsd-src/blob/releng/11.4/sys/dev/usb/usb_generic.c).

## Remaining core work

1. Choose the in-process callback path or standalone PCM/UDP adapter, with one
   owner for USB and one PCM client. Preserve controller forwarding throughout
   the Remote Play session, including PS-menu navigation.
2. Start/stop capture with the voice-call lifecycle. Combine hardware mute with
   Discord self-mute/deafen; a physical unmute must not override software mute.
   Reflect hardware mute in UI as appropriate.
3. Test an actual Discord call using the existing Opus/DAVE engine. The packaging
   work did not join or transmit to a Discord call.
4. Validate the requested louder speaker profile later. Speakerphone use needs
   echo cancellation/noise processing; this backend does not implement AEC.
5. Add input/controller liveness to the core's status view, call lifecycle cleanup
   and user instructions to press PS/reconnect after releasing ownership.

## Verification boundary

Hardware logs establish the original controller, mic, speaker, duplex and handoff
paths. The extracted module and standalone bridge were cross-compiled with
`-Wall -Wextra -Werror`, tested under ASan/UBSan for PCM framing/backpressure,
controller decoding, mute and output mapping, and checked with Clang's static
analyzer. They have **not** been deployed after extraction. In particular,
controller forwarding into the live core and real Discord call audio remain
integration tests. Do not describe the bundle as a completed Discord call feature.
