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

**Pre-releases** carry a channel suffix and are tagged on `main`:

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

- `ps5-discord-core-vX.Y.Z.elf`: the core payload
- `ps5-discord-app-vX.Y.Z.zip`: the native app (from 0.2.0)
- `SHA256SUMS`: checksums of the above
- Release notes taken from [CHANGELOG.md](CHANGELOG.md)

A release is cut only from `main`, from a commit whose CI and host self-checks pass, after the
milestone's exit criteria are verified on a console (firmware 13.00).

## Milestones

Every milestone lists its goal, scope, exit criteria and what it deliberately leaves out.
Exit criteria are the definition of done; a milestone is not released until all are met.

### 0.1.0: Session foundation (released)

**Goal.** Prove a native Discord session can live on the console.

**Scope.** QR login (remote auth v2), gateway session over TLS with zlib-stream, heartbeat,
resume, status web UI on the LAN.

**Exit criteria.** Met: QR login from the mobile app, READY on the console, session held with
acknowledged heartbeats.

### 0.2.0: Native app and text chat

**Goal.** Read and write Discord text on the TV with a controller, in the native app.

**Scope.**
- Core text API: guild, channel and DM lists, message history, live message events, sending;
  image proxy for avatars, icons and attachments (TLS stays in the core).
- Native app on the ps5-homebrew-ui kit, following the design framework: sign in, Home (All,
  Friends, Servers, Mentions), server page, channel chat, direct messages, reply composer with
  the system keyboard, message actions (reply, copy).
- Text shaping for names and messages outside ASCII.
- Core API versioning and the compatibility check.

**Exit criteria.**
- Open any text channel or DM, read history, send and receive messages, on a console.
- 60 fps on Home and in chat with a busy account (40+ guilds, 50+ online friends).
- A two-hour session without a crash, leak growth or lost gateway session.
- Host self-checks and preview snapshots of every screen pass in CI.

**Out of scope.** Voice, reactions, uploads, threads, notifications outside the app.

### 0.3.0: Presence and account

**Goal.** Make the app a dependable daily companion.

**Scope.** Friend profiles, mention and unread state synced with Discord (read acks), reactions,
in-app notices, reconnecting and error states, settings (account, appearance), sign out, and
the session token encrypted at rest on the console.

**Exit criteria.** Unread and mention counts match the official client after a session; token
unreadable from the console's file system without the app; all states in the design framework
reachable and handled.

**Out of scope.** Voice.

### 0.4.0: Voice connection

**Goal.** Join a voice channel and be present in it.

**Scope.** Voice gateway, UDP transport and IP discovery, transport encryption, DAVE
end-to-end encryption handshake (libdave), joining and leaving muted, the voice room and voice
dock screens.

**Exit criteria.** Joining shows the console's account in the channel on other clients, stays
connected for 30 minutes with others talking, and leaves cleanly.

**Out of scope.** Hearing or sending audio.

### 0.5.0: Voice audio

**Goal.** Talk and listen while playing.

**Scope.** Opus decode and encode, DAVE media frames, DualSense or headset microphone, voice
activity and push to talk, per-user volume, audio routed to the console's output.

**Exit criteria.** Two-way conversation with three or more participants; mouth-to-ear latency
under 250 ms on a home network; voice continues with the app in the background.

### 0.6.0: Beta hardening

**Goal.** Ready for people other than the developers.

**Scope.** Performance budgets at 1080p and 4K, memory ceilings, resilience to network loss,
accessibility settings (text size, reduced motion), documentation for installing and updating.

**Exit criteria.** A week of daily use by testers with no crash or data loss.

### 1.0.0: Stable

Feature-complete daily driver for text and voice, documented install and update path, stable
core API.

## Tracking

Each milestone is a GitHub milestone holding its issues. Work happens on `feat/*` and `fix/*`
branches merged into `main` through pull requests.
