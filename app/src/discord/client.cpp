// ps5-discord - The app's side of the core (see client.hpp).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "discord/client.hpp"

#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <vector>

#include "nlohmann/json.hpp"
#include "platform/ps5/system.hpp"

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

Reply http(int port, const char *method, const std::string &path, const std::string &body = {})
{
    Reply reply;
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return reply;
    timeval timeout{8, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(static_cast<std::uint16_t>(port));
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr *>(&to), sizeof to) != 0)
    {
        close(fd);
        return reply;
    }
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
        {
            close(fd);
            return reply;
        }
        sent += static_cast<std::size_t>(w);
    }
    std::string raw;
    char buf[16384];
    for (ssize_t r; (r = recv(fd, buf, sizeof buf, 0)) > 0;)
        raw.append(buf, static_cast<std::size_t>(r));
    close(fd);
    const std::size_t split = raw.find("\r\n\r\n");
    if (split == std::string::npos || std::sscanf(raw.c_str(), "HTTP/%*s %d", &reply.status) != 1)
        return reply;
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
    user.art = art_for(user.id, kAvatarArt);
    return user;
}

Message message_from(const json &m, const std::string &me_id)
{
    Message out;
    out.id = text(m, "id");
    out.channel_id = text(m, "channel_id");
    out.content = text(m, "content");
    out.time_ms = snowflake_ms(out.id);
    if (m.contains("author") && m["author"].is_object())
    {
        out.author_id = text(m["author"], "id");
        out.author_name = user_from(m["author"]).name;
    }
    out.edited = m.contains("edited_timestamp") && m["edited_timestamp"].is_string();
    if (m.contains("mentions") && m["mentions"].is_array())
        for (const json &u : m["mentions"])
            out.mentions_me |= text(u, "id") == me_id;
    if (m.contains("referenced_message") && m["referenced_message"].is_object())
    {
        const json &r = m["referenced_message"];
        out.reply_text = text(r, "content");
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
            const double size = a.value("size", 0.0);
            char sz[32];
            std::snprintf(sz, sizeof sz, size >= 1048576 ? "%.1f MB" : "%.0f KB",
                          size >= 1048576 ? size / 1048576 : size / 1024);
            att.size_text = sz;
            att.art = art_for(att.name, kSceneArt);
            out.attachments.push_back(att);
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
    s.mention_count = 0;
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
            guild.art = art_for(guild.id, kSceneArt);
            guild.members = static_cast<int>(g.value("member_count", 0.0));
            if (g.contains("channels"))
            {
                std::vector<std::pair<double, Channel>> ordered;
                for (const json &c : g["channels"])
                {
                    const int type = static_cast<int>(c.value("type", 0.0));
                    if (type == 4) // categories only group channels
                        continue;
                    Channel ch;
                    ch.id = text(c, "id");
                    ch.name = text(c, "name");
                    ch.topic = text(c, "topic");
                    ch.kind = type == 5 ? ChannelKind::announcement : ChannelKind::text;
                    const std::string last = text(c, "last_message_id");
                    const auto [has_unread, mentions] = read_of(ch.id, last);
                    ch.has_unread = has_unread;
                    ch.unread = mentions;
                    ch.mentioned = mentions > 0;
                    guild.unread += mentions;
                    s.mention_count += mentions;
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
                    ordered.emplace_back(c.value("position", 0.0), ch);
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
    pthread_attr_destroy(&attr);
    if (!started_)
        hui::sys::log("[CLIENT] could not start its thread");
}

void Client::submit(const Request &request)
{
    std::lock_guard<std::mutex> lock(mutex_);
    // Show a sent message at once; the gateway's echo replaces it.
    if (request.kind == Request::Kind::send)
    {
        Message m;
        m.id = "pending-" + std::to_string(now_ms());
        m.channel_id = request.channel_id;
        m.author_id = snapshot_.me.id;
        m.content = request.content;
        m.time_ms = now_ms();
        m.pending = true;
        snapshot_.messages[request.channel_id].push_back(m);
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
            poll_events();
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

void Client::poll_chat()
{
    const Reply reply = http(port_, "GET", "/api/chat");
    last_chat_ms_ = now_ms();
    if (reply.status != 200)
        return;
    const json tree = json::parse(reply.body, nullptr, false);
    if (tree.is_discarded() || !tree.is_object())
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    apply_tree(snapshot_, tree);
    publish();
}

void Client::poll_events()
{
    const Reply reply = http(port_, "GET", "/api/events?since=" + std::to_string(std::max(0L, events_next_)));
    if (reply.status != 200)
        return;
    const json e = json::parse(reply.body, nullptr, false);
    if (e.is_discarded())
        return;
    const long next = static_cast<long>(e.value("next", 0.0));
    if (events_next_ < 0) // first contact: start from now, history comes from /api/messages
    {
        events_next_ = next;
        return;
    }
    events_next_ = next;
    if (!e.contains("events") || e["events"].empty())
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const json &ev : e["events"])
    {
        const std::string type = text(ev, "t");
        const std::string channel = text(ev, "channel_id");
        auto list = snapshot_.messages.find(channel);
        if (list == snapshot_.messages.end())
            continue; // not open: its history loads when it is
        std::vector<Message> &ms = list->second;
        if (type == "MESSAGE_CREATE")
        {
            Message m = message_from(ev, snapshot_.me.id);
            // Replace the oldest pending message with the same text (our echo).
            auto pending = std::find_if(ms.begin(), ms.end(), [&](const Message &p) {
                return p.pending && p.author_id == m.author_id && p.content == m.content;
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
                    m.content = text(ev, "content");
                    m.edited = true;
                }
        }
        else if (type == "MESSAGE_DELETE")
        {
            ms.erase(std::remove_if(ms.begin(), ms.end(), [&](const Message &m) { return m.id == text(ev, "id"); }),
                     ms.end());
        }
    }
    publish();
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
        mention.content = text(m, "content");
        mention.time_ms = snowflake_ms(mention.message_id);
        if (m.contains("author") && m["author"].is_object())
            mention.author_id = text(m["author"], "id");
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
    publish();
}

void Client::handle(const Request &request)
{
    switch (request.kind)
    {
    case Request::Kind::open_channel:
    case Request::Kind::load_older: {
        std::string path = "/api/messages?channel=" + request.channel_id;
        if (!request.guild_id.empty())
            path += "&guild=" + request.guild_id;
        std::string oldest;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = snapshot_.messages.find(request.channel_id);
            if (request.kind == Request::Kind::load_older && it != snapshot_.messages.end() && !it->second.empty())
                oldest = it->second.front().id;
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
            hui::sys::log("[CLIENT] history for %s failed (%d)", request.channel_id.c_str(), reply.status);
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
            std::vector<Message> &ms = snapshot_.messages[request.channel_id];
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
        json body{{"channel", request.channel_id}, {"content", request.content}};
        const Reply reply = http(port_, "POST", "/api/send", body.dump());
        if (reply.status != 200)
            hui::sys::log("[CLIENT] send to %s failed (%d)", request.channel_id.c_str(), reply.status);
        break;
    }
    }
}

} // namespace discord
