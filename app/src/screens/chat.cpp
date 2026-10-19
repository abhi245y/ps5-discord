// ps5-discord - A channel or DM: read history, act on a message, reply.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Framework frames 06 (channel chat), 06b (message actions), 07 (writing a
// reply) and 08 (direct message):
//   - a breadcrumb header (or the person's header in a DM) and the voice pill;
//   - channel chips with unread counts, switched with L1/R1;
//   - the history, newest at the bottom: runs by one author are grouped,
//     mentions of you are tinted with your @name as a coral pill, images are
//     cards, your own DM messages sit on the right;
//   - focus starts on the composer and climbs the history; Cross on a message
//     opens its actions, Triangle opens the composer with quick replies.

#include "screens/chat.hpp"

#include <algorithm>
#include <functional>
#include <cmath>

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
constexpr float kWidth = kRight - kMargin;
constexpr float kHeaderY = 34.0f;
constexpr float kHeaderH = 68.0f;
constexpr float kChipsY = 112.0f;
constexpr float kChipH = 54.0f;
constexpr float kComposerY = 916.0f;
constexpr float kComposerH = 76.0f;
constexpr float kListBottom = kComposerY - 14.0f;

constexpr float kAvatarCol = 112.0f; // avatar column width
constexpr float kTextSize = 27.0f;
constexpr float kLineH = 37.0f;
constexpr float kNameLine = 36.0f;
constexpr float kReplyLine = 30.0f;
constexpr float kAttachH = 128.0f;
constexpr float kReactH = 40.0f;
constexpr float kPadTop = 10.0f;
constexpr float kPadGrouped = 2.0f;
constexpr float kPadBottom = 10.0f;
constexpr float kDmBubbleW = 860.0f;
constexpr std::int64_t kGroupMs = 7 * 60 * 1000;

const char *const kQuickReplies[] = {"On my way", "gg", "brb", "5 min", "\xF0\x9F\x91\x8D"};
constexpr int kQuickCount = 5;

struct Action_
{
    ui::Button button;
    const char *label;
};
const Action_ kActions[] = {
    {ui::Button::cross, "Reply"},
    {ui::Button::square, "React"},
};
// ponytail: copy, mark unread and mention return with free typing (system keyboard).
constexpr int kActionCount = 2;
const char *const kReactions[] = {"\xE2\x9D\xA4\xEF\xB8\x8F", "\xF0\x9F\x91\x8D", "\xF0\x9F\x98\x82",
                                  "\xF0\x9F\x98\xAE", "\xF0\x9F\x94\xA5"};
constexpr int kReactionCount = 5;

std::string clock_time(std::int64_t ms, int utc_offset_minutes)
{
    const std::int64_t day = 24 * 60;
    const std::int64_t minutes = ((ms / 60000 + utc_offset_minutes) % day + day) % day;
    int h = static_cast<int>(minutes / 60);
    const int m = static_cast<int>(minutes % 60);
    const char *suffix = h >= 12 ? "PM" : "AM";
    h = h % 12 == 0 ? 12 : h % 12;
    char out[16];
    std::snprintf(out, sizeof out, "%d:%02d %s", h, m, suffix);
    return out;
}

const discord::User *author_of(const discord::Snapshot &s, const discord::Message &m)
{
    return discord::find_user(s, m.author_id);
}

std::string author_name(const discord::Snapshot &s, const discord::Message &m)
{
    if (const discord::User *u = author_of(s, m))
        return u->name;
    return m.author_name.empty() ? std::string("Unknown") : m.author_name;
}

} // namespace

const std::vector<discord::Message> &Chat::messages(const discord::Snapshot &s) const
{
    static const std::vector<discord::Message> kNone;
    const auto found = s.messages.find(channel_id_);
    return found == s.messages.end() || !found->second ? kNone : *found->second;
}

std::vector<const discord::Channel *> Chat::chips(const discord::Snapshot &s) const
{
    std::vector<const discord::Channel *> out;
    if (const discord::Guild *g = discord::find_guild(s, guild_id_))
        for (const discord::Channel &c : g->channels)
            if (c.kind != discord::ChannelKind::voice)
                out.push_back(&c);
    return out;
}

// The other person in this DM: from the DM list, else from their messages.
const discord::User *Chat::dm_person(const discord::Snapshot &s) const
{
    for (const auto &[user_id, channel] : s.dm_by_user)
        if (channel == channel_id_)
            if (const discord::User *u = discord::find_user(s, user_id))
                return u;
    for (const discord::Message &m : messages(s))
        if (m.author_id != s.me.id)
            if (const discord::User *u = author_of(s, m))
                return u;
    return nullptr;
}

// Shows the system keyboard for this channel; false where there is none (host).
bool Chat::start_typing(const discord::Snapshot &s, hui::ui::Feedback &feedback)
{
    std::string title = "Message";
    for (const discord::Channel *c : chips(s))
        if (c->id == channel_id_)
            title = "Message #" + c->name;
    if (const discord::User *other = dm() ? dm_person(s) : nullptr)
        title = "Message " + other->name;
    for (const discord::Message &m : messages(s))
        if (!reply_to_.empty() && m.id == reply_to_)
            title = "Replying to " + author_name(s, m);
    if (ink_.art.keyboard_open && ink_.art.keyboard_open(title))
        mode_ = Mode::typing; // the system keyboard, where the console lets us have it
    else
    {
        keys_.open(title);
        mode_ = Mode::keys;
    }
    feedback.play(hui::audio::Cue::open);
    return true;
}

bool Chat::can_send(const discord::Snapshot &s) const
{
    for (const discord::Channel *c : chips(s))
        if (c->id == channel_id_)
            return c->can_send;
    return true; // DMs
}

void Chat::open(const discord::Snapshot &s, const std::string &guild_id, const std::string &channel_id,
                std::vector<discord::Request> &requests)
{
    if (channel_id != channel_id_)
        keys_.clear(); // a draft belongs to its channel
    guild_id_ = guild_id;
    channel_id_ = channel_id;
    mode_ = Mode::browse;
    focus_ = -1;
    reply_to_.clear();
    scroll_.snap(0.0f);
    menu_show_.snap(0.0f);
    discord::Request r;
    r.kind = discord::Request::Kind::open_channel;
    r.guild_id = guild_id;
    r.channel_id = channel_id;
    requests.push_back(r);
    (void)s;
}

float Chat::row_height(const discord::Snapshot &s, int index, bool grouped) const
{
    const discord::Message &m = messages(s)[static_cast<std::size_t>(index)];
    const float width = dm() ? kDmBubbleW - 48.0f : kWidth - kAvatarCol - 40.0f;
    float h = grouped ? kPadGrouped : kPadTop;
    if (!grouped)
        h += kNameLine;
    if (!m.reply_author.empty())
        h += kReplyLine;
    if (!m.content.empty())
    {
        const std::size_t lines = std::max<std::size_t>(1, ink_.fonts.regular.font->wrap(m.content, kTextSize, width).size());
        h += kLineH * static_cast<float>(lines);
    }
    h += static_cast<float>(m.attachments.size()) * (kAttachH + 10.0f);
    if (!m.reactions.empty())
        h += kReactH + 10.0f;
    if (dm())
        h += 24.0f; // bubble padding
    return h + kPadBottom;
}

std::vector<Chat::Row> Chat::layout(const discord::Snapshot &s) const
{
    const std::vector<discord::Message> &ms = messages(s);
    std::vector<Row> rows(ms.size());
    float bottom = 0.0f;
    for (int i = static_cast<int>(ms.size()) - 1; i >= 0; --i)
    {
        const discord::Message &m = ms[static_cast<std::size_t>(i)];
        const bool grouped = i > 0 && ms[static_cast<std::size_t>(i - 1)].author_id == m.author_id &&
                             m.time_ms - ms[static_cast<std::size_t>(i - 1)].time_ms < kGroupMs &&
                             m.reply_author.empty() && !m.mentions_me;
        const float h = row_height(s, i, grouped);
        rows[static_cast<std::size_t>(i)] = {i, bottom, h, grouped};
        bottom += h;
    }
    return rows;
}

Rect Chat::row_rect(const Row &row) const
{
    const float y = kListBottom - row.bottom - row.height + scroll_.value;
    return {kMargin - 16.0f, y, kWidth + 32.0f, row.height};
}

bool Chat::update(const discord::Snapshot &s, const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback,
                  std::vector<discord::Request> &requests)
{
    const std::vector<discord::Message> &ms = messages(s);
    const int count = static_cast<int>(ms.size());
    focus_ = std::min(focus_, count - 1);

    if (mode_ == Mode::actions)
    {
        const int entries = kActionCount + kReactionCount;
        if (input.nav == Direction::down && action_ < kActionCount - 1)
            ++action_;
        else if (input.nav == Direction::down && action_ < kActionCount)
            action_ = kActionCount;
        else if (input.nav == Direction::up && action_ >= kActionCount)
            action_ = kActionCount - 1;
        else if (input.nav == Direction::up && action_ > 0)
            --action_;
        else if (input.nav == Direction::right && action_ >= kActionCount && action_ < entries - 1)
            ++action_;
        else if (input.nav == Direction::left && action_ > kActionCount)
            --action_;
        else if (input.nav != Direction::none)
            feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.5f);
        if (input.nav != Direction::none)
            feedback.play(hui::audio::Cue::focus, 1.0f + 0.02f * static_cast<float>(action_));
        const bool reply = input.is_pressed(Action::confirm) && action_ == 0;
        if (input.is_pressed(Action::back))
        {
            mode_ = Mode::browse;
            feedback.play(hui::audio::Cue::modal_close);
        }
        else if (reply && !can_send(s))
        {
            mode_ = Mode::browse;
            feedback.play(hui::audio::Cue::error);
        }
        else if (reply)
        {
            reply_to_ = ms[static_cast<std::size_t>(focus_)].id;
            mode_ = Mode::compose;
            quick_ = 0;
            feedback.play(hui::audio::Cue::open);
        }
        else if ((input.is_pressed(Action::confirm) && action_ == 1) || input.is_pressed(Action::west))
        {
            action_ = kActionCount; // React (or Square): to the emoji row
            feedback.play(hui::audio::Cue::focus);
        }
        else if (input.is_pressed(Action::confirm) && action_ >= kActionCount)
        {
            discord::Request r;
            r.kind = discord::Request::Kind::react;
            r.channel_id = channel_id_;
            r.message_id = ms[static_cast<std::size_t>(focus_)].id;
            r.emoji = kReactions[action_ - kActionCount];
            requests.push_back(r);
            mode_ = Mode::browse;
            feedback.play(hui::audio::Cue::select);
        }
        menu_show_.target = mode_ == Mode::actions ? 1.0f : 0.0f;
        menu_show_.update(dt, 18.0f);
        return true;
    }

    if (mode_ == Mode::keys)
    {
        switch (keys_.update(input, dt, feedback))
        {
        case Keyboard::Result::send: {
            std::string typed = keys_.text();
            while (!typed.empty() && (typed.back() == ' ' || typed.back() == '\n'))
                typed.pop_back();
            discord::Request r;
            r.kind = discord::Request::Kind::send;
            r.guild_id = guild_id_;
            r.channel_id = channel_id_;
            r.content = typed;
            r.reply_to = reply_to_;
            requests.push_back(r);
            keys_.clear();
            reply_to_.clear();
            mode_ = Mode::browse;
            focus_ = -1;
            scroll_.target = 0.0f;
            feedback.play(hui::audio::Cue::select);
            break;
        }
        case Keyboard::Result::cancel:
            mode_ = Mode::browse; // the draft stays for next time
            break;
        case Keyboard::Result::none:
            break;
        }
        scroll_.update(dt, 14.0f);
        return true;
    }

    if (mode_ == Mode::typing)
    {
        // The system keyboard is up; the app waits for what was typed.
        std::string typed;
        const int state = ink_.art.keyboard_poll ? ink_.art.keyboard_poll(typed) : 2;
        while (!typed.empty() && (typed.back() == ' ' || typed.back() == '\n'))
            typed.pop_back();
        if (state == 1 && !typed.empty())
        {
            discord::Request r;
            r.kind = discord::Request::Kind::send;
            r.guild_id = guild_id_;
            r.channel_id = channel_id_;
            r.content = typed;
            r.reply_to = reply_to_;
            requests.push_back(r);
            reply_to_.clear();
            mode_ = Mode::browse;
            focus_ = -1;
            scroll_.target = 0.0f;
            feedback.play(hui::audio::Cue::select);
        }
        else if (state != 0)
            mode_ = Mode::compose; // cancelled or empty: back to the quick replies
        scroll_.update(dt, 14.0f);
        return true;
    }

    if (mode_ == Mode::compose)
    {
        if (input.is_pressed(Action::north) && start_typing(s, feedback))
            return true;
        if (input.nav == Direction::left && quick_ > 0)
            --quick_;
        else if (input.nav == Direction::right && quick_ < kQuickCount - 1)
            ++quick_;
        else if (input.nav == Direction::left || input.nav == Direction::right)
            feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.5f);
        if (input.nav == Direction::left || input.nav == Direction::right)
            feedback.play(hui::audio::Cue::focus, 1.0f + 0.03f * static_cast<float>(quick_));
        if (input.is_pressed(Action::confirm) || input.is_pressed(Action::jump_next))
        {
            discord::Request r;
            r.kind = discord::Request::Kind::send;
            r.guild_id = guild_id_;
            r.channel_id = channel_id_;
            r.content = kQuickReplies[quick_];
            r.reply_to = reply_to_;
            requests.push_back(r);
            reply_to_.clear();
            mode_ = Mode::browse;
            focus_ = -1;
            scroll_.target = 0.0f;
            feedback.play(hui::audio::Cue::select);
        }
        else if (input.is_pressed(Action::back))
        {
            reply_to_.clear();
            mode_ = Mode::browse;
            feedback.play(hui::audio::Cue::back);
        }
        scroll_.update(dt, 14.0f);
        menu_show_.target = 0.0f;
        menu_show_.update(dt, 18.0f);
        return true;
    }

    // Browsing.
    if (input.is_pressed(Action::back))
    {
        feedback.play(hui::audio::Cue::back);
        return false;
    }
    if (!dm() && (input.is_pressed(Action::page_next) || input.is_pressed(Action::page_prev)))
    {
        const std::vector<const discord::Channel *> list = chips(s);
        int at = 0;
        for (int i = 0; i < static_cast<int>(list.size()); ++i)
            if (list[static_cast<std::size_t>(i)]->id == channel_id_)
                at = i;
        const int step = input.is_pressed(Action::page_next) ? 1 : -1;
        const int next = at + step;
        if (next >= 0 && next < static_cast<int>(list.size()))
        {
            open(s, guild_id_, list[static_cast<std::size_t>(next)]->id, requests);
            feedback.play(hui::audio::Cue::tab, 1.0f, step * 0.3f);
        }
        else
            feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.5f);
        return true;
    }
    if (input.nav == Direction::up)
    {
        if (count == 0 || focus_ == 0)
        {
            feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.5f);
            if (count > 0 && focus_ == 0)
            {
                discord::Request r;
                r.kind = discord::Request::Kind::load_older;
                r.guild_id = guild_id_;
                r.channel_id = channel_id_;
                requests.push_back(r);
            }
        }
        else
        {
            focus_ = focus_ < 0 ? count - 1 : focus_ - 1;
            feedback.play(hui::audio::Cue::focus, 1.02f);
        }
    }
    else if (input.nav == Direction::down)
    {
        if (focus_ < 0)
            feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.5f);
        else
        {
            focus_ = focus_ + 1 >= count ? -1 : focus_ + 1;
            feedback.play(hui::audio::Cue::focus, 0.98f);
        }
    }
    if (input.is_pressed(Action::confirm) && focus_ >= 0)
    {
        mode_ = Mode::actions;
        action_ = 0;
        feedback.play(hui::audio::Cue::modal_open);
    }
    if ((input.is_pressed(Action::north) || (input.is_pressed(Action::confirm) && focus_ < 0)) && !can_send(s))
        feedback.play(hui::audio::Cue::error);
    else if (input.is_pressed(Action::north) && start_typing(s, feedback))
    {
        // Triangle: straight to the keyboard (quick replies stay a Cross away).
    }
    else if (input.is_pressed(Action::north) || (input.is_pressed(Action::confirm) && focus_ < 0))
    {
        mode_ = Mode::compose;
        quick_ = 0;
        feedback.play(hui::audio::Cue::open);
    }

    // Keep the focused message on screen; back to the newest at the composer.
    const std::vector<Row> rows = layout(s);
    float target = scroll_.target;
    if (focus_ >= 0)
    {
        const Row &row = rows[static_cast<std::size_t>(focus_)];
        const float top_edge = (dm() ? 170.0f : kChipsY + kChipH + 16.0f) + 10.0f;
        const float y = kListBottom - row.bottom - row.height + target;
        if (y < top_edge)
            target += top_edge - y;
        if (y + row.height > kListBottom)
            target -= y + row.height - kListBottom;
    }
    else
    {
        target = 0.0f;
    }
    scroll_.target = std::max(0.0f, target);
    scroll_.update(dt, 14.0f);
    menu_show_.target = 0.0f;
    menu_show_.update(dt, 18.0f);
    return true;
}

// ---- drawing -----------------------------------------------------------------

void Chat::draw_header(const discord::Snapshot &s, DrawList &list) const
{
    if (dm())
    {
        const discord::User *other = dm_person(s);
        if (other)
        {
            avatar(list, ink_, *other, kMargin + 56.0f, kHeaderY + 40.0f, 54.0f);
            ui::text(list, ink_.fonts.display, other->name, kMargin + 136.0f, kHeaderY + 50.0f, 56.0f, kText);
            const std::string line = status_line(*other);
            list.circle(kMargin + 146.0f, kHeaderY + 86.0f, 7.0f, kOnline);
            ui::text(list, ink_.fonts.regular, line, kMargin + 164.0f, kHeaderY + 95.0f, 25.0f, kDim);
        }
        // Profile and Call, left of the voice pill's place.
        const float call_w = 170.0f;
        const Rect call{kRight - call_w, kHeaderY + 4.0f, call_w, 64.0f};
        list.rounded_rect(call, 32.0f, kCoral);
        speaking_bars(list, call.x + 44.0f, call.cy(), 26.0f, kOnCoral);
        ui::text(list, ink_.fonts.semibold, "Call", call.x + 70.0f, call.cy() + 9.0f, 26.0f, kOnCoral);
        const Rect profile{call.x - 200.0f, call.y, 180.0f, 64.0f};
        list.rounded_rect(profile, 32.0f, kRaised);
        ui::text(list, ink_.fonts.semibold, "Profile", profile.cx(), profile.cy() + 9.0f, 26.0f, kText,
                 gfx::Align::center);
        return;
    }

    const discord::Guild *g = discord::find_guild(s, guild_id_);
    std::string channel_name = channel_id_;
    for (const discord::Channel *c : chips(s))
        if (c->id == channel_id_)
            channel_name = c->name;
    float x = kMargin;
    x += ui::text(list, ink_.fonts.display, g ? g->name : std::string(), x, 78.0f, 52.0f, kText);
    x += ui::text(list, ink_.fonts.display, "  /  ", x, 78.0f, 52.0f, kFaint);
    ui::text(list, ink_.fonts.display, "#" + channel_name, x, 78.0f, 52.0f, Color::rgb(0xb9bfdc));
    voice_pill(list, ink_, s.voice, kHeaderY, kHeaderH);

    constexpr float kChipGap = 16.0f;
    // Channel chips: the run starts late enough that the open channel is in it.
    const std::vector<const discord::Channel *> all = chips(s);
    std::vector<float> widths;
    std::size_t current = 0;
    for (std::size_t i = 0; i < all.size(); ++i)
    {
        const discord::Channel *c = all[i];
        const float badge = c->unread > 0 ? ink_.fonts.semibold.measure(count_text(c->unread), 22.0f) + 46.0f : 0.0f;
        widths.push_back(ink_.fonts.semibold.measure(c->name, 26.0f) + 96.0f + badge + kChipGap);
        if (c->id == channel_id_)
            current = i;
    }
    std::size_t first = 0;
    const auto run = [&](std::size_t from) {
        float w = 0.0f;
        for (std::size_t i = from; i <= current && i < widths.size(); ++i)
            w += widths[i];
        return w;
    };
    while (first < current && run(first) + 92.0f > kRight - kMargin)
        ++first;
    float cx = kMargin;
    for (std::size_t i = first; i < all.size(); ++i)
    {
        const discord::Channel *c = all[i];
        const float w = widths[i] - kChipGap;
        if (cx + w + (i + 1 < all.size() ? 92.0f : 0.0f) > kRight)
        {
            list.rounded_rect({cx, kChipsY, 92.0f, kChipH}, 16.0f, kRaised);
            ui::text(list, ink_.fonts.semibold, "+" + std::to_string(all.size() - i), cx + 46.0f, kChipsY + 41.0f, 26.0f,
                     kText, gfx::Align::center);
            break;
        }
        const bool on = c->id == channel_id_;
        const Rect r{cx, kChipsY, w, kChipH};
        list.rounded_rect(r, 16.0f, on ? kRaisedHigh : kRaised);
        ui::text(list, ink_.fonts.semibold, "#", r.x + 26.0f, r.y + 42.0f, 30.0f, on ? kText : kFaint);
        ui::text(list, ink_.fonts.semibold, c->name, r.x + 58.0f, r.y + 41.0f, 26.0f, on ? kText : kDim);
        if (c->unread > 0)
            count_badge(list, ink_, c->unread, r.x + r.w - 22.0f, r.cy(), 36.0f);
        if (on)
            list.rounded_rect({r.x + 22.0f, r.y + r.h - 5.0f, r.w - 44.0f, 4.0f}, 2.0f, kCoral);
        cx += w + kChipGap;
    }
}

void Chat::draw_message(const discord::Snapshot &s, DrawList &list, const Row &row, const Rect &r, bool focused) const
{
    const discord::Message &m = messages(s)[static_cast<std::size_t>(row.index)];
    const bool mine = m.author_id == s.me.id;
    const std::string mention_token = "@" + s.me.name;
    if (m.pending) // sent from here, not yet echoed by the gateway
        list.push_opacity(0.6f);

    if (m.mentions_me)
    {
        list.rounded_rect(r, 16.0f, kMentionTint);
        list.rounded_rect({r.x, r.y + 8.0f, 4.0f, r.h - 16.0f}, 2.0f, kCoral);
    }

    // Geometry of the content column.
    float text_x = kMargin + kAvatarCol;
    float width = kWidth - kAvatarCol - 40.0f;
    float y = r.y + (row.grouped ? kPadGrouped : kPadTop);
    Rect bubble{};
    if (dm())
    {
        width = kDmBubbleW - 48.0f;
        const float bw = kDmBubbleW;
        bubble = mine ? Rect{kRight - kAvatarCol + 12.0f - bw, y + (row.grouped ? 0.0f : kNameLine), bw,
                             r.h - (row.grouped ? kPadGrouped : kPadTop) - kPadBottom - (row.grouped ? 0.0f : kNameLine)}
                      : Rect{kMargin + kAvatarCol - 20.0f, y + (row.grouped ? 0.0f : kNameLine), bw,
                             r.h - (row.grouped ? kPadGrouped : kPadTop) - kPadBottom - (row.grouped ? 0.0f : kNameLine)};
        // Size the bubble to its longest line.
        float longest = 0.0f;
        for (const std::string &line : ink_.fonts.regular.font->wrap(m.content, kTextSize, width))
            longest = std::max(longest, ink_.fonts.regular.measure(line, kTextSize));
        if (!m.attachments.empty())
            longest = std::max(longest, 560.0f);
        const float fitted = std::min(bw, longest + 48.0f);
        if (mine)
            bubble.x += bw - fitted;
        bubble.w = fitted;
        list.rounded_rect(bubble, 18.0f, mine ? Color::rgb(0x3a2a28) : kRaised);
        if (mine)
            list.rounded_rect({bubble.x, bubble.y + 10.0f, 4.0f, bubble.h - 20.0f}, 2.0f, kCoral);
        text_x = bubble.x + 24.0f;
    }

    if (!row.grouped)
    {
        const float acx = dm() && mine ? kRight - 52.0f : kMargin + 52.0f;
        if (const discord::User *u = author_of(s, m))
        {
            discord::User face = *u;
            face.voice_room.clear();
            avatar(list, ink_, face, acx, y + 34.0f, 36.0f);
        }
        else
        {
            // Server members outside the friends list: their initial on stable art.
            discord::User face;
            face.name = author_name(s, m);
            face.art = static_cast<discord::ArtIndex>(std::hash<std::string>{}(m.author_id) % 12);
            avatar(list, ink_, face, acx, y + 34.0f, 36.0f, false);
        }
        const std::string name = mine ? std::string("You") : author_name(s, m);
        const std::string when = clock_time(m.time_ms, s.utc_offset_minutes);
        if (dm() && mine)
        {
            const float tw = ink_.fonts.regular.measure(when, 22.0f);
            ui::text(list, ink_.fonts.regular, when, kRight - kAvatarCol + 12.0f, y + 27.0f, 20.0f, kDim, gfx::Align::right);
            ui::text(list, ink_.fonts.semibold, name, kRight - kAvatarCol + 12.0f - tw - 16.0f, y + 27.0f, 25.0f, kText,
                     gfx::Align::right);
        }
        else
        {
            const float nx = dm() ? bubble.x + 4.0f : text_x;
            const float nw = ui::text(list, ink_.fonts.semibold, name, nx, y + 27.0f, 25.0f, kText);
            ui::text(list, ink_.fonts.regular, when, nx + nw + 14.0f, y + 27.0f, 20.0f, kDim);
        }
        y += kNameLine;
    }
    if (dm())
        y += 12.0f;

    if (!m.reply_author.empty())
    {
        list.rounded_rect({text_x, y + 6.0f, 4.0f, 22.0f}, 2.0f, kFaint);
        ui::text(list, ink_.fonts.regular,
                 fit(ink_.fonts.regular, m.reply_author + ": " + m.reply_text, 22.0f, width - 20.0f), text_x + 16.0f,
                 y + 25.0f, 22.0f, kDim);
        y += kReplyLine;
    }

    if (!m.content.empty())
    {
        float line_y = y + 27.0f;
        float last_end = text_x, last_y = line_y;
        for (const std::string &line : ink_.fonts.regular.font->wrap(m.content, kTextSize, width))
        {
            float lx = text_x;
            std::string rest = line;
            if (rest.rfind(mention_token, 0) == 0)
            {
                const float mw = ink_.fonts.semibold.measure(mention_token, 25.0f);
                list.rounded_rect({lx - 4.0f, line_y - 27.0f, mw + 22.0f, 37.0f}, 9.0f, kCoral);
                ui::text(list, ink_.fonts.semibold, mention_token, lx + 7.0f, line_y, 25.0f, kText);
                lx += mw + 34.0f;
                rest = rest.substr(mention_token.size());
                while (!rest.empty() && rest.front() == ' ')
                    rest.erase(rest.begin());
            }
            const float w = ui::text(list, ink_.fonts.regular, rest, lx, line_y, kTextSize, Color::rgb(0xdfe2e8));
            last_end = lx + w;
            last_y = line_y;
            line_y += kLineH;
        }
        if (m.edited)
            ui::text(list, ink_.fonts.regular, "(edited)", last_end + 12.0f, last_y, 20.0f, kFaint);
        y += kLineH * static_cast<float>(std::max<std::size_t>(1, ink_.fonts.regular.font->wrap(m.content, kTextSize, width).size()));
    }

    for (const discord::Attachment &a : m.attachments)
    {
        const Rect card{text_x, y + 6.0f, 560.0f, kAttachH};
        list.rounded_rect(card, 16.0f, kRaisedHigh);
        if (a.image)
            scene(list, ink_, a.art, {card.x + 10.0f, card.y + 10.0f, 220.0f, card.h - 20.0f}, 10.0f, a.url);
        ui::text(list, ink_.fonts.semibold, fit(ink_.fonts.semibold, a.name, 26.0f, 290.0f), card.x + 250.0f, card.y + 56.0f,
                 26.0f, kText);
        ui::text(list, ink_.fonts.regular, a.size_text, card.x + 250.0f, card.y + 92.0f, 22.0f, kDim);
        y += kAttachH + 10.0f;
    }

    if (!m.reactions.empty())
    {
        float rx = text_x;
        for (const discord::Reaction &reaction : m.reactions)
        {
            const std::string count = std::to_string(reaction.count);
            const float ew = ink_.fonts.regular.measure(reaction.emoji, 24.0f);
            const float cw = ink_.fonts.semibold.measure(count, 22.0f);
            const Rect chip{rx, y + 8.0f, ew + cw + 46.0f, kReactH};
            list.rounded_rect(chip, kReactH * 0.5f, reaction.me ? Color::rgb(0x3a2a28) : kRaisedHigh);
            if (reaction.me)
                list.bordered_rect(chip, kReactH * 0.5f, Color::rgb(0, 0.0f), 2.0f, kCoral);
            ui::text(list, ink_.fonts.regular, reaction.emoji, chip.x + 16.0f, chip.cy() + 9.0f, 24.0f, kText);
            ui::text(list, ink_.fonts.semibold, count, chip.x + 26.0f + ew, chip.cy() + 8.0f, 22.0f,
                     reaction.me ? kCoral : kDim);
            rx += chip.w + 10.0f;
        }
        y += kReactH + 10.0f;
    }
    if (m.failed)
        ui::text(list, ink_.fonts.semibold, "Not sent", text_x, y + 26.0f, 22.0f, kCoral);

    if (m.pending)
        list.pop_opacity();
    if (focused)
        focus_outline(list, r, 16.0f, 1.0f);
}

void Chat::draw_composer(const discord::Snapshot &s, DrawList &list) const
{
    const bool composing = mode_ == Mode::compose;
    const Rect bar{kMargin, kComposerY, kWidth, kComposerH};
    if (!can_send(s))
    {
        list.rounded_rect(bar, 22.0f, Color::rgb(0x1d2026));
        ui::text(list, ink_.fonts.regular, "You can't send messages in this channel.", bar.cx(), bar.cy() + 10.0f, 27.0f,
                 kFaint, gfx::Align::center);
        return;
    }
    list.rounded_rect(bar, 22.0f, kRaised);
    list.circle(bar.x + 52.0f, bar.cy(), 30.0f, kRaisedHigh);
    list.rounded_rect({bar.x + 50.0f, bar.cy() - 14.0f, 4.0f, 28.0f}, 2.0f, kText);
    list.rounded_rect({bar.x + 38.0f, bar.cy() - 2.0f, 28.0f, 4.0f}, 2.0f, kText);

    std::string placeholder = "Message ";
    if (dm())
    {
        if (const discord::User *other = dm_person(s))
            placeholder += other->name;
        else
        for (const discord::Message &m : messages(s))
            if (m.author_id != s.me.id)
            {
                placeholder += author_name(s, m);
                break;
            }
    }
    else
    {
        placeholder += "#" + channel_id_;
        for (const discord::Channel *c : chips(s))
            if (c->id == channel_id_)
                placeholder = "Message #" + c->name;
    }
    ui::text(list, ink_.fonts.regular, composing ? std::string("Pick a quick reply, or press Triangle to type") : placeholder,
             bar.x + 112.0f, bar.cy() + 10.0f, 28.0f, kDim);

    ui::GlyphStyle glyphs = ui::GlyphStyle::dark();
    const ui::Hint inline_hints[] = {{ui::Button::triangle, "Type"}, {ui::Button::square, dm() ? "Call" : "Voice"}};
    ui::HintLayout layout;
    layout.size = 36.0f;
    layout.text_size = 25.0f;
    layout.cy = bar.cy();
    layout.item_gap = 34.0f;
    ui::draw_hints(list, ink_.fonts, glyphs, inline_hints, 2, bar.x + bar.w - 30.0f, true, layout);

    if (focus_ < 0 || composing)
        focus_outline(list, bar, 22.0f, 1.0f);

    if (!composing)
        return;

    // Reply chip and quick replies above the bar.
    float y = bar.y - 74.0f;
    if (!reply_to_.empty())
    {
        for (const discord::Message &m : messages(s))
            if (m.id == reply_to_)
            {
                const std::string text = "Replying to " + author_name(s, m) + ": " + m.content;
                const float w = std::min(980.0f, ink_.fonts.regular.measure(text, 24.0f) + 80.0f);
                const Rect chip{kMargin + 260.0f, y - 70.0f, w, 56.0f};
                list.rounded_rect(chip, 28.0f, kRaisedHigh);
                ui::text(list, ink_.fonts.regular, fit(ink_.fonts.regular, text, 24.0f, w - 70.0f), chip.x + 26.0f,
                         chip.cy() + 8.0f, 24.0f, kText);
                list.line(chip.x + w - 38.0f, chip.cy() - 9.0f, chip.x + w - 20.0f, chip.cy() + 9.0f, 3.0f, kDim);
                list.line(chip.x + w - 20.0f, chip.cy() - 9.0f, chip.x + w - 38.0f, chip.cy() + 9.0f, 3.0f, kDim);
            }
    }
    float x = kMargin + 260.0f;
    for (int i = 0; i < kQuickCount; ++i)
    {
        const float w = std::max(110.0f, ink_.fonts.semibold.measure(kQuickReplies[i], 26.0f) + 64.0f);
        const Rect chip{x, y, w, 58.0f};
        list.rounded_rect(chip, 29.0f, kRaisedHigh);
        ui::text(list, ink_.fonts.semibold, kQuickReplies[i], chip.cx(), chip.cy() + 9.0f, 26.0f, kText,
                 gfx::Align::center);
        if (i == quick_)
            focus_outline(list, chip, 29.0f, 1.0f);
        x += w + 16.0f;
    }
}

void Chat::draw_actions(const discord::Snapshot &s, DrawList &list, const Rect &anchor) const
{
    (void)s;
    const float show = menu_show_.value;
    if (show <= 0.01f)
        return;
    const float w = 500.0f;
    const float h = 72.0f * kActionCount + 132.0f;
    const float y = std::clamp(anchor.cy() - h * 0.5f, 140.0f, kComposerY - h - 10.0f);
    const Rect panel{kRight - w, y, w, h};
    list.push_opacity(show);
    list.shadow(panel, 24.0f, 40.0f, Color::rgb(0, 0.5f));
    list.rounded_rect(panel, 24.0f, kRaised);
    for (int i = 0; i < kActionCount; ++i)
    {
        const Rect row{panel.x + 16.0f, panel.y + 16.0f + 72.0f * i, w - 32.0f, 64.0f};
        if (kActions[i].button != ui::Button::none)
            ui::draw_button(list, ink_.fonts, ui::GlyphStyle::dark(), kActions[i].button, row.x + 20.0f, row.cy(), 36.0f);
        ui::text(list, ink_.fonts.semibold, kActions[i].label, row.x + 82.0f, row.cy() + 10.0f, 28.0f, kText);
        if (action_ == i)
            focus_outline(list, row, 16.0f, 1.0f);
    }
    const float ry = panel.y + 16.0f + 72.0f * kActionCount + 18.0f;
    list.rounded_rect({panel.x + 24.0f, ry - 10.0f, w - 48.0f, 2.0f}, 1.0f, style::kLine);
    for (int i = 0; i < kReactionCount; ++i)
    {
        const Rect cell{panel.x + 20.0f + i * 92.0f, ry, 80.0f, 80.0f};
        list.rounded_rect(cell, 18.0f, kRaisedHigh);
        ui::text(list, ink_.fonts.regular, kReactions[i], cell.cx(), cell.cy() + 14.0f, 40.0f, kText, gfx::Align::center);
        if (action_ == kActionCount + i)
            focus_outline(list, cell, 18.0f, 1.0f);
    }
    list.pop_opacity();
}

void Chat::draw(const discord::Snapshot &s, DrawList &list) const
{
    list.rounded_rect({0, 0, 1920, 1080}, 0.0f, kGround);
    const float top = dm() ? 150.0f : kChipsY + kChipH + 10.0f;
    list.push_clip({0, top, 1920, kListBottom - top + 4.0f});
    const std::vector<Row> rows = layout(s);
    Rect focused_rect{};
    for (const Row &row : rows)
    {
        const Rect r = row_rect(row);
        if (r.y > kListBottom || r.y + r.h < top)
            continue;
        const bool focused = row.index == focus_ && mode_ != Mode::compose;
        if (focused)
            focused_rect = r;
        draw_message(s, list, row, r, focused);
    }
    if (rows.empty())
        ui::text(list, ink_.fonts.regular,
                 s.no_access.count(channel_id_)    ? "You can't read this channel."
                 : s.load_failed.count(channel_id_) ? "Couldn't load messages. Switch channels to try again."
                 : s.messages.count(channel_id_) == 0 ? "Loading messages…"
                                                      : "No messages yet. Say hi.",
                 960.0f,
                 520.0f, 30.0f, kDim, gfx::Align::center);
    list.pop_clip();
    // Header after the list so scrolled history slides under it, fading out.
    list.rounded_rect({0, 0, 1920, top}, 0.0f, kGround);
    list.gradient_rect({0, top, 1920, 56.0f}, 0.0f, kGround, kGround.with_alpha(0.0f));
    if (mode_ == Mode::compose) // writing: the history steps back
        list.gradient_rect({0, top, 1920, kComposerY - top}, 0.0f, kGround.with_alpha(0.55f), kGround.with_alpha(0.9f));
    draw_header(s, list);
    draw_composer(s, list);
    if (focus_ >= 0)
        draw_actions(s, list, focused_rect);

    ui::Hint hints[4] = {{ui::Button::cross, "Select"}, {ui::Button::circle, "Back"}, {ui::Button::triangle, "Type"}, {}};
    int count = 3;
    if (!dm())
        hints[count++] = {ui::Button::l1, "Channels", ui::Button::r1};
    ui::HintLayout layout;
    layout.size = 34.0f;
    layout.text_size = 23.0f;
    layout.cy = kHintsBaseline;
    layout.item_gap = 38.0f;
    ui::draw_hints(list, ink_.fonts, ui::GlyphStyle::dark(), hints, count, kRight, true, layout);
    if (mode_ == Mode::keys)
        keys_.draw(list);
}

} // namespace screens
