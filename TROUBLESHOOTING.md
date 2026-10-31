# Troubleshooting

Find what you see below. The app's version is in Settings (Options on Home), About; put it
in any bug report. Tested on firmware 13.00 only.

- [The app does not start](#the-app-does-not-start)
- [Signing in](#signing-in)
- [Connection](#connection)
- [Voice](#voice)
- [Go Live](#go-live)
- [Notifications](#notifications)
- [Updates](#updates)
- [Logs](#logs)
- [Starting over or removing the app](#starting-over-or-removing-the-app)
- [Reporting a bug](#reporting-a-bug)

## The app does not start

**The app is not on the home screen.** Run kstuff and ShadowMount after each console boot;
ShadowMount is what makes `/data/homebrew/PPSA99072` show up as a title. Check the folder is
there and holds `eboot.bin` and `sce_sys/` directly, not one folder deeper
(`/data/homebrew/PPSA99072/PPSA99072/`).

**CE-107750-0 when you open it.** The console starts an app only when every file in its
folder is open to all users (0777). Some FTP clients and `rsync`/`scp` set other modes. Copy
the folder again with an FTP client that keeps the zip's modes, or set the modes to 777
on the whole folder (most FTP clients: select it, Permissions, 777, recursive).

**It stays at "Starting the ps5-discord core...".** The app starts its background core by
sending it to elfldr on port 9021. Run elfldr (it comes with most payload loaders) and open
the app again. If another payload already uses port 9041, the core cannot start: stop it.

**"Updating the ps5-discord core..." for a long time.** After an update the old core is
still running; the app stops it and starts its own. During a call or Go Live this waits for
them to end (up to 30 s for Go Live to give the controller back).

**"This app needs core API ...".** The running core is too old or too new for this app.
Settings, Account, **Quit** stops it; open the app again and it starts its own. Without the
app, a reboot also stops it.

## Signing in

**The QR code does not work, or Discord asks for a captcha.** The app cannot show Discord's
captcha. Press Cross on **Try browser login**, sign in on Discord's website, complete any
check, then close the browser to come back. Closed it too soon? Cross opens it again;
Square goes back to the QR code.

**"Discord closed the session."** You were signed out elsewhere (a password change, Discord's
security check, or "Log out all devices"). Quit the app, open it again and sign in.

**Someone else's account shows up.** Each PS5 user has their own sign-in: the app follows
the user who is on screen. Switch to your PS5 user, or sign in with your own account there.

**Signing out.** Settings, Account, **Sign out** ends the session with Discord too and goes
back to the QR code.

## Connection

**"Reconnecting..." and an attempt count.** Discord cannot be reached: check the console's
network (Settings, Network, Test Internet Connection on the PS5). **Retry now** tries at
once instead of waiting.

**A DNS blocker or custom DNS on the console.** The app needs `discord.com`,
`gateway.discord.gg`, `cdn.discordapp.com`, `media.discordapp.net` and, for voice,
Discord's voice servers. Updates need `api.github.com`, `github.com` and
`release-assets.githubusercontent.com`. Blocking PlayStation servers is fine.

## Voice

How calls work, step by step: [VOICE_AND_GO_LIVE.md](VOICE_AND_GO_LIVE.md).

**Others cannot hear you.** The app's microphone works while the app is on screen. During a
game the console does not give the mic to the app: use Go Live, where the USB DualSense is
your headset. Check mute (the room's mic button) and Settings, Voice & audio, Mic gain.

**You cannot hear the call.** Check deafen and Settings, Voice & audio, Call volume. Outside
Go Live the call plays while the app is on screen.

**Joining fails at once.** If you built the core yourself: voice needs libdave (`make
dave`); without it every join fails with close code 4017. Release builds include it.

## Go Live

**Go Live does not start.**
- The DualSense must be plugged in by USB. A Bluetooth controller does not work for Go Live.
- Remote Play must be on (PS5 Settings, System, Remote Play) for a user signed in to PSN.
- Pair once: Settings, Voice & audio, Go Live pairing. Pair again if you removed the device
  from the PS5's Remote Play list.
- The steps: [VOICE_AND_GO_LIVE.md](VOICE_AND_GO_LIVE.md#go-live).
- Above 720p · 30 fps needs Nitro or a boosted server; 720p · 30 fps is the safe choice.

**The second Go Live does not start.** After a Go Live, unplug the controller and plug it
back in (the app asks you to).

**The controller does not work after a Go Live.** Unplug it and plug it back in. If the core
was stopped while it held the controller, a reboot frees it.

## Notifications

**No PS5 alerts for DMs or mentions.** The core keeps running in the background after you
close the app, and it shows the PS5's alerts while the app is not in front. Check:
- Settings, Notifications: Direct messages and mentions, Server messages, PS5 alerts while
  playing, Alerts during Go Live and Quiet hours.
- The channel or server is not muted in Discord (the app follows Discord's mutes).
- You did not use Settings, Account, **Quit**: that stops the core too, and with it the
  alerts, until you open the app again.

## Updates

Updates are in Settings, About, Updates. The app installs only releases from this project's
GitHub page that are signed with the project's keys.

| You see | What to do |
|---|---|
| Could not reach GitHub | Check the network and any DNS blocker (see [Connection](#connection)). Try again later: some networks cannot reach one of GitHub's download servers for a while; the app tries the others. |
| GitHub is busy; try later | GitHub allows 60 checks an hour from one address. Wait an hour. |
| Up to date | There is no newer release. An alpha or beta is offered prereleases; a stable version only stable releases. |
| Not enough free space | An update needs about three times its size free (around 100 MB) on the console's storage. |
| Download failed / Download does not match the release | Press Update again. |
| The app is still open | Restart now closes the app by itself; if you opened it again in the few seconds after, close it and press Restart now again. |
| vX must be installed by hand | This version cannot update to that one: install it by FTP. |
| Unpacking failed: ... | Press Update again. If it repeats, install by FTP and report the text. |
| Rollback failed: install a release by FTP | Install a release by FTP (below). |

**The new version does not open.** Nothing rolls back by itself. Copy the `PPSA99072` folder
from a release zip over `/data/homebrew/PPSA99072` by FTP (any version you like), then open
the app.

**Roll back.** Settings, About, **Previous version, Roll back** puts back the version you had
before the last update; the app closes and you open it again. It is there until the next
update. A rolled-back version is not offered when the app starts, but Check offers it again.

The update's files are in `/data/homebrew/.PPSA99072.update` (the download, and the previous
version until the next update). Deleting that folder while nothing is downloading or
installing is safe; it also drops the Roll back.

## Logs

The core and the app write to the console's kernel log: lines start with `[discord]` (the
core) and `[CLIENT]` (the app). With a klog server payload running you can read it from a PC,
for example `nc <ps5-ip> 3232`. Lines like these tell most of the story:

```
[discord] starting, version 0.5.0-alpha.1
[discord] update: check: Could not reach GitHub
[discord] tls: connect to release-assets.githubusercontent.com (185.199.109.133, 1 of 4) failed: Operation timed out
[discord] crashed: signal 11
```

A log can hold names and message text from your chats: read it before you post it.

## Starting over or removing the app

With the app closed (and the core stopped: Settings, Account, Quit, or a reboot), delete by
FTP:

- `/data/ps5-discord`: every PS5 user's sign-in, settings and the Remote Play pairing.
  Deleting it signs everyone out; the app starts at the QR code.
- `/data/homebrew/.PPSA99072.update`: updates in progress and the previous version.
- `/data/homebrew/PPSA99072`: the app itself.

Sign out first if you want Discord to end the session too (Settings, Account, Sign out).

## Reporting a bug

Open an issue on [GitHub](https://github.com/abhi245y/ps5-discord/issues) with:
- the version (Settings, About) and the console's firmware,
- what you did, what you expected and what happened,
- the log lines around it, if you can get them.

Never post the files from `/data/ps5-discord`: they hold your sign-in.
