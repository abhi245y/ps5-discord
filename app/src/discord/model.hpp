// ps5-discord - What the screens show: one immutable snapshot of the account.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Filled from the core's control API on the console (/api/chat, /api/events,
// /api/messages) and from discord/sample.cpp in host snapshots. Screens only
// read it; changes arrive as a new snapshot.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace discord
{

enum class Presence : std::uint8_t
{
    online,
    idle,
    dnd,
    offline,
};

// Placeholder artwork shipped with the app: Discord gives voice rooms no art
// and many users and servers no image, so each gets a themed scene by index.
using ArtIndex = int;

struct User
{
    std::string id;
    std::string name; // display name
    std::string avatar_url; // empty: themed placeholder with the initial
    ArtIndex art = 0;
    Presence presence = Presence::offline;
    std::string activity;   // "Playing a game", empty when none
    std::string voice_room; // room name when in voice, else empty
    bool speaking = false;
    bool muted = false;
    bool is_friend = false; // on the friends list (others are kept so names and faces resolve)
};

struct Room // a voice channel
{
    std::string id;
    std::string guild_id;
    std::string guild_name;
    std::string name;
    ArtIndex art = 0;
    std::vector<std::string> member_ids; // first few, for avatar stacks
    int member_count = 0;
};

enum class ChannelKind : std::uint8_t
{
    text,
    announcement,
    voice,
};

struct Channel
{
    std::string id;
    std::string name;
    ChannelKind kind = ChannelKind::text;
    std::string topic;
    int unread = 0;          // mentions of you, shown as a count
    bool has_unread = false; // newer messages than you have read, shown as a dot
    bool mentioned = false;
    bool can_send = true; // false: read-only for this account (announcements, rules)
    std::string category; // the category it sits under, empty at the top
    std::int64_t last_ms = 0; // when its newest message was sent
};

// A server's own emoji, for the picker. Without Nitro only the server's own (and none
// animated) can be sent there.
struct GuildEmoji
{
    std::string id;
    std::string name;
    bool animated = false;
    bool available = true; // false: the server lost the boost level it needs
};

struct Guild
{
    std::string id;
    std::string name;
    std::string icon_url;
    std::string banner_url; // the server's banner or invite splash, when it has one
    ArtIndex art = 0;
    std::vector<Channel> channels;
    std::vector<Room> rooms;
    int online = 0;
    int members = 0;
    int unread = 0;
    std::vector<GuildEmoji> emojis;
};

struct Conversation // a row in Recent conversations
{
    std::string channel_id;
    std::string guild_id; // empty for a DM
    std::string title;    // "#general" or the person's name
    std::string preview;  // "Mira: lobby up at 9"
    std::string user_id;  // the DM's other person, for the avatar
    std::int64_t time_ms = 0;
    int unread = 0;
    bool mention = false;
};

struct Mention
{
    std::string message_id;
    std::string channel_id;
    std::string author_id;
    std::string context; // "@you in #game-night · Night Shift"
    std::string content;
    std::int64_t time_ms = 0;
    bool unread = true;
    User author; // as the message carried it, for authors outside the friends list
};

struct Attachment
{
    std::string name; // "screenshot.png"
    std::string url;  // a small preview through the media proxy; empty in samples
    std::string full_url; // a screen-sized copy, for viewing it full screen
    std::string size_text; // "1.8 MB"
    bool image = false;
    ArtIndex art = 0; // stand-in thumbnail while the image loads (and in samples)
};

// A sticker sent in a message. format: 1 PNG, 2 APNG, 3 Lottie, 4 GIF.
struct Sticker
{
    std::string id;
    std::string name;
    int format = 1;
};

// Where its picture comes from: Lottie as JSON (drawn by ThorVG), GIF animated, the
// rest as a still from the media proxy.
inline std::string sticker_url(const Sticker &s)
{
    if (s.format == 3)
        return "https://cdn.discordapp.com/stickers/" + s.id + ".json";
    return "https://media.discordapp.net/stickers/" + s.id + (s.format == 4 ? ".gif" : ".png") + "?size=160";
}
inline constexpr int kStickerPixels = 160; // how large a Lottie sticker is rendered

struct Reaction
{
    std::string emoji; // the Unicode emoji, or ":name:" for a server's own
    std::string emoji_id; // a server emoji's id, empty for Unicode
    int count = 0;
    bool me = false; // this account reacted
};

// An emoji in a message's text, a server's (id) or a Unicode one (no id, name is the
// emoji itself). The text holds kEmojiMark (an em space, about an emoji wide) where each
// one goes, in order.
// ponytail: an em space someone typed reads as an emoji slot; nobody types one.
inline constexpr const char *kEmojiMark = "\xE2\x80\x83";
struct Emoji
{
    std::string id;
    std::string name;
};

struct Message
{
    std::string id;
    std::string channel_id;
    std::string author_id;
    std::string author_name; // as sent, for authors not in the friends list
    std::string author_avatar_url; // likewise: their picture, or Discord's default
    std::string content;
    std::int64_t time_ms = 0;
    bool mentions_me = false;
    bool edited = false;
    bool pending = false; // sent from this console, not yet echoed by the gateway
    bool failed = false;  // Discord refused it (shown as not sent)
    std::string nonce;    // pending messages: what the echo will carry
    std::string reply_author; // the message this one answers, when it is a reply
    std::string reply_text;
    std::vector<Attachment> attachments;
    std::vector<Reaction> reactions;
    std::vector<Emoji> emojis; // server emoji in content, in order
    std::vector<Sticker> stickers;
};

// The text with each server emoji as :name: (previews, toasts), or as Discord's
// <:name:id> markup (raw: to edit the message); Unicode emoji as themselves.
inline std::string plain_text(const Message &m, bool raw = false)
{
    std::string out = m.content;
    std::size_t at = 0;
    for (const Emoji &e : m.emojis)
    {
        at = out.find(kEmojiMark, at);
        if (at == std::string::npos)
            break;
        const std::string shown = e.id.empty() ? e.name // a Unicode emoji is itself
                                  : raw        ? "<:" + e.name + ":" + e.id + ">"
                                               : ":" + e.name + ":";
        out.replace(at, 3, shown);
        at += shown.size();
    }
    return out;
}

// The core's voice state (GET /api/voice, docs/voice-api.md).
enum class VoiceState : std::uint8_t
{
    idle,
    joining,
    connecting,
    securing,
    connected,
    error,
};

// The account's own voice connection: state and self mute from /api/voice,
// names and member count worked out from the chat tree.
// A console user and their Remote Play pairing for Go Live.
struct RemotePlayUser
{
    std::string id; // the console's user id, hex
    std::string name;
    bool account = false;   // has a PSN account id (Remote Play needs one)
    bool paired = false;
    bool on_screen = false; // the user signed in on screen
    std::string console;    // the paired console's name
    std::int64_t since = 0; // when it was paired, unix seconds
};

struct Voice
{
    VoiceState state = VoiceState::idle;
    std::string channel_id;   // the room or DM call we are in; empty when idle
    std::string guild_id;     // empty for a DM call
    std::string room_name;    // "Raid Night", or the other person's name in a DM call
    std::string guild_name;
    int member_count = 0;
    bool muted = false;
    bool deafened = false;
    int ping_ms = 0;           // voice gateway heartbeat, 0 when unknown
    bool secure = false;       // the DAVE session is up
    std::int64_t since_ms = 0; // when it became connected (epoch ms), 0 otherwise
    std::string error;         // a sentence, when state is error
    std::set<std::string> speaking; // user ids heard lately, us included while we send
    float mic_level = 0.0f;    // our own mic, 0..1 (the console's meter; host: sample)
    std::string controller;       // the USB controller as the call's audio: "off", "on", "missing"
    bool output_tv = true;        // the pick while the app is on screen: TV, or the controller
    std::string broadcast;        // Go Live: "off", "starting", "live", "error"
    std::string broadcast_error;  // why it stopped, when it did
    bool paired = false;          // Go Live's Remote Play pairing with the console exists
    std::string pair_state;       // "idle", "pairing", "paired", "need_pin", "error"
    std::string pair_error;
    std::string pair_console;     // the paired console's name, the console user, and when (unix s)
    std::string pair_user;
    std::int64_t pair_since = 0;
    std::vector<RemotePlayUser> rp_users;
    std::string rp_use; // the user Go Live runs as, "" when nobody is paired

    bool active() const
    {
        return state != VoiceState::idle;
    }
    bool dm() const // a DM call: no guild
    {
        return active() && guild_id.empty();
    }
};

// Someone in a voice channel (or a DM call).
struct VoiceSeat
{
    std::string user_id;
    bool muted = false;
    bool deafened = false;
    bool speaking = false; // the core heard them in the last ~300 ms (Voice::speaking)
};

// A DM call going on, or ringing (the chat tree's "calls").
struct Call
{
    std::string channel_id; // the DM
    std::string peer_id;    // the other person in the DM, empty if unknown
    std::vector<std::string> ringing; // user ids the call still rings
};

enum class Connection : std::uint8_t
{
    starting,
    signing_in, // QR on screen
    connecting,
    ready,
    reconnecting,
};

// A live message worth a banner, as the core judged it (Discord's mutes and levels,
// then Prefs): a DM, a mention, or a server message.
struct Notice
{
    std::uint64_t seq = 0; // grows with each notice; 0 = none yet
    std::string guild_id;  // empty for a DM
    std::string channel_id;
    std::string title;     // who wrote: "Abhi2nd"
    std::string place;     // "Raid Night · #general"; empty for a DM
    std::string text;
    User author;
    int kind = 0;          // 1 a server message, 2 a mention of you, 3 a DM
    bool for_me = false;   // a DM or a mention of you
    bool quiet = false;    // in the quiet hours: no sound
};

// The settings kept by the core (its /api/voice "prefs"), so they outlast the app.
struct Prefs
{
    int golive_quality = 0; // 0 720p30, 1 720p60, 2 1080p30, 3 1080p60
    int mic_gain = 5;       // 0..10, 5 = as captured
    bool notify_dm = true;
    int notify_server = 1;     // 0 off, 1 as set in Discord, 2 every message
    bool notify_system = true; // PS5 alerts while the app is not on screen
    bool notify_live = false;  // ... also while Go Live is on
    int quiet_hours = 0;       // 0 off, else a preset (screens/settings.cpp kQuiet)

    void set(const std::string &key, int value) // a set_pref request, applied here
    {
        if (key == "golive_quality")
            golive_quality = value;
        else if (key == "mic_gain")
            mic_gain = value;
        else if (key == "notify_dm")
            notify_dm = value != 0;
        else if (key == "notify_server")
            notify_server = value;
        else if (key == "notify_system")
            notify_system = value != 0;
        else if (key == "notify_live")
            notify_live = value != 0;
        else if (key == "quiet_hours")
            quiet_hours = value;
    }
};

struct Snapshot
{
    Connection connection = Connection::starting;
    std::string status_message; // why we are starting or reconnecting, for the user
    // While signing in: the core's login QR, row-major '1' = dark module.
    std::string login_qr;
    int login_qr_size = 0;
    bool login_scanned = false; // waiting for approval on the phone
    bool login_browser = false; // browser login/import attempt is active
    std::string login_note;     // why the last sign-in failed, from the core
    std::int64_t now_ms = 0; // for "12m ago"
    int utc_offset_minutes = 0; // the console's time zone, for clock times
    User me;
    std::vector<User> friends; // everyone the Home rows can show
    std::vector<Room> live_rooms;
    std::vector<Conversation> recent;
    std::vector<Guild> guilds;
    std::vector<Mention> mentions;
    // Loaded history by channel id, oldest first (DM channels included). Shared,
    // copy on write: handing the UI a snapshot copies pointers, not every message.
    std::map<std::string, std::shared_ptr<std::vector<Message>>> messages;
    std::set<std::string> no_access; // channels Discord refused to read (403)
    std::set<std::string> load_failed; // history that did not load (network, core); retried on open
    std::set<std::string> history_start; // channels loaded back to their first message
    std::map<std::string, std::string> dm_by_user; // one-to-one DM channel by the other person's id
    bool nitro = false;          // the account has Nitro: any server's emoji, animated too
    std::vector<Notice> notices; // the latest few, oldest first
    std::string sign_out_error;  // the last sign out failed, and why
    Prefs prefs;
    Voice voice;
    std::map<std::string, std::vector<VoiceSeat>> seats; // by channel id: rooms and DM calls
    std::vector<Call> calls;
    int friends_online = 0;
    int unread_total = 0;
    int mention_count = 0; // unread mentions
};

const User *find_user(const Snapshot &snapshot, const std::string &id);
// A channel's loaded history to change: a fresh copy, so snapshots already handed out stay as they were.
inline std::vector<Message> &edit_messages(Snapshot &s, const std::string &channel_id)
{
    std::shared_ptr<std::vector<Message>> &slot = s.messages[channel_id];
    slot = slot ? std::make_shared<std::vector<Message>>(*slot) : std::make_shared<std::vector<Message>>();
    return *slot;
}
const Guild *find_guild(const Snapshot &snapshot, const std::string &id);
// The DM call that rings us (we are in the ring's list), or nullptr.
const Call *ringing_call(const Snapshot &snapshot);

} // namespace discord
