// ps5-discord - What the screens show: one immutable snapshot of the account.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Filled from the core's control API on the console (/api/chat, /api/events,
// /api/messages) and from discord/sample.cpp in host snapshots. Screens only
// read it; changes arrive as a new snapshot.

#pragma once

#include <cstdint>
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
    int unread = 0;
    bool mentioned = false;
};

struct Guild
{
    std::string id;
    std::string name;
    std::string icon_url;
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
    std::int64_t now_ms = 0; // for "12m ago"
    User me;
    std::vector<User> friends; // everyone the Home rows can show
    std::vector<Room> live_rooms;
    std::vector<Conversation> recent;
    std::vector<Guild> guilds;
    std::vector<Mention> mentions;
    Voice voice;
    int friends_online = 0;
    int unread_total = 0;
    int mention_count = 0; // unread mentions
};

const User *find_user(const Snapshot &snapshot, const std::string &id);

} // namespace discord
