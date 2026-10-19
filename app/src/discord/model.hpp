// ps5-discord - What the screens show: one immutable snapshot of the account.
// SPDX-License-Identifier: GPL-3.0-or-later
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
    std::string url;  // through the core's image proxy; empty in samples
    std::string size_text; // "1.8 MB"
    bool image = false;
    ArtIndex art = 0; // stand-in thumbnail while the image loads (and in samples)
};

struct Reaction
{
    std::string emoji; // the Unicode emoji, or ":name:" for a server's own
    std::string emoji_id; // a server emoji's id, empty for Unicode
    int count = 0;
    bool me = false; // this account reacted
};

struct Message
{
    std::string id;
    std::string channel_id;
    std::string author_id;
    std::string author_name; // as sent, for authors not in the friends list
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
};

struct Voice // the account's own voice connection
{
    bool connected = false;
    std::string room_name;
    std::string guild_name;
    int member_count = 0;
    bool muted = false;
    bool deafened = false;
};

enum class Connection : std::uint8_t
{
    starting,
    signing_in, // QR on screen
    connecting,
    ready,
    reconnecting,
};

struct Snapshot
{
    Connection connection = Connection::starting;
    std::string status_message; // why we are starting or reconnecting, for the user
    // While signing in: the core's login QR, row-major '1' = dark module.
    std::string login_qr;
    int login_qr_size = 0;
    bool login_scanned = false; // waiting for approval on the phone
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
    std::map<std::string, std::string> dm_by_user; // one-to-one DM channel by the other person's id
    Voice voice;
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

} // namespace discord
