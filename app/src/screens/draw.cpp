// ps5-discord - Drawing pieces every screen shares.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "screens/draw.hpp"

#include <algorithm>
#include <string>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <cctype>
#include <cstdio>

#include "screens/style.hpp"

namespace screens
{

namespace
{

using namespace style;
namespace gfx = hui::gfx;
namespace ui = hui::ui;

// Flat stand-ins while (or if) the art is missing: deep, distinct hues.
const Color kArtFallback[] = {
    Color::rgb(0x8a3f54), Color::rgb(0x283c8a), Color::rgb(0x3d5a5c), Color::rgb(0x8a4a6e),
    Color::rgb(0x23463a), Color::rgb(0x1f2f66), Color::rgb(0x4d2f7a), Color::rgb(0x7a3d6a),
    Color::rgb(0x5a4a2a), Color::rgb(0x2f5a73), Color::rgb(0x6a3a3a), Color::rgb(0x3a5a3a),
};

Color fallback(discord::ArtIndex art)
{
    constexpr int n = sizeof kArtFallback / sizeof kArtFallback[0];
    return kArtFallback[((art % n) + n) % n];
}

std::uint32_t texture(const std::vector<std::uint32_t> &set, discord::ArtIndex art)
{
    if (set.empty())
        return 0;
    const int n = static_cast<int>(set.size());
    return set[static_cast<std::size_t>(((art % n) + n) % n)];
}

std::string initial(const std::string &name)
{
    // ponytail: first byte only; a non-ASCII first letter shows as its lead byte's glyph until
    // names go through the shaper (gfx::Shaper) like message text.
    return name.empty() ? std::string("?") : std::string(1, static_cast<char>(std::toupper(name[0])));
}

} // namespace

void focus_outline(DrawList &list, const Rect &r, float radius, float strength)
{
    if (strength <= 0.01f)
        return;
    // Outline only: the focused card keeps its own colours. A wider, faint
    // ring outside it gives the soft edge a glow would, without tinting the card.
    const Rect halo{r.x - 5.0f, r.y - 5.0f, r.w + 10.0f, r.h + 10.0f};
    list.bordered_rect(halo, radius + 5.0f, Color::rgb(0, 0.0f), 6.0f, kCoral.with_alpha(0.22f * strength));
    list.bordered_rect(r, radius, Color::rgb(0, 0.0f), kFocusWidth, kCoral.with_alpha(strength));
}

void art_circle(DrawList &list, const Ink &ink, discord::ArtIndex art, const std::string &letter, float cx,
                float cy, float radius, const std::string &url)
{
    const Rect r{cx - radius, cy - radius, radius * 2, radius * 2};
    if (!url.empty() && ink.art.remote)
        if (const std::uint32_t tex = ink.art.remote(url).texture)
        {
            list.image(tex, r, gfx::kFullUv, Color::rgb(0xffffff), radius);
            return;
        }
    if (const std::uint32_t tex = texture(ink.art.avatars, art))
        list.image(tex, r, gfx::kFullUv, Color::rgb(0xffffff), radius);
    else
        list.rounded_rect(r, radius, fallback(art));
    const float size = radius * 1.05f;
    // A soft shadow under the letter keeps it readable on bright art.
    ui::text(list, ink.fonts.semibold, letter, cx + 2, cy + size * 0.36f + 3, size, Color::rgb(0, 0.35f),
             gfx::Align::center);
    ui::text(list, ink.fonts.semibold, letter, cx, cy + size * 0.36f, size, kText, gfx::Align::center);
}

void avatar(DrawList &list, const Ink &ink, const discord::User &user, float cx, float cy, float radius,
            bool show_status)
{
    const bool in_voice = !user.voice_room.empty();
    if (in_voice)
        list.ring(cx, cy, radius + 5.0f, 4.0f, kOnline);
    art_circle(list, ink, user.art, initial(user.name), cx, cy, radius, user.avatar_url);
    if (!show_status)
        return;

    // Badge at 45 degrees on the lower right, cut out of the avatar by a ring of ground.
    const float br = std::max(9.0f, radius * 0.26f);
    const float bx = cx + radius * 0.72f;
    const float by = cy + radius * 0.72f;
    if (user.speaking)
    {
        list.circle(bx, by, br + 4.0f, kGround);
        list.circle(bx, by, br, kCoral);
        speaking_bars(list, bx, by, br * 1.0f, kText);
        return;
    }
    Color dot = kFaint;
    switch (user.presence)
    {
    case discord::Presence::online:
        dot = kOnline;
        break;
    case discord::Presence::idle:
        dot = kIdle;
        break;
    case discord::Presence::dnd:
        dot = kDnd;
        break;
    case discord::Presence::offline:
        break;
    }
    const float r = br * 0.62f;
    list.circle(bx, by, r + 4.0f, kGround);
    list.circle(bx, by, r, dot);
    if (user.presence == discord::Presence::idle) // a moon bite tells idle apart without colour
        list.circle(bx - r * 0.45f, by - r * 0.45f, r * 0.62f, kGround);
}

void scene(DrawList &list, const Ink &ink, discord::ArtIndex art, const Rect &r, float radius, const std::string &url)
{
    hui::app::ArtSet::Remote real;
    if (!url.empty() && ink.art.remote)
        real = ink.art.remote(url);
    const std::uint32_t tex = real.texture ? real.texture : texture(ink.art.scenes, art);
    if (tex)
    {
        // Crop the picture to the card's aspect, centred.
        const float aspect = r.w / r.h;
        const float kArtAspect = real.texture && real.h > 0 ? static_cast<float>(real.w) / real.h : 16.0f / 9.0f;
        Rect uv = gfx::kFullUv;
        if (aspect > kArtAspect)
        {
            uv.h = kArtAspect / aspect;
            uv.y = (1.0f - uv.h) * 0.5f;
        }
        else
        {
            uv.w = aspect / kArtAspect;
            uv.x = (1.0f - uv.w) * 0.5f;
        }
        list.image(tex, r, uv, Color::rgb(0xffffff), radius);
    }
    else
    {
        list.gradient_rect(r, radius, fallback(art), fallback(art + 3));
    }
}

void speaking_bars(DrawList &list, float cx, float cy, float height, Color color)
{
    const float heights[] = {0.45f, 0.85f, 1.0f, 0.65f};
    const float w = height * 0.13f;
    const float gap = height * 0.12f;
    const float total = 4 * w + 3 * gap;
    float x = cx - total * 0.5f;
    for (float h : heights)
    {
        const float bh = height * h;
        list.rounded_rect({x, cy - bh * 0.5f, w, bh}, w * 0.5f, color);
        x += w + gap;
    }
}

float count_badge_width(const Ink &ink, int count, float height)
{
    return std::max(height, ink.fonts.semibold.measure(count_text(count), height * 0.62f) + height * 0.6f);
}

void empty_note(DrawList &list, const Ink &ink, const Rect &r, const std::string &title, const std::string &body)
{
    list.rounded_rect(r, kCardRadius, kRaised);
    const float top = r.cy() - 14.0f;
    hui::ui::text(list, ink.fonts.semibold, title, r.x + 48.0f, top, 34.0f, kText);
    hui::ui::text(list, ink.fonts.regular, body, r.x + 48.0f, top + 46.0f, kBodySize, kDim);
}

float count_badge(DrawList &list, const Ink &ink, int count, float right, float cy, float height)
{
    const std::string text = count_text(count);
    const float size = height * 0.62f;
    const float w = count_badge_width(ink, count, height);
    list.rounded_rect({right - w, cy - height * 0.5f, w, height}, height * 0.5f, kCoral);
    hui::ui::text(list, ink.fonts.semibold, text, right - w * 0.5f, cy + size * 0.36f, size, kText,
                  hui::gfx::Align::center);
    return w;
}

std::string ago(std::int64_t now_ms, std::int64_t then_ms)
{
    const std::int64_t minutes = std::max<std::int64_t>(0, (now_ms - then_ms) / 60000);
    char out[24];
    if (minutes < 1)
        return "now";
    if (minutes < 60)
        std::snprintf(out, sizeof out, "%lldm ago", static_cast<long long>(minutes));
    else if (minutes < 24 * 60)
        std::snprintf(out, sizeof out, "%lldh ago", static_cast<long long>(minutes / 60));
    else
        std::snprintf(out, sizeof out, "%lldd ago", static_cast<long long>(minutes / (24 * 60)));
    return out;
}

std::string status_line(const discord::User &u)
{
    if (!u.voice_room.empty())
        return "In voice";
    if (!u.activity.empty())
        return u.activity;
    switch (u.presence)
    {
    case discord::Presence::idle:
        return "Away";
    case discord::Presence::dnd:
        return "Do not disturb";
    case discord::Presence::offline:
        return "Offline";
    default:
        return "Online";
    }
}

// Cuts text to max_width with an ellipsis.
std::string fit(const hui::ui::FontRef &font, const std::string &text, float size, float max_width)
{
    // Called for every row every frame: remembered, and found by halving rather
    // than trimming a letter at a time (that measured long mentions hundreds of times).
    static std::unordered_map<std::string, std::string> memo;
    std::string key = std::to_string(reinterpret_cast<std::uintptr_t>(font.font)) + '|' +
                      std::to_string(static_cast<int>(size * 4)) + '|' + std::to_string(static_cast<int>(max_width)) +
                      '|' + text;
    if (const auto found = memo.find(key); found != memo.end())
        return found->second;
    if (memo.size() > 4096) // ponytail: dropped wholesale; an LRU if it churns
        memo.clear();
    std::string out;
    if (font.measure(text, size) <= max_width)
        out = text;
    else
    {
        std::vector<std::size_t> cuts; // ends of whole UTF-8 letters
        for (std::size_t i = 1; i <= text.size(); ++i)
            if (i == text.size() || (static_cast<unsigned char>(text[i]) & 0xc0) != 0x80)
                cuts.push_back(i);
        std::size_t lo = 0, hi = cuts.size(); // letters kept: lo fits, hi does not
        while (hi - lo > 1)
        {
            const std::size_t mid = (lo + hi) / 2;
            if (font.measure(text.substr(0, cuts[mid - 1]) + "…", size) <= max_width)
                lo = mid;
            else
                hi = mid;
        }
        out = (lo == 0 ? std::string() : text.substr(0, cuts[lo - 1])) + "…";
    }
    return memo.emplace(std::move(key), std::move(out)).first->second;
}

void row_label(DrawList &list, const Ink &ink, float baseline, const std::string &text, const std::string &count,
           bool count_hot)
{
    float x = style::kMargin;
    x += ui::text(list, ink.fonts.semibold, text, x, baseline, kLabelSize, kDim);
    if (count.empty())
        return;
    x += ui::text(list, ink.fonts.semibold, " · ", x, baseline, kLabelSize, kFaint);
    ui::text(list, ink.fonts.semibold, count, x, baseline, kLabelSize, count_hot ? kCoral : kDim);
}


void mic_off(DrawList &list, float cx, float cy, float s, Color c)
{
    list.rounded_rect({cx - s * 0.18f, cy - s * 0.5f, s * 0.36f, s * 0.62f}, s * 0.18f, c);
    list.arc(cx, cy - s * 0.05f, s * 0.34f, s * 0.07f, 0.0f, 3.14159f, c);
    list.line(cx, cy + s * 0.3f, cx, cy + s * 0.48f, s * 0.07f, c);
    list.line(cx - s * 0.42f, cy - s * 0.5f, cx + s * 0.42f, cy + s * 0.42f, s * 0.08f, c);
}

void voice_pill(DrawList &list, const Ink &ink, const discord::Voice &voice, float y, float h)
{
    if (!voice.connected)
        return;
    std::string text = "In " + voice.room_name + " · " + std::to_string(voice.member_count);
    if (voice.muted)
        text += " · muted";
    const float right = 1920.0f - style::kMargin;
    const float tw = ink.fonts.regular.measure(text, 24.0f);
    const float w = tw + (voice.muted ? 120.0f : 84.0f);
    const Rect pill{right - w, y, w, h};
    list.rounded_rect(pill, pill.h * 0.5f, kRaised);
    list.circle(pill.x + 36.0f, pill.cy(), 9.0f, kCoral);
    hui::ui::text(list, ink.fonts.regular, text, pill.x + 60.0f, pill.cy() + 9.0f, 24.0f, kText);
    if (voice.muted)
        mic_off(list, right - 38.0f, pill.cy(), 30.0f, kDim);
}

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

std::string count_text(int count)
{
    return count > 99 ? std::string("99+") : std::to_string(count);
}

} // namespace screens
