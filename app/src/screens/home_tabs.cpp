// ps5-discord - Home's Friends, Servers and Mentions tabs.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Framework frames 02-04. Each tab is a page of cards laid out in page
// coordinates; focus moves to the nearest card in the pressed direction and
// the page scrolls on a spring to keep it in view. Leaving the top of a page
// goes back to the tab chips.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "screens/home.hpp"
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
constexpr float kPageTop = 132.0f;    // below the header
constexpr float kPageBottom = 995.0f; // above the hints
constexpr float kSectionLabel = 58.0f;

// Friends tab.
constexpr int kFriendColumns = 4;
constexpr float kFriendW = (kRowWidth - (kFriendColumns - 1) * kGap) / kFriendColumns;
constexpr float kFriendH = 236.0f;
// Servers tab.
constexpr int kServerColumns = 3;
constexpr float kServerW = (kRowWidth - (kServerColumns - 1) * kGap) / kServerColumns;
constexpr float kServerH = 330.0f;
constexpr float kBannerH = 196.0f;
// Mentions tab.
constexpr float kMentionH = 112.0f;
constexpr float kMentionGap = 12.0f;

enum class FriendGroup
{
    voice,
    online,
    away,
};

FriendGroup group_of(const discord::User &u)
{
    if (!u.voice_room.empty())
        return FriendGroup::voice;
    if (u.presence == discord::Presence::idle || u.presence == discord::Presence::dnd)
        return FriendGroup::away;
    return FriendGroup::online;
}

const char *group_name(FriendGroup g)
{
    switch (g)
    {
    case FriendGroup::voice:
        return "In voice";
    case FriendGroup::online:
        return "Online";
    case FriendGroup::away:
        return "Away";
    }
    return "";
}

// Section headings of the Friends page: where each starts and how many it holds.
struct Section
{
    FriendGroup group;
    float y;
    int count;
};

std::vector<Section> friend_sections(const discord::Snapshot &s)
{
    std::vector<Section> out;
    float y = kPageTop + kSectionLabel;
    for (FriendGroup g : {FriendGroup::voice, FriendGroup::online, FriendGroup::away})
    {
        int count = 0;
        for (const discord::User &u : s.friends)
            if (u.presence != discord::Presence::offline && group_of(u) == g)
                ++count;
        if (count == 0)
            continue;
        out.push_back({g, y, count});
        const int rows = (count + kFriendColumns - 1) / kFriendColumns;
        y += rows * (kFriendH + kGap) + kSectionLabel + 18.0f;
    }
    return out;
}

void pill_button(DrawList &list, const Ink &ink, const Rect &r, const char *text, bool hot, bool voice_icon)
{
    list.rounded_rect(r, r.h * 0.5f, hot ? kCoral : kRaisedHigh);
    const Color ink_color = hot ? kOnCoral : kText;
    float tx = r.cx();
    if (voice_icon)
    {
        const float tw = ink.fonts.semibold.measure(text, 23.0f);
        speaking_bars(list, r.cx() - tw * 0.5f - 6.0f, r.cy(), 22.0f, ink_color);
        tx += 14.0f;
    }
    ui::text(list, ink.fonts.semibold, text, tx, r.cy() + 8.0f, 23.0f, ink_color, gfx::Align::center);
}

// 213515 -> "213.5K": member counts of big servers.
std::string compact(int n)
{
    char out[16];
    if (n >= 1000000)
        std::snprintf(out, sizeof out, "%.1fM", n / 1e6);
    else if (n >= 10000)
        std::snprintf(out, sizeof out, "%.1fK", n / 1e3);
    else
        std::snprintf(out, sizeof out, "%d", n);
    return out;
}

} // namespace

std::vector<Home::TabItem> Home::layout_tab(const discord::Snapshot &s) const
{
    std::vector<TabItem> items;
    switch (tab_)
    {
    case HomeTab::friends: {
        for (const Section &section : friend_sections(s))
        {
            int n = 0;
            for (int i = 0; i < static_cast<int>(s.friends.size()); ++i)
            {
                const discord::User &u = s.friends[static_cast<std::size_t>(i)];
                if (u.presence == discord::Presence::offline || group_of(u) != section.group)
                    continue;
                const int col = n % kFriendColumns;
                const int row = n / kFriendColumns;
                items.push_back({{kMargin + col * (kFriendW + kGap), section.y + row * (kFriendH + kGap), kFriendW,
                                  kFriendH},
                                 i});
                ++n;
            }
        }
        break;
    }
    case HomeTab::servers:
        for (int i = 0; i < static_cast<int>(s.guilds.size()); ++i)
            items.push_back({{kMargin + (i % kServerColumns) * (kServerW + kGap),
                              kPageTop + 6.0f + (i / kServerColumns) * (kServerH + kGap), kServerW, kServerH},
                             i});
        break;
    case HomeTab::mentions:
        for (int i = 0; i < static_cast<int>(s.mentions.size()); ++i)
            items.push_back({{kMargin, kPageTop + kSectionLabel + i * (kMentionH + kMentionGap), kRowWidth, kMentionH}, i});
        break;
    default:
        break;
    }
    return items;
}

Rect Home::tab_focus_rect(const discord::Snapshot &s) const
{
    const std::vector<TabItem> items = layout_tab(s);
    const int t = static_cast<int>(tab_);
    if (items.empty()) // nothing to focus on the page: stay on its chip
        return item_rect(s, kChips, t);
    Rect r = items[static_cast<std::size_t>(std::clamp(tab_focus_[t], 0, static_cast<int>(items.size()) - 1))].rect;
    r.y -= tab_scroll_[t].value;
    return r;
}

void Home::update_tab(const discord::Snapshot &s, const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback)
{
    const std::vector<TabItem> items = layout_tab(s);
    const int t = static_cast<int>(tab_);
    int &focus = tab_focus_[t];
    focus = std::clamp(focus, 0, std::max(0, static_cast<int>(items.size()) - 1));

    if (input.nav != Direction::none && !items.empty())
    {
        const Rect from = items[static_cast<std::size_t>(focus)].rect;
        int dx = 0, dy = 0;
        switch (input.nav)
        {
        case Direction::up:
            dy = -1;
            break;
        case Direction::down:
            dy = 1;
            break;
        case Direction::left:
            dx = -1;
            break;
        case Direction::right:
            dx = 1;
            break;
        default:
            break;
        }
        // Nearest item whose centre lies in the pressed direction; sideways
        // distance counts double so moves stay in their row or column.
        int best = -1;
        float best_score = 1e9f;
        for (int i = 0; i < static_cast<int>(items.size()); ++i)
        {
            const Rect &r = items[static_cast<std::size_t>(i)].rect;
            const float ddx = r.cx() - from.cx();
            const float ddy = r.cy() - from.cy();
            const float along = dx != 0 ? ddx * dx : ddy * dy;
            const float across = dx != 0 ? std::fabs(ddy) : std::fabs(ddx);
            if (along <= 1.0f)
                continue;
            const float score = along + 2.0f * across;
            if (score < best_score)
            {
                best_score = score;
                best = i;
            }
        }
        if (best >= 0)
        {
            focus = best;
            feedback.play(hui::audio::Cue::focus, 1.0f + 0.02f * static_cast<float>(best % 6));
        }
        else if (dy < 0)
        {
            zone_ = kChips; // off the top of the page: back to the tabs
            feedback.play(hui::audio::Cue::focus, 0.96f);
        }
        else
        {
            feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.5f);
        }
    }
    if (input.is_pressed(Action::confirm) && !items.empty())
    {
        const int ref = items[static_cast<std::size_t>(focus)].ref;
        if (tab_ == HomeTab::mentions)
        {
            const discord::Mention &m = s.mentions[static_cast<std::size_t>(ref)];
            std::string guild;
            for (const discord::Guild &g : s.guilds)
                for (const discord::Channel &c : g.channels)
                    if (c.id == m.channel_id)
                        guild = g.id;
            open_ = HomeOpen{guild, m.channel_id};
        }
        else if (tab_ == HomeTab::servers)
        {
            // ponytail: the first text channel until the server page (framework frame 05) lands.
            const discord::Guild &g = s.guilds[static_cast<std::size_t>(ref)];
            for (const discord::Channel &c : g.channels)
                if (c.kind != discord::ChannelKind::voice)
                {
                    open_ = HomeOpen{g.id, c.id};
                    break;
                }
        }
        else if (tab_ == HomeTab::friends)
        {
            // Message: their DM, when one is known.
            const discord::User &u = s.friends[static_cast<std::size_t>(ref)];
            for (const discord::Conversation &c : s.recent)
                if (c.guild_id.empty() && c.user_id == u.id)
                    open_ = HomeOpen{"", c.channel_id};
        }
        feedback.play(open_ ? hui::audio::Cue::open : hui::audio::Cue::error);
    }

    // Keep the focused item between the header and the hints.
    hui::tween::Spring &scroll = tab_scroll_[t];
    if (!items.empty())
    {
        const Rect r = items[static_cast<std::size_t>(focus)].rect;
        float target = scroll.target;
        if (r.y - target < kPageTop + 8.0f)
            target = r.y - kPageTop - 8.0f;
        if (r.y + r.h - target > kPageBottom)
            target = r.y + r.h - kPageBottom;
        scroll.target = std::max(0.0f, target);
    }
    scroll.update(dt, 14.0f);
}

void Home::draw_tab(const discord::Snapshot &s, DrawList &list) const
{
    const std::vector<TabItem> items = layout_tab(s);
    const int t = static_cast<int>(tab_);
    const float scroll = tab_scroll_[t].value;
    const bool content_focus = zone_ != kChips;
    list.push_clip({0, kPageTop - 20.0f, 1920, kPageBottom - kPageTop + 40.0f});

    const Rect empty{kMargin, kPageTop + 10.0f, kRowWidth, 200.0f};
    if (tab_ == HomeTab::friends && items.empty())
        empty_note(list, ink_, empty, s.friends.empty() ? "No friends yet" : "No friends online",
                   s.friends.empty() ? "Friends you add on Discord show up here."
                                     : "Friends who come online show up here.");
    else if (tab_ == HomeTab::mentions && items.empty())
        empty_note(list, ink_, empty, "No mentions", "Messages that mention you show up here.");
    else if (tab_ == HomeTab::friends)
    {
        for (const Section &section : friend_sections(s))
            row_label(list, ink_, section.y - 22.0f - scroll, group_name(section.group), std::to_string(section.count));
        for (std::size_t i = 0; i < items.size(); ++i)
        {
            Rect r = items[i].rect;
            r.y -= scroll;
            if (r.y > 1080.0f || r.y + r.h < 0.0f)
                continue;
            const discord::User &u = s.friends[static_cast<std::size_t>(items[i].ref)];
            const bool focused = content_focus && static_cast<int>(i) == tab_focus_[t];
            list.rounded_rect(r, kCardRadius, kRaised);
            avatar(list, ink_, u, r.x + 86.0f, r.y + 88.0f, 62.0f);
            ui::text(list, ink_.fonts.semibold, fit(ink_.fonts.semibold, u.name, 30.0f, r.w - 190.0f), r.x + 172.0f,
                     r.y + 64.0f, 30.0f, kText);
            float line_y = r.y + 100.0f;
            if (!u.voice_room.empty())
            {
                ui::text(list, ink_.fonts.regular, fit(ink_.fonts.regular, u.voice_room, 23.0f, r.w - 190.0f),
                         r.x + 172.0f, line_y, 23.0f, kDim);
                line_y += 32.0f;
            }
            const Color dot = u.presence == discord::Presence::idle ? kIdle : kOnline;
            list.circle(r.x + 180.0f, line_y - 8.0f, 6.0f, dot);
            ui::text(list, ink_.fonts.regular, fit(ink_.fonts.regular, status_line(u), 23.0f, r.w - 210.0f),
                     r.x + 196.0f, line_y, 23.0f, kDim);
            // Message always; Join only when there is a room to join.
            const bool joinable = !u.voice_room.empty();
            const float bw = joinable ? (r.w - 3 * 20.0f) * 0.5f : r.w - 40.0f;
            const Rect message{r.x + 20.0f, r.y + r.h - 76.0f, bw, 56.0f};
            pill_button(list, ink_, message, "Message", false, false);
            if (joinable)
                pill_button(list, ink_, {message.x + bw + 20.0f, message.y, bw, 56.0f}, "Join", focused, true);
        }
    }
    else if (tab_ == HomeTab::servers)
    {
        for (std::size_t i = 0; i < items.size(); ++i)
        {
            Rect r = items[i].rect;
            r.y -= scroll;
            if (r.y > 1080.0f || r.y + r.h < 0.0f)
                continue;
            const discord::Guild &g = s.guilds[static_cast<std::size_t>(items[i].ref)];
            list.rounded_rect(r, kCardRadius, kRaised);
            list.push_clip({r.x, r.y, r.w, kBannerH});
            scene(list, ink_, g.art, {r.x, r.y, r.w, kBannerH + kCardRadius}, kCardRadius);
            list.pop_clip();
            const float icx = r.x + 72.0f;
            const float icy = r.y + kBannerH + 8.0f;
            list.circle(icx, icy, 60.0f, kRaised);
            art_circle(list, ink_, g.art + 2, g.name.substr(0, 1), icx, icy, 54.0f);
            ui::text(list, ink_.fonts.semibold, fit(ink_.fonts.semibold, g.name, 31.0f, r.w - 250.0f), r.x + 148.0f,
                     r.y + kBannerH + 52.0f, 31.0f, kText);
            int live = 0;
            for (const discord::Room &room : s.live_rooms)
                live += room.guild_id == g.id ? 1 : 0;
            // Member presence is not tracked per server yet; the member count stands in.
            const std::string line = std::to_string(live) + (live == 1 ? " live room · " : " live rooms · ") +
                                     (g.online > 0 ? std::to_string(g.online) + " online"
                                                   : compact(g.members) + " members");
            const float badge = g.unread > 0 ? count_badge_width(ink_, g.unread, 44.0f) + 30.0f : 0.0f;
            list.circle(r.x + 156.0f, r.y + kBannerH + 88.0f, 6.0f, kOnline);
            ui::text(list, ink_.fonts.regular, fit(ink_.fonts.regular, line, 23.0f, r.w - 196.0f - badge),
                     r.x + 172.0f, r.y + kBannerH + 96.0f, 23.0f, kDim);
            if (g.unread > 0)
                count_badge(list, ink_, g.unread, r.x + r.w - 26.0f, r.y + kBannerH + 66.0f, 44.0f);
        }
    }
    else if (tab_ == HomeTab::mentions)
    {
        int unread = 0;
        for (const discord::Mention &m : s.mentions)
            unread += m.unread ? 1 : 0;
        row_label(list, ink_, kPageTop + kSectionLabel - 22.0f - scroll, "Mentions",
                  unread > 0 ? std::to_string(unread) + " unread" : std::string(), true);
        for (std::size_t i = 0; i < items.size(); ++i)
        {
            Rect r = items[i].rect;
            r.y -= scroll;
            if (r.y > 1080.0f || r.y + r.h < 0.0f)
                continue;
            const discord::Mention &m = s.mentions[static_cast<std::size_t>(items[i].ref)];
            const bool focused = content_focus && static_cast<int>(i) == tab_focus_[t];
            list.rounded_rect(r, 16.0f, kRaised);
            list.push_opacity(m.unread ? 1.0f : 0.72f);
            if (m.unread)
                list.circle(r.x + 32.0f, r.cy(), 8.0f, kCoral);
            if (const discord::User *author = discord::find_user(s, m.author_id))
            {
                discord::User face = *author;
                face.voice_room.clear();
                avatar(list, ink_, face, r.x + 106.0f, r.cy(), 40.0f);
                const float nx = r.x + 168.0f;
                const float nw = ui::text(list, ink_.fonts.semibold, author->name, nx, r.y + 44.0f, 25.0f, kText);
                ui::text(list, ink_.fonts.regular, fit(ink_.fonts.regular, m.context, 21.0f, r.w - 600.0f),
                         nx + nw + 18.0f, r.y + 44.0f, 21.0f, kDim);
            }
            ui::text(list, ink_.fonts.regular, fit(ink_.fonts.regular, m.content, 29.0f, r.w - 520.0f), r.x + 168.0f,
                     r.y + 88.0f, 29.0f, kText);
            ui::text(list, ink_.fonts.regular, ago(s.now_ms, m.time_ms), r.x + r.w - 220.0f, r.y + 46.0f, 20.0f, kDim,
                     gfx::Align::right);
            const Rect jump{r.x + r.w - 186.0f, r.cy() - 30.0f, 162.0f, 60.0f};
            list.rounded_rect(jump, 30.0f, m.unread || focused ? kCoral : kRaisedHigh);
            const Color on = m.unread || focused ? kOnCoral : kText;
            ui::draw_button(list, ink_.fonts, ui::GlyphStyle::mono(on, Color::rgb(0, 0.0f)), ui::Button::cross,
                            jump.x + 26.0f, jump.cy(), 28.0f);
            ui::text(list, ink_.fonts.semibold, "Jump", jump.x + 68.0f, jump.cy() + 9.0f, 25.0f, on);
            list.pop_opacity();
        }
    }
    list.pop_clip();
}

} // namespace screens
