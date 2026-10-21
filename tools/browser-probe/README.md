# PS5 Discord browser feasibility probe

## Browser session handoff experiment

User confirmed Discord login completed without error; no CAPTCHA appeared, so
challenge handling itself remains untested. User then requested transfer to the
signed-out native app.

Located this active profile's Discord-only WebKit storage at
`/user/home/<user-id>/webkit/shell/local/https_discord.com_0.localstorage`.
The database has `ItemTable(key TEXT, value BLOB)`; the active `token` value is a
UTF-16LE JSON string. Do not print or persist this value in diagnostic logs.

One-time test performed via existing LAN FTP (not a production transport):

- Verified no Discord WAL sidecar was present and two reads matched.
- Inspected an in-memory copy; changed only its SQLite journal header bytes
  from WAL mode to rollback mode for deserialize. Remote storage was untouched.
  `PRAGMA quick_check` returned `ok`.
- Validated the extracted session with HTTPS GET `/api/v9/users/@me` on
  `discord.com`; received a valid account response. Identity/token not printed.
- Confirmed native core was unavailable and token destination empty. Parent
  directory was 0700. Uploaded a random temporary file, set 0600 through FTP
  SITE CHMOD, wrote and verified bytes, then renamed to `/data/ps5-discord/token`.
- No browser database or token was saved on the laptop. No new payload executed.

The user opened the native app and confirmed the handoff worked. This validates
browser login -> saved Discord session -> native client acceptance for this test.
It does not establish CAPTCHA handling, long-term stability, or seamless integration.
A production importer
should operate on-console for the active user, use a consistent SQLite read,
validate before replacement, handle logout/expiry, and coordinate core startup
without exposing an unauthenticated LAN credential-import endpoint.

## Discord no-watchdog deployment

At the user's request, rebuilt `launch.c` with `PROBE_URL=https://discord.com/login`
and saved `discord-login-no-watchdog.elf` (110,424 bytes; SHA-256
`c08b5a257eaaa845a7b1060c8376ac4a6fd94274b5b2217ba4b6c3680d350662`).
Sent once. UserService init, launch, and terminate returned zero. Live klog
records PID 103 calling exit with status 0. FTP remained responsive at 5, 35,
and 50 seconds. Capture was stopped after observation.

Evidence: `tmp/browser-probe-recovery/discord-no-watchdog-test/`. User confirmation
of Discord page rendering and CAPTCHA/login behavior is pending. No session
extraction or import is implemented. The generic `discord-browser-no-watchdog.elf`
was also rebuilt with this Discord URL; rebuild explicitly for local-page tests.

## Current candidate: one-shot launcher without watchdog

The user explicitly requested omitting the watchdog where unnecessary. `launch.c`
now provides a separate one-shot launcher with no fork, kill, waitpid, sleep, or
polling loop. It initializes UserService, launches the browser, terminates
UserService, and returns. It has no timeout recovery if a service call hangs.
Previous experimental variants remain quarantined.

```sh
make -C tools/browser-probe no-watchdog \
  PS5_PAYLOAD_SDK=/home/arl/Documents/code/spot5/.deps/native/ps5-payload-sdk \
  PROBE_URL=http://<pc-ip>:9062/probe
```

Build passed with warnings treated as errors. Undefined-symbol inspection confirms
no fork/kill/waitpid imports. Output: `discord-browser-no-watchdog.elf`, 110,424
bytes, SHA-256 `9d14ab78bb0e66942d99c7bfe1d7a4e99ea55dd90d2345f8240d36dae2012c05`.
Log path: `/mnt/usb0/discord-browser-no-watchdog.log`.

Deployed once at the user's request. All three service calls returned zero; live
klog records PID 100 (`payload.elf`) calling exit with status 0. PS5 fetched
`/probe` and `/js-ok`, confirming HTTP and JavaScript execution. FTP remained
responsive at 5, 35, and 50 seconds, beyond the earlier 25-second failure window.
No crash was observed during this window. This supports the watchdog-lifecycle
hypothesis but is not a long-duration stability test or an exact root cause.
Logs are in `tmp/browser-probe-recovery/no-watchdog-test/`. The temporary laptop
HTTP server and klog capture were stopped after observation; refreshing the test
page now requires restarting that server. Discord compatibility remains untested.
Start `local_page.py` before a local-page test; its server stops after 120 seconds.

## Passive-watchdog browser trial: rendering succeeded, later crash

The user requested browser launch without the suspect parent-side watchdog
kill/reap. Built `discord-browser-passive.elf` (118,688 bytes, SHA-256
`c93a5a37dae7055baebbfa5e6c3b1559c07c0aa01175f930ab8bc8340ee031b2`).
The binary has no waitpid import. It retains a child that sleeps 25 seconds,
checks its parent, and exits; the parent returns without killing or reaping it.

Sent once with live klog capture and a fixed local HTTP page. Evidence:

- PS5 requested `/probe` and `/js-ok`: HTTP fetch and JavaScript execution worked.
- User confirmed the browser/page loaded, then reported a later console crash.
- FTP remained responsive at five seconds, but timed out at 32 seconds.
- Live klog records PID 109 (`payload.elf`) calling exit with status 0. The
  watchdog PID was 110. No PID 110 exit or panic backtrace was captured.
- Timing is consistent with the watchdog's 25-second wake/exit path, but does
  not establish which instruction or kernel path failed. Parent-side waitpid is
  not necessary for this failure, since this variant does not call it.

Captured evidence: `tmp/browser-probe-recovery/passive-test/`. The laptop HTTP
server and klog reader were stopped. Quarantine this variant too. Browser launch
and basic rendering are now demonstrated; Discord login and session handoff are
still untested. Do not launch another payload until the child lifecycle is fixed.

## Recovery evidence after the second crash

Read-only FTP recovery found the lifecycle log ending at `after watchdog kill:
0x00000000`, with parent PID 101 and watchdog PID 102. No `after watchdog reap`
or `returning from main` marker was persisted. This narrows the last observed
interval to child termination/reaping, including concurrent child cleanup; it
does not prove waitpid itself caused the crash.

Captured 98,559 bytes from the existing klog service after restart. No matching
panic/crash stack was found. The persisted coredump directory was empty.
Error-history records `000001AC.json` and `000001AD.json` both report CE-108262-9,
title NPXS45092, with empty logReport fields. Persistent logger records include
UnexpectedShutdown events. These confirm abnormal shutdowns, not the fault site.

Recovery copies are in ignored project storage `tmp/browser-probe-recovery/`:
probe logs, current klog, VSH log, 16 MiB system log, and SQLite logger databases.
The raw system log yielded no matching readable panic strings; its full binary
format has not been decoded. No previous-boot kernel backtrace was recovered.
The logger database was copied while live and is not an atomic snapshot.
No payload was executed during recovery.

## Controlled follow-up (built, not deployed)

**UPDATE: lifecycle-only probe also crashed the console. All probes quarantined.**
After explicit user approval, sent `discord-browser-lifecycle.elf` once
(118,704 bytes). FTP was responsive and no lifecycle log existed before the run.
Loader EOF was not observed within four seconds; subsequent FTP retrieval timed
out. The user confirmed another console crash. No post-run log has been recovered
yet. No local-page payload was sent.

This reproduces failure without invoking the browser launch API, so Discord and
browser launch are not necessary triggers. It does not isolate fork/watchdog,
UserService lifecycle, linked-module startup, or SDK teardown. Stop console tests;
recover the persisted lifecycle log after manual restart and investigate offline.

The default build now produces `discord-browser-lifecycle.elf`. It uses the same
service linkage, UserService init/terminate, and fork watchdog, but does NOT open
a browser. New logs bracket watchdog kill and reap. Reaping is nonblocking with
a two-second bound instead of an unprotected blocking wait after killing the
watchdog. Its log is `/mnt/usb0/discord-browser-lifecycle.log`.

`discord-browser-local.elf` adds only a browser launch to a fixed laptop page.
Its log is `/mnt/usb0/discord-browser-local.log`. Build and serve from repo root:

```sh
make -C tools/browser-probe local \
  PS5_PAYLOAD_SDK=/home/arl/Documents/code/spot5/.deps/native/ps5-payload-sdk \
  PROBE_URL=http://<pc-ip>:9062/probe
python3 tools/browser-probe/local_page.py --bind <pc-ip>
```

The fixed-page server exits after 120 seconds, serves no filesystem content, and
records `/probe` and `/js-ok` requests. These prove HTTP fetch and JavaScript
execution respectively, not visible rendering or successful browser shutdown.
Confirm the laptop LAN address before rebuilding for a different network.

Both follow-up builds passed with warnings treated as errors. Initially neither
was sent. Automatic approval review rejected lifecycle-only deployment after the
previous crash, requiring explicit user approval for the unverified lifecycle
path. The later explicitly approved baseline failed as recorded above; the
local-page test must not proceed.

Offline review: the upstream browser sample also immediately terminates
UserService; that alone does not establish a bug in our ordering. A locally
available SDK source tree runs library finalization/unloading after main returns,
so a final main log is not process-exit evidence. This source is corroborating
context, not a verified source match for the installed SDK binary. No root cause
has been established. The Claude review command timed out without advice.

The original Discord payload remains retained for diagnosis. Its make target is
quarantined; do not send the existing original ELF.

Experimental, launch-only payload for `feat/browser-login-probe`. Not a working
browser login integration. Local build passes, but the first console deployment
was followed by a user-confirmed console crash. **Do not redeploy this probe.**

## Failed console trial — 2026-10-21

Sent 118,752 bytes to elfldr at `<ps5-ip>:9021`. The connection stayed open
beyond 35 seconds. Before deployment the named USB log did not exist; afterward,
FTP log retrieval timed out, then console services became unreachable. The user
confirmed that the console crashed. No successful browser launch or process exit
was established, and no crash-stage log was recovered.

After the user restarted the console, FTP recovered and the persisted log showed:

```text
pid=101 start (25 second watchdog): 0x00000000
pid=101 sceUserServiceInitialize: 0x00000000
pid=101 opening https://discord.com/login: 0x00000000
pid=101 sceSystemServiceLaunchWebBrowser (not login status): 0x00000000
pid=101 sceUserServiceTerminate: 0x00000000
pid=101 probe exit; browser rendering and login require manual observation: 0x00000000
```

This establishes successful service return codes and progress to the final log
statement. That statement occurs BEFORE watchdog kill/reap and return from main,
so it does not establish process exit. Browser rendering/login remain unobserved.
The log does not distinguish asynchronous browser startup failure from cleanup
or another system failure. Do not attribute the crash to a particular call yet.

The user subsequently confirmed that only a blank screen appeared before the
crash; no Discord page was visible. This is not evidence of a CAPTCHA failure.
Browser startup/rendering and payload cleanup remain separate unresolved causes.
Before another console run, review the launch context and cleanup lifecycle.
A later controlled test should use a minimal local page to separate basic browser
startup from Discord compatibility; do not repeat the failed Discord probe.

The cause is unknown: build success and the SDK sample do not establish runtime
safety in this firmware/loader context. A fork watchdog limits a stuck process;
it cannot recover a kernel or system-service crash. Recover the console manually
and inspect any persisted probe/crash logs before proposing another runtime test.

The SDK's [browser sample](https://github.com/ps5-payload-dev/sdk/blob/master/samples/browser/main.c)
establishes the launch API and UserService lifecycle; its
[Makefile](https://github.com/ps5-payload-dev/sdk/blob/master/samples/browser/Makefile)
identifies the required libraries. The existing SDK under `spot5/.deps/native`
links the required imports successfully. Behavior on firmware 13.00 remains
unverified.

## Behavior

- Starts a fork-based 25-second watchdog before service/file operations.
- Initializes UserService and requests `https://discord.com/login` in the system browser.
- Logs numeric service results to stderr and, if USB is mounted,
  `/mnt/usb0/discord-browser-probe.log`.
- Terminates its UserService session, reaps the watchdog, and exits.
- Does not wait for login, collect credentials, change the current Discord app,
  or write `/data/ps5-discord/token`.

A successful launch return only means the launch call succeeded. It does not
prove the page rendered, CAPTCHA worked, or a session can be exported.

## Build

Use an isolated folder such as `~/Documents/ps5-discord-browser-login-probe` on
`arlp-pc`. Copy these source files there; use the already installed SDK. Do not
install dependencies without approval. From that folder:

```sh
make PS5_PAYLOAD_SDK=/absolute/path/to/existing/ps5-payload-sdk
```

Output: `discord-browser-probe.elf`. No automatic deployment target is provided.
The home PC was unreachable (`No route to host`). The user explicitly authorized
a local build for this probe, using the existing SDK without installing anything:

```sh
make -C tools/browser-probe \
  PS5_PAYLOAD_SDK=/home/arl/Documents/code/spot5/.deps/native/ps5-payload-sdk
```

Build passed with Clang 22.1.8 and `-Wall -Wextra -Werror`. The resulting 118,752-byte
x86-64 FreeBSD ELF links `libSceSystemService.sprx` and `libSceUserService.sprx`.
SHA-256: `fcf0ad37658495e892a4b25bc14bb8c22819952313919fad7a8417c6a72f6128`.
The first console trial failed as described above; this artifact is retained for
offline diagnosis, not redeployment.

## Manual console checks

1. With firmware 13.00 and elfldr available, deploy this ELF once through the
   usual payload loader. Keep the Discord app's saved session intact.
2. Observe whether the browser opens, the address is Discord's HTTPS login page,
   and the page renders. Record any error text without credentials.
3. If it renders correctly, the user enters their own login and completes any
   CAPTCHA/MFA. Do not interact with PSN or update prompts.
4. Record whether Discord reaches its authenticated UI and whether closing the
   browser restores the previous application. Do not assume either behavior.
5. Inspect the named USB log. If the payload times out, do not repeatedly deploy;
   inspect the last logged stage first.

## Remaining gate: session handoff

This browser-launch API has no session callback in the sample. Opening Discord
does not give this payload access to its origin's storage or network responses.
An explicit browser integration/export mechanism must be established before
implementing a credential handoff. No memory scanning or origin-policy bypass
is part of this probe.

Even after handoff is established, the core needs a controlled import path:
validate the session, atomically save with owner-only permissions, cancel the
in-flight QR attempt, and connect. The current QR loop does not automatically
notice a token file written by another process while it is waiting for login.
Sharing a console does not guarantee Discord accepts the native-client session.


## Native crash after fresh browser login (2026-10-21)

Live klog captured successful browser validation and gateway READY before the
native title aborted. Relocating the application backtrace by 0x400000 resolves
to `apply_tree`'s read-state lambda, `json::value<double>` and an uncaught JSON
exception. The core read missing `mention_count` with `cJSON_GetNumberValue`,
producing NaN, serialized as null. The native client then required a number.

The core now normalizes missing/invalid mention counts to zero. The native
reader also tolerates null, missing, wrong-type, negative and out-of-range counts,
including replies from older cores. Regression coverage includes READY fixtures
with omitted/null/valid counts and native reader tests. Browser session capture
was successful in this trace; Discord UI logout separately invalidates the
shared token. Keep the browser signed in when testing app restart.
