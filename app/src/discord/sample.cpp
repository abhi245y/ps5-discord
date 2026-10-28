// ps5-discord - Synthetic account for host snapshots and the tour.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Mirrors the design framework's busy account (approved Home comp): every
// person, server and message here is invented.

// Sample data lists only the fields it sets.
#pragma clang diagnostic ignored "-Wmissing-field-initializers"

#include "discord/sample.hpp"
#include "discord/emoji.hpp"

#include <algorithm>

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

const Guild *find_guild(const Snapshot &snapshot, const std::string &id)
{
    for (const Guild &guild : snapshot.guilds)
        if (guild.id == id)
            return &guild;
    return nullptr;
}

const Call *ringing_call(const Snapshot &snapshot)
{
    for (const Call &call : snapshot.calls)
    {
        const bool rings_me = std::find(call.ringing.begin(), call.ringing.end(), snapshot.me.id) != call.ringing.end();
        const bool in_it = snapshot.voice.active() && snapshot.voice.channel_id == call.channel_id;
        if (rings_me && !in_it)
            return &call;
    }
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
    s.dm_by_user["theo"] = "dm-theo";

    // In a call: the raid room, unmuted (our mic meter shows), with the people who are in it.
    // Mira and Theo are heard: their speaking rings show in the room, the dock and Home.
    s.voice.state = VoiceState::connected;
    s.voice.channel_id = "raid";
    s.voice.guild_id = "nightshift";
    s.voice.room_name = "Raid Night";
    s.voice.guild_name = "Night Shift";
    s.voice.muted = false;
    s.voice.ping_ms = 42;
    s.voice.secure = true;
    // Two console users: the one on screen, and a second one for Go Live over Bluetooth.
    s.voice.rp_users = {{"10000000", "Ana", true, false, true, "", 0}, {"10000001", "Guest", true, false, false, "", 0}};
    s.voice.since_ms = kNow - 42 * kMinute;
    s.voice.speaking = {"mira", "theo"};
    s.voice.mic_level = 0.55f;
    s.seats["raid"] = {{"me"}, {"mira"}, {"theo"}, {"kenji", true, false}, {"ana"}, {"jo", false, true},
                       {"sam"}, {"lily"}, {"rin"}, {"alex"}, {"nova"}, {"zed"}};
    for (VoiceSeat &seat : s.seats["raid"])
        seat.speaking = s.voice.speaking.count(seat.user_id) > 0;
    for (User &u : s.friends)
        u.speaking = s.voice.speaking.count(u.id) > 0;
    s.voice.member_count = static_cast<int>(s.seats["raid"].size());

    Room raid{"raid", "nightshift", "Night Shift", "Raid Night", 0, {"mira", "ana", "lily", "nova"}, 12};
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

    auto msg = [](const char *id, const char *channel, const char *author, const char *content, std::int64_t time,
                  bool mentions_me = false) {
        Message m;
        m.id = id;
        m.channel_id = channel;
        m.author_id = author;
        m.content = content;
        m.time_ms = time;
        m.mentions_me = mentions_me;
        return m;
    };
    std::vector<Message> &general = edit_messages(s, "general");
    general = {
        msg("g0", "general", "rin", "anyone tried the new photo mode filters?", kNow - 70 * kMinute),
        msg("g1", "general", "zed", "yeah the film grain one is great for night shots", kNow - 66 * kMinute),
        msg("g2", "general", "mira", "Lobby is up for tonight. Starting around 9 if everyone is in.", kNow - 26 * kMinute),
        msg("g3", "general", "theo", "In. Bringing snacks and bad aim.", kNow - 24 * kMinute),
        msg("g4", "general", "kenji", "@Starcraft are you in for the raid?", kNow - 23 * kMinute, true),
        msg("g5", "general", "ana", "", kNow - 21 * kMinute),
        msg("g6", "general", "mira", "Deal. Party code in a minute.", kNow - 20 * kMinute),
    };
    Attachment shot;
    shot.name = "screenshot.png";
    shot.size_text = "1.8 MB";
    shot.image = true;
    shot.art = 0;
    general[5].attachments.push_back(shot);
    general[6].edited = true;

    s.messages["gamenight"] = std::make_shared<std::vector<Message>>(std::vector<Message>{
        msg("n0", "gamenight", "mira", "Raid at 9, then whatever people want.", kNow - 50 * kMinute),
        msg("n1", "gamenight", "kenji", "@Starcraft can you bring the party code?", kNow - 35 * kMinute, true),
        msg("n2", "gamenight", "ana", "voting for the cave map after", kNow - 5 * kMinute),
        msg("n3", "gamenight", "kenji", "are you in?", kNow - 4 * kMinute, true),
    });
    s.messages["dm-theo"] = std::make_shared<std::vector<Message>>(std::vector<Message>{
        msg("d0", "dm-theo", "theo", "Lobby is up for tonight. Starting around 9 if everyone is in.", kNow - 55 * kMinute),
        msg("d1", "dm-theo", "me", "I'll be there! Finishing dinner in a bit.", kNow - 54 * kMinute),
        msg("d2", "dm-theo", "theo", "We can try the new route on the left side. Looks super chill.", kNow - 50 * kMinute),
        msg("d3", "dm-theo", "me", "Looks amazing! Count me in.", kNow - 49 * kMinute),
        msg("d4", "dm-theo", "theo", "bring snacks 🍕🍿 and good vibes 😎👍🏽", kNow - 42 * kMinute),
        // The cases that once spilled out of a bubble: a short reply to a long message, a
        // short message with many reactions, a word longer than the bubble, an edit.
        msg("d5", "dm-theo", "me", "ok!", kNow - 41 * kMinute),
        msg("d6", "dm-theo", "theo", "https://clips.example.com/watch/ThisIsAVeryLongClipIdentifierWithoutAnySpaces_8f3a91c2d7e4",
            kNow - 40 * kMinute),
        msg("d7", "dm-theo", "me", "same, see you at nine", kNow - 39 * kMinute),
        msg("d8", "dm-theo", "theo", "", kNow - 38 * kMinute),
    });
    {
        std::vector<Message> &dm = *s.messages["dm-theo"];
        dm[5].reply_author = "Theo";
        dm[5].reply_text = "We can try the new route on the left side. Looks super chill.";
        dm[5].reactions = {{"🔥", "", 3, true}, {"😂", "", 2}, {"👍", "", 5}, {"🎮", "", 1}, {"🍕", "", 2}};
        dm[7].edited = true;
        dm[8].stickers = {{"749054660769218631", "Wave", 3}}; // Wumpus waving (Lottie)
        for (Message &m : dm) // Unicode emoji become marks, as the client makes them
        {
            std::string out;
            for (std::size_t i = 0; i < m.content.size();)
                if (const std::size_t n = emoji_length(m.content, i))
                {
                    m.emojis.push_back({std::string(), m.content.substr(i, n)});
                    out += kEmojiMark;
                    i += n;
                }
                else
                    out += m.content[i++];
            m.content = out;
        }
    }
    return s;
}

} // namespace discord

namespace discord
{

namespace
{

// The voice requests, done the way the core would finish them (at once).
void apply_voice(Snapshot &s, const Request &r)
{
    const std::string me = s.me.id;
    auto drop_me = [&](const std::string &channel) {
        auto &seats = s.seats[channel];
        seats.erase(std::remove_if(seats.begin(), seats.end(), [&](const VoiceSeat &seat) { return seat.user_id == me; }),
                    seats.end());
    };
    auto stop_ringing = [&](const std::string &channel) {
        for (Call &call : s.calls)
            if (call.channel_id == channel)
                call.ringing.erase(std::remove(call.ringing.begin(), call.ringing.end(), me), call.ringing.end());
    };
    switch (r.kind)
    {
    case Request::Kind::voice_join:
    case Request::Kind::voice_call:
    case Request::Kind::voice_answer:
        if (s.voice.active())
            drop_me(s.voice.channel_id); // moving: out of the old room first
        drop_me(r.channel_id);            // and no second seat in the new one
        if (r.kind == Request::Kind::voice_answer)
            stop_ringing(r.channel_id);
        if (r.kind == Request::Kind::voice_call)
        {
            // A call rings the other person until they answer.
            std::string peer;
            for (const auto &[user_id, channel] : s.dm_by_user)
                if (channel == r.channel_id)
                    peer = user_id;
            auto call = std::find_if(s.calls.begin(), s.calls.end(), [&](const Call &c) { return c.channel_id == r.channel_id; });
            if (call == s.calls.end())
            {
                s.calls.push_back(Call{r.channel_id, peer, {}});
                call = s.calls.end() - 1;
            }
            call->ringing.clear();
            if (!peer.empty())
                call->ringing.push_back(peer);
        }
        s.voice.state = VoiceState::connected;
        s.voice.channel_id = r.channel_id;
        s.voice.guild_id = r.kind == Request::Kind::voice_join ? r.guild_id : std::string();
        s.voice.since_ms = s.now_ms;
        s.seats[r.channel_id].push_back({me, s.voice.muted, s.voice.deafened});
        break;
    case Request::Kind::voice_decline:
        stop_ringing(r.channel_id);
        break;
    case Request::Kind::voice_leave:
        if (s.voice.active())
            drop_me(s.voice.channel_id);
        s.voice = Voice{};
        break;
    case Request::Kind::voice_set:
        s.voice.muted = r.mute;
        s.voice.deafened = r.deaf;
        break;
    case Request::Kind::voice_broadcast:
        s.voice.broadcast = r.on ? "live" : "off";
        break;
    case Request::Kind::voice_output:
        s.voice.output_tv = r.tv;
        s.voice.controller = r.tv ? "off" : "on";
        break;
    default:
        break;
    }
    // Names and counts follow the seats, as the client resolves them.
    s.voice.member_count = s.voice.active() ? static_cast<int>(s.seats[s.voice.channel_id].size()) : 0;
    s.voice.room_name.clear();
    for (const Guild &g : s.guilds)
        for (const Room &room : g.rooms)
            if (room.id == s.voice.channel_id)
            {
                s.voice.room_name = room.name;
                s.voice.guild_name = g.name;
            }
    if (s.voice.room_name.empty())
        for (const auto &[user_id, channel] : s.dm_by_user)
            if (channel == s.voice.channel_id)
                if (const User *u = find_user(s, user_id))
                    s.voice.room_name = u->name;
}

} // namespace

void apply_sample(Snapshot &s, const Request &r)
{
    if (r.kind == Request::Kind::set_pref)
    {
        s.prefs.set(r.content, r.value);
        return;
    }
    if (r.kind == Request::Kind::sign_out || r.kind == Request::Kind::quit)
        return; // the sample account stays
    if (r.kind >= Request::Kind::voice_join)
    {
        apply_voice(s, r);
        return;
    }
    if (r.kind == Request::Kind::remoteplay_pair || r.kind == Request::Kind::remoteplay_unpair ||
        r.kind == Request::Kind::remoteplay_use)
    {
        for (RemotePlayUser &u : s.voice.rp_users)
            if (u.id == r.user_id || (r.user_id.empty() && u.on_screen))
            {
                if (r.kind == Request::Kind::remoteplay_pair)
                    u.paired = true, u.console = "PS5-0429", u.since = s.now_ms / 1000;
                else if (r.kind == Request::Kind::remoteplay_unpair)
                    u.paired = false;
                if (u.paired && (r.kind == Request::Kind::remoteplay_use || s.voice.rp_use.empty()))
                    s.voice.rp_use = u.id;
                if (!u.paired && s.voice.rp_use == u.id)
                    s.voice.rp_use.clear();
            }
        s.voice.paired = !s.voice.rp_use.empty();
        s.voice.pair_state = r.kind == Request::Kind::remoteplay_pair ? "paired" : "idle";
        for (const RemotePlayUser &u : s.voice.rp_users)
            if (u.id == s.voice.rp_use)
                s.voice.pair_console = u.console, s.voice.pair_user = u.name, s.voice.pair_since = u.since;
        return;
    }
    if (r.kind == Request::Kind::load_sticker_packs) // a pack like Discord's own (only Wave's id is real)
    {
        s.sticker_packs = {{"847199849233514549", "Wumpus Beyond",
                            {{"749054660769218631", "Wave", 3}, {"749043879713701898", "Hi", 3},
                             {"751606065447305216", "Wumpus Nom", 3}, {"816087792291282944", "Party", 3}}}};
        return;
    }
    if (r.kind != Request::Kind::send)
        return; // history is all loaded already
    std::vector<Message> &list = edit_messages(s, r.channel_id);
    Message m;
    m.id = "sent-" + std::to_string(list.size());
    m.channel_id = r.channel_id;
    m.author_id = s.me.id;
    m.content = mark_emoji(r.content, m.emojis);
    if (!r.sticker_id.empty())
        m.stickers.push_back({r.sticker_id, r.sticker_name, r.sticker_format});
    m.time_ms = s.now_ms;
    for (const Message &original : list)
        if (original.id == r.reply_to)
        {
            const User *author = find_user(s, original.author_id);
            m.reply_author = author ? author->name : original.author_name;
            m.reply_text = original.content;
        }
    list.push_back(m);
}

} // namespace discord

namespace discord
{

void sample_picture(Snapshot &s, const std::string &picture)
{
    // Tour pictures are named "<nn>-<design>-<step>": match the step's end.
    auto is = [&](const char *step) {
        const std::string tail = std::string("-") + step;
        return picture.size() >= tail.size() && picture.compare(picture.size() - tail.size(), tail.size(), tail) == 0;
    };
    if (is("voice-incoming")) // Theo rings us, from the DM, while we are in no call
    {
        s.voice = Voice{};
        s.calls = {Call{"dm-theo", "theo", {"me"}}};
    }
    else if (is("voice-error")) // still in the raid room, which has failed
    {
        s.voice.state = VoiceState::error;
        s.voice.channel_id = "raid";
        s.voice.guild_id = "nightshift";
        s.voice.room_name = "Raid Night";
        s.voice.guild_name = "Night Shift";
        s.voice.member_count = static_cast<int>(s.seats["raid"].size());
        s.voice.ping_ms = 0;
        s.voice.since_ms = 0;
        s.voice.error = "The voice server closed the connection. Join again to get back in.";
    }
}

} // namespace discord
