// ps5-discord - The app's side of the core (see client.hpp).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "discord/client.hpp"
#include "third_party/stb/stb_image.h"

#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

#include "platform/ps5/system.hpp"

// A failed internal check is logged instead of aborting the app. The one seen
// (json.hpp:8661, decimals read short) came from the libc's empty decimal
// point, fixed by localeconv() in runtime_shims.c; this stays as a safety net.
static void json_assert_failed(const char *what, int line)
{
    hui::sys::log("[CLIENT] json assert at json.hpp:%d: %s", line, what);
}
#define JSON_ASSERT(x) ((x) ? (void)0 : json_assert_failed(#x, __LINE__))
#include "nlohmann/json.hpp"

namespace discord
{

namespace
{

using json = nlohmann::json;

constexpr std::int64_t kDiscordEpochMs = 1420070400000;
constexpr int kAvatarArt = 12;
constexpr int kSceneArt = 8;

std::int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::uint64_t snowflake(const std::string &id)
{
    std::uint64_t v = 0;
    for (char c : id)
    {
        if (c < '0' || c > '9')
            return 0;
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return v;
}

std::int64_t snowflake_ms(const std::string &id)
{
    const std::uint64_t v = snowflake(id);
    return v ? static_cast<std::int64_t>(v >> 22) + kDiscordEpochMs : 0;
}

// A stable placeholder scene per id.
int art_for(const std::string &id, int count)
{
    std::uint32_t h = 2166136261u;
    for (char c : id)
        h = (h ^ static_cast<std::uint8_t>(c)) * 16777619u;
    return static_cast<int>(h % static_cast<std::uint32_t>(count));
}

std::string text(const json &j, const char *key)
{
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// ---- loopback HTTP -----------------------------------------------------------

struct Reply
{
    int status = -1;
    std::string body;
};

// Logs a failed exchange once per distinct cause, so a dead core does not flood klog.
Reply failed(Reply reply, const char *stage, int fd = -1)
{
    static std::string last; // only the client thread calls this
    const std::string cause = std::string(stage) + " " + std::strerror(errno);
    if (cause != last)
        hui::sys::log("[CLIENT] core %s", cause.c_str());
    last = cause;
    if (fd >= 0)
        close(fd);
    return reply;
}

Reply http(int port, const char *method, const std::string &path, const std::string &body = {})
{
    Reply reply;
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return failed(reply, "socket");
    timeval timeout{8, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    sockaddr_in to{};
#ifndef __linux__ // the console's BSD sockets have sin_len; the host build has not
    to.sin_len = sizeof to; // as spot5 does for its loopback helper
#endif
    to.sin_family = AF_INET;
    to.sin_port = htons(static_cast<std::uint16_t>(port));
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr *>(&to), sizeof to) != 0)
        return failed(reply, "connect", fd);
    char head[512];
    const int n = std::snprintf(head, sizeof head,
                                "%s %s HTTP/1.0\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                                "Content-Length: %zu\r\n\r\n",
                                method, path.c_str(), body.size());
    std::string request(head, static_cast<std::size_t>(n));
    request += body;
    for (std::size_t sent = 0; sent < request.size();)
    {
        const ssize_t w = send(fd, request.data() + sent, request.size() - sent, 0);
        if (w <= 0)
            return failed(reply, "send", fd);
        sent += static_cast<std::size_t>(w);
    }
    std::string raw;
    char buf[16384];
    for (ssize_t r; (r = recv(fd, buf, sizeof buf, 0)) > 0;)
        raw.append(buf, static_cast<std::size_t>(r));
    close(fd);
    const std::size_t split = raw.find("\r\n\r\n");
    if (split == std::string::npos || std::sscanf(raw.c_str(), "HTTP/%*s %d", &reply.status) != 1)
        return failed(reply, raw.empty() ? "recv (empty)" : "recv (no HTTP head)");
    reply.body = raw.substr(split + 4);
    return reply;
}

// ---- JSON to the model ---------------------------------------------------------

Presence presence_of(const std::string &status)
{
    if (status == "online")
        return Presence::online;
    if (status == "idle")
        return Presence::idle;
    if (status == "dnd")
        return Presence::dnd;
    return Presence::offline;
}

User user_from(const json &u)
{
    User user;
    user.id = text(u, "id");
    user.name = text(u, "global_name");
    if (user.name.empty())
        user.name = text(u, "username");
    const std::string avatar = text(u, "avatar");
    if (!avatar.empty())
        user.avatar_url = "https://cdn.discordapp.com/avatars/" + user.id + "/" + avatar + ".png?size=128";
    else if (!user.id.empty())
    {
        // No picture of their own: Discord's default for the account, as the official client shows.
        const unsigned long long id = std::strtoull(user.id.c_str(), nullptr, 10);
        user.avatar_url = "https://cdn.discordapp.com/embed/avatars/" + std::to_string((id >> 22) % 6) + ".png";
    }
    user.art = art_for(user.id, kAvatarArt);
    return user;
}

// Discord markup as people read it: <:name:id> and <a:name:id> become :name:,
// <@id> becomes @name (from the message's mentions), <@&id> @role, <#id> #channel.
std::string readable(const std::string &content, const json &m)
{
    std::string out;
    out.reserve(content.size());
    for (std::size_t i = 0; i < content.size();)
    {
        const std::size_t close = content[i] == '<' ? content.find('>', i) : std::string::npos;
        if (close == std::string::npos || close - i > 96)
        {
            out += content[i++];
            continue;
        }
        const std::string tag = content.substr(i + 1, close - i - 1);
        std::string shown;
        if (tag.rfind(":", 0) == 0 || tag.rfind("a:", 0) == 0)
        {
            const std::size_t start = tag.find(':') + 1;
            const std::size_t end = tag.find(':', start);
            if (end != std::string::npos)
                shown = ":" + tag.substr(start, end - start) + ":";
        }
        else if (tag.rfind("@&", 0) == 0)
            shown = "@role";
        else if (tag.rfind("@", 0) == 0)
        {
            const std::string id = tag.substr(tag.rfind("@!", 0) == 0 ? 2 : 1);
            shown = "@user";
            if (m.contains("mentions") && m["mentions"].is_array())
                for (const json &u : m["mentions"])
                    if (text(u, "id") == id)
                        shown = "@" + user_from(u).name;
        }
        else if (tag.rfind("#", 0) == 0)
            shown = "#channel";
        if (shown.empty())
        {
            out += content[i++];
            continue;
        }
        out += shown;
        i = close + 1;
    }
    // Markdown markers read as noise without styling: drop the paired ones.
    // ponytail: no bold/italic/code styling yet; draw runs with the semibold and mono faces later.
    for (const char *marker : {"**", "__", "~~", "||", "```", "`"})
    {
        const std::string mk = marker;
        std::size_t first = out.find(mk);
        while (first != std::string::npos)
        {
            const std::size_t second = out.find(mk, first + mk.size());
            if (second == std::string::npos)
                break;
            out.erase(second, mk.size());
            out.erase(first, mk.size());
            first = out.find(mk, second - mk.size());
        }
    }
    // *italic* and _italic_: only hugging a word, so 2*3*4 and snake_case stay.
    for (const char mk : {'*', '_'})
        for (std::size_t first = out.find(mk); first != std::string::npos; first = out.find(mk, first + 1))
        {
            const std::size_t second = out.find(mk, first + 1);
            if (second == std::string::npos)
                break;
            const bool hugs = second > first + 1 && out[first + 1] != ' ' && out[second - 1] != ' ' &&
                              (first == 0 || !std::isalnum(static_cast<unsigned char>(out[first - 1]))) &&
                              (second + 1 >= out.size() || !std::isalnum(static_cast<unsigned char>(out[second + 1])));
            if (!hugs)
                continue;
            out.erase(second, 1);
            out.erase(first, 1);
        }
    return out;
}

Message message_from(const json &m, const std::string &me_id)
{
    Message out;
    out.id = text(m, "id");
    out.channel_id = text(m, "channel_id");
    out.content = readable(text(m, "content"), m);
    out.time_ms = snowflake_ms(out.id);
    // Messages without text of their own: say what they are.
    const int type = static_cast<int>(m.value("type", 0.0));
    if (type == 7)
        out.content = "joined the server. \xF0\x9F\x91\x8B";
    else if (type == 6)
        out.content = "pinned a message.";
    else if (type >= 8 && type <= 11)
        out.content = "boosted the server.";
    else if (type == 3)
        out.content = "started a call. \xF0\x9F\x93\x9E";
    else if (type == 1 || type == 2)
        out.content = type == 1 ? "added someone to the group." : "left the group.";
    else if (type == 4 || type == 5)
        out.content = "changed the name or icon.";
    else if (type == 18)
        out.content = "started a thread.";
    if (out.content.empty() && m.contains("sticker_items") && m["sticker_items"].is_array() &&
        !m["sticker_items"].empty())
        out.content = "[Sticker: " + text(m["sticker_items"][0], "name") + "]";
    if (out.content.empty() && m.contains("embeds") && m["embeds"].is_array() && !m["embeds"].empty())
    {
        const std::string title = text(m["embeds"][0], "title");
        out.content = title.empty() ? std::string("[Embed]") : "[Embed: " + title + "]";
    }
    // A forwarded message carries its text in a snapshot of the original.
    if (out.content.empty() && m.contains("message_snapshots") && m["message_snapshots"].is_array() &&
        !m["message_snapshots"].empty() && m["message_snapshots"][0].contains("message"))
    {
        const json &original = m["message_snapshots"][0]["message"];
        const std::string text_of = readable(text(original, "content"), original);
        out.content = "\xE2\x86\xAA Forwarded" + (text_of.empty() ? std::string() : ": " + text_of);
    }
    if (out.content.empty() && out.attachments.empty() && !(m.contains("attachments") && !m["attachments"].empty()))
        out.content = type != 0 ? "[Discord notice]" : "[A message this app cannot show yet]";
    if (m.contains("author") && m["author"].is_object())
    {
        out.author_id = text(m["author"], "id");
        out.author_name = user_from(m["author"]).name;
    }
    out.edited = m.contains("edited_timestamp") && m["edited_timestamp"].is_string();
    if (m.contains("mentions") && m["mentions"].is_array())
        for (const json &u : m["mentions"])
            out.mentions_me |= text(u, "id") == me_id && out.author_id != me_id;
    if (m.contains("referenced_message") && m["referenced_message"].is_object())
    {
        const json &r = m["referenced_message"];
        out.reply_text = readable(text(r, "content"), r);
        if (r.contains("author") && r["author"].is_object())
            out.reply_author = user_from(r["author"]).name;
    }
    if (m.contains("attachments") && m["attachments"].is_array())
        for (const json &a : m["attachments"])
        {
            Attachment att;
            att.name = text(a, "filename");
            att.url = text(a, "proxy_url");
            if (att.url.empty())
                att.url = text(a, "url");
            att.image = text(a, "content_type").rfind("image/", 0) == 0;
            if (att.image) // a small preview from the media proxy, not the full file
                att.url += std::string(att.url.find('?') == std::string::npos ? "?" : "&") + "width=440&height=248";
            const double size = a.value("size", 0.0);
            char sz[32];
            std::snprintf(sz, sizeof sz, size >= 1048576 ? "%.1f MB" : "%.0f KB",
                          size >= 1048576 ? size / 1048576 : size / 1024);
            att.size_text = sz;
            att.art = art_for(att.name, kSceneArt);
            out.attachments.push_back(att);
        }
    if (m.contains("reactions") && m["reactions"].is_array())
        for (const json &r : m["reactions"])
        {
            Reaction reaction;
            if (r.contains("emoji") && r["emoji"].is_object())
            {
                reaction.emoji_id = text(r["emoji"], "id");
                reaction.emoji = reaction.emoji_id.empty() ? text(r["emoji"], "name") : ":" + text(r["emoji"], "name") + ":";
            }
            reaction.count = static_cast<int>(r.value("count", 0.0));
            reaction.me = r.value("me", false);
            if (reaction.count > 0 && !reaction.emoji.empty())
                out.reactions.push_back(reaction);
        }
    return out;
}

// Rebuilds the account part of s from /api/chat, keeping loaded messages and mentions.
void apply_tree(Snapshot &s, const json &t)
{
    std::map<std::string, User> users;
    if (t.contains("users") && t["users"].is_object())
        for (const auto &[id, u] : t["users"].items())
            users[id] = user_from(u);
    const json &me = t.contains("me") ? t["me"] : json::object();
    s.me = user_from(me);
    s.me.presence = Presence::online;
    const json &presence = t.contains("presence") ? t["presence"] : json::object();
    const json &voice = t.contains("voice") ? t["voice"] : json::object();
    const json &reads = t.contains("reads") ? t["reads"] : json::object();

    // Rooms by channel id, to name a friend's room and to list live rooms.
    struct RoomRef
    {
        std::string guild_id, guild_name, name;
    };
    std::map<std::string, RoomRef> rooms;
    s.guilds.clear();
    s.live_rooms.clear();
    std::vector<Conversation> unread;
    s.unread_total = 0;
    auto read_of = [&reads](const std::string &channel, const std::string &last_message) {
        std::pair<bool, int> out{false, 0};
        const auto r = reads.find(channel);
        if (r == reads.end())
            return out;
        out.second = static_cast<int>(r->value("mentions", 0.0));
        const std::string last_read = text(*r, "last_read");
        out.first = !last_message.empty() && snowflake(last_message) > snowflake(last_read);
        return out;
    };

    if (t.contains("guilds") && t["guilds"].is_array())
        for (const json &g : t["guilds"])
        {
            Guild guild;
            guild.id = text(g, "id");
            guild.name = text(g, "name");
            const std::string icon = text(g, "icon");
            if (!icon.empty())
                guild.icon_url = "https://cdn.discordapp.com/icons/" + guild.id + "/" + icon + ".png?size=128";
            if (const std::string banner = text(g, "banner"); !banner.empty())
                guild.banner_url = "https://cdn.discordapp.com/banners/" + guild.id + "/" + banner + ".png?size=600";
            else if (const std::string splash = text(g, "splash"); !splash.empty())
                guild.banner_url = "https://cdn.discordapp.com/splashes/" + guild.id + "/" + splash + ".png?size=640";
            guild.art = art_for(guild.id, kSceneArt);
            guild.members = static_cast<int>(g.value("member_count", 0.0));
            if (g.contains("channels"))
            {
                // Discord's order: channels without a category first, then each
                // category by its position, channels by theirs within it.
                std::map<std::string, std::pair<double, std::string>> categories;
                for (const json &c : g["channels"])
                    if (static_cast<int>(c.value("type", 0.0)) == 4)
                        categories[text(c, "id")] = {c.value("position", 0.0), text(c, "name")};
                std::vector<std::pair<std::pair<double, double>, Channel>> ordered;
                for (const json &c : g["channels"])
                {
                    const int type = static_cast<int>(c.value("type", 0.0));
                    if (type == 4) // categories only group channels
                        continue;
                    Channel ch;
                    ch.id = text(c, "id");
                    ch.name = text(c, "name");
                    ch.topic = text(c, "topic");
                    ch.can_send = c.value("can_send", true);
                    ch.kind = type == 5 ? ChannelKind::announcement : ChannelKind::text;
                    const std::string last = text(c, "last_message_id");
                    ch.last_ms = snowflake_ms(last);
                    const auto parent = categories.find(text(c, "parent_id"));
                    const double category_pos = parent == categories.end() ? -1.0 : parent->second.first;
                    if (parent != categories.end())
                        ch.category = parent->second.second;
                    const auto [has_unread, mentions] = read_of(ch.id, last);
                    ch.has_unread = has_unread;
                    ch.unread = mentions;
                    ch.mentioned = mentions > 0;
                    guild.unread += mentions;
                    if (has_unread || mentions > 0)
                    {
                        ++s.unread_total;
                        Conversation conv;
                        conv.channel_id = ch.id;
                        conv.guild_id = guild.id;
                        conv.title = mentions > 0 ? "@you in #" + ch.name : "#" + ch.name;
                        conv.preview = guild.name;
                        conv.time_ms = snowflake_ms(last);
                        conv.unread = mentions;
                        conv.mention = mentions > 0;
                        unread.push_back(conv);
                    }
                    ordered.emplace_back(std::make_pair(category_pos, c.value("position", 0.0)), ch);
                }
                std::stable_sort(ordered.begin(), ordered.end(),
                                 [](const auto &a, const auto &b) { return a.first < b.first; });
                for (auto &[pos, ch] : ordered)
                    guild.channels.push_back(std::move(ch));
            }
            if (g.contains("rooms"))
                for (const json &r : g["rooms"])
                {
                    Room room;
                    room.id = text(r, "id");
                    room.name = text(r, "name");
                    room.guild_id = guild.id;
                    room.guild_name = guild.name;
                    room.art = art_for(room.id, kSceneArt);
                    rooms[room.id] = {guild.id, guild.name, room.name};
                    const auto states = voice.find(room.id);
                    if (states != voice.end() && states->is_array() && !states->empty())
                    {
                        for (const json &st : *states)
                            room.member_ids.push_back(text(st, "user_id"));
                        room.member_count = static_cast<int>(room.member_ids.size());
                        s.live_rooms.push_back(room);
                    }
                    guild.rooms.push_back(room);
                }
            s.guilds.push_back(std::move(guild));
        }
    std::sort(s.live_rooms.begin(), s.live_rooms.end(),
              [](const Room &a, const Room &b) { return a.member_count > b.member_count; });

    // Friends with presence and the room they are in.
    std::map<std::string, std::string> in_room;
    for (const auto &[channel, states] : voice.items())
        if (states.is_array())
            for (const json &st : states)
                in_room[text(st, "user_id")] = channel;
    s.friends.clear();
    s.friends_online = 0;
    if (t.contains("friends") && t["friends"].is_array())
        for (const json &id_json : t["friends"])
        {
            const std::string id = id_json.is_string() ? id_json.get<std::string>() : std::string();
            User u = users.count(id) ? users[id] : User{};
            u.id = id;
            u.art = art_for(id, kAvatarArt);
            const auto p = presence.find(id);
            if (p != presence.end())
            {
                u.presence = presence_of(text(*p, "status"));
                u.activity = text(*p, "activity");
                if (!u.activity.empty())
                    u.activity = "Playing " + u.activity;
            }
            const auto room = in_room.find(id);
            if (room != in_room.end() && rooms.count(room->second))
                u.voice_room = rooms[room->second].name;
            if (u.presence != Presence::offline)
                ++s.friends_online;
            s.friends.push_back(u);
        }
    // Other people seen in rooms or DMs, so avatars and names resolve.
    for (const auto &[id, u] : users)
    {
        const bool known = id == s.me.id || std::any_of(s.friends.begin(), s.friends.end(),
                                                         [&](const User &f) { return f.id == id; });
        if (!known)
        {
            User other = u;
            other.presence = Presence::offline;
            s.friends.push_back(other); // offline: Home rows skip them, lookups find them
        }
    }

    // Direct messages: by recency, unread ones counted.
    std::vector<Conversation> dms;
    if (t.contains("dms") && t["dms"].is_array())
        for (const json &d : t["dms"])
        {
            Conversation conv;
            conv.channel_id = text(d, "id");
            const std::string last = text(d, "last_message_id");
            conv.time_ms = snowflake_ms(last);
            std::string name = text(d, "name");
            if (d.contains("recipient_ids") && d["recipient_ids"].is_array() && !d["recipient_ids"].empty())
            {
                conv.user_id = d["recipient_ids"][0].is_string() ? d["recipient_ids"][0].get<std::string>() : "";
                if (name.empty())
                    for (const json &rid : d["recipient_ids"])
                        if (rid.is_string() && users.count(rid.get<std::string>()))
                            name += (name.empty() ? "" : ", ") + users[rid.get<std::string>()].name;
            }
            conv.title = name.empty() ? std::string("Direct message") : name;
            if (static_cast<int>(d.value("type", 1.0)) == 1 && !conv.user_id.empty())
                s.dm_by_user[conv.user_id] = conv.channel_id;
            const auto [has_unread, mentions] = read_of(conv.channel_id, last);
            conv.unread = has_unread ? std::max(1, mentions) : 0;
            conv.preview = has_unread ? "New messages" : "Direct message";
            if (has_unread)
                ++s.unread_total;
            dms.push_back(conv);
        }

    // Recent: unread channels and DMs first by time, then the latest DMs.
    std::vector<Conversation> recent = unread;
    for (const Conversation &d : dms)
        if (d.unread > 0)
            recent.push_back(d);
    std::sort(recent.begin(), recent.end(), [](const auto &a, const auto &b) { return a.time_ms > b.time_ms; });
    std::sort(dms.begin(), dms.end(), [](const auto &a, const auto &b) { return a.time_ms > b.time_ms; });
    for (const Conversation &d : dms)
        if (d.unread == 0 && recent.size() < 8)
            recent.push_back(d);
    if (recent.size() > 8)
        recent.resize(8);
    s.recent = recent;

    // Your own voice connection.
    s.voice = {};
    const std::string my_room = text(t, "my_voice");
    if (!my_room.empty() && rooms.count(my_room))
    {
        s.voice.connected = true;
        s.voice.room_name = rooms[my_room].name;
        s.voice.guild_name = rooms[my_room].guild_name;
        const auto states = voice.find(my_room);
        s.voice.member_count = states != voice.end() && states->is_array() ? static_cast<int>(states->size()) : 0;
        if (states != voice.end() && states->is_array())
            for (const json &st : *states)
                if (text(st, "user_id") == s.me.id)
                    s.voice.muted = st.value("self_mute", false);
    }
}

} // namespace

// ---- client thread -------------------------------------------------------------

Client::~Client()
{
    stop_ = true;
    if (started_)
        pthread_join(thread_, nullptr);
    if (image_started_)
        pthread_join(image_thread_, nullptr);
    if (events_started_)
        pthread_join(events_thread_, nullptr);
}

// pthread rather than std::thread, as spot5 does on the console.
void Client::start()
{
    // The core may close a connection mid-request; that is an error to retry, not SIGPIPE.
    std::signal(SIGPIPE, SIG_IGN);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1u << 20); // JSON parsing of a large account
    started_ = pthread_create(
                   &thread_, &attr,
                   [](void *self) -> void * {
                       static_cast<Client *>(self)->run();
                       return nullptr;
                   },
                   this) == 0;
    events_started_ = pthread_create(
                          &events_thread_, &attr,
                          [](void *self) -> void * {
                              static_cast<Client *>(self)->run_events();
                              return nullptr;
                          },
                          this) == 0;
    image_started_ = pthread_create(
                         &image_thread_, &attr,
                         [](void *self) -> void * {
                             static_cast<Client *>(self)->run_images();
                             return nullptr;
                         },
                         this) == 0;
    pthread_attr_destroy(&attr);
    if (!started_ || !image_started_ || !events_started_)
        hui::sys::log("[CLIENT] could not start its threads");
}

void Client::submit(const Request &incoming)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Request request = incoming;
    // Show a sent message at once; the gateway's echo (same nonce) replaces it.
    if (request.kind == Request::Kind::send)
    {
        static std::uint64_t sequence = 0;
        // A snowflake-shaped nonce, as the official client sends.
        request.nonce = std::to_string(((static_cast<std::uint64_t>(now_ms()) - 1420070400000ull) << 22) +
                                       (++sequence & 0x3fffff));
        Message m;
        m.id = "pending-" + request.nonce;
        m.nonce = request.nonce;
        m.channel_id = request.channel_id;
        m.author_id = snapshot_.me.id;
        m.content = request.content;
        m.time_ms = now_ms();
        m.pending = true;
        std::vector<Message> &ms = edit_messages(snapshot_, request.channel_id);
        for (const Message &original : ms)
            if (!request.reply_to.empty() && original.id == request.reply_to)
            {
                m.reply_author = original.author_name;
                m.reply_text = original.content;
                if (const User *u = find_user(snapshot_, original.author_id))
                    m.reply_author = u->name;
            }
        ms.push_back(m);
        publish();
    }
    queue_.push_back(request);
}

bool Client::take(Snapshot &out)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (taken_ == version_)
        return false;
    out = snapshot_;
    taken_ = version_;
    return true;
}

void Client::publish()
{
    snapshot_.now_ms = now_ms();
    ++version_;
}

void Client::want_image(const std::string &url)
{
    std::lock_guard<std::mutex> lock(mutex_);
    image_queue_.push_back(url);
}

std::vector<Image> Client::take_images()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Image> out;
    out.swap(images_);
    return out;
}

// Everything queued, newest asks first (what is on screen now).
void Client::fetch_images()
{
    for (;;)
    {
        Image image;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (image_queue_.empty())
                return;
            image.url = image_queue_.back();
            image_queue_.pop_back();
        }
        std::string query;
        for (const unsigned char c : image.url)
        {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
                query += static_cast<char>(c);
            else
            {
                char hex[4];
                std::snprintf(hex, sizeof hex, "%%%02X", c);
                query += hex;
            }
        }
        const Reply reply = http(port_, "GET", "/api/image?url=" + query);
        if (reply.status == 200)
        {
            int n = 0;
            if (std::uint8_t *px = stbi_load_from_memory(reinterpret_cast<const std::uint8_t *>(reply.body.data()),
                                                         static_cast<int>(reply.body.size()), &image.w, &image.h, &n, 4))
            {
                image.rgba.assign(px, px + static_cast<std::size_t>(image.w) * image.h * 4);
                stbi_image_free(px);
            }
            else
                image.w = image.h = 0;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        images_.push_back(std::move(image));
    }
}

void Client::run_images()
{
    while (!stop_)
    {
        if (ready_)
            fetch_images();
        usleep(50000);
    }
}

void Client::run()
{
    hui::sys::log("[CLIENT] start, core on 127.0.0.1:%d", port_);
    std::int64_t last_state = 0;
    while (!stop_)
    {
        const std::int64_t now = now_ms();
        if (now - last_state >= 1000)
        {
            poll_state();
            last_state = now;
        }
        if (ready_)
        {
            if (now - last_chat_ms_ >= 3000)
                poll_chat();
            if (now - last_mentions_ms_ >= 30000)
                poll_mentions();
        }
        for (;;)
        {
            Request request;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (queue_.empty())
                    break;
                request = queue_.front();
                queue_.pop_front();
            }
            handle(request);
        }
        usleep(250000);
    }
}

void Client::poll_state()
{
    const Reply reply = http(port_, "GET", "/api/state");
    std::lock_guard<std::mutex> lock(mutex_);
    if (reply.status != 200)
    {
        if (reply.status > 0)
            hui::sys::log("[CLIENT] /api/state answered %d", reply.status);
        snapshot_.connection = Connection::starting;
        snapshot_.status_message = "Waiting for the ps5-discord core. Send its payload to elfldr (port 9021).";
        ready_ = false;
        publish();
        return;
    }
    const json state = json::parse(reply.body, nullptr, false);
    if (state.is_discarded())
        return;
    const int api = static_cast<int>(state.value("api_version", 0.0));
    if (api < kApiVersionMin || api > kApiVersionMax)
    {
        snapshot_.connection = Connection::starting;
        snapshot_.status_message = "This app needs core API " + std::to_string(kApiVersionMin) + "-" +
                                   std::to_string(kApiVersionMax) + ", the running core has " +
                                   std::to_string(api) + ". Update both to the same release.";
        ready_ = false;
        publish();
        return;
    }
    const std::string s = text(state, "state");
    const Connection before = snapshot_.connection;
    if (s == "ready")
        snapshot_.connection = Connection::ready;
    else if (s == "login" || s == "scanned")
        snapshot_.connection = Connection::signing_in;
    else if (s == "connecting" && before == Connection::ready)
        snapshot_.connection = Connection::reconnecting;
    else
        snapshot_.connection = Connection::connecting;
    snapshot_.login_scanned = s == "scanned";
    snapshot_.login_qr = text(state, "qr");
    snapshot_.login_qr_size = static_cast<int>(state.value("qr_size", 0.0));
    snapshot_.status_message.clear();
    const bool was_ready = ready_;
    ready_ = snapshot_.connection == Connection::ready;
    if (ready_ && !was_ready)
        last_chat_ms_ = last_mentions_ms_ = 0; // fetch everything now
    publish();
}

void refresh_mentions(Snapshot &s);

void Client::poll_chat()
{
    const Reply reply = http(port_, "GET", "/api/chat");
    last_chat_ms_ = now_ms();
    if (reply.status != 200)
        return;
    const json tree = json::parse(reply.body, nullptr, false);
    if (tree.is_discarded() || !tree.is_object())
        return;
    // Built outside the lock (the UI thread takes snapshots under it), from a
    // copy that is cheap because message lists are shared. Only this thread
    // changes the account part; submit() may add pending messages meanwhile.
    Snapshot next;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        next = snapshot_;
    }
    apply_tree(next, tree);
    refresh_mentions(next);
    std::lock_guard<std::mutex> lock(mutex_);
    next.messages = std::move(snapshot_.messages);
    snapshot_ = std::move(next);
    if (!prefetched_)
    {
        // Warm the conversations people open first, so they show at once.
        prefetched_ = true;
        int queued = 0;
        for (const Conversation &c : snapshot_.recent)
            if (queued < 6 && !snapshot_.messages.count(c.channel_id))
            {
                Request r;
                r.kind = Request::Kind::prefetch;
                r.channel_id = c.channel_id;
                queue_.push_back(r);
                ++queued;
            }
    }
    publish();
}

void Client::run_events()
{
    while (!stop_)
    {
        if (!ready_ || !poll_events())
            usleep(250000); // not signed in, or the core is away: try again shortly
    }
}

// One long poll: the core answers when an event lands, or after a few seconds.
bool Client::poll_events()
{
    const std::string wait = events_next_ < 0 ? "0" : "6"; // under the 8 s socket timeout
    const Reply reply =
        http(port_, "GET", "/api/events?since=" + std::to_string(std::max(0L, events_next_)) + "&wait=" + wait);
    if (reply.status != 200)
        return false;
    const json e = json::parse(reply.body, nullptr, false);
    if (e.is_discarded())
        return true;
    const long next = static_cast<long>(e.value("next", 0.0));
    if (events_next_ < 0) // first contact: start from now, history comes from /api/messages
    {
        events_next_ = next;
        return true;
    }
    events_next_ = next;
    if (!e.contains("events") || e["events"].empty())
        return true;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const json &ev : e["events"])
    {
        const std::string type = text(ev, "t");
        const std::string channel = text(ev, "channel_id");
        if (type == "MESSAGE_CREATE")
        {
            // A DM or a mention from someone else becomes a notice, open channel or not.
            const Message m = message_from(ev, snapshot_.me.id);
            const std::string guild = text(ev, "guild_id");
            if (m.author_id != snapshot_.me.id && !m.author_id.empty() && (guild.empty() || m.mentions_me))
            {
                Notice &n = snapshot_.notice;
                ++n.seq;
                n.guild_id = guild;
                n.channel_id = channel;
                n.author = ev.contains("author") && ev["author"].is_object() ? user_from(ev["author"]) : User{};
                n.title = n.author.name;
                if (!guild.empty())
                    for (const Guild &g : snapshot_.guilds)
                        for (const Channel &c : g.channels)
                            if (c.id == channel)
                                n.title += " in #" + c.name;
                n.text = m.content.empty() && !m.attachments.empty() ? "sent an attachment" : m.content;
            }
        }
        if (snapshot_.messages.find(channel) == snapshot_.messages.end())
        {
            last_chat_ms_ = 0; // not open: its history loads when it is; refresh unread counts now
            continue;
        }
        std::vector<Message> &ms = edit_messages(snapshot_, channel);
        if (type == "MESSAGE_CREATE")
        {
            Message m = message_from(ev, snapshot_.me.id);
            // Our own echo replaces its pending copy: by nonce, else by text.
            const std::string nonce = text(ev, "nonce");
            auto pending = std::find_if(ms.begin(), ms.end(), [&](const Message &p) {
                return p.pending && ((!nonce.empty() && p.nonce == nonce) ||
                                     (p.author_id == m.author_id && p.content == m.content));
            });
            if (pending != ms.end())
                *pending = m;
            else if (std::none_of(ms.begin(), ms.end(), [&](const Message &x) { return x.id == m.id; }))
                ms.push_back(m);
        }
        else if (type == "MESSAGE_UPDATE")
        {
            for (Message &m : ms)
                if (m.id == text(ev, "id") && ev.contains("content"))
                {
                    m.content = readable(text(ev, "content"), ev);
                    m.edited = true;
                }
        }
        else if (type == "MESSAGE_DELETE")
        {
            ms.erase(std::remove_if(ms.begin(), ms.end(), [&](const Message &m) { return m.id == text(ev, "id"); }),
                     ms.end());
        }
        else if (type == "MESSAGE_REACTION_ADD" || type == "MESSAGE_REACTION_REMOVE")
        {
            const bool add = type == "MESSAGE_REACTION_ADD";
            const json emoji = ev.contains("emoji") && ev["emoji"].is_object() ? ev["emoji"] : json::object();
            const std::string emoji_id = text(emoji, "id");
            const std::string name = emoji_id.empty() ? text(emoji, "name") : ":" + text(emoji, "name") + ":";
            const bool mine = text(ev, "user_id") == snapshot_.me.id;
            for (Message &m : ms)
            {
                if (m.id != text(ev, "message_id"))
                    continue;
                auto r = std::find_if(m.reactions.begin(), m.reactions.end(),
                                      [&](const Reaction &x) { return x.emoji == name; });
                if (add && r == m.reactions.end())
                    r = m.reactions.insert(m.reactions.end(), Reaction{name, emoji_id, 0, false});
                if (r == m.reactions.end())
                    continue;
                r->count += add ? 1 : -1;
                if (mine)
                    r->me = add;
                if (r->count <= 0)
                    m.reactions.erase(r);
            }
        }
    }
    publish();
    return true;
}

// Inbox entries are unread while their channel still counts mentions of us;
// the Mentions badge counts those, so it matches the page.
void refresh_mentions(Snapshot &s)
{
    s.mention_count = 0;
    for (Mention &mention : s.mentions)
    {
        mention.unread = false;
        for (const Guild &g : s.guilds)
            for (const Channel &c : g.channels)
                if (c.id == mention.channel_id)
                    mention.unread = c.unread > 0;
        s.mention_count += mention.unread ? 1 : 0;
    }
}

void Client::poll_mentions()
{
    const Reply reply = http(port_, "GET", "/api/mentions");
    last_mentions_ms_ = now_ms();
    if (reply.status != 200)
        return;
    const json r = json::parse(reply.body, nullptr, false);
    if (r.is_discarded() || !r.contains("body") || !r["body"].is_array())
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.mentions.clear();
    for (const json &m : r["body"])
    {
        Mention mention;
        mention.message_id = text(m, "id");
        mention.channel_id = text(m, "channel_id");
        mention.content = readable(text(m, "content"), m);
        mention.time_ms = snowflake_ms(mention.message_id);
        if (m.contains("author") && m["author"].is_object())
        {
            mention.author_id = text(m["author"], "id");
            mention.author = user_from(m["author"]);
        }
        std::string where;
        for (const Guild &g : snapshot_.guilds)
            for (const Channel &c : g.channels)
                if (c.id == mention.channel_id)
                {
                    where = "@you in #" + c.name + " · " + g.name;
                    mention.unread = c.unread > 0;
                }
        mention.context = where.empty() ? std::string("@you") : where;
        snapshot_.mentions.push_back(mention);
    }
    refresh_mentions(snapshot_);
    publish();
}

void Client::handle(const Request &request)
{
    switch (request.kind)
    {
    case Request::Kind::open_channel:
    case Request::Kind::prefetch:
    case Request::Kind::load_older: {
        std::string path = "/api/messages?channel=" + request.channel_id;
        if (!request.guild_id.empty())
            path += "&guild=" + request.guild_id;
        std::string oldest;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = snapshot_.messages.find(request.channel_id);
            if (request.kind == Request::Kind::load_older && it != snapshot_.messages.end() && !it->second->empty())
                oldest = it->second->front().id;
        }
        if (request.kind == Request::Kind::load_older)
        {
            if (oldest.empty())
                return;
            path += "&before=" + oldest;
        }
        const Reply reply = http(port_, "GET", path);
        const json r = json::parse(reply.body, nullptr, false);
        if (reply.status != 200 || r.is_discarded() || !r.contains("body") || !r["body"].is_array())
        {
            const int discord_status = r.is_discarded() ? 0 : static_cast<int>(r.value("status", 0.0));
            hui::sys::log("[CLIENT] history for %s failed (%d/%d)", request.channel_id.c_str(), reply.status,
                          discord_status);
            std::lock_guard<std::mutex> lock(mutex_);
            if (discord_status == 403)
                snapshot_.no_access.insert(request.channel_id);
            else if (request.kind == Request::Kind::open_channel)
                snapshot_.load_failed.insert(request.channel_id);
            publish();
            return;
        }
        std::vector<Message> page;
        std::string me;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            me = snapshot_.me.id;
        }
        for (const json &m : r["body"]) // newest first from Discord
            page.push_back(message_from(m, me));
        std::reverse(page.begin(), page.end());
        std::string newest = page.empty() ? std::string() : page.back().id;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.load_failed.erase(request.channel_id);
            if (page.size() < 50) // Discord's page size: a short page reached the start
                snapshot_.history_start.insert(request.channel_id);
            std::vector<Message> &ms = edit_messages(snapshot_, request.channel_id);
            if (request.kind == Request::Kind::load_older)
                ms.insert(ms.begin(), page.begin(), page.end());
            else
                ms = page;
            publish();
        }
        // Opening a channel reads it.
        if (request.kind == Request::Kind::open_channel && !newest.empty())
            http(port_, "POST", "/api/ack", json{{"channel", request.channel_id}, {"message", newest}}.dump());
        break;
    }
    case Request::Kind::send: {
        json body{{"channel", request.channel_id}, {"content", request.content}, {"nonce", request.nonce}};
        if (!request.reply_to.empty())
            body["reply_to"] = request.reply_to;
        const Reply reply = http(port_, "POST", "/api/send", body.dump());
        const json r = json::parse(reply.body, nullptr, false);
        const int discord_status = r.is_discarded() ? 0 : static_cast<int>(r.value("status", 0.0));
        if (reply.status != 200 || discord_status < 200 || discord_status >= 300)
        {
            hui::sys::log("[CLIENT] send to %s failed (%d/%d)", request.channel_id.c_str(), reply.status,
                          discord_status);
            std::lock_guard<std::mutex> lock(mutex_);
            for (Message &m : edit_messages(snapshot_, request.channel_id))
                if (m.pending && m.nonce == request.nonce)
                {
                    m.pending = false;
                    m.failed = true;
                }
            publish();
        }
        break;
    }
    case Request::Kind::react: {
        json body{{"channel", request.channel_id}, {"message", request.message_id}, {"emoji", request.emoji}};
        const Reply reply = http(port_, "POST", "/api/react", body.dump());
        if (reply.status != 200)
            hui::sys::log("[CLIENT] react failed (%d)", reply.status);
        break;
    }
    case Request::Kind::edit:
    case Request::Kind::remove: {
        json body{{"channel", request.channel_id}, {"message", request.message_id}};
        if (request.kind == Request::Kind::edit)
            body["content"] = request.content;
        const Reply reply = http(port_, "POST", request.kind == Request::Kind::edit ? "/api/edit" : "/api/delete",
                                 body.dump());
        const json r = json::parse(reply.body, nullptr, false);
        const int discord_status = r.is_discarded() ? 0 : static_cast<int>(r.value("status", 0.0));
        if (reply.status != 200 || discord_status < 200 || discord_status >= 300)
            hui::sys::log("[CLIENT] %s failed (%d/%d)", request.kind == Request::Kind::edit ? "edit" : "delete",
                          reply.status, discord_status);
        break; // the gateway's MESSAGE_UPDATE / MESSAGE_DELETE changes the list
    }
    case Request::Kind::open_dm: {
        const Reply reply = http(port_, "POST", "/api/dm", json{{"user", request.user_id}}.dump());
        const json r = json::parse(reply.body, nullptr, false);
        if (reply.status == 200 && !r.is_discarded() && r.contains("body") && r["body"].is_object())
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.dm_by_user[request.user_id] = text(r["body"], "id");
            publish();
        }
        else
            hui::sys::log("[CLIENT] open DM failed (%d)", reply.status);
        break;
    }
    }
}

} // namespace discord
