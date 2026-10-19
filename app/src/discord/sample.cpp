// ps5-discord - Synthetic account for host snapshots and the tour.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mirrors the design framework's busy account (approved Home comp): every
// person, server and message here is invented.

#include "discord/sample.hpp"

namespace discord
{

const User *find_user(const Snapshot &snapshot, const std::string &id)
{
    if (snapshot.me.id == id)
        return &snapshot.me;
    for (const User &user : snapshot.friends)
        if (user.id == id)
            return &user;
    return nullptr;
}

namespace
{

constexpr std::int64_t kNow = 1792400000000; // fixed, so pictures do not change
constexpr std::int64_t kMinute = 60000;

User person(const char *id, const char *name, ArtIndex art, Presence presence, const char *activity = "",
            const char *voice_room = "", bool speaking = false, bool muted = false)
{
    User user;
    user.id = id;
    user.name = name;
    user.art = art;
    user.presence = presence;
    user.activity = activity;
    user.voice_room = voice_room;
    user.speaking = speaking;
    user.muted = muted;
    return user;
}

} // namespace

Snapshot sample_snapshot()
{
    Snapshot s;
    s.connection = Connection::ready;
    s.now_ms = kNow;
    s.me = person("me", "Starcraft", 8, Presence::online, "", "Raid Night", false, true);

    s.friends = {
        person("mira", "Mira", 0, Presence::online, "", "Raid Night", true),
        person("theo", "Theo", 1, Presence::online, "Playing a game"),
        person("kenji", "Kenji", 2, Presence::online),
        person("ana", "Ana", 3, Presence::online, "", "Raid Night", true),
        person("jo", "Jo", 4, Presence::idle),
        person("sam", "Sam", 5, Presence::online, "Playing a game"),
        person("lily", "Lily", 6, Presence::online, "", "Raid Night"),
        person("rin", "Rin", 7, Presence::online),
        person("alex", "Alex", 9, Presence::idle),
        person("nova", "Nova", 10, Presence::online, "", "Raid Night"),
        person("zed", "Zed", 11, Presence::online),
    };
    s.friends_online = 48;

    s.voice = {true, "Raid Night", "Night Shift", 18, true, false};

    Room raid{"raid", "nightshift", "Night Shift", "Raid Night", 0, {"mira", "ana", "lily", "nova"}, 18};
    Room race{"race", "speedrun", "Speedrun Lab", "Race Room", 1, {"theo", "sam", "kenji"}, 6};
    Room chill{"chill", "coop", "Co-op Club", "Chill", 2, {"kenji", "ana", "jo", "sam"}, 9};
    Room lake{"lake", "photo", "Photo Mode", "Golden Hour", 3, {"rin", "zed"}, 4};
    s.live_rooms = {raid, race, chill, lake};

    s.recent = {
        {"general", "nightshift", "#general", "Mira: lobby up at 9", "", kNow - 12 * kMinute, 99, false},
        {"gamenight", "nightshift", "@you in #game-night", "Kenji: are you in?", "kenji", kNow - 4 * kMinute, 7, true},
        {"dm-theo", "", "Theo", "Theo: bring snacks", "theo", kNow - 42 * kMinute, 3, false},
        {"clips", "nightshift", "#clips", "Kenji posted a clip", "", kNow - 120 * kMinute, 12, false},
    };
    s.unread_total = 23;

    auto guild = [](const char *id, const char *name, ArtIndex art, int online, int members, int unread) {
        Guild g;
        g.id = id;
        g.name = name;
        g.art = art;
        g.online = online;
        g.members = members;
        g.unread = unread;
        return g;
    };
    Guild night = guild("nightshift", "Night Shift", 0, 48, 312, 23);
    night.channels = {
        {"announcements", "announcements", ChannelKind::announcement, "Updates and news", 0, false},
        {"general", "general", ChannelKind::text, "Chat about anything", 99, false},
        {"gamenight", "game-night", ChannelKind::text, "Friday plans", 7, true},
        {"clips", "clips", ChannelKind::text, "Post your best moments", 12, false},
        {"builds", "builds", ChannelKind::text, "Share your setups", 0, false},
    };
    night.rooms = {raid, Room{"lounge", "nightshift", "Night Shift", "Lounge", 4, {"theo", "kenji", "sam"}, 3},
                   Room{"afk", "nightshift", "Night Shift", "AFK", 5, {}, 0}};
    s.guilds = {night,
                guild("speedrun", "Speedrun Lab", 1, 31, 204, 7),
                guild("coop", "Co-op Club", 2, 22, 146, 12),
                guild("kitchen", "Pixel Kitchen", 3, 9, 88, 5),
                guild("photo", "Photo Mode", 4, 14, 120, 3),
                guild("lofi", "Lo-fi Room", 5, 40, 530, 9)};

    s.mentions = {
        {"m1", "gamenight", "mira", "@you in #game-night · Night Shift", "are you in for the raid?", kNow - 4 * kMinute, true},
        {"m2", "clips", "theo", "@you in #clips · Night Shift", "check the clip I posted", kNow - 12 * kMinute, true},
        {"m3", "gamenight", "kenji", "@you in #game-night · Night Shift", "can you bring the party code?", kNow - 35 * kMinute, true},
        {"m4", "general", "ana", "@you in #general · Night Shift", "nice play earlier!", kNow - 60 * kMinute, false},
        {"m5", "general", "jo", "@you in #general · Night Shift", "I'll be on in 10", kNow - 120 * kMinute, false},
    };
    s.mention_count = 3; // unread mentions, as the Mentions chip counts them
    return s;
}

} // namespace discord
