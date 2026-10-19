# ps5-discord

Native Discord client for PS5. The core runs as a background payload (loaded
with elfldr) that holds the Discord session; a native app UI will talk to it later.

Status: pre-alpha proof of concept.

## What works

- QR code login: no password or token typing, scan with the Discord mobile app
- Gateway session: zlib-stream, heartbeat, resume after drops, READY
- Status web UI at `http://<ps5-ip>:9041`: state, login QR, account, live log, stop

## Build

Builds on Linux with clang (no llvm-devel needed).

```sh
make deps                        # pinned ps5-payload-sdk, mbedTLS, zlib, cJSON, qrcodegen
make                             # build/ps5-discord.elf
make test                        # host self-checks
make deploy PS5_HOST=<ps5-ip>    # stop the running copy, send the new one to elfldr (9021)
make stop PS5_HOST=<ps5-ip>
```

`make host` builds the same core for Linux (state in the current directory) for
testing against Discord without the console.

## Use

1. Send the payload (`make deploy` or any payload sender to port 9021).
2. Open `http://<ps5-ip>:9041` and scan the QR code: Discord app, Settings, Scan QR Code.
3. The session token is kept in `/data/ps5-discord/token`; later runs skip the QR.

## Known limitations

- The token is stored unencrypted. Use a throwaway account until it is encrypted at rest.
- The status UI has no authentication; anyone on the LAN can view it or stop the payload.
  It never shows the token.
- Third-party clients break Discord's ToS and can get an account banned.

## Target

- Console firmware 13.00 (never install or trigger >= 14.00)

## Workflow

- `main` stays releasable; work on `feat/*` and `fix/*` branches, merged by PR.
- Tags `vX.Y.Z` with a GitHub release only on meaningful milestones.

## Milestones

1. `v0.1.0`: QR login and gateway session, status web UI
2. `v0.2.0`: read and send text messages
3. `v0.3.0`: join a voice channel (muted placeholder)
4. `v0.4.0`: voice audio (Opus + DAVE)
5. `v0.5.0`: native PS5 app UI talking to the background core
