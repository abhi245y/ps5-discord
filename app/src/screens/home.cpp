// ps5-discord - Home: who is around, what is live, what was said.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The framework's "TV Media Rows" Home (design/framework, approved comp
// .impeccable/mocks/decision/r3/crowded.png):
//   - a header with the page title, the tab chips (L1/R1) and the voice pill;
//   - Friends online: big round avatars, in-voice rings, status badges, a +N chip;
//   - Live rooms: the dominant row, scene cards with who is talking and Join;
//   - Recent conversations: four cards with previews and unread counts.
// One coral outline glides between rows on springs; each row scrolls on its
// own spring to keep its focused item in view, and sizes never shrink for a
// busy account: overflow becomes counts and a peeking card.

#include "screens/home.hpp"

#include <algorithm>
#include <cmath>
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

// Header.
constexpr float kHeaderY = 34.0f;
constexpr float kHeaderH = 68.0f;
// Friends row.
constexpr float kFriendsLabel = 164.0f;
constexpr float kAvatarR = 78.0f;
constexpr float kAvatarCy = 268.0f;
constexpr float kAvatarStep = 232.0f;
constexpr int kFriendsShown = 7; // then a +N chip
// Live rooms row.
constexpr float kRoomsLabel = 480.0f;
constexpr float kRoomY = 502.0f;
constexpr float kRoomW = 562.0f;
constexpr float kRoomH = 262.0f;
constexpr float kGap = 22.0f;
// Recent conversations row.
constexpr float kRecentLabel = 840.0f;
constexpr float kRecentY = 860.0f;
constexpr float kRecentW = (kRowWidth - 3 * kGap) / 4.0f;
constexpr float kRecentH = 124.0f;

const char *const kTabNames[] = {"All", "Friends", "Servers", "Mentions"};

// Friends in the order the row shows them: talking, in voice, playing, online, idle.
std::vector<const discord::User *> ordered_friends(const discord::Snapshot &s)
{
    std::vector<const discord::User *> out;
    for (const discord::User &u : s.friends)
        if (u.presence != discord::Presence::offline)
            out.push_back(&u);
    auto rank = [](const discord::User *u) {
        if (u->speaking)
            return 0;
        if (!u->voice_room.empty())
            return 1;
        if (!u->activity.empty())
            return 2;
        return u->presence == discord::Presence::online ? 3 : 4;
    };
    std::stable_sort(out.begin(), out.end(), [&](auto *a, auto *b) { return rank(a) < rank(b); });
    return out;
}

} // namespace

// ---- focus -------------------------------------------------------------------

void Home::Glide::to(const Rect &r, bool snap)
{
    if (snap)
    {
        x.snap(r.x);
        y.snap(r.y);
        w.snap(r.w);
        h.snap(r.h);
        return;
    }
    x.target = r.x;
    y.target = r.y;
    w.target = r.w;
    h.target = r.h;
}

void Home::Glide::update(float dt)
{
    for (hui::tween::Spring *s : {&x, &y, &w, &h})
        s->update(dt, 18.0f);
}

int Home::row_count(const discord::Snapshot &s, int zone) const
{
    switch (zone)
    {
    case kChips:
        return static_cast<int>(HomeTab::count);
    case kFriends: {
        const int shown = std::min<int>(kFriendsShown, static_cast<int>(ordered_friends(s).size()));
        return shown + (s.friends_online > shown ? 1 : 0);
    }
    case kRooms:
        return std::max<int>(1, static_cast<int>(s.live_rooms.size()));
    case kRecent:
        return static_cast<int>(s.recent.size());
    }
    return 0;
}

// Where an item sits now, scroll applied.
Rect Home::item_rect(const discord::Snapshot &s, int zone, int index) const
{
    switch (zone)
    {
    case kChips: {
        const float title_w = ink_.fonts.display.measure("Home", kTitleSize);
        float x = kMargin + title_w + 40.0f + 6.0f;
        for (int i = 0; i < index; ++i)
        {
            x += ink_.fonts.semibold.measure(kTabNames[i], 25.0f) + 52.0f + 4.0f; // Mentions is last
        }
        float w = ink_.fonts.semibold.measure(kTabNames[index], 25.0f) + 52.0f;
        if (index == static_cast<int>(HomeTab::mentions) && s.mention_count > 0)
            w += count_badge_width(ink_, s.mention_count) + 2.0f;
        return {x, kHeaderY + 6.0f, w, kHeaderH - 12.0f};
    }
    case kFriends: {
        const float cx = kMargin + kAvatarR + 10.0f + kAvatarStep * index - scroll_[kFriends].value;
        return {cx - kAvatarR - 12.0f, kAvatarCy - kAvatarR - 12.0f, (kAvatarR + 12.0f) * 2, (kAvatarR + 12.0f) * 2};
    }
    case kRooms:
        if (s.live_rooms.empty()) // the one empty card spans the row
            return {kMargin, kRoomY, kRowWidth, kRoomH};
        return {kMargin + (kRoomW + kGap) * index - scroll_[kRooms].value, kRoomY, kRoomW, kRoomH};
    case kRecent:
        return {kMargin + (kRecentW + kGap) * index - scroll_[kRecent].value, kRecentY, kRecentW, kRecentH};
    }
    return {};
}

void Home::enter()
{
    placed_ = false;
}

void Home::move(const discord::Snapshot &s, int dx, int dy, hui::ui::Feedback &feedback)
{
    if (dy != 0)
    {
        int zone = zone_ + dy;
        while (zone >= 0 && zone < kZones && row_count(s, zone) == 0)
            zone += dy;
        if (zone < 0 || zone >= kZones)
        {
            feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.5f);
            return;
        }
        // Keep roughly the same column when changing rows.
        const float x = item_rect(s, zone_, index_[zone_]).cx();
        int best = 0;
        float best_d = 1e9f;
        for (int i = 0; i < row_count(s, zone); ++i)
        {
            const float d = std::fabs(item_rect(s, zone, i).cx() - x);
            if (d < best_d)
            {
                best_d = d;
                best = i;
            }
        }
        if (zone != kChips)
            index_[zone] = best;
        zone_ = zone;
        feedback.play(hui::audio::Cue::focus, 0.96f + 0.03f * static_cast<float>(zone));
        return;
    }
    const int next = index_[zone_] + dx;
    if (next < 0 || next >= row_count(s, zone_))
    {
        feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.5f);
        return;
    }
    index_[zone_] = next;
    feedback.play(hui::audio::Cue::focus, 1.0f + 0.02f * static_cast<float>(next));
}

void Home::update(const discord::Snapshot &s, const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback)
{
    for (int z = 0; z < kZones; ++z)
        index_[z] = std::clamp(index_[z], 0, std::max(0, row_count(s, z) - 1));

    if (input.is_pressed(Action::page_next) || input.is_pressed(Action::page_prev))
    {
        const int n = static_cast<int>(HomeTab::count);
        const int step = input.is_pressed(Action::page_next) ? 1 : -1;
        tab_ = static_cast<HomeTab>((static_cast<int>(tab_) + step + n) % n);
        index_[kChips] = static_cast<int>(tab_);
        if (zone_ != kChips)
            zone_ = tab_ == HomeTab::all ? kRooms : kFriends;
        feedback.play(hui::audio::Cue::tab, 1.0f, step * 0.3f);
    }
    if (tab_ != HomeTab::all && zone_ != kChips)
    {
        update_tab(s, input, dt, feedback);
    }
    else if (zone_ == kChips && tab_ != HomeTab::all)
    {
        // On the chips of a tab page: sideways picks a tab, down enters the page.
        if (input.nav == Direction::down)
        {
            zone_ = kFriends; // any zone other than chips means "in the page"
            feedback.play(hui::audio::Cue::focus, 1.0f);
        }
        else if (input.nav == Direction::left || input.nav == Direction::right)
            move(s, input.nav == Direction::left ? -1 : 1, 0, feedback);
        if (input.is_pressed(Action::confirm))
        {
            tab_ = static_cast<HomeTab>(index_[kChips]);
            feedback.play(hui::audio::Cue::tab);
        }
    }
    else
    {
        switch (input.nav)
        {
        case Direction::up:
            move(s, 0, -1, feedback);
            break;
        case Direction::down:
            move(s, 0, 1, feedback);
            break;
        case Direction::left:
            move(s, -1, 0, feedback);
            break;
        case Direction::right:
            move(s, 1, 0, feedback);
            break;
        default:
            break;
        }
        if (input.is_pressed(Action::confirm))
        {
            if (zone_ == kChips)
                tab_ = static_cast<HomeTab>(index_[kChips]);
            else if (zone_ == kRecent && index_[kRecent] < static_cast<int>(s.recent.size()))
            {
                const discord::Conversation &c = s.recent[static_cast<std::size_t>(index_[kRecent])];
                open_ = HomeOpen{c.guild_id, c.channel_id};
            }
            // ponytail: Join on a live room comes with voice (0.4.0).
            feedback.play(open_ ? hui::audio::Cue::open : hui::audio::Cue::select);
        }

    }
    // Scroll each row so its focused item is fully on screen, with a peek of the next.
    for (int z = kFriends; z < kZones; ++z)
    {
        const float item_x = item_rect(s, z, index_[z]).x + scroll_[z].target;
        const float item_w = item_rect(s, z, index_[z]).w;
        float target = scroll_[z].target;
        if (item_x - target < kMargin)
            target = item_x - kMargin;
        if (item_x - target + item_w > kRight)
            target = item_x + item_w - kRight;
        scroll_[z].target = std::max(0.0f, target);
        if (placed_)
            scroll_[z].update(dt, 14.0f);
        else
            scroll_[z].snap(scroll_[z].target);
    }
    focus_.to(tab_ != HomeTab::all && zone_ != kChips ? tab_focus_rect(s) : item_rect(s, zone_, index_[zone_]),
              !placed_);
    focus_.update(dt);
    placed_ = true;
}

// ---- drawing -----------------------------------------------------------------

void Home::draw_header(const discord::Snapshot &s, DrawList &list) const
{
    ui::text(list, ink_.fonts.display, "Home", kMargin, kTitleBaseline, kTitleSize, kText);

    // Tab chips in one pill.
    const Rect first = item_rect(s, kChips, 0);
    const Rect last = item_rect(s, kChips, static_cast<int>(HomeTab::count) - 1);
    list.rounded_rect({first.x - 6.0f, kHeaderY, last.x + last.w - first.x + 12.0f, kHeaderH}, kHeaderH * 0.5f,
                      kRaised);
    for (int i = 0; i < static_cast<int>(HomeTab::count); ++i)
    {
        const Rect r = item_rect(s, kChips, i);
        const bool on = static_cast<int>(tab_) == i;
        if (on)
            list.rounded_rect(r, r.h * 0.5f, kRaisedHigh);
        const float tx = r.x + 26.0f;
        const float tw = ui::text(list, ink_.fonts.semibold, kTabNames[i], tx, r.cy() + 9.0f, 25.0f, on ? kText : kDim);
        if (i == static_cast<int>(HomeTab::mentions) && s.mention_count > 0)
            count_badge(list, ink_, s.mention_count, tx + tw + 12.0f + count_badge_width(ink_, s.mention_count), r.cy());
    }

    voice_pill(list, ink_, s.voice, kHeaderY, kHeaderH);
}

void Home::draw_friends(const discord::Snapshot &s, DrawList &list) const
{
    const std::vector<const discord::User *> people = ordered_friends(s);
    row_label(list, ink_, kFriendsLabel, "Friends online", std::to_string(s.friends_online));
    if (people.empty())
    {
        ui::text(list, ink_.fonts.regular,
                 s.friends.empty() ? "Friends you add on Discord show up here." : "None of your friends are online.",
                 kMargin, kAvatarCy + 10.0f, kBodySize, kDim);
        return;
    }
    const int shown = std::min<int>(kFriendsShown, static_cast<int>(people.size()));
    for (int i = 0; i < row_count(s, kFriends); ++i)
    {
        const Rect r = item_rect(s, kFriends, i);
        const float cx = r.cx();
        if (r.x > 1920.0f || r.x + r.w < 0.0f)
            continue;
        if (i >= shown) // the +N chip
        {
            list.rounded_rect({cx - kAvatarR * 0.82f, kAvatarCy - kAvatarR * 0.82f, kAvatarR * 1.64f, kAvatarR * 1.64f},
                              kAvatarR * 0.82f, kRaised);
            ui::text(list, ink_.fonts.semibold, "+" + std::to_string(s.friends_online - shown), cx, kAvatarCy + 13.0f,
                     38.0f, kText, gfx::Align::center);
            continue;
        }
        const discord::User &u = *people[static_cast<std::size_t>(i)];
        avatar(list, ink_, u, cx, kAvatarCy, kAvatarR);
        const float name_y = kAvatarCy + kAvatarR + 46.0f;
        ui::text(list, ink_.fonts.semibold, fit(ink_.fonts.semibold, u.name, kNameSize, kAvatarStep - 16.0f), cx, name_y,
                 kNameSize, kText, gfx::Align::center);
        const std::string status = fit(ink_.fonts.regular, status_line(u), kMetaSize, kAvatarStep - 40.0f);
        const float sw = ink_.fonts.regular.measure(status, kMetaSize);
        const Color dot = u.presence == discord::Presence::idle ? kIdle : kOnline;
        list.circle(cx - sw * 0.5f - 12.0f, name_y + 26.0f, 6.0f, dot);
        ui::text(list, ink_.fonts.regular, status, cx - sw * 0.5f + 2.0f, name_y + 33.0f, kMetaSize, kDim);
    }
}

void Home::draw_rooms(const discord::Snapshot &s, DrawList &list) const
{
    row_label(list, ink_, kRoomsLabel, "Live rooms", std::to_string(s.live_rooms.size()));
    if (s.live_rooms.empty())
    {
        empty_note(list, ink_, {kMargin, kRoomY, kRowWidth, kRoomH}, "No one is in voice right now",
                   "Rooms with people in them show up here.");
        return;
    }
    for (int i = 0; i < static_cast<int>(s.live_rooms.size()); ++i)
    {
        const Rect r = item_rect(s, kRooms, i);
        if (r.x > 1920.0f || r.x + r.w < 0.0f)
            continue;
        const discord::Room &room = s.live_rooms[static_cast<std::size_t>(i)];
        const bool focused = zone_ == kRooms && index_[kRooms] == i;
        scene(list, ink_, room.art, r, kCardRadius);
        // Darken the left side for the text, and the bottom for the avatars.
        list.gradient_rect_h({r.x, r.y, r.w * 0.75f, r.h}, kCardRadius, Color::rgb(0x0d0e12, 0.82f),
                             Color::rgb(0x0d0e12, 0.0f));
        list.gradient_rect({r.x, r.y + r.h * 0.5f, r.w, r.h * 0.5f}, kCardRadius, Color::rgb(0x0d0e12, 0.0f),
                           Color::rgb(0x0d0e12, 0.6f));
        // Room icon: coral bars in a dark disc.
        list.circle(r.x + 62.0f, r.y + 64.0f, 34.0f, Color::rgb(0x16181d, 0.78f));
        speaking_bars(list, r.x + 62.0f, r.y + 64.0f, 34.0f, kCoral);
        ui::text(list, ink_.fonts.semibold, fit(ink_.fonts.semibold, room.guild_name, 34.0f, r.w - 140.0f),
                 r.x + 112.0f, r.y + 62.0f, 34.0f, kText);
        const std::string sub = room.name + " · " + std::to_string(room.member_count) + " talking";
        ui::text(list, ink_.fonts.regular, fit(ink_.fonts.regular, sub, 24.0f, r.w - 140.0f), r.x + 112.0f,
                 r.y + 98.0f, 24.0f, Color::rgb(0xd8dbe2));
        // Who is in it.
        const float ay = r.y + r.h - 54.0f;
        float ax = r.x + 54.0f;
        int drawn = 0;
        for (const std::string &id : room.member_ids)
        {
            if (drawn == 4)
                break;
            if (const discord::User *u = discord::find_user(s, id))
            {
                list.circle(ax, ay, 30.0f, kGround);
                discord::User face = *u;
                face.voice_room.clear(); // no ring inside a room card
                avatar(list, ink_, face, ax, ay, 26.0f, false);
                list.circle(ax + 18.0f, ay + 18.0f, 8.0f, kGround);
                list.circle(ax + 18.0f, ay + 18.0f, 5.5f, kOnline);
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
            const Rect join{r.x + r.w - 178.0f, ay - 30.0f, 150.0f, 60.0f};
            list.rounded_rect(join, 30.0f, kCoral);
            ui::text(list, ink_.fonts.semibold, "Join", join.cx(), join.cy() + 10.0f, 27.0f, kOnCoral, gfx::Align::center);
        }
    }
}

void Home::draw_recent(const discord::Snapshot &s, DrawList &list) const
{
    row_label(list, ink_, kRecentLabel, "Recent conversations",
          s.unread_total > 0 ? std::to_string(s.unread_total) + " unread" : std::string(), true);
    for (int i = 0; i < static_cast<int>(s.recent.size()); ++i)
    {
        const Rect r = item_rect(s, kRecent, i);
        if (r.x > 1920.0f || r.x + r.w < 0.0f)
            continue;
        const discord::Conversation &c = s.recent[static_cast<std::size_t>(i)];
        list.rounded_rect(r, 16.0f, c.mention ? kMentionTint : kRaised);
        const float icx = r.x + 54.0f;
        const float icy = r.cy();
        const discord::User *person = c.guild_id.empty() ? discord::find_user(s, c.user_id) : nullptr;
        if (person)
        {
            discord::User face = *person;
            face.voice_room.clear();
            avatar(list, ink_, face, icx, icy, 32.0f, false);
        }
        else
        {
            list.circle(icx, icy, 32.0f, c.mention ? Color::rgb(0x5a3a33) : kRaisedHigh);
            ui::text(list, ink_.fonts.semibold, c.mention ? "@" : "#", icx, icy + 13.0f, 38.0f,
                     c.mention ? kCoral : kText, gfx::Align::center);
        }
        const float tx = r.x + 102.0f;
        const std::string when = ago(s.now_ms, c.time_ms);
        const float when_w = ink_.fonts.regular.measure(when, 19.0f);
        ui::text(list, ink_.fonts.regular, when, r.x + r.w - 22.0f, r.y + 44.0f, 19.0f, kDim, gfx::Align::right);
        const std::string title = c.guild_id.empty() ? std::string("DM") : c.title;
        ui::text(list, ink_.fonts.semibold, fit(ink_.fonts.semibold, title, 25.0f, r.w - 102.0f - when_w - 34.0f), tx,
                 r.y + 46.0f, 25.0f, kText);
        float badge_w = 0.0f;
        if (c.unread > 0)
            badge_w = count_badge(list, ink_, c.unread, r.x + r.w - 22.0f, r.y + 84.0f, 36.0f) + 12.0f;
        ui::text(list, ink_.fonts.regular, fit(ink_.fonts.regular, c.preview, 23.0f, r.w - 102.0f - 22.0f - badge_w), tx,
                 r.y + 92.0f, 23.0f, Color::rgb(0xc9cdd6));
    }
}

void Home::draw(const discord::Snapshot &s, DrawList &list) const
{
    list.rounded_rect({0, 0, 1920, 1080}, 0.0f, kGround);
    draw_header(s, list);
    if (tab_ == HomeTab::all)
    {
        draw_friends(s, list);
        draw_rooms(s, list);
        draw_recent(s, list);
    }
    else
    {
        draw_tab(s, list);
    }

    // The focus outline glides over everything.
    const Rect f = focus_.rect();
    float radius = kCardRadius + 4.0f;
    if (tab_ == HomeTab::mentions && zone_ != kChips)
        radius = 20.0f;
    else if (zone_ == kFriends && tab_ == HomeTab::all)
        radius = f.w * 0.5f;
    else if (zone_ == kChips)
        radius = f.h * 0.5f;
    focus_outline(list, f, radius, 1.0f);

    const ui::Hint hints[] = {
        {ui::Button::cross, "Select"},
        {ui::Button::circle, "Back"},
        {ui::Button::square, "Voice"},
        {ui::Button::l1, "Tabs", ui::Button::r1},
    };
    ui::HintLayout layout;
    layout.size = 34.0f;
    layout.text_size = 23.0f;
    layout.cy = kHintsBaseline;
    layout.item_gap = 38.0f;
    ui::draw_hints(list, ink_.fonts, ui::GlyphStyle::dark(), hints, 4, kRight, true, layout);
}

} // namespace screens
