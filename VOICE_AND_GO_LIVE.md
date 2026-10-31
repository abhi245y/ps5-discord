# Voice calls and Go Live

How to talk in Discord voice from the PS5, and how to stream your game into the call.
Something going wrong? See [TROUBLESHOOTING.md](TROUBLESHOOTING.md#voice).

![A voice room](screenshots/10-voice-room.jpg)

## Voice calls

### Joining

- **A server's voice room:** Home, under **Live rooms** (rooms with people in them), or open
  a server and pick a room under **Voice rooms**. Cross joins.
- **A DM call:** open the DM and press Square (**Call**). When someone calls you, the app
  shows **Incoming call**: Cross answers, Circle declines.

The room shows who is in it and who is speaking, your mic level, and the connection with its
ping (top right). Calls are end-to-end encrypted (Discord's DAVE), like on the desktop app.

### In the room

| Button | Does |
|---|---|
| Square | Mute or unmute your mic |
| Triangle | Deafen: you hear nothing |
| Cross on **Go Live** | Stream your game into the call ([below](#go-live)) |
| Cross on **Leave** | Leave the call |
| Circle | Back to the app; the call stays up |

You can chat while you talk: in a server's chat the call sits under the channel list
(Unmute, Deafen, Leave), the top right shows who is in it, and Square (**Voice**) opens the
room.

![Chatting while in a call](screenshots/13-chat-in-call.jpg)

Settings, **Voice & audio**: **Mic gain** (how loud you are; join a call to see your level
there) and **Call volume** (how loud the call is for you).

### Things to know

- **The app must be on screen for you to talk and hear.** You can leave the call running and
  use the rest of the app. When a game is in front, the console does not give the app the
  mic or the sound, so the call stays connected but you are silent and hear nothing. To talk
  while you play, use [Go Live](#go-live): there the USB controller is your headset.
- **What ends a call:** Leave, Settings, Account, **Sign out** or **Quit Discord**, and
  **Restart now** or **Roll back** in Settings, About. Closing the app with the PS button does
  not end it.

## Go Live

Go Live streams what the PS5 shows into the call, picture and game sound, end-to-end
encrypted. Viewers watch it like any Discord stream, on a PC or phone.

How it works: the core opens a Remote Play session with your own console to get the picture.
For as long as you are live, the **DualSense on USB plays the game through that session and
is your headset**: its mic for your voice, its speaker or the headphones plugged into it for
the call and the game.

### Before the first time

1. **A USB cable for the DualSense.** Go Live does not work with a controller on Bluetooth.
2. **Remote Play on:** PS5 Settings, System, Remote Play, Enable Remote Play. The console
   user must be signed in to PSN.
3. **Pair Go Live once per console user:** in the app, Settings, **Voice & audio**, **Go Live
   pairing**. Pick yourself (only the user on screen can pair). The app asks the PS5 for a
   code by itself; if that fails, Square (**Enter a PIN**) and type the 8 digits from PS5
   Settings, System, Remote Play, **Link Device**.
4. **Pick a quality:** Settings, Voice & audio, **Go Live quality**. 720p · 30 fps works for
   everyone. 720p · 60, 1080p · 30 and 1080p · 60 need Nitro or a boosted server, and Discord
   may act on an account that streams above its limits.

### Going live

1. Plug the DualSense in by USB.
2. Join a voice room or a DM call.
3. In the room, Cross on **Go Live**. The room shows "Going live…", then "Live · the controller
   is the headset and plays the game". The controller's lightbar turns teal with player 1's
   light while Go Live holds it.
4. Go to your game and play. Your friends see it in the call.
5. To stop, come back to the room and press **Stop live**, or Leave.

### Things to know

- **Viewers see the screen, not only the game:** the app, menus and PS5 alerts too. Settings,
  Notifications, **Alerts during Go Live** is off unless you turn it on, so message alerts
  stay off your stream. Games and scenes that block Remote Play or screen sharing may show viewers a
  black picture.
- **Replug the controller between Go Lives.** When Go Live ends, it gives the controller
  back to the console. Before the next Go Live, unplug its USB cable and plug it back in; the
  app asks you to. If you skip it, pressing the PS button while live can make the console ask
  "Who's using this controller?", and answering ends the stream.
- **Your PS button and controller** go through Remote Play while you are live, so everything
  takes a moment longer than usual.
- **Network:** a stream at 720p · 30 fps sends about 2.5 to 5 Mbit/s. A wired connection
  helps.
- **When Go Live stops by itself** (the network drops, Remote Play ends), the app shows why in
  the room and the stream ends in the call too. Replug the controller and try again.
- **If the controller is dead after a Go Live**, unplug it and plug it back in. If the core
  was stopped while it held the controller, a reboot frees it.
