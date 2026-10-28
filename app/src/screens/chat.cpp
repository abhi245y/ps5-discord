// ps5-discord - A channel or DM: read history, act on a message, reply.
// SPDX-License-Identifier: AGPL-3.0-or-later
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
#include <cctype>
#include <cmath>
#include <cstdlib>

#include "core/save_file.hpp"
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
// Message text size, switched with R3 (Small, Medium, Large) and kept across launches.
// ponytail: file-scope, as there is one chat screen; a Settings field when settings get a page.
float kTextSize = 27.0f;
float kLineH = 37.0f;
float kNameLine = 36.0f;
float kReplyLine = 30.0f;
int g_text_level = 1;
constexpr const char *kTextSizePath = "/download0/hui/text_size";
const char *const kTextLevelNames[] = {"Small", "Medium", "Large"};

void apply_text_level(int level)
{
    static const float sizes[][4] = {{24.0f, 33.0f, 33.0f, 28.0f}, {27.0f, 37.0f, 36.0f, 30.0f}, {31.0f, 43.0f, 42.0f, 34.0f}};
    g_text_level = std::clamp(level, 0, 2);
    kTextSize = sizes[g_text_level][0];
    kLineH = sizes[g_text_level][1];
    kNameLine = sizes[g_text_level][2];
    kReplyLine = sizes[g_text_level][3];
}
constexpr float kAttachH = 128.0f;
constexpr float kReactH = 40.0f;
constexpr float kPadTop = 10.0f;
constexpr float kPadGrouped = 2.0f;
constexpr float kPadBottom = 10.0f;
constexpr float kDmBubbleW = 860.0f;
constexpr std::int64_t kGroupMs = 7 * 60 * 1000;

// A reaction chip's emoji: a server's own is its picture from the CDN.
constexpr float kReactEmoji = 28.0f;
// A Unicode one is Twemoji's colour picture (the font's emoji are one colour,
// and draw flags and keycaps as boxed letters).
const char *const kQuickReplies[] = {"On my way", "gg", "brb", "5 min", "\xF0\x9F\x91\x8D"};
constexpr int kQuickCount = 5;

enum class Act
{
    view, // the message's pictures, full screen
    reply,
    react,
    edit,
    remove,
};

struct Action_
{
    Act act;
    ui::Button button;
    const char *label;
};

bool has_image(const discord::Message &m)
{
    for (const discord::Attachment &a : m.attachments)
        if (a.image)
            return true;
    return false;
}

// The focused message's menu: View image when it has pictures, Reply and React for
// everyone's messages, Edit and Delete for your own too.
std::vector<Action_> actions_for(const discord::Message &m, bool mine)
{
    std::vector<Action_> a;
    if (has_image(m))
        a.push_back({Act::view, ui::Button::none, "View image"});
    a.push_back({Act::reply, ui::Button::cross, "Reply"});
    a.push_back({Act::react, ui::Button::square, "React"});
    if (mine)
    {
        a.push_back({Act::edit, ui::Button::triangle, "Edit"});
        a.push_back({Act::remove, ui::Button::none, "Delete"});
    }
    return a;
}
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

// Tops of the channel list's rows, from 0: a category heading adds room above its first row.
std::vector<float> list_tops(const std::vector<const discord::Channel *> &all)
{
    std::vector<float> tops;
    std::string category = "\x01";
    float y = 0.0f;
    for (const discord::Channel *c : all)
    {
        if (c->category != category)
        {
            category = c->category;
            if (!category.empty())
                y += 34.0f;
        }
        tops.push_back(y);
        y += 54.0f;
    }
    return tops;
}

} // namespace

Chat::Chat(const Ink &ink) : ink_(ink), keys_(ink)
{
    std::string saved;
    if (hui::save::read_file(kTextSizePath, &saved, 8))
        apply_text_level(std::atoi(saved.c_str()));
}

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
        const std::size_t lines = std::max<std::size_t>(1, text_lines(s, m, width).size());
        h += kLineH * static_cast<float>(lines + (edit_tag_wraps(s, m, width) ? 1 : 0));
    }
    h += static_cast<float>(m.attachments.size()) * (kAttachH + 10.0f);
    if (!m.reactions.empty())
        h += (kReactH + 10.0f) * static_cast<float>(reaction_lines(m, width));
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
        // Far above the view, a one-line estimate: a fresh page of history is
        // then wrapped as it scrolls in, not all in the frame it lands.
        const bool far = bottom > std::max(scroll_.value, scroll_.target) + 2600.0f && i != focus_;
        const float h = far ? (grouped ? kPadGrouped : kPadTop + kNameLine) + kLineH + kPadBottom
                            : row_height(s, i, grouped);
        rows[static_cast<std::size_t>(i)] = {i, bottom, h, grouped};
        bottom += h;
    }
    return rows;
}

float Chat::draw_line(DrawList &list, const discord::Message &m, const std::string &line, float x, float baseline,
                      std::size_t &emoji) const
{
    // Text runs between server emoji; each emoji is its picture in the slot the
    // mark took when the line was wrapped.
    const Color ink = Color::rgb(0xdfe2e8);
    const float slot = ink_.fonts.regular.measure(discord::kEmojiMark, kTextSize);
    const float size = slot * 0.96f; // in its slot, so emoji side by side do not touch
    float cx = x;
    std::size_t from = 0;
    while (true)
    {
        const std::size_t at = line.find(discord::kEmojiMark, from);
        const std::string run = line.substr(from, at == std::string::npos ? std::string::npos : at - from);
        if (!run.empty())
            cx += ui::text(list, ink_.fonts.regular, run, cx, baseline, kTextSize, ink);
        if (at == std::string::npos)
            break;
        const Rect box{cx + (slot - size) * 0.5f, baseline - kTextSize * 0.82f - (size - kTextSize) * 0.5f, size, size};
        if (emoji < m.emojis.size())
            emoji_picture(list, ink_, m.emojis[emoji].name, m.emojis[emoji].id, box);
        ++emoji;
        cx += slot;
        from = at + 3;
    }
    return cx - x;
}

std::pair<float, float> Chat::focus_range(const Row &row) const
{
    // Scroll values that keep a row in view. A row taller than the view
    // swaps the ends: it then fills the view, and Up/Down read through it.
    const float top_edge = (dm() ? 170.0f : kChipsY + kChipH + 16.0f) + 10.0f;
    const float base = kListBottom - row.bottom - row.height;
    const float a = top_edge - base;
    const float b = kListBottom - row.height - base;
    return {std::max(0.0f, std::min(a, b)), std::max(0.0f, std::max(a, b))};
}

int Chat::reaction_lines(const discord::Message &m, float width) const
{
    int lines = 1;
    float x = 0.0f;
    for (const discord::Reaction &r : m.reactions)
    {
        const float w = reaction_width(r);
        if (x > 0.0f && x + w > width)
        {
            ++lines;
            x = 0.0f;
        }
        x += w + 10.0f;
    }
    return lines;
}

float Chat::reaction_width(const discord::Reaction &r) const
{
    return kReactEmoji + ink_.fonts.semibold.measure(std::to_string(r.count), 22.0f) + 46.0f; // the picture, its count
}

float Chat::pill_extra(const discord::Snapshot &s) const
{
    const std::string token = "@" + s.me.name;
    return std::max(0.0f, ink_.fonts.semibold.measure(token, 25.0f) + 34.0f - ink_.fonts.regular.measure(token + " ", kTextSize));
}

std::vector<std::string> Chat::text_lines(const discord::Snapshot &s, const discord::Message &m, float width) const
{
    const bool pill = m.content.rfind("@" + s.me.name, 0) == 0;
    return ink_.fonts.regular.font->wrap(m.content, kTextSize, width - (pill ? pill_extra(s) : 0.0f));
}

// "(edited)" goes after the last line, or under it when the line is full.
bool Chat::edit_tag_wraps(const discord::Snapshot &s, const discord::Message &m, float width) const
{
    if (!m.edited || m.content.empty())
        return false;
    const std::vector<std::string> lines = text_lines(s, m, width);
    float last = lines.empty() ? 0.0f : ink_.fonts.regular.measure(lines.back(), kTextSize);
    if (lines.size() == 1 && m.content.rfind("@" + s.me.name, 0) == 0)
        last += pill_extra(s);
    return last + 12.0f + ink_.fonts.regular.measure("(edited)", 20.0f) > width;
}

float Chat::bubble_fit(const discord::Snapshot &s, const discord::Message &m, float width) const
{
    float w = 0.0f;
    const std::vector<std::string> lines = text_lines(s, m, width);
    for (std::size_t i = 0; i < lines.size(); ++i)
        w = std::max(w, ink_.fonts.regular.measure(lines[i], kTextSize) +
                            (i == 0 && m.content.rfind("@" + s.me.name, 0) == 0 ? pill_extra(s) : 0.0f));
    if (m.edited && !lines.empty() && !edit_tag_wraps(s, m, width))
        w = std::max(w, ink_.fonts.regular.measure(lines.back(), kTextSize) + 12.0f +
                            ink_.fonts.regular.measure("(edited)", 20.0f));
    if (!m.reply_author.empty())
        w = std::max(w, 16.0f + ink_.fonts.regular.measure(m.reply_author + ": " + m.reply_text, 22.0f));
    if (!m.attachments.empty())
        w = std::max(w, 560.0f);
    float x = 0.0f; // the reactions wrap as drawn
    for (const discord::Reaction &r : m.reactions)
    {
        const float rw = reaction_width(r);
        if (x > 0.0f && x + rw > width)
            x = 0.0f;
        x += rw;
        w = std::max(w, x);
        x += 10.0f;
    }
    return std::min(w, width);
}

Rect Chat::row_rect(const Row &row) const
{
    const float y = kListBottom - row.bottom - row.height + scroll_.value;
    return {kMargin - 16.0f, y, kWidth + 32.0f, row.height};
}

bool Chat::update(const discord::Snapshot &s, const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback,
                  std::vector<discord::Request> &requests)
{
    size_note_ = std::max(0.0f, size_note_ - dt);
    const std::vector<discord::Message> &ms = messages(s);
    const int count = static_cast<int>(ms.size());
    // An older page landed above: keep the focus on the same message.
    if (count > 0 && !front_id_.empty() && ms.front().id != front_id_ && focus_ >= 0)
        for (int i = 0; i < count; ++i)
            if (ms[static_cast<std::size_t>(i)].id == front_id_)
            {
                focus_ += i;
                break;
            }
    front_id_ = count > 0 ? ms.front().id : std::string();
    focus_ = std::min(focus_, count - 1);
    // Near the top of what is loaded: fetch the page above before it is needed.
    if (focus_ >= 0 && focus_ < 8 && count > 0 && !s.history_start.count(channel_id_) &&
        older_asked_for_ != ms.front().id)
    {
        older_asked_for_ = ms.front().id;
        discord::Request r;
        r.kind = discord::Request::Kind::load_older;
        r.guild_id = guild_id_;
        r.channel_id = channel_id_;
        requests.push_back(r);
    }

    if (mode_ == Mode::image)
    {
        // Left and right go through the message's pictures; Circle (or Cross) closes.
        int n = 0;
        if (focus_ >= 0)
            for (const discord::Attachment &a : ms[static_cast<std::size_t>(focus_)].attachments)
                n += a.image ? 1 : 0;
        if (n == 0 || input.is_pressed(Action::back) || input.is_pressed(Action::confirm))
        {
            mode_ = Mode::browse;
            feedback.play(hui::audio::Cue::back);
        }
        else if (input.nav == Direction::left || input.nav == Direction::right)
        {
            const int next = image_at_ + (input.nav == Direction::right ? 1 : -1);
            if (next >= 0 && next < n)
            {
                image_at_ = next;
                feedback.play(hui::audio::Cue::focus);
            }
            else
                feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.5f);
        }
        return true;
    }
    if (mode_ == Mode::actions)
    {
        const discord::Message &fm = ms[static_cast<std::size_t>(std::max(0, focus_))];
        const bool mine = focus_ >= 0 && fm.author_id == s.me.id;
        const std::vector<Action_> acts = actions_for(fm, mine);
        const int kActionCount = static_cast<int>(acts.size());
        const int entries = kActionCount + kReactionCount;
        const Act at = action_ < kActionCount ? acts[static_cast<std::size_t>(action_)].act : Act::react;
        auto chosen = [&](Act a) { return input.is_pressed(Action::confirm) && action_ < kActionCount && at == a; };
        if (input.nav != Direction::none)
            delete_armed_ = false;
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
        const bool reply = chosen(Act::reply);
        if (input.is_pressed(Action::back))
        {
            mode_ = Mode::browse;
            feedback.play(hui::audio::Cue::modal_close);
        }
        else if (chosen(Act::view))
        {
            mode_ = Mode::image;
            image_at_ = 0;
            feedback.play(hui::audio::Cue::open);
        }
        else if (reply && !can_send(s))
        {
            mode_ = Mode::browse;
            feedback.play(hui::audio::Cue::error);
        }
        else if (reply)
        {
            reply_to_ = fm.id;
            mode_ = Mode::compose;
            quick_ = 0;
            feedback.play(hui::audio::Cue::open);
        }
        else if (mine && (chosen(Act::edit) || input.is_pressed(Action::north)))
        {
            // Edit: the keyboard opens on the message's text; the draft waits.
            editing_id_ = fm.id;
            draft_before_edit_ = keys_.text();
            keys_.open("Editing your message", discord::plain_text(fm, true));
            mode_ = Mode::keys;
            feedback.play(hui::audio::Cue::open);
        }
        else if (mine && chosen(Act::remove))
        {
            if (!delete_armed_)
            {
                delete_armed_ = true; // asks once more: deleting cannot be undone
                feedback.play(hui::audio::Cue::error, 0.8f);
            }
            else
            {
                discord::Request r;
                r.kind = discord::Request::Kind::remove;
                r.channel_id = channel_id_;
                r.message_id = fm.id;
                requests.push_back(r);
                delete_armed_ = false;
                mode_ = Mode::browse;
                feedback.play(hui::audio::Cue::erase);
            }
        }
        else if (chosen(Act::react) || input.is_pressed(Action::west))
        {
            action_ = kActionCount; // React (or Square): to the emoji row
            feedback.play(hui::audio::Cue::focus);
        }
        else if (input.is_pressed(Action::confirm) && action_ >= kActionCount)
        {
            discord::Request r;
            r.kind = discord::Request::Kind::react;
            r.channel_id = channel_id_;
            r.message_id = fm.id;
            r.emoji = kReactions[action_ - kActionCount];
            requests.push_back(r);
            mode_ = Mode::browse;
            feedback.play(hui::audio::Cue::select);
        }
        menu_show_.target = mode_ == Mode::actions ? 1.0f : 0.0f;
        menu_show_.update(dt, 18.0f);
        return true;
    }

    if (mode_ == Mode::profile)
    {
        if (input.is_pressed(Action::confirm)) // Cross: call them
            voice_ = true;
        if (input.is_pressed(Action::back) || input.is_pressed(Action::menu) || input.is_pressed(Action::confirm))
        {
            mode_ = Mode::browse;
            feedback.play(hui::audio::Cue::modal_close);
        }
        return true;
    }

    if (mode_ == Mode::channels)
    {
        const std::vector<const discord::Channel *> all = chips(s);
        const int n = static_cast<int>(all.size());
        if (input.nav == Direction::down || input.nav == Direction::up)
        {
            const int next = list_index_ + (input.nav == Direction::down ? 1 : -1);
            if (next >= 0 && next < n)
            {
                list_index_ = next;
                feedback.play(hui::audio::Cue::focus, 1.0f + 0.01f * static_cast<float>(next % 8));
            }
            else
                feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.5f);
        }
        if (input.is_pressed(Action::confirm) && list_index_ < n)
        {
            open(s, guild_id_, all[static_cast<std::size_t>(list_index_)]->id, requests);
            feedback.play(hui::audio::Cue::open);
        }
        else if (input.is_pressed(Action::back) || input.is_pressed(Action::menu))
        {
            mode_ = Mode::browse;
            feedback.play(hui::audio::Cue::modal_close);
        }
        // Keep the focused row inside the panel.
        const std::vector<float> tops = list_tops(all);
        // Room above the focused row for its category heading.
        const float row_y = 150.0f + (list_index_ < n ? tops[static_cast<std::size_t>(list_index_)] : 0.0f);
        if (row_y - list_scroll_.target < 184.0f)
            list_scroll_.target = std::max(0.0f, row_y - 184.0f);
        if (row_y - list_scroll_.target > 900.0f)
            list_scroll_.target = row_y - 900.0f;
        list_scroll_.update(dt, 16.0f);
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
            if (!editing_id_.empty())
            {
                discord::Request r;
                r.kind = discord::Request::Kind::edit;
                r.channel_id = channel_id_;
                r.message_id = editing_id_;
                r.content = typed;
                requests.push_back(r);
                editing_id_.clear();
                keys_.set_text(draft_before_edit_);
                mode_ = Mode::browse;
                feedback.play(hui::audio::Cue::select);
                break;
            }
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
            if (!editing_id_.empty()) // an abandoned edit: the draft comes back
            {
                editing_id_.clear();
                keys_.set_text(draft_before_edit_);
            }
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

    // Typing on a hardware keyboard while reading: the keyboard opens with it.
    if ((mode_ == Mode::browse || mode_ == Mode::compose) && !input.typed.empty())
    {
        std::string text;
        for (const char c : input.typed)
            if (c != '\b' && c != '\n' && c != '\r' && c != '\x1b')
                text += c;
        if (!text.empty() && !can_send(s))
            feedback.play(hui::audio::Cue::error);
        else if (!text.empty())
        {
            start_typing(s, feedback);
            if (mode_ == Mode::keys)
                keys_.set_text(keys_.text() + text);
            return true;
        }
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
    if (input.is_pressed(Action::west)) // Square: call the DM, or open the voice room you are in
        voice_ = true;
    if (dm() && input.is_pressed(Action::menu) && dm_person(s))
    {
        mode_ = Mode::profile;
        feedback.play(hui::audio::Cue::modal_open);
        return true;
    }
    if (input.is_pressed(Action::r3))
    {
        apply_text_level((g_text_level + 1) % 3);
        hui::save::write_atomic(kTextSizePath, std::to_string(g_text_level));
        size_note_ = 1.6f;
        feedback.play(hui::audio::Cue::toggle);
        return true;
    }
    if (!dm() && input.is_pressed(Action::menu))
    {
        const std::vector<const discord::Channel *> all = chips(s);
        list_index_ = 0;
        for (int i = 0; i < static_cast<int>(all.size()); ++i)
            if (all[static_cast<std::size_t>(i)]->id == channel_id_)
                list_index_ = i;
        const std::vector<float> tops = list_tops(all);
        list_scroll_.snap(tops.empty() ? 0.0f : std::max(0.0f, tops[static_cast<std::size_t>(list_index_)] - 380.0f));
        mode_ = Mode::channels;
        feedback.play(hui::audio::Cue::modal_open);
        return true;
    }
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
    // A message taller than the view: Up/Down scroll through it first.
    const float view_step = (kListBottom - kChipsY - kChipH) * 0.6f;
    std::pair<float, float> range;
    if (focus_ >= 0 && focus_ < count && (input.nav == Direction::up || input.nav == Direction::down))
    {
        const Row row = layout(s)[static_cast<std::size_t>(focus_)];
        if (row.height > kListBottom - kChipsY - kChipH - 26.0f) // a short row: Up/Down move the focus
            range = focus_range(row);
        else
            range = {scroll_.target, scroll_.target};
    }
    else
        range = {scroll_.target, scroll_.target};
    if (input.nav == Direction::up && scroll_.target < range.second - 1.0f)
    {
        scroll_.target = std::min(range.second, scroll_.target + view_step);
        feedback.play(hui::audio::Cue::focus, 1.02f, 0.0f, 0.4f);
    }
    else if (input.nav == Direction::down && scroll_.target > range.first + 1.0f)
    {
        scroll_.target = std::max(range.first, scroll_.target - view_step);
        feedback.play(hui::audio::Cue::focus, 0.98f, 0.0f, 0.4f);
    }
    else if (input.nav == Direction::up)
    {
        if (count == 0 || focus_ == 0)
        {
            feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.5f);
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
        const auto [lo, hi] = focus_range(row);
        target = std::clamp(target, lo, hi);
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
            const Color dot = other->presence == discord::Presence::online ? kOnline
                              : other->presence == discord::Presence::idle ? kIdle
                              : other->presence == discord::Presence::dnd  ? kCoral
                                                                            : kFaint;
            list.circle(kMargin + 146.0f, kHeaderY + 86.0f, 7.0f, dot);
            ui::text(list, ink_.fonts.regular, line, kMargin + 164.0f, kHeaderY + 95.0f, 25.0f, kDim);
        }
        // Call (Square) and Profile (Options). The call's state sits to its left.
        const Rect profile{kRight - 220.0f, kHeaderY + 4.0f, 220.0f, 64.0f};
        const Rect call{profile.x - 186.0f, kHeaderY + 4.0f, 170.0f, 64.0f};
        const bool calling = s.voice.active() && s.voice.channel_id == channel_id_;
        list.rounded_rect(call, 32.0f, kRaised);
        ui::draw_button(list, ink_.fonts, hint_glyphs(), ui::Button::square, call.x + 40.0f, call.cy(), 34.0f);
        ui::text(list, ink_.fonts.semibold, calling ? "In call" : "Call", call.x + 84.0f, call.cy() + 9.0f, 26.0f,
                 calling ? kOnline : kText);
        if (s.voice.active())
            voice_dock(list, ink_, s, kHeaderY, kHeaderH, call.x - 16.0f);
        list.rounded_rect(profile, 32.0f, kRaised);
        ui::draw_button(list, ink_.fonts, hint_glyphs(), ui::Button::options, profile.x + 40.0f, profile.cy(),
                        34.0f);
        ui::text(list, ink_.fonts.semibold, "Profile", profile.x + 84.0f, profile.cy() + 9.0f, 26.0f, kText);
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
    voice_dock(list, ink_, s, kHeaderY, kHeaderH, kRight);

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
        // Size the bubble to all it holds: lines, reply quote, reactions, a card.
        const float fitted = std::min(bw, bubble_fit(s, m, width) + 48.0f);
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
            if (face.avatar_url.empty())
                face.avatar_url = m.author_avatar_url;
            avatar(list, ink_, face, acx, y + 34.0f, 36.0f);
        }
        else
        {
            // Server members outside the friends list: their initial on stable art.
            discord::User face;
            face.name = author_name(s, m);
            face.avatar_url = m.author_avatar_url;
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
        std::size_t emoji = 0; // the next of m.emojis
        const std::vector<std::string> lines = text_lines(s, m, width);
        for (const std::string &line : lines)
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
            const float w = draw_line(list, m, rest, lx, line_y, emoji);
            last_end = lx + w;
            last_y = line_y;
            line_y += kLineH;
        }
        const bool tag_below = edit_tag_wraps(s, m, width);
        if (m.edited)
            ui::text(list, ink_.fonts.regular, "(edited)", tag_below ? text_x : last_end + 12.0f,
                     tag_below ? last_y + kLineH : last_y, 20.0f, kFaint);
        y += kLineH * static_cast<float>(std::max<std::size_t>(1, lines.size()) + (tag_below ? 1 : 0));
    }

    for (const discord::Attachment &a : m.attachments)
    {
        const Rect card{text_x, y + 6.0f, 560.0f, kAttachH};
        list.rounded_rect(card, 16.0f, kRaisedHigh);
        if (a.image)
            scene(list, ink_, a.art, {card.x + 10.0f, card.y + 10.0f, 220.0f, card.h - 20.0f}, 10.0f, a.url);
        else
        {
            // A file: its extension on a plain tile.
            const Rect tile{card.x + 10.0f, card.y + 10.0f, 220.0f, card.h - 20.0f};
            list.rounded_rect(tile, 10.0f, kRaised);
            const std::size_t dot = a.name.rfind('.');
            std::string ext = dot == std::string::npos ? std::string("FILE") : a.name.substr(dot + 1);
            for (char &c : ext)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            ui::text(list, ink_.fonts.semibold, fit(ink_.fonts.semibold, ext, 30.0f, tile.w - 30.0f), tile.cx(),
                     tile.cy() + 11.0f, 30.0f, kDim, gfx::Align::center);
        }
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
            const float cw = reaction_width(reaction);
            if (rx > text_x && rx + cw > text_x + width) // wrap, as Discord does
            {
                rx = text_x;
                y += kReactH + 10.0f;
            }
            const Rect chip{rx, y + 8.0f, cw, kReactH};
            list.rounded_rect(chip, kReactH * 0.5f, reaction.me ? Color::rgb(0x3a2a28) : kRaisedHigh);
            if (reaction.me)
                list.bordered_rect(chip, kReactH * 0.5f, Color::rgb(0, 0.0f), 2.0f, kCoral);
            const float ew = cw - ink_.fonts.semibold.measure(count, 22.0f) - 46.0f;
            emoji_picture(list, ink_, reaction.emoji, reaction.emoji_id,
                          {chip.x + 16.0f + (ew - kReactEmoji) * 0.5f, chip.cy() - kReactEmoji * 0.5f, kReactEmoji,
                           kReactEmoji});
            ui::text(list, ink_.fonts.semibold, count, chip.x + 26.0f + ew, chip.cy() + 8.0f, 22.0f,
                     reaction.me ? kCoral : kDim);
            rx += cw + 10.0f;
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

    ui::GlyphStyle glyphs = hint_glyphs();
    const ui::Hint inline_hints[] = {{ui::Button::triangle, "Type"}};
    ui::HintLayout layout;
    layout.size = 36.0f;
    layout.text_size = 25.0f;
    layout.cy = bar.cy();
    layout.item_gap = 34.0f;
    ui::draw_hints(list, ink_.fonts, glyphs, inline_hints, 1, bar.x + bar.w - 30.0f, true, layout);

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
                const std::string text = "Replying to " + author_name(s, m) + ": " + discord::plain_text(m);
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
    const std::vector<discord::Message> &ms = messages(s);
    if (focus_ < 0 || focus_ >= static_cast<int>(ms.size()))
        return;
    const discord::Message &fm = ms[static_cast<std::size_t>(focus_)];
    const std::vector<Action_> acts = actions_for(fm, fm.author_id == s.me.id);
    const int kActionCount = static_cast<int>(acts.size());
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
        const Action_ &a = acts[static_cast<std::size_t>(i)];
        if (a.button != ui::Button::none)
            ui::draw_button(list, ink_.fonts, hint_glyphs(), a.button, row.x + 20.0f, row.cy(), 36.0f);
        const bool arming = a.act == Act::remove && delete_armed_;
        ui::text(list, ink_.fonts.semibold, arming ? "Press again to delete" : a.label, row.x + 82.0f,
                 row.cy() + 10.0f, 28.0f, a.act == Act::remove ? kCoral : kText);
        if (action_ == i)
            focus_outline(list, row, 16.0f, 1.0f);
    }
    const float ry = panel.y + 16.0f + 72.0f * kActionCount + 18.0f;
    list.rounded_rect({panel.x + 24.0f, ry - 10.0f, w - 48.0f, 2.0f}, 1.0f, style::kLine);
    for (int i = 0; i < kReactionCount; ++i)
    {
        const Rect cell{panel.x + 20.0f + i * 92.0f, ry, 80.0f, 80.0f};
        list.rounded_rect(cell, 18.0f, kRaisedHigh);
        emoji_picture(list, ink_, kReactions[i], std::string(), {cell.cx() - 24.0f, cell.cy() - 24.0f, 48.0f, 48.0f});
        if (action_ == kActionCount + i)
            focus_outline(list, cell, 18.0f, 1.0f);
    }
    list.pop_opacity();
}

// A picture full screen, fitted whole: the larger copy once it has loaded, the
// message's preview stretched meanwhile. Who sent it, its name and size beneath.
void Chat::draw_image(const discord::Snapshot &s, DrawList &list) const
{
    const std::vector<discord::Message> &ms = messages(s);
    if (focus_ < 0 || focus_ >= static_cast<int>(ms.size()))
        return;
    const discord::Message &m = ms[static_cast<std::size_t>(focus_)];
    std::vector<const discord::Attachment *> pics;
    for (const discord::Attachment &a : m.attachments)
        if (a.image)
            pics.push_back(&a);
    if (pics.empty())
        return;
    const discord::Attachment &a = *pics[static_cast<std::size_t>(std::clamp(image_at_, 0, static_cast<int>(pics.size()) - 1))];
    list.rounded_rect({0, 0, 1920, 1080}, 0.0f, Color::rgb(0x08090c, 0.96f));
    const Rect area{80.0f, 60.0f, 1760.0f, 860.0f};
    hui::app::ArtSet::Remote pic = ink_.art.remote && !a.full_url.empty() ? ink_.art.remote(a.full_url)
                                                                          : hui::app::ArtSet::Remote{};
    const bool loading = !a.full_url.empty() && !pic.texture;
    if (loading && ink_.art.remote && !a.url.empty())
        pic = ink_.art.remote(a.url);
    if (pic.texture && pic.w > 0 && pic.h > 0)
    {
        const float scale = std::min(area.w / static_cast<float>(pic.w), area.h / static_cast<float>(pic.h));
        const float w = pic.w * scale, h = pic.h * scale;
        list.image(pic.texture, {area.cx() - w * 0.5f, area.cy() - h * 0.5f, w, h}, gfx::kFullUv,
                   Color::rgb(0xffffff), 12.0f);
    }
    else
        scene(list, ink_, a.art, {area.cx() - 480.0f, area.cy() - 270.0f, 960.0f, 540.0f}, 12.0f, std::string());
    if (loading)
        ui::text(list, ink_.fonts.semibold, "Loading…", area.cx(), area.y + 40.0f, 26.0f, kDim, gfx::Align::center);
    std::string line = author_name(s, m) + "  ·  " + a.name + "  ·  " + a.size_text;
    if (pics.size() > 1)
        line += "  ·  " + std::to_string(image_at_ + 1) + " of " + std::to_string(pics.size());
    ui::text(list, ink_.fonts.regular, fit(ink_.fonts.regular, line, 24.0f, 1100.0f), kMargin, kHintsBaseline + 8.0f,
             24.0f, kDim);
    ui::Hint hints[2] = {{ui::Button::circle, "Close"}, {}};
    int count = 1;
    if (pics.size() > 1)
        hints[count++] = {ui::Button::dpad, "Next"};
    ui::HintLayout layout;
    layout.size = 34.0f;
    layout.text_size = 23.0f;
    layout.cy = kHintsBaseline;
    layout.item_gap = 38.0f;
    ui::draw_hints(list, ink_.fonts, hint_glyphs(), hints, count, kRight, true, layout);
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
    if (!rows.empty())
    {
        // Above the oldest message: the channel's start, or more to come.
        const Rect first = row_rect(rows.front());
        if (first.y - 60.0f > top - 40.0f)
        {
            std::string name = "this conversation";
            for (const discord::Channel *c : chips(s))
                if (c->id == channel_id_)
                    name = "#" + c->name;
            const bool start = s.history_start.count(channel_id_) > 0;
            ui::text(list, ink_.fonts.regular, start ? "This is the start of " + name + "." : "Loading older messages…",
                     kMargin + kAvatarCol, first.y - 22.0f, 22.0f, kFaint);
        }
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

    ui::Hint hints[7] = {{ui::Button::cross, "Select"}, {ui::Button::circle, "Back"}, {ui::Button::triangle, "Type"},
                         {ui::Button::right_stick, "Text size"}, {}};
    int count = 4;
    if (!dm())
        hints[count++] = {ui::Button::options, "Channels"};
    if (dm())
        hints[count++] = {ui::Button::square, s.voice.active() && s.voice.channel_id == channel_id_ ? "In call" : "Call"};
    else if (s.voice.active())
        hints[count++] = {ui::Button::square, "Voice"};
    ui::HintLayout layout;
    layout.size = 34.0f;
    layout.text_size = 23.0f;
    layout.cy = kHintsBaseline;
    layout.item_gap = 38.0f;
    ui::draw_hints(list, ink_.fonts, hint_glyphs(), hints, count, kRight, true, layout);
    if (mode_ == Mode::keys)
        keys_.draw(list);
    if (mode_ == Mode::image)
        draw_image(s, list);
    if (mode_ == Mode::channels)
        draw_channel_list(s, list);
    if (mode_ == Mode::profile)
        if (const discord::User *u = dm_person(s))
        {
            // The person's card: face, name, what they are up to.
            list.rounded_rect({0, 0, 1920, 1080}, 0.0f, Color::rgb(0x0d0e12, 0.6f));
            const Rect card{660.0f, 250.0f, 600.0f, 520.0f};
            list.shadow(card, 28.0f, 50.0f, Color::rgb(0, 0.5f));
            list.rounded_rect(card, 28.0f, kRaised);
            list.rounded_rect({card.x, card.y, card.w, 150.0f}, 28.0f, kRaisedHigh);
            list.circle(card.cx(), card.y + 150.0f, 92.0f, kRaised);
            avatar(list, ink_, *u, card.cx(), card.y + 150.0f, 84.0f);
            ui::text(list, ink_.fonts.display, fit(ink_.fonts.display, u->name, 46.0f, card.w - 60.0f), card.cx(),
                     card.y + 310.0f, 46.0f, kText, gfx::Align::center);
            if (!u->voice_room.empty())
                ui::text(list, ink_.fonts.semibold, "In voice: " + u->voice_room, card.cx(), card.y + 362.0f, 26.0f,
                         kOnline, gfx::Align::center);
            else
                ui::text(list, ink_.fonts.regular, fit(ink_.fonts.regular, status_line(*u), 26.0f, card.w - 60.0f),
                         card.cx(), card.y + 362.0f, 26.0f, kDim, gfx::Align::center);
            const ui::Hint hints[] = {{ui::Button::cross, "Call"}, {ui::Button::circle, "Close"}};
            ui::HintLayout layout;
            layout.size = 30.0f;
            layout.text_size = 22.0f;
            layout.cy = card.y + card.h - 40.0f;
            ui::draw_hints(list, ink_.fonts, hint_glyphs(), hints, 2, card.x + card.w - 30.0f, true, layout);
        }
    if (size_note_ > 0.0f)
    {
        const std::string note = std::string("Text size: ") + kTextLevelNames[g_text_level];
        const float w = ink_.fonts.semibold.measure(note, 26.0f) + 60.0f;
        const Rect r{960.0f - w * 0.5f, 470.0f, w, 64.0f};
        list.push_opacity(std::min(1.0f, size_note_ * 3.0f));
        list.rounded_rect(r, 32.0f, kRaisedHigh);
        ui::text(list, ink_.fonts.semibold, note, r.cx(), r.cy() + 9.0f, 26.0f, kText, gfx::Align::center);
        list.pop_opacity();
    }
}

// Every channel of the server, under its categories (Options in a server).
void Chat::draw_channel_list(const discord::Snapshot &s, DrawList &list) const
{
    list.rounded_rect({0, 0, 1920, 1080}, 0.0f, Color::rgb(0x0d0e12, 0.55f));
    const Rect panel{0, 0, 640, 1080};
    list.shadow(panel, 0.0f, 40.0f, Color::rgb(0, 0.5f));
    list.rounded_rect(panel, 0.0f, kRaised);
    const discord::Guild *g = discord::find_guild(s, guild_id_);
    ui::text(list, ink_.fonts.display, fit(ink_.fonts.display, g ? g->name : std::string(), 40.0f, 540.0f), kMargin,
             86.0f, 40.0f, kText);
    // A rule under the title; the list scrolls beneath it with room to spare.
    list.rounded_rect({kMargin, 116.0f, 640.0f - 2 * kMargin, 2.0f}, 1.0f, kLine);
    list.push_clip({0, 124.0f, 640, 876.0f});
    const std::vector<const discord::Channel *> all = chips(s);
    const std::vector<float> tops = list_tops(all);
    std::string category = "\x01"; // none drawn yet
    for (int i = 0; i < static_cast<int>(all.size()); ++i)
    {
        const discord::Channel &c = *all[static_cast<std::size_t>(i)];
        const float y = 150.0f + tops[static_cast<std::size_t>(i)] - list_scroll_.value;
        if (c.category != category)
        {
            category = c.category;
            if (!category.empty())
                ui::text(list, ink_.fonts.semibold, fit(ink_.fonts.semibold, category, 19.0f, 520.0f), kMargin,
                         y - 6.0f, 19.0f, kFaint);
        }
        if (y > 1080.0f || y + 54.0f < 100.0f)
            continue;
        const Rect r{kMargin - 12.0f, y + 2.0f, 640.0f - 2 * kMargin + 24.0f, 46.0f};
        const bool on = c.id == channel_id_;
        const bool hot = i == list_index_;
        if (on)
            list.rounded_rect(r, 12.0f, kRaisedHigh);
        const Color ink = c.has_unread || c.unread > 0 || on ? kText : kDim;
        ui::text(list, ink_.fonts.semibold, "#", r.x + 14.0f, r.cy() + 9.0f, 25.0f, kFaint);
        ui::text(list, c.has_unread ? ink_.fonts.semibold : ink_.fonts.regular,
                 fit(ink_.fonts.regular, c.name, 24.0f, r.w - 130.0f), r.x + 42.0f, r.cy() + 8.0f, 24.0f, ink);
        if (c.unread > 0)
            count_badge(list, ink_, c.unread, r.x + r.w - 12.0f, r.cy(), 30.0f);
        else if (c.has_unread)
            list.circle(r.x + r.w - 22.0f, r.cy(), 6.0f, kText);
        if (hot)
            focus_outline(list, r, 12.0f, 1.0f);
    }
    list.pop_clip();
    const ui::Hint hints[] = {{ui::Button::cross, "Open"}, {ui::Button::circle, "Close"}};
    ui::HintLayout layout;
    layout.size = 30.0f;
    layout.text_size = 22.0f;
    layout.cy = kHintsBaseline;
    ui::draw_hints(list, ink_.fonts, hint_glyphs(), hints, 2, 640.0f - kMargin, true, layout);
}

} // namespace screens
