// ps5-discord - The emoji picker.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// D-pad moves in the grid (down from a category's last row goes on into the next),
// L1 and R1 jump a category, Cross picks, Circle closes. Server emoji that cannot be
// sent from here (another server's without Nitro, animated ones, unavailable ones) show
// faint and say why.

#include "screens/emoji_picker.hpp"

#include <algorithm>

#include "core/save_file.hpp"
#include "discord/emoji.hpp"
#include "screens/style.hpp"

namespace screens
{

namespace
{

using namespace style;
namespace gfx = hui::gfx;
namespace ui = hui::ui;
using hui::Action;
using hui::Direction;

constexpr Rect kPanel{410.0f, 70.0f, 1100.0f, 940.0f};
constexpr float kRailW = 96.0f, kCell = 84.0f, kHeadH = 56.0f, kFootH = 120.0f;
constexpr int kColumns = 11;
constexpr float kGridX = kPanel.x + kRailW + 30.0f, kGridY = kPanel.y + 96.0f;
constexpr float kGridH = kPanel.h - 96.0f - kFootH;
constexpr std::size_t kRecentMax = 33; // three rows

// The rail's picture for each Unicode group, in emoji-test order.
const char *const kGroupIcons[] = {"😀", "👋", "🐻", "🍔", "🚗", "⚽", "💡", "❤️", "🏁"};

std::string g_store;
std::vector<std::string> g_recent; // "😀", or "c:<id>:<animated 0|1>:<name>"
bool g_loaded = false;

int rows_of(std::size_t n)
{
    return static_cast<int>((n + kColumns - 1) / kColumns);
}

} // namespace

EmojiPicker::EmojiPicker(const Ink &ink) : ink_(ink) {}

void EmojiPicker::set_store(const std::string &path)
{
    g_store = path;
}

void EmojiPicker::open(const discord::Snapshot &s, const std::string &guild_id, const std::string &title)
{
    if (!g_loaded)
    {
        g_loaded = true;
        std::string data;
        if (!g_store.empty() && hui::save::read_file(g_store, &data, 16384))
            for (std::size_t at = 0; at < data.size();)
            {
                const std::size_t end = std::min(data.find('\n', at), data.size());
                if (end > at)
                    g_recent.push_back(data.substr(at, end - at));
                at = end + 1;
            }
    }
    title_ = title;
    sections_.clear();
    section_ = index_ = 0;
    scroll_ = scroll_target_ = 0.0f;

    // Each server's own emoji, the chat's server first.
    std::vector<Section> servers;
    for (const discord::Guild &g : s.guilds)
    {
        if (g.emojis.empty())
            continue;
        Section sec;
        sec.title = g.name;
        sec.icon = {std::string(), g.emojis.front().id, g.emojis.front().name};
        const bool here = g.id == guild_id;
        for (const discord::GuildEmoji &e : g.emojis)
        {
            Item it{std::string(), e.id, e.name, e.animated};
            if (!e.available)
                it.usable = false, it.why = "Unavailable: the server lost its boost";
            else if (!s.nitro && e.animated)
                it.usable = false, it.why = "Animated emoji need Nitro";
            else if (!s.nitro && !here)
                it.usable = false, it.why = "Another server's emoji need Nitro";
            sec.items.push_back(it);
        }
        if (here)
            servers.insert(servers.begin(), std::move(sec));
        else
            servers.push_back(std::move(sec));
    }

    Section recent;
    recent.title = "Frequently used";
    recent.icon.text = "🕒";
    std::vector<std::string> keys = g_recent;
    if (keys.empty())
        keys = {"❤️", "👍", "😂", "😮", "🔥", "🎉", "😭", "👀"};
    for (const std::string &k : keys)
    {
        if (k.rfind("c:", 0) != 0)
        {
            recent.items.emplace_back().text = k;
            continue;
        }
        // A server's: still usable here only if it would be in its server's section.
        const std::size_t a = k.find(':', 2), b = a == std::string::npos ? a : k.find(':', a + 1);
        if (b == std::string::npos)
            continue;
        const std::string id = k.substr(2, a - 2);
        for (const Section &sec : servers)
            for (const Item &it : sec.items)
                if (it.id == id)
                    recent.items.push_back(it);
    }
    sections_.push_back(std::move(recent));
    for (Section &sec : servers)
        sections_.push_back(std::move(sec));
    for (int g = 0; g < discord::kEmojiGroupCount; ++g)
    {
        Section sec;
        sec.title = discord::kEmojiGroups[g];
        sec.icon.text = g < static_cast<int>(sizeof kGroupIcons / sizeof *kGroupIcons) ? kGroupIcons[g] : "😀";
        sections_.push_back(std::move(sec));
    }
    const int first_group = static_cast<int>(sections_.size()) - discord::kEmojiGroupCount;
    for (int i = 0; i < discord::kEmojiCount; ++i)
    {
        const discord::EmojiEntry &e = discord::kEmojiTable[i];
        Item &it = sections_[static_cast<std::size_t>(first_group + e.group)].items.emplace_back();
        it.text = e.text;
        it.name = e.name;
    }
}

float EmojiPicker::section_top(int section) const
{
    float y = 0.0f;
    for (int i = 0; i < section; ++i)
        y += kHeadH + rows_of(sections_[static_cast<std::size_t>(i)].items.size()) * kCell + 12.0f;
    return y;
}

const EmojiPicker::Item &EmojiPicker::focused() const
{
    return sections_[static_cast<std::size_t>(section_)].items[static_cast<std::size_t>(index_)];
}

void EmojiPicker::remember(const Item &it)
{
    const std::string key = it.id.empty() ? it.text : "c:" + it.id + ":" + (it.animated ? "1" : "0") + ":" + it.name;
    g_recent.erase(std::remove(g_recent.begin(), g_recent.end(), key), g_recent.end());
    g_recent.insert(g_recent.begin(), key);
    if (g_recent.size() > kRecentMax)
        g_recent.resize(kRecentMax);
    if (g_store.empty())
        return;
    std::string data;
    for (const std::string &k : g_recent)
        data += k + "\n";
    hui::save::write_atomic(g_store, data);
}

EmojiPicker::Result EmojiPicker::update(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback)
{
    if (sections_.empty())
        return Result::cancel;
    if (input.is_pressed(Action::back))
    {
        feedback.play(hui::audio::Cue::modal_close);
        return Result::cancel;
    }
    const int sections = static_cast<int>(sections_.size());
    auto count = [&](int s) { return static_cast<int>(sections_[static_cast<std::size_t>(s)].items.size()); };
    auto land = [&](int s, int column) { // a section's first row, at the column (or its last item)
        section_ = s;
        index_ = std::min(column, count(s) - 1);
    };
    if (input.is_pressed(Action::page_prev) || input.is_pressed(Action::page_next))
    {
        int s = section_;
        do
            s += input.is_pressed(Action::page_next) ? 1 : -1;
        while (s >= 0 && s < sections && count(s) == 0);
        if (s >= 0 && s < sections)
        {
            land(s, 0);
            feedback.play(hui::audio::Cue::focus, 1.05f);
        }
        else
            feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.5f);
    }
    else if (input.nav != Direction::none)
    {
        const int col = index_ % kColumns, n = count(section_);
        bool moved = true;
        if (input.nav == Direction::left && index_ > 0)
            --index_;
        else if (input.nav == Direction::right && index_ < n - 1)
            ++index_;
        else if (input.nav == Direction::down && index_ + kColumns < n)
            index_ += kColumns;
        else if (input.nav == Direction::down && index_ / kColumns < (n - 1) / kColumns)
            index_ = n - 1; // the short last row
        else if (input.nav == Direction::up && index_ >= kColumns)
            index_ -= kColumns;
        else if (input.nav == Direction::down || input.nav == Direction::right)
        {
            int s = section_ + 1;
            while (s < sections && count(s) == 0)
                ++s;
            if (s < sections)
                land(s, input.nav == Direction::down ? col : 0);
            else
                moved = false;
        }
        else if (input.nav == Direction::up || input.nav == Direction::left)
        {
            int s = section_ - 1;
            while (s >= 0 && count(s) == 0)
                --s;
            if (s >= 0)
            {
                section_ = s;
                const int last_row = (count(s) - 1) / kColumns;
                index_ = input.nav == Direction::left ? count(s) - 1 : std::min(last_row * kColumns + col, count(s) - 1);
            }
            else
                moved = false;
        }
        else
            moved = false;
        feedback.play(moved ? hui::audio::Cue::tick : hui::audio::Cue::error, 1.0f, 0.0f, moved ? 1.0f : 0.5f);
    }
    if (input.is_pressed(Action::confirm))
    {
        if (!focused().usable)
            feedback.play(hui::audio::Cue::error);
        else
        {
            picked_ = focused();
            remember(picked_);
            feedback.play(hui::audio::Cue::select);
            return Result::picked;
        }
    }
    // Keep the focused row in view, a row of the next one showing.
    const float row_y = section_top(section_) + kHeadH + (index_ / kColumns) * kCell;
    if (row_y - kHeadH < scroll_target_)
        scroll_target_ = std::max(0.0f, row_y - kHeadH);
    else if (row_y + kCell * 1.5f > scroll_target_ + kGridH)
        scroll_target_ = row_y + kCell * 1.5f - kGridH;
    scroll_ += (scroll_target_ - scroll_) * std::min(1.0f, dt * 16.0f);
    return Result::none;
}

void EmojiPicker::draw(DrawList &list) const
{
    if (sections_.empty())
        return;
    list.rounded_rect({0, 0, 1920, 1080}, 0.0f, Color::rgb(0x0d0e12, 0.7f));
    list.shadow(kPanel, 28.0f, 50.0f, Color::rgb(0, 0.5f));
    list.rounded_rect(kPanel, 28.0f, kRaised);
    ui::text(list, ink_.fonts.display, title_, kPanel.x + 40.0f, kPanel.y + 66.0f, 40.0f, kText);

    // The rail: a picture per category, the current one lit.
    list.rounded_rect({kPanel.x, kPanel.y + 90.0f, kRailW, kPanel.h - 90.0f - kFootH}, 0.0f, Color::rgb(0x1b1e24));
    const float rail_step = std::min(64.0f, (kPanel.h - 110.0f - kFootH) / static_cast<float>(sections_.size()));
    for (std::size_t i = 0; i < sections_.size(); ++i)
    {
        const float cy = kPanel.y + 110.0f + rail_step * (static_cast<float>(i) + 0.5f);
        const bool on = static_cast<int>(i) == section_;
        if (on)
            list.rounded_rect({kPanel.x + 14.0f, cy - rail_step * 0.5f + 2.0f, kRailW - 28.0f, rail_step - 4.0f}, 14.0f,
                              kRaisedHigh);
        const float sz = std::min(40.0f, rail_step - 16.0f);
        list.push_opacity(on ? 1.0f : 0.55f);
        emoji_picture(list, ink_, sections_[i].icon.text, sections_[i].icon.id,
                      {kPanel.x + kRailW * 0.5f - sz * 0.5f, cy - sz * 0.5f, sz, sz});
        list.pop_opacity();
    }

    // The grid, only the rows in view drawn.
    list.push_clip({kGridX - 10.0f, kGridY, kColumns * kCell + 20.0f, kGridH});
    for (std::size_t si = 0; si < sections_.size(); ++si)
    {
        const Section &sec = sections_[si];
        if (sec.items.empty())
            continue;
        const float top = kGridY + section_top(static_cast<int>(si)) - scroll_;
        const float bottom = top + kHeadH + rows_of(sec.items.size()) * kCell;
        if (bottom < kGridY || top > kGridY + kGridH)
            continue;
        ui::text(list, ink_.fonts.semibold, fit(ink_.fonts.semibold, sec.title, 24.0f, kColumns * kCell),
                 kGridX, top + 38.0f, 24.0f, kDim);
        for (std::size_t i = 0; i < sec.items.size(); ++i)
        {
            const float x = kGridX + (i % kColumns) * kCell, y = top + kHeadH + (i / kColumns) * kCell;
            if (y + kCell < kGridY || y > kGridY + kGridH)
                continue;
            const Item &it = sec.items[i];
            const bool on = static_cast<int>(si) == section_ && static_cast<int>(i) == index_;
            if (on)
                list.rounded_rect({x + 2.0f, y + 2.0f, kCell - 4.0f, kCell - 4.0f}, 16.0f, kRaisedHigh);
            list.push_opacity(it.usable ? 1.0f : 0.3f);
            emoji_picture(list, ink_, it.text, it.id, {x + 16.0f, y + 16.0f, kCell - 32.0f, kCell - 32.0f});
            list.pop_opacity();
            if (on)
                focus_outline(list, {x + 2.0f, y + 2.0f, kCell - 4.0f, kCell - 4.0f}, 16.0f, 1.0f);
        }
    }
    list.pop_clip();

    // The footer: the focused emoji large, its name, and why not when it cannot be sent.
    const Rect foot{kPanel.x, kPanel.y + kPanel.h - kFootH, kPanel.w, kFootH};
    list.rounded_rect({foot.x + 24.0f, foot.y, foot.w - 48.0f, 2.0f}, 1.0f, kLine);
    const Item &it = focused();
    emoji_picture(list, ink_, it.text, it.id, {foot.x + 40.0f, foot.cy() - 30.0f, 60.0f, 60.0f});
    std::string name = it.id.empty() ? it.name : ":" + it.name + ":";
    if (name.empty() && discord::emoji_index(it.text) >= 0) // a recent one: Unicode's name
        name = discord::kEmojiTable[discord::emoji_index(it.text)].name;
    ui::text(list, ink_.fonts.semibold, fit(ink_.fonts.semibold, name, 28.0f, 520.0f),
             foot.x + 124.0f, foot.cy() + (it.usable ? 10.0f : -6.0f), 28.0f, kText);
    if (!it.usable)
        ui::text(list, ink_.fonts.regular, it.why, foot.x + 124.0f, foot.cy() + 28.0f, 22.0f, kDnd);
    const ui::Hint hints[] = {{ui::Button::l1, "Category", ui::Button::r1},
                              {ui::Button::cross, "Pick"},
                              {ui::Button::circle, "Close"}};
    ui::HintLayout layout;
    layout.size = 32.0f;
    layout.text_size = 22.0f;
    layout.cy = foot.cy();
    ui::draw_hints(list, ink_.fonts, hint_glyphs(), hints, 3, foot.x + foot.w - 40.0f, true, layout);
}

std::string EmojiPicker::markup() const
{
    if (picked_.id.empty())
        return picked_.text;
    return std::string(picked_.animated ? "<a:" : "<:") + picked_.name + ":" + picked_.id + ">";
}

std::string EmojiPicker::reaction() const
{
    return picked_.id.empty() ? picked_.text : picked_.name + ":" + picked_.id;
}

} // namespace screens
