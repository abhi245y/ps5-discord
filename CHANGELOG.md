# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org) as described in [ROADMAP.md](ROADMAP.md).

## [Unreleased]

### Added
- Core: text chat API for the native app, served to local callers only: guild, channel and DM
  lists (`/api/chat`), live message events (`/api/events`), message history
  (`/api/messages`) and sending (`/api/send`).
- Core: guild subscription (gateway op 37) when a guild is opened.

### Fixed
- Core: heartbeats could be starved while the gateway delivered a steady stream of events.

### Security
- Core: chat routes refuse callers off the console; message and channel IDs are validated
  before they reach Discord's API.

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
