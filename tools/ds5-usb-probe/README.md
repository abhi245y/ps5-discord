# DualSense USB probes

Five separate, manually launched PS5 payloads. `make` only builds; it never deploys
or records anything. No PSN authentication is involved.

- `ds5-usb-probe.elf`: original 30-second controller-input probe from
  `feat/ds5-usb-probe`, unchanged. Its USB driver takeover approach came from
  Ghostcontrol's `payload/gc_main.c` (GPL-3.0-or-later). Earlier console tests
  captured approximately 500 reports/second without errors. Press PS after the
  probe ends to reconnect; re-enumeration alone did not reconnect in those tests.
- `ds5-mic-probe.elf`: USB PCM capture **verified on hardware**. Launching it records
  up to 30 seconds from the controller's USB audio input into a WAV file, with
  console notifications when recording starts and when the recording is saved. It does
  not forward audio to Discord or Remote Play.
- `ds5-speaker-probe.elf`: eight-second built-in speaker test, with four half-second
  beeps separated by silence and start/end notifications. Audible output was
  confirmed on the console. Haptic audio channels remain zero.
- `ds5-music-probe.elf`: file playback through the same built-in speaker route,
  using 48 kHz mono S16_LE PCM from `/data/ps5-discord-probe/song-mono.raw`.
- `ds5-loopback-probe.elf`: 60-second simultaneous mic capture/playback test,
  with automatic headphone/speaker handoff, mute-button handling and PS-to-stop.

## Console result (2026-10-27)

DualSense connected directly to the PS5 at `/dev/ugen2.2`, VID:PID `054c:0ce6`.
The probe selected interface 2, alternate 1, endpoint `0x82`, and detached the
driver attached to AudioControl interface 0. Both queued transfers opened.

- 625 completed transfers in 10,004 ms; zero empty USB frames.
- 480,000 stereo PCM frames, exactly 10 seconds at 48 kHz.
- WAV contains 1,920,000 PCM bytes plus its 44-byte header.
- L RMS 113.3 / peak 4073; R RMS 262.5 / peak 4840 (signed 16-bit scale).
- Alternate-setting cleanup and re-enumeration completed; result 0.
- No fatal-signal or panic lines in the collected kernel log.

These results establish non-silent USB PCM capture; the user also confirmed
audible playback. The recording remains local in the gitignored
`tmp/ds5-mic-test/` directory; no audio is included in the repository.
Full diagnostic output: [results-mic-usb.txt](results-mic-usb.txt).

A second run tested the extended 30-second capture and console notifications:
1,874 transfers in 30,006 ms, zero empty USB frames, 1,439,232 stereo PCM frames
(29.984 seconds), and successful cleanup. Both start and end notification calls
returned 0. The final transfer boundary accounts for the slightly shorter PCM
duration. Recording: `tmp/ds5-mic-test-30s/ds5mic.wav` (local only).
The user confirmed that this extended capture works flawlessly.
Diagnostic output: [results-mic-usb-30s.txt](results-mic-usb-30s.txt).

## Build and tests

Use the home PC, per workspace instructions, in an isolated feature directory.
Point `PS5_PAYLOAD_SDK` at the existing payload SDK and `LLVM_CONFIG` at the
project's `tools/llvm-config` shim if necessary:

```sh
make PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk LLVM_CONFIG=/path/to/tools/llvm-config
make test CC=clang
```

The host test runs descriptor parsing and WAV-header checks under ASan/UBSan,
including truncated descriptors, unsupported formats, non-contiguous interface
numbers/alternate settings, and every single-byte mutation of its fixture.
These checks do not prove that the PS5 kernel supports isochronous USB_FS capture.

## Mic hardware test

1. Plug the DualSense into the PS5 with a data-capable USB cable. Start at the
   home screen, without another USB probe or Remote Play session running.
2. Unmute the controller if you want audible input. The probe leaves hardware
   mute, volume and HID output settings unchanged.
3. Manually send `ds5-mic-probe.elf` through your existing payload loader. Speak
   or tap near the microphone for 30 seconds after the start notification.
4. Wait for `done` in the log. Press PS afterward if the controller remains
   disconnected; unplug/replug its USB cable if necessary.
5. Fetch `/data/ps5-discord-probe/ds5mic.wav` and `ds5mic.txt` using your existing
   file-transfer setup. Listen to the WAV and compare L/R RMS and peak values.

Output files:

- `/data/ps5-discord-probe/ds5mic.wav`: 48 kHz, stereo, signed 16-bit little-endian
  PCM in a standard WAV container. Maximum 5,760,044 bytes. A previous WAV is
  removed at launch so a failed test cannot be mistaken for a fresh recording.
- `/data/ps5-discord-probe/ds5mic.txt`: descriptors, selected endpoint, transfer
  geometry, channel levels and cleanup results. Replaced on each launch.
- `/mnt/usb0/ds5-mic-probe.log`: the same log, appended when a USB stick is mounted
  there, so runs are distinguishable from other `payload.elf` processes.

## Implementation and expected limits

The mic probe selects a UAC1 Type I PCM input from the device's active USB
configuration. Only the known DualSense format is accepted: a single discrete
48 kHz rate, two channels and 16-bit samples. Interface and alternate-setting
**indices** are derived from descriptor order instead of assuming that they
equal USB interface numbers.

It detaches the mic streaming interface and, when a driver is attached there,
its descriptor-associated AudioControl parent. This can pause all controller USB
audio, including playback. It does not detach the HID interface. Cleanup
re-enumerates the whole controller to let system drivers attach again, which can
briefly disconnect controller input too.

Two USB_FS transfers keep the isochronous input queued, each with its own frame
buffers and lengths. Completion uses the returned transfer index and actual
frame count. Requested lengths are reset before resubmission. PCM is held in a
bounded 5.76 MB buffer and saved after USB cleanup, keeping disk writes out of the
audio capture loop. Missing USB frames are omitted; the WAV duration is the
amount actually received, not necessarily the full wall-clock capture interval.

Ordinary failures stop/close transfers, uninitialize USB_FS, restore the original
alternate setting, and re-enumerate after any successful detach. Partial PCM is
saved with a valid header and the run reports failure. No received PCM produces
no WAV. A successful transfer containing silence is not proof of working mic
activation: verify audible speech, and check mute state if both channels are flat.

A watchdog requests graceful termination after 43 seconds and kills/reaps a
stalled worker after 45 seconds, allowing the requested 30-second recording plus
setup and cleanup time. A forced kill cannot restore USB settings or
save buffered audio; reconnect the controller if that happens.

API references: the SDK's FreeBSD 11.4 `dev/usb/usb_ioctl.h` and
[FreeBSD's USB_FS implementation](https://github.com/freebsd/freebsd-src/blob/releng/11.4/sys/dev/usb/usb_generic.c).
The known interface/format is corroborated by a
[DualSense USB descriptor capture](https://github.com/nondebug/dualsense/blob/main/lsusb-descriptor-info.txt),
but endpoint selection always uses descriptors read from the connected device.

## Built-in speaker test

Keep the DualSense plugged directly into the PS5 and send
`ds5-speaker-probe.elf` through the existing payload loader. Listen for four
half-second beeps (440, 660, 880, 660 Hz), two seconds apart. The probe sends
exactly eight seconds of 48 kHz, four-channel S16_LE PCM to the descriptor-selected
audio OUT endpoint. Channel 1 carries the tone; channel 0 and both haptic channels
are zero. Ten-millisecond ramps avoid abrupt edges.

It temporarily owns the audio collection and HID interface so system HID reports
cannot overwrite the test's speaker route. Controller input pauses until cleanup.
Selective HID report `0x02` sets the built-in speaker route to `0x30`, speaker
volume to `0x50`, and speaker pre-gain to 2. It does not set microphone-volume or
power-save/mute validity flags. Routing and volume fields follow
[dualsensectl's output report definitions](https://github.com/nowrep/dualsensectl/blob/main/main.c).
These temporary hardware values are distinct from the PS5 settings UI; reconnect
the controller with PS after the run before checking system preferences.

The initial audible test used peak PCM amplitude 1500. The louder version uses
6000 (four times the signal amplitude, about +12 dB), with the same speaker-volume
setting. Both values are below signed 16-bit full scale. Generated samples are
tested for amplitude bounds, silent intervals and zero haptic channels.

Cleanup closes all transfers, restores the audio alternate setting and
re-enumerates the controller. The same 25-second watchdog discipline as the
original bounded probe applies (graceful termination at 23 seconds). A forced
kill requires reconnecting the controller. No microphone recording runs during
this test; simultaneous mic/speaker operation is not yet tested.

Logs: `/data/ps5-discord-probe/ds5speaker.txt` and, when available,
`/mnt/usb0/ds5-speaker-probe.log`. The known controller layout uses interface 1,
alternate 1, endpoint `0x01`, 392-byte maximum packets, and high-speed interval 4
(1 ms). The probe sends 384 bytes per packet (48 sample frames), not the maximum
packet size. PS5 rejects `USB_GET_DEVICEINFO` with `ENOTTY`, so this diagnostic
query is omitted; the 1 ms cadence is verified by actual mic and speaker runs.

First audible result: 500 successful audio transfers, 384,000 sample frames and
8,000 ms wall time; HID speaker routing, notifications and cleanup succeeded.
The user confirmed all four beeps came from the built-in speaker.
Log: [results-speaker-usb.txt](results-speaker-usb.txt).

The user also confirmed playback of the louder version: the same 500 audio
transfers and 384,000 sample frames completed in 8,002 ms, result 0.
Log: [results-speaker-usb-louder.txt](results-speaker-usb-louder.txt).

## Vocal music playback

`ds5-music-probe.elf` builds the speaker source with `DS5_PLAY_SONG`. It reads
`/data/ps5-discord-probe/song-mono.raw`, a headerless 48 kHz mono S16_LE file,
and places its samples in output channel 1. Other audio and haptic channels stay
zero. The source must be a nonempty regular file, an even number of bytes, and
at most five minutes long. A final partial transfer is padded with silence.
The payload validates the source before taking any USB interfaces.

Music mode uses speaker volume `0x64` (maximum), with speaker pre-gain still 2.
It streams from disk with small buffers, not a whole-song allocation. Playback
timeout and watchdog are based on the validated source length: playback allows
four seconds of headroom; the watchdog allows 15 seconds for setup and cleanup,
then kills a stalled worker. Start/end notifications and controller handback work
the same way as the tone probe. The parent closes its inherited copy of the
source file after forking; only the worker reads it.

The selected vocal test recording is
[Auld Lang Syne, sung by Frank C. Stanley in 1910](https://commons.wikimedia.org/wiki/File:Auld_Lang_Syne.ogg),
marked public domain on Wikimedia Commons. It lasts 141.546 seconds. Original
Ogg audio, conversion metadata and converted PCM stay in the gitignored
`tmp/ds5-song/` directory; no song audio is included in the repository.

Conversion used:

```sh
ffmpeg -i auld-lang-syne-1910.ogg -ac 1 -ar 48000 \
  -af 'loudnorm=I=-18:TP=-3:LRA=7' -c:a pcm_s16le -f s16le song-mono.raw
```

Upload the PCM using the existing console file-transfer service, then manually
send `ds5-music-probe.elf` through the payload loader. The current music-mode
notification names this selected recording. Building never downloads, deploys
or starts playback automatically.

During vocal playback the user confirmed clear voice from the built-in speaker.
Inserting headphones did not move audio to the headphones: the speaker continued
playing, at a higher perceived volume. Unplugging headphones returned it to its
previous volume. Speaker-only route `0x30` is forced and the system audio/HID
drivers are detached, so automatic headphone routing is not exercised by this
probe. The payload sends its route/volume report once before playback and does
not change those fields during the song. The cause of the insertion-dependent
volume change is not established; a controller gain/path change is a hypothesis.
Future headset tests should measure this behavior and explicitly select the
desired output route instead of assuming system jack handling remains active.

The full song completed successfully: 8,847 audio transfers, 6,794,496 PCM frames
(141.552 seconds including final padding), speaker volume `0x64`, successful
start/end notifications, re-enumeration result 0, and final result 0.
Log: [results-speaker-music.txt](results-speaker-music.txt).

## Full-duplex loopback and headphone handoff

Keep the DS5 on USB, start with headphones plugged into its 3.5 mm jack, and send
`ds5-loopback-probe.elf`. Speak into the controller's built-in mic. During the
60-second run, unplug headphones at the 10-second console prompt and reconnect
them at the 30-second prompt. The output should
follow the jack, with a notification for each confirmed route change. The
controller's mute button toggles mic mute; PS stops the test early. If the mic
was already muted at startup, it stays muted until the user toggles it.

The probe owns six USB_FS transfers: two mic inputs, two quad-channel audio
outputs, HID input for jack/mute/PS events, and HID output for route updates.
It selects both audio alternates before USB_FS_INIT and restores both after
closing all transfers. All control/audio work uses one completion loop, dispatching
the endpoint index returned by USB_FS_COMPLETE; route writes never block waiting
on a separate completion consumer. A 75-second watchdog bounds the longer run
(graceful termination at 73 seconds).

The stereo mic stream is downmixed to mono and queued in an 8,192-sample ring.
A silence prefill maintains a short buffer reserve; route/mute changes clear it
to avoid stale voice. Current output gain is 1/2 for headphones and unity for the
controller speaker, with headphone output bounded to +/-16384 and speaker output
using the full signed 16-bit range. The headphone path retains
enough headroom for full-scale input without the first version's limiter clipping.
Headphones receive the mono signal on both channels;
speaker mode uses only the right audio channel. Haptic channels stay zero.
This test has no echo cancellation. Speaker gain was deliberately low in the
hardware tests; the user requested normal speaker playback gain afterward and
deferred its next test. The current louder speaker loopback is untested.

Current HID controls: internal mic forced on both output routes, mic volume
`0x20`, headphone volume `0x7f`, speaker volume `0x64`, speaker pre-gain 2.
Mute state and LED are updated together. Headphone detection uses USB input
report `0x01`, byte 54 bit 0; mute status is bit 2. Output changes wait for 100 ms
of stable jack state. These report fields follow
[Sony's Linux hid-playstation driver](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-playstation.c)
and [dualsensectl](https://github.com/nowrep/dualsensectl/blob/main/main.c).

Logs are `/data/ps5-discord-probe/ds5loopback.txt` and, when available,
`/mnt/usb0/ds5-loopback-probe.log`. The probe logs raw levels, full-scale input
samples, route changes, transfer totals and ring underruns/overruns. It does not
save microphone audio or transmit it over a network.

First console result: 960,000 stereo input frames and 960,000 output frames in
20,001 ms; 1,250 transfers in each direction, 9,977 HID reports, zero ring
underruns/overruns, clean re-enumeration and result 0. The user heard their voice
in headphones, establishing simultaneous capture and playback, but reported
poor clarity. The original mic volume was `0x40` and headphone gain was 1/4;
input reached full scale and output reached its +/-6000 limiter. Mic gain and
playback gain were then reduced to the values above. Headphones stayed connected
throughout this first run, so it did not test jack handoff.
Log: [results-loopback-initial.txt](results-loopback-initial.txt).

The next 20-second run used mic volume `0x20` and headphone gain 1/8. Input RMS
fell from 10,508.2 to 4,562.9, output peak stayed at 4096, and both ring counters
remained zero. 657 stereo input frames reached full scale. The user reported
clearer voice but very low playback volume; headphones again stayed connected.
The subsequent 60-second version therefore kept mic gain at `0x20`, raised
headphone hardware volume to `0x7f`, and changed headphone playback gain to 1/2
with a larger output ceiling. Speaker gain remained conservative.
Log: [results-loopback-low-gain.txt](results-loopback-low-gain.txt).

This establishes a local audio path only. Discord transport, speech processing,
echo cancellation and controller input forwarding still require integration.

The 60-second run completed with 2,879,208 input frames, 2,879,232 output frames,
3,749 transfers each way, 29,919 HID reports and four output handoffs. Ring
underruns/overruns remained zero and cleanup succeeded. The user reported good
headphone audio; the tested speaker gain (1/16, hardware volume `0x50`) was too
quiet. At the user's request, speaker volume was subsequently set to `0x64`
with unity PCM, matching normal speaker/music playback, with testing left for
later. Log: [results-loopback-60s.txt](results-loopback-60s.txt).

The reusable packaging is now in `../ds5-usb-bridge/`: USB callback API,
standalone core-PCM bridge payload and `INTEGRATION.md` handoff for Claude.
No packaged-payload console test or louder-speaker retest was performed.
