# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org) as described in [ROADMAP.md](ROADMAP.md).

## [Unreleased]

### Added
- Voice: join server voice channels and DM calls (DAVE end-to-end encryption), mute and
  deafen, two-way audio with the app on screen; the call stays up while a game runs.
- Go Live: the voice room's Go Live button streams the game into the call (720p30 H.264 and
  game sound over a loopback Remote Play session, vendored chiaki-ng). The DualSense on USB
  plays the game through the session and is the call's headset meanwhile.
- tools/ds5-usb-bridge: a payload runtime that owns a USB DualSense (buttons, mic, speaker,
  headphones, mute button).
- Settings (Options on Home): sign out (ends the session with Discord too), quit, Go Live
  pairing per console user without typing a PIN, Go Live quality (720p or 1080p, 30 or 60
  fps), mic gain, notification choices, interface sounds and motion.
- Notifications: banners stack in the app; while it is not in front (home screen, a game,
  the PS menu) the PS5's own notifications show messages, rings and Go Live stopping, with
  the sender's avatar. Discord's own mutes and per-server levels are followed; quiet hours;
  hidden during Go Live unless allowed.
- A Discord sign-in per console user: the core runs the account of the user on screen and
  switches when another user comes on screen.
- Chat: View image in a message's menu shows its pictures full screen.
- Emoji in colour, as Discord draws them (Twemoji, baked in; skin tones and newer ones from
  the network), in messages, reactions, the keyboard and the message bar. A message of
  emoji alone shows them large.
- Emoji picker (L3 in a chat, R1 or the Emoji key in the keyboard): recently used, the
  servers' own emoji and Unicode's groups; emoji are added to the text and the picker stays
  open for more. Without Nitro, other servers' emoji sit at the bottom, marked as needing it.
- Reactions: React, or More at the end of the quick reactions, opens the picker (server emoji
  too); a message's menu lists each of its reactions to add or remove.
- Stickers: shown in messages (Discord's animated Lottie ones through ThorVG, GIF and PNG
  ones too) and sent from the picker's Stickers tab (Square), Discord's packs and the
  servers' own.
- Settings, Voice & audio: Call volume sets how loud the call plays.
- Go Live quality: choosing more than 720p · 30 fps first says it is for Nitro or boosted
  servers and recommends 720p · 30 fps.
- Go Live: the controller's lightbar turns teal (a colour the console never gives a pad)
  with player 1's light while Go Live holds it; before going live again with a controller
  Go Live already used, the app asks to unplug and replug it first (else a PS press during
  Go Live gives the console a second pad with no user, and choosing one ends the stream).
- One VERSION file names the core and the app (Settings, About shows it; the core's
  /api/state reports it); `make release` builds both in release mode into dist/ with
  SHA256SUMS.

### Changed
- Chat: what you write waits in the message bar; the keyboard's R2 is Done, R2 in the chat
  sends it. Triangle edits it again; a reply shows above the bar meanwhile. The quick replies
  are gone, and the bar's plus (it did nothing) is an emoji face for L3.
- Pictures (avatars, emoji, stickers) download four at a time.
- Button hints: L1, R1, L2 and R2 are plain bordered keys, all four alike, as the
  framework draws them; the message bar's hints sit between dividers.
- Server chat: the channel list stays at the left as a card (server icon and name, unread
  counts), with the call's dock under it while in voice (who is in it, Mute, Deafen,
  Leave); the channel chips are gone. Left or Options moves into the list, Right or Circle
  back to the chat. A read-only channel focuses its newest message.
- The core keeps running when the app is closed with the PS button (for notifications and
  calls); Quit (Circle on Home) stops it.
- License: ps5-discord's own code is now AGPL-3.0-or-later (was GPL-3.0-or-later), so
  Remote Play code from chiaki-ng can ship in it. Files from the ps5-homebrew-ui kit keep
  GPL-3.0-or-later. The status page links to the source.
- Lighter on big accounts: /api/state carries the QR code only while signing in and the
  log only in development builds; /api/chat answers "not modified" while nothing changed, the
  app refreshes it at most once a second after events, and the core keeps only the people
  the app shows (friends, DMs, voice, calls). Core API version 4.
- Controller audio without Go Live (parked) is left out of the build unless
  `CONTROLLER_VOICE=1`; the never-built console keyboard reader is removed.
- Build: `-Wextra` everywhere, our sources build with `-Werror` in the tests, stack
  protection and format checks in the core and the app; CI runs `make test` on every push.
  `make deploy` checks that the console then runs the new core.
- Not yet (carried to the next pass): the app still copies its whole snapshot on each
  change (audit M26, deferred: changes are now rarer); the call audio link on port 9042
  takes the newest client without a secret (M27, a decision before beta); tests for
  gateway resume versus identify, the heartbeat timeout and Go Live teardown.

### Security
- Voice: the account joining voice on another device (a phone) ends the call here instead
  of being taken for a join of this console's.
- The core trusts one pinned set of root certificates (curl's extract of Mozilla's,
  2026-10-20), the same on every build machine.
- A sign-in file cut short or holding junk is refused instead of read as an old plain one.
- The Discord sign-in is stored encrypted to the console (AES-256-GCM, key from the
  console's OpenPSID, the file name authenticated): a copy of the file is no use elsewhere.
  A sign-in saved in the clear by an older build is encrypted on first start.
- The core answers only the app on the console: the LAN status page is gone, and nothing
  on the network can reach port 9041 (development builds can open it with `CORE_LAN=1`).
- Hostile sizes are refused: WebSocket messages and REST replies over 16 MiB, pictures over
  4096 px a side, GIF stickers over 512x512, Lottie stickers over 1 MiB. The image proxy
  matches Discord's hosts by suffix and refuses CR, LF or spaces in a URL.
- During a DAVE group reset nothing is sent until the new group is ready (was: audio
  without end-to-end encryption for a moment).

### Fixed
- Chats: after a long game (events missed) or a core restart the chats reload; a channel
  reopened keeps what arrived meanwhile; a send with no answer in time waits for its echo
  instead of showing twice; a message that never reached Discord can be deleted; focus stays
  on its message when the list changes; mention counts include @everyone and roles, and
  every DM message; a DM is listed once and group DMs appear when created.
- Pictures: a failed one is retried after 10 s, and the app keeps them within 256 MB (the
  least recently shown go first); the sticker picker frees its previews when it closes.
- Voice: calls of up to 99 people; a new DAVE epoch takes effect when Discord executes it;
  a join is never left hanging with the app closed.
- Go Live: stopping and starting again at once keeps the new stream; the sound no longer
  drifts from the picture over a long stream; one failed USB transfer no longer ends it, and
  the controller is re-enumerated (or you are told to replug it) whenever it was detached.
- Calls survive a network drop: a short one resumes, and the call's audio socket (which the
  console aborts with the network) is replaced, so sound comes back both ways; a long one
  rejoins the call once Discord is back. Losing the network no longer counts against the
  voice server's retries (up to 10 minutes). A call Discord still lists after a drop can be
  joined again.
- Reconnecting: one card over the screen that was up (No internet connection, Can't reach
  Discord, Restarting the core), the attempt, Retry now and Settings. The launch keeps its
  own Starting / Connecting card.
- The core reopens its control port when the console aborts it (Wi-Fi off): the app no
  longer starts a second core beside a running one.
- Nothing goes out on a new gateway connection before Discord resumes it (a call's rejoin
  was lost with a 4003 close).
- Network calls give up in time: 10 s to connect, 15 s for a stalled read or write; the
  control API answers busy past 16 requests instead of stalling.
- The app replaces a running core of another version after an update (not during a call);
  it restarts the core only after three missed checks, and clears another account's data.
- A channel switch Discord refuses keeps the current call; a failed call is not rejoined in a
  loop; two quick Go Live presses start one session; the controller is handed back if the
  core crashes or its USB stop hangs; Quit sends the leave before exiting.
- A join started while a chat is loading no longer reports a failure.
- GIF stickers are capped by frame count before decoding; a big account's chat list is
  allowed up to 256 MiB (pictures stay at 16 MiB).
- Pictures in messages showed their stand-in: Discord's media proxy sends WebP unless asked,
  and the app reads PNG and JPEG; it now asks for those.
- A call opens its screen only once connected, with a spinner meanwhile (Circle cancels);
  Leave goes back only once the call is over. A failed join says why.
- The app crashed on every start while one of your servers was unavailable at Discord.
- TLS on several threads at once (pictures loading while a message is sent) could fail or
  crash: mbedTLS is now built thread-safe.
- A session Discord ends for good (signed out elsewhere) gives the USB controller back
  first, and other fatal closes say why instead of spinning.
- The gateway no longer reconnects every second when Discord closes right after HELLO.
- Go Live that ends by itself also stops the stream in the call (no frozen picture).
- The message menu no longer reads past the list when its message is deleted elsewhere.
- A core started before anyone signs in to the console waits for them, instead of filing
  the sign-in under no user.
- `make release` stops when libdave is missing (voice would fail with 4017).
- Quit waits for Go Live to give the USB controller back before the core exits (up to 30 s,
  was 10 s), and Go Live stopped while Remote Play is connecting no longer takes it.
- Editing a message with server emoji showed them as raw <:name:id>.
- Chat: a DM bubble's reply quote, reactions and "(edited)" ran past the bubble and off the
  screen.
- Voice: our voice reached others with seconds of delay after pauses (the RTP clock stood
  still while we were silent).

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
