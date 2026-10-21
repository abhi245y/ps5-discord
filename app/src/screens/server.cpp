// ps5-discord - A server's own page, between the Servers tab and its chat.
// SPDX-License-Identifier: GPL-3.0-or-later
//
//   - a header with its icon, name and size, and the voice pill;
//   - its banner (or invite splash, else art);
//   - Voice rooms: who is in each, and Cross joins one;
//   - Text channels: by latest activity, with unread counts and the last line
//     when its history is already loaded.
// Up/Down pick the row, Left/Right walk it, Cross opens a channel.

#include "screens/server.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "screens/style.hpp"
#include "ui/glyphs.hpp"

namespace screens
{

namespace
{

using namespace style;
namespace gfx = hui::gfx;
namespace ui = hui::ui;
using hui::Action;
using hui::Direction;

constexpr float kRight = 1920.0f - kMargin;
constexpr float kRowWidth = kRight - kMargin;
constexpr float kGap = 22.0f;

constexpr float kIconR = 58.0f;
constexpr float kIconCy = 96.0f;
constexpr float kBannerY = 184.0f;
constexpr float kBannerH = 296.0f;
constexpr float kRoomsLabel = 540.0f;
constexpr float kRoomY = 562.0f;
constexpr float kRoomW = 560.0f;
constexpr float kRoomH = 210.0f;
constexpr float kChannelsLabel = 836.0f;
constexpr float kChannelY = 858.0f;
constexpr float kChannelW = 420.0f;
constexpr float kChannelH = 120.0f;

std::string initial_of(const std::string &name)
{
    if (name.empty())
        return "?";
    const unsigned char c = static_cast<unsigned char>(name[0]);
    const std::size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    return name.substr(0, n);
}

} // namespace

void Server::open(const std::string &guild_id)
{
    if (guild_id != guild_id_)
    {
        index_ = {};
        for (hui::tween::Spring &s : scroll_)
            s.snap(0.0f);
        zone_ = kChannels;
    }
    guild_id_ = guild_id;
}

const discord::Guild *Server::find(const discord::Snapshot &s) const
{
    for (const discord::Guild &g : s.guilds)
        if (g.id == guild_id_)
            return &g;
    return nullptr;
}

std::vector<const discord::Channel *> Server::channels(const discord::Guild &g) const
{
    std::vector<const discord::Channel *> out;
    for (const discord::Channel &c : g.channels)
        if (c.kind != discord::ChannelKind::voice)
            out.push_back(&c);
    std::stable_sort(out.begin(), out.end(), [](auto *a, auto *b) { return a->last_ms > b->last_ms; });
    return out;
}

std::vector<const discord::Room *> Server::rooms(const discord::Guild &g) const
{
    std::vector<const discord::Room *> out;
    for (const discord::Room &r : g.rooms)
        out.push_back(&r);
    std::stable_sort(out.begin(), out.end(), [](auto *a, auto *b) { return a->member_count > b->member_count; });
    return out;
}

int Server::count(const discord::Guild &g, int zone) const
{
    return zone == kRooms ? static_cast<int>(g.rooms.size()) : static_cast<int>(channels(g).size());
}

Rect Server::item_rect(int zone, int index) const
{
    const float w = zone == kRooms ? kRoomW : kChannelW;
    const float x = kMargin + static_cast<float>(index) * (w + kGap) - scroll_[static_cast<std::size_t>(zone)].value;
    return zone == kRooms ? Rect{x, kRoomY, w, kRoomH} : Rect{x, kChannelY, w, kChannelH};
}

bool Server::update(const discord::Snapshot &s, const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback)
{
    const discord::Guild *g = find(s);
    if (!g || input.is_pressed(Action::back))
    {
        feedback.play(hui::audio::Cue::back);
        return false; // also when the server is gone (left, or removed)
    }
    if (count(*g, zone_) == 0)
        zone_ = count(*g, kChannels) > 0 ? kChannels : kRooms;
    int &index = index_[static_cast<std::size_t>(zone_)];
    const int n = count(*g, zone_);
    index = std::clamp(index, 0, std::max(0, n - 1));

    if (input.nav == Direction::left || input.nav == Direction::right)
    {
        const int next = index + (input.nav == Direction::right ? 1 : -1);
        if (next >= 0 && next < n)
        {
            index = next;
            feedback.play(hui::audio::Cue::focus, input.nav == Direction::right ? 1.02f : 0.98f);
        }
        else
            feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.5f);
    }
    else if (input.nav == Direction::up || input.nav == Direction::down)
    {
        const int want = input.nav == Direction::up ? kRooms : kChannels;
        if (want != zone_ && count(*g, want) > 0)
        {
            zone_ = want;
            feedback.play(hui::audio::Cue::focus);
        }
        else
            feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.5f);
    }
    if (input.is_pressed(Action::confirm) && n > 0)
    {
        if (zone_ == kChannels)
        {
            open_ = channels(*g)[static_cast<std::size_t>(index_[kChannels])]->id;
            feedback.play(hui::audio::Cue::open);
        }
        else
        {
            room_ = rooms(*g)[static_cast<std::size_t>(index_[kRooms])]->id;
            feedback.play(hui::audio::Cue::open);
        }
    }

    // Each row scrolls to keep its focused card on screen.
    for (int z = 0; z < kZones; ++z)
    {
        hui::tween::Spring &scroll = scroll_[static_cast<std::size_t>(z)];
        const float w = z == kRooms ? kRoomW : kChannelW;
        const float x = static_cast<float>(index_[static_cast<std::size_t>(z)]) * (w + kGap);
        if (x - scroll.target < 0.0f)
            scroll.target = x;
        if (x + w - scroll.target > kRowWidth)
            scroll.target = x + w - kRowWidth;
        scroll.update(dt, 16.0f);
    }
    return true;
}

void Server::draw(const discord::Snapshot &s, DrawList &list) const
{
    const discord::Guild *g = find(s);
    if (!g)
        return;

    // Header.
    art_circle(list, ink_, g->art, initial_of(g->name), kMargin + kIconR, kIconCy, kIconR, g->icon_url);
    list.ring(kMargin + kIconR, kIconCy, kIconR + 3.0f, 3.0f, kLine);
    const float tx = kMargin + kIconR * 2.0f + 32.0f;
    const float title_w = s.voice.active() ? 1000.0f : 1600.0f;
    ui::text(list, ink_.fonts.display, fit(ink_.fonts.display, g->name, 60.0f, title_w), tx, kIconCy - 4.0f, 60.0f, kText);
    const std::vector<const discord::Channel *> chans = channels(*g);
    std::string sub = compact(g->members) + " members";
    if (g->unread > 0)
        sub += " · " + std::to_string(g->unread) + " mentions";
    ui::text(list, ink_.fonts.regular, sub, tx, kIconCy + 44.0f, 27.0f, kDim);
    voice_dock(list, ink_, s, 46.0f, 64.0f, kRight);

    // Banner.
    const Rect banner{kMargin, kBannerY, kRowWidth, kBannerH};
    scene(list, ink_, g->art, banner, kCardRadius, g->banner_url);
    list.gradient_rect({banner.x, banner.y + banner.h * 0.55f, banner.w, banner.h * 0.45f}, kCardRadius,
                       Color::rgb(0x16181d, 0.0f), Color::rgb(0x16181d, 0.45f));

    // Voice rooms.
    row_label(list, ink_, kRoomsLabel, "Voice rooms", std::to_string(g->rooms.size()));
    if (g->rooms.empty())
        empty_note(list, ink_, {kMargin, kRoomY, kRowWidth, kRoomH}, "No voice rooms you can see",
                   "Rooms you can join show up here.");
    const std::vector<const discord::Room *> rs = rooms(*g);
    for (int i = 0; i < static_cast<int>(rs.size()); ++i)
    {
        const Rect r = item_rect(kRooms, i);
        if (r.x > 1920.0f || r.x + r.w < 0.0f)
            continue;
        const discord::Room &room = *rs[static_cast<std::size_t>(i)];
        const bool focused = zone_ == kRooms && index_[kRooms] == i;
        const bool empty = room.member_count == 0;
        if (empty)
            list.push_opacity(0.55f);
        scene(list, ink_, room.art, r, kCardRadius);
        list.gradient_rect_h({r.x, r.y, r.w * 0.8f, r.h}, kCardRadius, Color::rgb(0x0d0e12, 0.85f),
                             Color::rgb(0x0d0e12, 0.0f));
        list.gradient_rect({r.x, r.y + r.h * 0.45f, r.w, r.h * 0.55f}, kCardRadius, Color::rgb(0x0d0e12, 0.0f),
                           Color::rgb(0x0d0e12, 0.65f));
        list.circle(r.x + 58.0f, r.y + 58.0f, 32.0f, Color::rgb(0x16181d, 0.8f));
        speaking_bars(list, r.x + 58.0f, r.y + 58.0f, 30.0f, empty ? kDim : kCoral);
        ui::text(list, ink_.fonts.semibold, fit(ink_.fonts.semibold, room.name, 32.0f, r.w - 130.0f), r.x + 106.0f,
                 r.y + 56.0f, 32.0f, kText);
        // Who is in it; who is talking needs the voice connection itself.
        const std::string in = empty ? std::string("Empty") : std::to_string(room.member_count) + " in room";
        ui::text(list, ink_.fonts.regular, in, r.x + 106.0f, r.y + 90.0f, 24.0f, Color::rgb(0xd8dbe2));
        const float ay = r.y + r.h - 50.0f;
        float ax = r.x + 52.0f;
        int drawn = 0;
        for (const std::string &id : room.member_ids)
        {
            if (drawn == 4)
                break;
            if (const discord::User *u = discord::find_user(s, id))
            {
                list.circle(ax, ay, 30.0f, kGround);
                discord::User face = *u;
                face.voice_room.clear();
                avatar(list, ink_, face, ax, ay, 26.0f, false);
                ax += 52.0f;
                ++drawn;
            }
        }
        if (room.member_count > drawn)
        {
            list.circle(ax, ay, 30.0f, Color::rgb(0x16181d, 0.85f));
            ui::text(list, ink_.fonts.semibold, "+" + std::to_string(room.member_count - drawn), ax, ay + 8.0f, 22.0f,
                     kText, gfx::Align::center);
        }
        if (focused)
        {
            // Cross joins the focused room; the room's own screen follows it.
            const bool here = s.voice.active() && s.voice.channel_id == room.id;
            const Rect join{r.x + r.w - 236.0f, ay - 28.0f, 208.0f, 56.0f};
            list.rounded_rect(join, 28.0f, Color::rgb(0x16181d, 0.85f));
            ui::text(list, ink_.fonts.semibold, here ? "In call" : "Join", join.cx(), join.cy() + 9.0f, 24.0f,
                     here ? kOnline : kText, gfx::Align::center);
        }
        if (empty)
            list.pop_opacity();
        if (focused)
            focus_outline(list, r, kCardRadius + 4.0f, 1.0f);
    }

    // Text channels.
    row_label(list, ink_, kChannelsLabel, "Text channels", std::to_string(chans.size()));
    for (int i = 0; i < static_cast<int>(chans.size()); ++i)
    {
        const Rect r = item_rect(kChannels, i);
        if (r.x > 1920.0f || r.x + r.w < 0.0f)
            continue;
        const discord::Channel &c = *chans[static_cast<std::size_t>(i)];
        list.rounded_rect(r, 16.0f, c.mentioned ? kMentionTint : kRaised);
        const float icx = r.x + 52.0f, icy = r.cy();
        list.circle(icx, icy, 30.0f, c.mentioned ? Color::rgb(0x5a3a33) : kRaisedHigh);
        ui::text(list, ink_.fonts.semibold, c.mentioned ? "@" : "#", icx, icy + 13.0f, 36.0f, c.mentioned ? kCoral : kText,
                 gfx::Align::center);
        const float x = r.x + 98.0f;
        const std::string when = c.last_ms > 0 ? ago(s.now_ms, c.last_ms) : std::string();
        const float when_w = ink_.fonts.regular.measure(when, 19.0f);
        ui::text(list, ink_.fonts.regular, when, r.x + r.w - 20.0f, r.y + 44.0f, 19.0f, kDim, gfx::Align::right);
        ui::text(list, ink_.fonts.semibold, fit(ink_.fonts.semibold, c.name, 25.0f, r.w - 98.0f - when_w - 32.0f), x,
                 r.y + 46.0f, 25.0f, c.has_unread || c.mentioned ? kText : kDim);
        // The last line when its history is loaded, else what the channel is for.
        std::string line = c.topic.empty() ? c.category : c.topic;
        if (const auto it = s.messages.find(c.id); it != s.messages.end() && it->second && !it->second->empty())
        {
            const discord::Message &m = it->second->back();
            const discord::User *u = discord::find_user(s, m.author_id);
            line = (u ? u->name : m.author_name) + ": " + discord::plain_text(m);
        }
        for (char &ch : line)
            if (ch == '\n')
                ch = ' ';
        float badge_w = 0.0f;
        if (c.unread > 0)
            badge_w = count_badge(list, ink_, c.unread, r.x + r.w - 20.0f, r.y + 84.0f, 34.0f) + 12.0f;
        else if (c.has_unread)
        {
            list.circle(r.x + r.w - 30.0f, r.y + 84.0f, 6.0f, kText);
            badge_w = 28.0f;
        }
        ui::text(list, ink_.fonts.regular, fit(ink_.fonts.regular, line, 22.0f, r.w - 98.0f - 20.0f - badge_w), x,
                 r.y + 90.0f, 22.0f, Color::rgb(0xc9cdd6));
        if (zone_ == kChannels && index_[kChannels] == i)
            focus_outline(list, r, 20.0f, 1.0f);
    }

    ui::Hint hints[3] = {{ui::Button::cross, "Select"}, {ui::Button::circle, "Back"}};
    int count = 2;
    if (s.voice.active())
        hints[count++] = {ui::Button::square, "Voice"};
    ui::HintLayout layout;
    layout.size = 34.0f;
    layout.text_size = 23.0f;
    layout.cy = kHintsBaseline;
    layout.item_gap = 38.0f;
    ui::draw_hints(list, ink_.fonts, hint_glyphs(), hints, count, kRight, true, layout);
}

} // namespace screens
