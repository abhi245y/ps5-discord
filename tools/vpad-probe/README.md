# Virtual-pad probe

Standalone research payload for testing whether one virtual pad created directly
through `/dev/hid` can control a PS5 on **FW 13.00**, without a Remote Play session
and with the physical DualSense left untouched. FW 13.00 behavior is unverified.
Successful ioctls alone do not prove that the shell or a game consumes the input.

The payload uses only the add, insert, and delete HID ioctls. It does not open,
read, disconnect, or reassign the physical controller, load libScePad, patch or
inject into another process, change credentials, or start Remote Play. Button
masks are restricted to D-pad Right, D-pad Left, and L2. It never sends PS,
Options, Cross, Circle, or Create.

## Build

On the build host, from the repository root:

```sh
PS5_PAYLOAD_SDK=/absolute/path/to/ps5-payload-sdk make -C tools/vpad-probe
```

Output: `tools/vpad-probe/vpad-probe.elf` (ignored by Git). This follows
`tools/ds5-usb-probe`: `prospero-clang`, SDK environment, GNU C11, warnings as
errors, and optimization; it links `kernel_sys` and `SceUserService` explicitly.
The Makefile only builds or cleans; it never deploys.

## Run and observe

1. Leave the PS5 on its home screen, with no Remote Play session. Leave the
   physical DualSense connected and untouched during the test. Start at a home
   screen position with enough items to the right to observe five moves.
2. With an existing payload loader listening on port 9021, run from your host:

   ```sh
   nc <ps5> 9021 < tools/vpad-probe/vpad-probe.elf
   ```

   Equivalently, from `tools/vpad-probe`: `nc <ps5> 9021 < vpad-probe.elf`.
   Replace `<ps5>` with the console address.
3. Watch for **selection moving right five times, then left five times**. The
   later stick-right test may move selection again. L2 may have no visible
   effect on the home screen. Record any assignment dialog, missing moves, or
   unexpected routing; do not dismiss a dialog with the physical controller
   during this run, since that changes the experiment.

The sequence is about **19 seconds**, plus ioctl/logging overhead, with a
30-second monotonic deadline checked between operations:

- Open `/dev/hid`, add exactly one virtual pad, then wait 3 seconds.
- Send neutral; send Right five times (150 ms pressed, 850 ms released each).
- Pause neutral for 1 second; repeat five Left pulses; pause neutral for 1 second.
- Hold the left stick fully right for 1 second, then centre it.
- Hold L2 at 255, with its digital bit set, for 1 second, then release it.
- Send neutral for 2 seconds, delete the virtual pad, and close the device.

Current state is resent every 16 ms during holds, release gaps, pauses, and the
final wait. The initial 3-second wait sends no samples. The cadence follows the
references' continuous input/hold feeding; it is not proof that FW 13.00 requires
a keepalive. Scheduling and synchronous logging can lengthen frame intervals.

## User assignment override

The log records both foreground and initial user IDs. Normally the foreground
user is selected, with the initial user as fallback. To test a different user,
use an existing local user that is already signed in, and place their actual
numeric ID in `/data/ps5-discord/vpad-probe-user` before running again. Example
file contents (replace this example with that user's actual ID):

```text
0x12345678
```

Decimal `305419896` means the same ID. Hex must use `0x`; leading zeroes in decimal
do not mean octal. Surrounding whitespace is accepted. Empty, oversized,
malformed, negative, and `0xffffffff` IDs cause an error before creating a pad.
An unreadable override also causes an error; there is no silent fallback. The
probe uses the supplied ID exactly and does not sign in or create a user.
Remove the override file to restore foreground/initial-user selection.

AnyPad's tests report that a virtual pad assigned to the physical DualSense's
user worked in system menus but was ignored in games; assigning it to another
user resolved that in their setup. This file tests that variable through the HID
descriptor only. It does not reproduce AnyPad's MBus binding or PS-button flow.

## Logs and cleanup

Every step and every ioctl return value and captured `errno` go to both the
kernel log (`klog_printf`) and **`/data/ps5-discord/vpad-probe.log`**, appended and
flushed per line. Lines include PID and elapsed monotonic milliseconds. A log
open failure stops the probe before any pad is created; later logging failures
also stop input and trigger cleanup. The console directory is created if needed.

`/data/ps5-discord/vpad-probe.lock` is held with a nonblocking exclusive `flock`.
A second instance exits without creating a pad. The lock file remains on disk;
closing its descriptor releases the lock. Do not remove it while a probe runs.

Any failed ioctl stops the sequence and produces a nonzero exit status. Cleanup
deletes only the handle returned for this probe's pad, then closes `/dev/hid`.
SIGINT/SIGTERM set a stop flag; the main loop performs cleanup outside the signal
handler. An idempotent `atexit` cleanup is also registered. Delete gets at most
two attempts; even a successful retry leaves a failed run's exit status nonzero.
The final line records failure, signal, and whether deletion remained unresolved.

The deadline cannot interrupt a kernel call that never returns. SIGKILL, a
process crash, or a rejected delete cannot guarantee pad removal; an unresolved
delete is explicitly logged with the handle. Do not interpret a released file
lock as evidence that a crashed run's virtual pad was removed.

## Reference layouts and credits

This probe is independently written under **AGPL-3.0-or-later**, matching the
project [license](../../LICENSE). Credits to these GPL-3.0-family research
implementations; paths and line numbers refer to the local reference snapshots:

- **FGGstore/FGG-XSense (GPL-3.0)**,
  `tmp/research/FGG-XSense/src/vpad.c:13-75`: ioctl numbers, command words,
  descriptor/input offsets, button masks, and native request layouts.
  `:96-106`: zeroed 168-byte descriptor, protocol byte 3 at `+0x00`, user ID at
  `+0x1c`, class byte 1 at `+0x20`; add request command 3 and output-handle pointer.
  `:119-140`: insert request uses the handle as its first uint32 and report pointer
  at `+8`; delete is a 16-byte request with command byte 5 and handle at `+8`.
  `:187-204`: analog/digital trigger pairing, active byte 1, identity quaternion
  at `+0x84`. `src/vpad.h:41`: input buffer size `0xa0`.
  The reused ABI is enforced with request-size/pointer-offset static assertions.
- **sinfiltros/AnyPad-PS5-English (GPL-3.0-or-later)**,
  `tmp/research/AnyPad-PS5-English/src/ps5_vpad.c:253-319`: user-selection context;
  `:534-613`: state submission and repeated held-state feeding.
  `PRUEBAS.md:360-395`: same-user versus different-user shell/game observations.
  Its 120-byte `ScePadData` is a libScePad input, not this probe's kernel buffer.
- **Ghostpad (GPL-3.0-or-later for payload sources)**,
  `tmp/research/Ghostpad/virtualDS5research.md:28-38`: contrasting injected/MBus
  assignment approach; `:282-339`: button and trigger observations. Its patching,
  physical-pad eviction, and automatic assignment dismissal are not used here.

The kernel report uses buttons at `+0x0c`, four unsigned stick bytes at `+0x10`
(centre 128, full right 255), L2/R2 at `+0x14/+0x15`, and active byte at `+0x17`.
All unused bytes are zero except the identity quaternion's W component.

**FW 13.00 unknowns:** whether the documented ioctls/layouts remain accepted,
whether setting the descriptor's user ID is enough for assignment without MBus
binding or a PS press, and whether shell/game routing accepts this pad alongside
the untouched physical DualSense without Remote Play. An assignment dialog or
successful insert with no movement is useful evidence, not a passing result.
No build or console validation is claimed by these source files.
