# Roadmap

ps5-discord ships as one product: the **core** (a background payload that holds the Discord
session) and the **app** (the native PS5 interface). They are versioned and released
together.

## Versioning

The project follows [Semantic Versioning 2.0.0](https://semver.org).

| Part | Before 1.0.0 | From 1.0.0 |
|---|---|---|
| MAJOR | `0` while the product is in development | Breaking change for users: stored data, install layout, or a required core and app pairing |
| MINOR | A milestone below: a coherent set of user-facing capability | New capability, backwards compatible |
| PATCH | Fixes and small improvements inside a milestone | Same |

**Pre-releases** carry a channel suffix and are tagged at the release branch's commit:

| Channel | Tag | Meaning |
|---|---|---|
| Alpha | `vX.Y.Z-alpha.N` | Milestone in progress; runs on a console, incomplete, may break |
| Beta | `vX.Y.Z-beta.N` | Milestone scope complete; under console soak testing |
| Release candidate | `vX.Y.Z-rc.N` | No known blocking issues; final verification |
| Release | `vX.Y.Z` | Exit criteria met (below) |

**Core API version.** The control API the app uses (`/api/*` on the core) carries its own
integer `api_version`, reported by `GET /api/state`. It increases on any incompatible change.
The app states the range it supports and refuses to run against a core outside it, with a
clear message, instead of failing in odd ways.

**Build metadata.** Development builds identify themselves as `X.Y.Z-dev+g<commit>`.

## Releases

Each release on GitHub contains:

- `ps5-discord-app-vX.Y.Z.zip`: the native app, with the core payload inside it (the app
  starts the core itself)
- `update.txt` and `update.sig`: what the app's updater checks, signed with the project's
  offline keys
- `SHA256SUMS`: checksums of the above
- Release notes taken from [CHANGELOG.md](CHANGELOG.md)

Work happens on `feat/*` and `fix/*` branches off `develop`. `develop` goes to `main` once it
has been tested on a console (firmware 13.00), and a release is built and signed from a
`release/vX.Y.Z` branch cut from `main`. From 0.5.0-alpha.1 on, consoles update from inside
the app (Settings, About, Updates).

## Milestones

Every milestone lists its goal, scope, exit criteria and what it deliberately leaves out.
Exit criteria are the definition of done; a milestone leaves alpha only when all are met.

### 0.1.0: Session foundation (released 2026-10-19)

**Goal.** Prove a native Discord session can live on the console.

**Scope.** QR login (remote auth v2), gateway session over TLS with zlib-stream, heartbeat,
resume, status web UI on the LAN.

**Exit criteria.** Met: QR login from the mobile app, READY on the console, session held with
acknowledged heartbeats.

### 0.2.0: Native app and text chat (0.2.0-alpha.1, 2026-10-20)

**Goal.** Read and write Discord text on the TV with a controller, in the native app.

**Delivered.** Native app on the ps5-homebrew-ui kit: QR sign-in, Home (All, Friends,
Servers, Mentions), server pages, channel chat and DMs (read, send, reply, react, edit,
delete), the PS5 system keyboard, avatars, icons and image previews, text in many scripts,
the core API version check.

### 0.5.0: Voice, Go Live and a daily app (0.5.0-alpha.1, 2026-10-31)

The planned 0.3.0 (presence and account), 0.4.0 (voice connection) and 0.5.0 (voice audio)
were built together and shipped as one release.

**Goal.** Talk with friends from the PS5, show them the game, and keep the app up to date
without a PC.

**Delivered.**
- Voice: server voice channels and DM calls, DAVE end-to-end encryption, Opus both ways,
  mute and deafen, mic gain and call volume.
- Go Live: the game streamed into the call (Remote Play loopback, up to 1080p60 for Nitro),
  with the USB DualSense as the headset.
- Account: a sign-in per console user, encrypted to the console; sign out; unread and
  mention state from Discord's read state.
- Notifications in the app and as PS5 alerts, following Discord's mutes, with quiet hours.
- Colour emoji, the emoji picker, reactions with any emoji, stickers, full-screen images.
- "Playing" on Discord for the game in front.
- Settings: account, voice & audio, notifications, appearance (reduced motion, swapped
  Cross and Circle), privacy, about.
- Signed updates from GitHub inside the app, with roll back to the previous version.

**Exit criteria (open).**
- A two-way conversation with three or more people for 30 minutes, joined and left cleanly.
- Go Live for 30 minutes, started and stopped twice in a row without a reboot.
- Unread and mention counts match the official client after a session.
- A two-hour session without a crash, leak growth or lost gateway session.
- An update from one release to the next and a roll back, on a console, from the release
  build.

### 0.5.x: Voice and Go Live follow-ups

**Goal.** Make calls and Go Live work the way people expect from the desktop app.

**Scope.**
- Voice while a game is in front without Go Live (the console gives the app the mic only
  while it is on screen today).
- Push to talk and per-user volume.
- Go Live: no replug of the controller between streams, resuming a stream after a network
  drop, quality chosen to fit the connection.
- Go Live and calls together with a controller on Bluetooth.

**Exit criteria.** The open checks of 0.5.0 above, plus a game played for an hour in a call
with Go Live on, without touching the cable.

### 0.6.0: Beta hardening

**Goal.** Ready for people other than the developers.

**Scope.** Performance budgets at 1080p and 4K, memory ceilings, resilience to network loss,
sign-in when Discord asks for a captcha, text size settings, uploads and threads, the
troubleshooting and install guides kept current.

**Exit criteria.** A week of daily use by testers with no crash or data loss, and updates
between betas from inside the app.

### 1.0.0: Stable

Feature-complete daily driver for text and voice, documented install and update path, stable
core API.

## Tracking

Each milestone is a GitHub milestone holding its issues. Bugs and requests:
[GitHub issues](https://github.com/abhi245y/ps5-discord/issues).
