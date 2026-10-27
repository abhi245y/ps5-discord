# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org) as described in [ROADMAP.md](ROADMAP.md).

## [Unreleased]

### Changed
- License: ps5-discord's own code is now AGPL-3.0-or-later (was GPL-3.0-or-later), so
  Remote Play code from chiaki-ng can ship in it. Files from the ps5-homebrew-ui kit keep
  GPL-3.0-or-later. The status page links to the source.

## [0.2.0-alpha.1] - 2026-10-20

First alpha of the native app. Text chat works; voice does not exist yet.

### Added
- Native PS5 app (title PPSA99072) on the ps5-homebrew-ui kit, talking to the core over
  loopback: QR sign-in, Home (friends online, live rooms, recent conversations), Friends,
  Servers and Mentions tabs, server chat and DMs.
- App: real avatars (Discord's default for accounts without one), server icons and banners,
  image attachment previews, through the core's image route.
- App: replies, reactions, edit and delete of your own messages, DMs opened from a friend.
- App: on-screen keyboard (Triangle) with an emoji row, held keys repeating, the 2000-character
  limit and a draft per channel; quick replies stay a Cross away.
- App: a page for each server (banner, voice rooms with who is in them, text channels by
  activity); Join is marked "soon" until voice lands. Channel list on Options, history pages
  load ahead of the focus, a marker at the start of a channel.
- App: the core ships inside the app and starts through elfldr when it is not running; test
  builds of the core quit 30 s after the app goes quiet (`make CORE_FOLLOWS_APP=0` for release).
- App: the PS5 system keyboard on Triangle, found through the core's local `/api/dlsym`; the
  app's own keyboard remains the fallback.
- App: colour emoji in reactions (server emoji and Twemoji), wrapping onto more lines; role
  and channel names in mentions; markdown links, headings and escapes read as Discord shows them.
- App: server emoji inside message text drawn as pictures; button hints with the coloured
  rings of the console's face buttons.
- App: toast and chime for a live DM or mention; text size on R3 (Small, Medium, Large).
- App: Malayalam, Devanagari, Tamil, Kannada, Telugu and Bengali fonts; the console's own
  Japanese, Chinese, Korean, Arabic and Thai fonts.
- Core: live account tree (users, presence, voice, read state), channel permissions from READY,
  servers and channels created or removed while running, reactions and forwards in events.
- Core: `/api/mentions`, `/api/image`, `/api/ack`, `/api/react`, `/api/dm`, `/api/edit`,
  `/api/delete`; replies on `/api/send`; long-poll `/api/events?wait=`; `api_version` 1.
- Core: text chat API for the native app, served to local callers only: guild, channel and DM
  lists (`/api/chat`), live message events (`/api/events`), message history
  (`/api/messages`) and sending (`/api/send`).
- Core: guild subscription (gateway op 37) when a guild is opened.

### Fixed
- App: a message taller than the screen no longer jitters when focused; Up/Down read through it.
- App: server members in chat show their own picture, not placeholder art.
- Core: heartbeats could be starved while the gateway delivered a steady stream of events.
- App: SIGPIPE crash when the core closed a connection; abort in the JSON parser (the runtime
  libc reported an empty decimal point; decimals parsed short).
- Core: the TLS random generator was shared between threads without a lock.

### Changed
- Core: each web connection is served on its own thread; rate-limited REST calls retry once.
- App: messages arrive by long poll; images load on their own thread; text fitting and
  wrapping are cached; message lists are shared copy-on-write.

### Security
- Core: chat routes refuse callers off the console; message and channel IDs are validated
  before they reach Discord's API.

### Known issues
- No voice: rooms and who is in them show, Join says "soon".
- No uploads, threads, or notifications outside the app.
- The session token is stored unencrypted on the console: use a throwaway account.
- A USB keyboard plugged into the console does not type (its library freezes a payload on 13.00).

## [0.1.0] - 2026-10-19

### Added
- Core payload with QR login (Discord remote auth v2); the session token is saved for later
  runs.
- Gateway session over TLS with zlib-stream compression, heartbeat, resume and backoff.
- Status web UI on port 9041: session state, login QR, account, live log and stop.

### Known issues
- The session token is stored unencrypted on the console.
- The status web UI has no authentication on the local network.

[Unreleased]: https://github.com/abhi245y/ps5-discord/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/abhi245y/ps5-discord/releases/tag/v0.1.0
