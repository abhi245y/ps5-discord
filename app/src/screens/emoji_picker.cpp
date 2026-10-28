// ps5-discord - The emoji (and sticker) picker.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// D-pad moves in the grid (down from a category's last row goes on into the next),
// L1 and R1 jump a category, Square switches between emoji and stickers (when writing a
// message), Cross picks, Circle closes. What cannot be sent from here (another server's
// emoji or stickers without Nitro, animated emoji, unavailable ones) shows faint and says
// why. Sticker previews are single still frames, freed when the picker closes.

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
constexpr float kRailW = 96.0f, kHeadH = 56.0f, kFootH = 120.0f;
constexpr float kGridX = kPanel.x + kRailW + 30.0f, kGridY = kPanel.y + 96.0f;
constexpr float kGridW = 924.0f, kGridH = kPanel.h - 96.0f - kFootH;
constexpr std::size_t kRecentMax = 33; // three rows

// The rail's picture for each Unicode group, in emoji-test order.
const char *const kGroupIcons[] = {"😀", "👋", "🐻", "🍔", "🚗", "⚽", "💡", "❤️", "🏁"};

std::string g_store;
std::vector<std::string> g_recent; // "😀", or "c:<id>:<animated 0|1>:<name>"
bool g_loaded = false;

// A sticker's preview: a still frame for a Lottie one (discord/stickers.hpp), the picture else.
std::string preview_url(const std::string &id, int format)
{
    const std::string url = discord::sticker_url({id, std::string(), format});
    return format == 3 ? url + "?still" : url;
}

} // namespace

EmojiPicker::EmojiPicker(const Ink &ink) : ink_(ink) {}

void EmojiPicker::set_store(const std::string &path)
{
    g_store = path;
}

int EmojiPicker::columns() const
{
    return on_stickers_ ? 6 : 11;
}

float EmojiPicker::cell() const
{
    return kGridW / static_cast<float>(columns());
}

void EmojiPicker::open(const discord::Snapshot &s, const std::string &guild_id, const std::string &title,
                       bool stickers)
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
    guild_id_ = guild_id;
    nitro_ = s.nitro;
    stickers_allowed_ = stickers;
    on_stickers_ = false;
    build(s);
}

void EmojiPicker::close()
{
    if (ink_.art.forget)
        for (const std::string &url : previews_)
            if (url.find("?still") != std::string::npos) // its own; a PNG one may show in the chat too
                ink_.art.forget(url);
    previews_.clear();
}

void EmojiPicker::build(const discord::Snapshot &s)
{
    sections_.clear();
    section_ = index_ = 0;
    scroll_ = scroll_target_ = 0.0f;
    packs_seen_ = s.sticker_packs.size();

    // Each server's own, the chat's server first.
    std::vector<Section> servers;
    for (const discord::Guild &g : s.guilds)
    {
        const bool here = g.id == guild_id_;
        Section sec;
        sec.title = g.name;
        if (on_stickers_)
            for (const discord::Sticker &st : g.stickers)
            {
                Item it;
                it.id = st.id, it.name = st.name, it.format = st.format;
                if (!st.available)
                    it.usable = false, it.why = "Unavailable: the server lost its boost";
                else if (!nitro_ && !here)
                    it.usable = false, it.why = "Another server's stickers need Nitro";
                sec.items.push_back(it);
            }
        else
            for (const discord::GuildEmoji &e : g.emojis)
            {
                Item it;
                it.id = e.id, it.name = e.name, it.animated = e.animated;
                if (!e.available)
                    it.usable = false, it.why = "Unavailable: the server lost its boost";
                else if (!nitro_ && e.animated)
                    it.usable = false, it.why = "Animated emoji need Nitro";
                else if (!nitro_ && !here)
                    it.usable = false, it.why = "Another server's emoji need Nitro";
                sec.items.push_back(it);
            }
        if (sec.items.empty())
            continue;
        sec.icon = sec.items.front();
        if (here)
            servers.insert(servers.begin(), std::move(sec));
        else
            servers.push_back(std::move(sec));
    }

    if (on_stickers_)
    {
        for (Section &sec : servers)
            sections_.push_back(std::move(sec));
        for (const discord::StickerPack &pack : s.sticker_packs)
        {
            Section sec;
            sec.title = pack.name;
            for (const discord::Sticker &st : pack.stickers)
            {
                Item it;
                it.id = st.id, it.name = st.name, it.format = st.format;
                sec.items.push_back(it);
            }
            sec.icon = sec.items.front();
            sections_.push_back(std::move(sec));
        }
        if (sections_.empty())
        {
            Section waiting; // Discord's packs are on their way
            waiting.title = "Loading Discord's stickers…";
            sections_.push_back(std::move(waiting));
        }
        return;
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
        // A server's: shown as it is in its server's section (usable or not, here).
        const std::size_t a = k.find(':', 2);
        if (a == std::string::npos)
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
    const int first_group = static_cast<int>(sections_.size());
    for (int g = 0; g < discord::kEmojiGroupCount; ++g)
    {
        Section sec;
        sec.title = discord::kEmojiGroups[g];
        sec.icon.text = g < static_cast<int>(sizeof kGroupIcons / sizeof *kGroupIcons) ? kGroupIcons[g] : "😀";
        sections_.push_back(std::move(sec));
    }
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
    {
        const std::size_t n = sections_[static_cast<std::size_t>(i)].items.size();
        y += kHeadH + static_cast<float>((n + columns() - 1) / columns()) * cell() + 12.0f;
    }
    return y;
}

const EmojiPicker::Item &EmojiPicker::focused() const
{
    static const Item none;
    const Section &sec = sections_[static_cast<std::size_t>(section_)];
    return index_ < static_cast<int>(sec.items.size()) ? sec.items[static_cast<std::size_t>(index_)] : none;
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

EmojiPicker::Result EmojiPicker::update(const discord::Snapshot &s, const hui::InputFrame &input, float dt,
                                        hui::ui::Feedback &feedback)
{
    if (on_stickers_ && s.sticker_packs.size() != packs_seen_)
        build(s); // Discord's packs arrived
    if (sections_.empty())
        return Result::cancel;
    if (input.is_pressed(Action::back))
    {
        feedback.play(hui::audio::Cue::modal_close);
        return Result::cancel;
    }
    if (input.is_pressed(Action::west) && stickers_allowed_)
    {
        on_stickers_ = !on_stickers_;
        build(s);
        feedback.play(hui::audio::Cue::toggle);
        return Result::none;
    }
    const int sections = static_cast<int>(sections_.size()), cols = columns();
    auto count = [&](int sec) { return static_cast<int>(sections_[static_cast<std::size_t>(sec)].items.size()); };
    if (input.is_pressed(Action::page_prev) || input.is_pressed(Action::page_next))
    {
        int sec = section_;
        do
            sec += input.is_pressed(Action::page_next) ? 1 : -1;
        while (sec >= 0 && sec < sections && count(sec) == 0);
        if (sec >= 0 && sec < sections)
        {
            section_ = sec, index_ = 0;
            feedback.play(hui::audio::Cue::focus, 1.05f);
        }
        else
            feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.5f);
    }
    else if (input.nav != Direction::none && count(section_) > 0)
    {
        const int col = index_ % cols, n = count(section_);
        bool moved = true;
        if (input.nav == Direction::left && index_ > 0)
            --index_;
        else if (input.nav == Direction::right && index_ < n - 1)
            ++index_;
        else if (input.nav == Direction::down && index_ + cols < n)
            index_ += cols;
        else if (input.nav == Direction::down && index_ / cols < (n - 1) / cols)
            index_ = n - 1; // the short last row
        else if (input.nav == Direction::up && index_ >= cols)
            index_ -= cols;
        else if (input.nav == Direction::down || input.nav == Direction::right)
        {
            int sec = section_ + 1;
            while (sec < sections && count(sec) == 0)
                ++sec;
            if (sec < sections)
                section_ = sec, index_ = std::min(input.nav == Direction::down ? col : 0, count(sec) - 1);
            else
                moved = false;
        }
        else if (input.nav == Direction::up || input.nav == Direction::left)
        {
            int sec = section_ - 1;
            while (sec >= 0 && count(sec) == 0)
                --sec;
            if (sec >= 0)
            {
                section_ = sec;
                const int last_row = (count(sec) - 1) / cols;
                index_ = input.nav == Direction::left ? count(sec) - 1 : std::min(last_row * cols + col, count(sec) - 1);
            }
            else
                moved = false;
        }
        else
            moved = false;
        feedback.play(moved ? hui::audio::Cue::tick : hui::audio::Cue::error, 1.0f, 0.0f, moved ? 1.0f : 0.5f);
    }
    if (input.is_pressed(Action::confirm) && count(section_) > 0)
    {
        const Item &it = focused();
        if (!it.usable)
            feedback.play(hui::audio::Cue::error);
        else if (it.format)
        {
            picked_sticker_ = {it.id, it.name, it.format};
            feedback.play(hui::audio::Cue::select);
            return Result::sticker;
        }
        else
        {
            picked_ = it;
            remember(picked_);
            feedback.play(hui::audio::Cue::select);
            return Result::picked;
        }
    }
    // Keep the focused row in view, a little of the next one showing.
    const float row_y = section_top(section_) + kHeadH + static_cast<float>(index_ / cols) * cell();
    if (row_y - kHeadH < scroll_target_)
        scroll_target_ = std::max(0.0f, row_y - kHeadH);
    else if (row_y + cell() * 1.5f > scroll_target_ + kGridH)
        scroll_target_ = row_y + cell() * 1.5f - kGridH;
    scroll_ += (scroll_target_ - scroll_) * std::min(1.0f, dt * 16.0f);
    return Result::none;
}

void EmojiPicker::draw_item(DrawList &list, const Item &it, const Rect &r) const
{
    if (!it.format)
    {
        emoji_picture(list, ink_, it.text, it.id, r);
        return;
    }
    const std::string url = preview_url(it.id, it.format);
    if (std::find(previews_.begin(), previews_.end(), url) == previews_.end())
        previews_.push_back(url);
    const hui::app::ArtSet::Remote pic = ink_.art.remote ? ink_.art.remote(url) : hui::app::ArtSet::Remote{};
    if (!pic.texture || pic.w <= 0 || pic.h <= 0)
    {
        list.rounded_rect(r, 14.0f, kRaisedHigh);
        ui::paragraph(list, ink_.fonts.semibold, it.name, r.cx(), r.cy() + 7.0f, 19.0f, r.w - 16.0f, 24.0f, kDim, 2,
                      gfx::Align::center);
        return;
    }
    // A still (or the first frame of a GIF's sheet), fitted.
    const int rows = (pic.frames + pic.columns - 1) / pic.columns;
    const float fw = static_cast<float>(pic.w) / pic.columns, fh = static_cast<float>(pic.h) / rows;
    const float scale = std::min(r.w / fw, r.h / fh);
    list.image(pic.texture, {r.cx() - fw * scale * 0.5f, r.cy() - fh * scale * 0.5f, fw * scale, fh * scale},
               {0.0f, 0.0f, 1.0f / pic.columns, 1.0f / rows}, Color::rgb(0xffffff), 0.0f);
}

void EmojiPicker::draw(DrawList &list) const
{
    if (sections_.empty())
        return;
    list.rounded_rect({0, 0, 1920, 1080}, 0.0f, Color::rgb(0x0d0e12, 0.7f));
    list.shadow(kPanel, 28.0f, 50.0f, Color::rgb(0, 0.5f));
    list.rounded_rect(kPanel, 28.0f, kRaised);
    ui::text(list, ink_.fonts.display, title_, kPanel.x + 40.0f, kPanel.y + 66.0f, 40.0f, kText);
    if (stickers_allowed_) // the two tabs, Square between them
    {
        const char *tabs[] = {"Emoji", "Stickers"};
        float x = kPanel.x + kPanel.w - 40.0f;
        for (int i = 1; i >= 0; --i)
        {
            const float w = ink_.fonts.semibold.measure(tabs[i], 24.0f) + 40.0f;
            x -= w;
            const Rect tab{x, kPanel.y + 30.0f, w, 48.0f};
            const bool on = (i == 1) == on_stickers_;
            list.rounded_rect(tab, 24.0f, on ? kRaisedHigh : kRaised);
            ui::text(list, ink_.fonts.semibold, tabs[i], tab.cx(), tab.cy() + 8.0f, 24.0f, on ? kText : kDim,
                     gfx::Align::center);
            x -= 10.0f;
        }
    }

    // The rail: a picture per category, the current one lit.
    list.rounded_rect({kPanel.x, kPanel.y + 90.0f, kRailW, kPanel.h - 90.0f - kFootH}, 0.0f, Color::rgb(0x1b1e24));
    const float rail_step = std::min(64.0f, (kPanel.h - 110.0f - kFootH) / static_cast<float>(sections_.size()));
    for (std::size_t i = 0; i < sections_.size(); ++i)
    {
        if (sections_[i].items.empty())
            continue;
        const float cy = kPanel.y + 110.0f + rail_step * (static_cast<float>(i) + 0.5f);
        const bool on = static_cast<int>(i) == section_;
        if (on)
            list.rounded_rect({kPanel.x + 14.0f, cy - rail_step * 0.5f + 2.0f, kRailW - 28.0f, rail_step - 4.0f}, 14.0f,
                              kRaisedHigh);
        const float sz = std::min(40.0f, rail_step - 16.0f);
        list.push_opacity(on ? 1.0f : 0.55f);
        draw_item(list, sections_[i].icon, {kPanel.x + kRailW * 0.5f - sz * 0.5f, cy - sz * 0.5f, sz, sz});
        list.pop_opacity();
    }

    // The grid, only the rows in view drawn.
    const int cols = columns();
    const float size = cell(), pad = on_stickers_ ? 10.0f : 16.0f;
    list.push_clip({kGridX - 10.0f, kGridY, kGridW + 20.0f, kGridH});
    for (std::size_t si = 0; si < sections_.size(); ++si)
    {
        const Section &sec = sections_[si];
        const float top = kGridY + section_top(static_cast<int>(si)) - scroll_;
        const float bottom = top + kHeadH + static_cast<float>((sec.items.size() + cols - 1) / cols) * size;
        if (bottom < kGridY || top > kGridY + kGridH)
            continue;
        ui::text(list, ink_.fonts.semibold, fit(ink_.fonts.semibold, sec.title, 24.0f, kGridW), kGridX, top + 38.0f,
                 24.0f, kDim);
        for (std::size_t i = 0; i < sec.items.size(); ++i)
        {
            const float x = kGridX + static_cast<float>(i % cols) * size;
            const float y = top + kHeadH + static_cast<float>(i / cols) * size;
            if (y + size < kGridY || y > kGridY + kGridH)
                continue;
            const Item &it = sec.items[i];
            const bool on = static_cast<int>(si) == section_ && static_cast<int>(i) == index_;
            const Rect box{x + 2.0f, y + 2.0f, size - 4.0f, size - 4.0f};
            if (on)
                list.rounded_rect(box, 16.0f, kRaisedHigh);
            list.push_opacity(it.usable ? 1.0f : 0.3f);
            draw_item(list, it, {x + pad, y + pad, size - 2 * pad, size - 2 * pad});
            list.pop_opacity();
            if (on)
                focus_outline(list, box, 16.0f, 1.0f);
        }
    }
    list.pop_clip();

    // The footer: the focused one large, its name, and why not when it cannot be sent.
    const Rect foot{kPanel.x, kPanel.y + kPanel.h - kFootH, kPanel.w, kFootH};
    list.rounded_rect({foot.x + 24.0f, foot.y, foot.w - 48.0f, 2.0f}, 1.0f, kLine);
    const Item &it = focused();
    if (!it.text.empty() || !it.id.empty())
    {
        draw_item(list, it, {foot.x + 40.0f, foot.cy() - 30.0f, 60.0f, 60.0f});
        std::string name = it.id.empty() || it.format ? it.name : ":" + it.name + ":";
        if (name.empty() && discord::emoji_index(it.text) >= 0) // a recent one: Unicode's name
            name = discord::kEmojiTable[discord::emoji_index(it.text)].name;
        ui::text(list, ink_.fonts.semibold, fit(ink_.fonts.semibold, name, 28.0f, 420.0f), foot.x + 124.0f,
                 foot.cy() + (it.usable ? 10.0f : -6.0f), 28.0f, kText);
        if (!it.usable)
            ui::text(list, ink_.fonts.regular, it.why, foot.x + 124.0f, foot.cy() + 28.0f, 22.0f, kDnd);
    }
    ui::Hint hints[4] = {{ui::Button::l1, "Category", ui::Button::r1}};
    int count = 1;
    if (stickers_allowed_)
        hints[count++] = {ui::Button::square, on_stickers_ ? "Emoji" : "Stickers"};
    hints[count++] = {ui::Button::cross, on_stickers_ ? "Send" : "Pick"};
    hints[count++] = {ui::Button::circle, "Close"};
    ui::HintLayout layout;
    layout.size = 32.0f;
    layout.text_size = 22.0f;
    layout.cy = foot.cy();
    ui::draw_hints(list, ink_.fonts, hint_glyphs(), hints, count, foot.x + foot.w - 40.0f, true, layout);
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
