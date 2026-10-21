# sceAudioIn (microphone) probe

Question: can ps5-discord capture the DualSense mic, or a headset plugged into
the controller, from (a) a payload or (b) the homebrew app? Where can it play
voice audio back?

Nothing here is deployed. The probe ELF was built on arlp-pc and copied back;
you run it yourself.

## Results (2026-10-21, firmware 13.00, DualSense built-in mic)

- **Payload: no.** Every `sceAudioInOpen` is refused before any read:
  foreground user, types 0-5 -> `0x809b0081`; system user 0xff -> `0x809b0001`;
  types 6-7 -> `0x80260104` (invalid type). The `0x809b` family is the same
  "no app identity" refusal the Spot5 work hit for payload audio output.
  Full log: `results-payload-mic.txt`.
- **App: yes.** Foreground user (`sceUserServiceGetInitialUser`), types 0, 1 and 5,
  16 kHz and 48 kHz, return real audio (RMS followed a podcast near the
  controller, ~20 in pauses to ~10000 in speech). System user refused; types 2-4
  invalid. Full log: `results-app-mic.txt`.
- Format param (from AnyPS5's libSceAudioIn): 1 = S16 mono, 2 = S16 stereo,
  0x10/0x11 = F32 mono, 0x12 = F32 stereo.

Open question for later research: anything that gives a payload an app identity
(or runs the capture inside an app or SceShellUI) should turn the `0x809b0081`
refusals into opens. Re-run `mic-probe.elf` after any such change.

## Files

- `mic_probe.c`: payload probe (one shot, 60 s hard limit, no fork, no watchdog).
- `Makefile`: builds `mic-probe.elf` with the payload SDK. Does not deploy.
- `mic-probe.elf`: prebuilt payload (gitignored, built on arlp-pc).
- `app_mic_probe.cpp`: the app-side probe that worked. To use it: copy it to
  `app/src/platform/ps5/mic_probe.cpp`; in `app/src/main.cpp` declare
  `namespace hui::ps5 { void probe_mic_debug(); }` under `#ifdef HUI_MIC_PROBE` and
  call `ps5::probe_mic_debug();` after `audio_out.start(mixer);`; build AND deploy
  with `APP_DEFINITIONS=HUI_MIC_PROBE` (deploy.sh rebuilds, so a plain deploy drops
  the probe). It waits 15 s for the core (which answers its symbol lookups), then
  sweeps and logs `[HUI-MIC]` lines to klog (port 3232): the app cannot write
  `/data`. Plain `dlopen` fails in the app; it loads the module like the IME code.
- `results-payload-mic.txt`, `results-app-mic.txt`: the runs above.

## What the payload does

1. `dlopen("libSceAudioIn.sprx")`, then `dlsym` for `sceAudioInOpen`,
   `sceAudioInInput`, `sceAudioInClose` (optional `sceAudioInInit`).
   The SDK has no libSceAudioIn stub, so this follows `samples/hello_dlfcn`.
2. `sceUserServiceInitialize`, `sceUserServiceGetForegroundUser`.
3. For users {foreground, 0xff system} x types {0..7} x freq {16000, 48000}
   x param {0, 1}: call `sceAudioInOpen(user, type, 0, 256, freq, param)`.
   Every return code is logged.
4. On each successful open, read about 3 s and log one line per 100 ms:
   RMS and peak (of 32767), plus the raw first 16 bytes and the input rc.
5. Close, `sceUserServiceTerminate`, summary of successes.

Logs go to stdout and stderr (`[mic-probe]` prefix) and are appended to
`/data/ps5-discord-probe/mic.txt`.

## Enum sources (honest status)

| Value | Used as | Source |
|---|---|---|
| user 0xff (SYSTEM), -1 (INVALID) | system user | OpenOrbis `include/orbis/_types/user.h` (fetched) |
| port type 0 = VOICE, 2 = RAW | sweep includes 0 and 2 | Vita SDK `audioin.h` via search snippet only. Vita, not PS5. Not verified |
| types 1, 3-7 | swept, not named | none found. The names GENERAL / HIGH_QUALITY from the brief have no source I could reach |
| freq 16000, 48000 | sweep | brief. 16 kHz also matches the Vita SDL usage snippet |
| param 0, 1 | sweep | guessed. No source found for the format enum |
| grain 256 frames, S16 mono | open and read assumption | guessed. The raw-bytes log line checks the format |

OpenOrbis `AudioIn.h` is a stub with no constants. psdevwiki returned 403 and
the Vita docs page 404. So the port types are swept, not named. The raw-bytes
line and RMS show whether an opened port really is S16 mono.

## Test steps (you run these)

Build (optional, the ELF is already there):

    cd tools/audio-probe && make PS5_PAYLOAD_SDK=../../.deps/ps5-payload-sdk

Send to elfldr on the PS5 (port 9021), using your own console IP:

    cat tools/audio-probe/mic-probe.elf > /dev/tcp/<PS5_IP>/9021

The run takes up to 60 s. Stop any other payload first.

Run A, built-in mic: sit on the home screen (no game running). Start the
probe, then speak continuously for the whole minute. Tap the controller lightly
too (the controller mic picks up taps).

Run B, headset: plug a headset with a mic into the DualSense, repeat run A.

Run C, in a game (optional): start a game, then repeat run A. This is the
case that matters for voice in play.

Read the results:

- `/data/ps5-discord-probe/mic.txt` (via FTP, or the elfldr stdout capture).
- Lines starting `[mic-probe]`. Key lines: `open ... -> 0x...` (each open
  rc), `OPEN OK`, `t= ... rms=...` (levels), `summary:` at the end.

## What the outcomes mean

- All opens fail (`-> 0x8026xxxx`), no `OPEN OK`: a payload cannot open the
  mic at all. Mic capture must come from the app or from SceShellUI. Check
  the rc codes: `0x80260011` is the same refusal the Spotify notes saw on
  AudioOut for foreground/VOICE, and points to a privilege or port-type gate.
- An `OPEN OK` but rms stays near 0 while you speak: the open works but gets
  no signal. Likely wrong port type, freq, or param. Try the same type in
  run B. A headset that moves the level and the built-in mic that does not
  points to the headset port.
- rms moves with your voice and taps: the payload can capture from that
  (user, type, freq, param) combo. Note the combo from the `summary:` line.
  Then test run C to see whether it survives a game.
- Foreground open works, system open does not (or the reverse): the user
  parameter matters for the design. Use the working one.
- A run where `input rc` is a negative code mid-capture: note the code and
  the read count (`capture done: reads=`).

For the app (`app_mic_probe.cpp`, see Files): outcomes read the same way.

## Caveats

- No watchdog, so `sceAudioInInput` could block. If the console stops
  responding, that is a result too. Reboot it and report.
- The 60 s limit is checked between calls, not inside a blocking read.
- Do not commit the patch to the app. It is a probe.
