# ps5-discord

Native Discord client for PS5. A native app (title PPSA99072) draws the UI; a
background core payload, which the app starts itself, holds the Discord session.

Status: alpha. 0.2.0-alpha.1 (text chat) is the latest release; `main` adds voice calls and
Go Live, not released yet.

## What works

- QR code login: no password or token typing, scan with the Discord mobile app
- Home with friends online, live voice rooms and recent chats; Friends, Servers, Mentions tabs
- Server pages, channel chat and DMs: read, send, reply, react, edit and delete your own
- The PS5 system keyboard on Triangle (the app's own keyboard as a fallback)
- Avatars, server icons, banners, image previews, colour emoji, many scripts
- Status web UI at `http://<ps5-ip>:9041`: state, login QR, account, live log, stop
- Voice: server voice channels and DM calls, with Discord's end-to-end encryption (DAVE),
  mute and deafen. The call stays up while a game covers the app.
- Go Live: stream the game you are playing into the call (1280x720 H.264 at 30 fps and the
  game's sound, end-to-end encrypted). The picture comes from a Remote Play session with the
  console itself; the DualSense, plugged in by USB, plays the game through it and is your
  headset (its mic and its speaker or headphones) while you are live.

## Build

Builds on Linux with clang (no llvm-devel needed).

```sh
make deps                        # pinned ps5-payload-sdk, mbedTLS, zlib, cJSON, qrcodegen, SQLite
make dave                        # OpenSSL + mlspp + libdave into .deps/dave (slow; needs full perl)
make                             # build/ps5-discord.elf
make test                        # host self-checks (needs the app's deps: cd app && make deps)
cd app && make                   # app folder in app/dist/PPSA99072, core bundled (build it first)
make deploy PS5_HOST=<ps5-ip>    # stop the running copy, send the new one to elfldr (9021)
make stop PS5_HOST=<ps5-ip>
```

`make host` builds the same core for Linux (state in the current directory) for
testing against Discord without the console.

**Voice needs libdave.** Since March 2026 Discord refuses voice calls (all but stage
channels) from clients without DAVE end-to-end encryption. `make deps` does not fetch
it: run `make dave` once per machine. Without `.deps/dave` the build still succeeds and
the tests still pass, but it links a no-DAVE stub and every voice join fails (close code
4017). Before any release or a build on a fresh machine, run `make dave` and check the
"building the no-DAVE stub" warning does not appear.

## Use

1. Copy the `PPSA99072` folder from the app zip to `/data/homebrew/` on the console (FTP).
2. Run kstuff and ShadowMount so the folder shows up as a title, then launch it from the
   home screen. The app starts the core itself through elfldr (port 9021), so elfldr must
   be running.
3. Scan the QR code it shows: Discord app, Settings, Scan QR Code.
4. If QR login fails, press Cross on **Try browser login**. Sign in on Discord's website,
   complete any verification, then close the browser to return. The core imports and
   validates the active console user's browser session automatically, including an
   existing signed-in session. Closed the browser too soon? Cross opens it again; Square
   cancels and returns to QR login.
5. The session token is kept in `/data/ps5-discord/token`; later runs skip the QR.

Browser login is experimental on this branch. It waits up to ten minutes, reads
WebKit storage without modifying it, and saves the validated session atomically
with owner-only permissions. No watchdog is used. Tested with a real human
verification challenge.

The core still runs on its own too: send `ps5-discord-core-*.elf` to elfldr and use the
status page.

## Known limitations

- The token is stored unencrypted. Use a throwaway account until it is encrypted at rest.
- The status UI has no authentication; anyone on the LAN can view it or stop the payload.
  It never shows the token.
- Third-party clients break Discord's ToS and can get an account banned.
- Go Live needs the DualSense on USB (a Bluetooth controller is dropped by Remote Play) and a
  Remote Play pairing in `/data/ps5-discord/remoteplay.bin`, which the app cannot make yet.
  The quality is fixed at 720p30. Press PS after going off live to give the controller back.
- During a game the mic is only available through Go Live (the console refuses payloads the
  mic); outside it, voice needs the app on screen.

## Target

- Console firmware 13.00 (never install or trigger >= 14.00)

## Workflow

- `main` stays releasable; work on `feat/*` and `fix/*` branches, merged by PR.
- Versioning, release channels and milestones: [ROADMAP.md](ROADMAP.md). Changes: [CHANGELOG.md](CHANGELOG.md).


## License

ps5-discord is free software under the GNU Affero General Public License, version 3 or
later ([LICENSE](LICENSE)). The app builds on the ps5-homebrew-ui kit by BlackBearReloaded;
those files keep their GPL-3.0-or-later headers ([app/LICENSE.GPL-3.0](app/LICENSE.GPL-3.0),
[app/THIRD_PARTY_NOTICES.md](app/THIRD_PARTY_NOTICES.md)) and are combined with the AGPL code
under GPLv3 section 13. Because the core serves a status page over the network, that page
links to this source.

The core includes the Remote Play client from chiaki-ng (`third_party/`, see
[third_party/README.md](third_party/README.md)), which is AGPL-3.0-only, so the core as built
is distributed under AGPL-3.0-only as a whole.
