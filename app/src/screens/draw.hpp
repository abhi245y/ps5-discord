// ps5-discord - Drawing pieces every screen shares: avatars, scenes, chips,
// badges, the focus outline and relative times.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>

#include "app/concept.hpp"
#include "discord/model.hpp"
#include "gfx/draw_list.hpp"
#include "ui/fonts.hpp"

namespace screens
{

using hui::gfx::Color;
using hui::gfx::DrawList;
using hui::gfx::Rect;

// What the drawing pieces need from the app.
struct Ink
{
    const hui::ui::Fonts &fonts;
    const hui::app::ArtSet &art;
};

// The coral outline around the focused element; strength 0..1 fades it.
void focus_outline(DrawList &list, const Rect &r, float radius, float strength);

// A round avatar: the user's art (placeholder scene) with their initial, a
// green ring while they are in voice, and a status badge at the lower right
// (speaking bars, playing, idle, online dot).
void avatar(DrawList &list, const Ink &ink, const discord::User &user, float cx, float cy, float radius,
            bool show_status = true);

// The same circle for something that is not a user (a server icon).
// url: the real picture (avatar, server icon) when it has loaded; art and letter until then.
void art_circle(DrawList &list, const Ink &ink, discord::ArtIndex art, const std::string &letter, float cx,
                float cy, float radius, const std::string &url = {});

// A wide placeholder scene filling r (rooms, server banners).
// url: a real picture (server banner, attached image) shown once loaded, cropped to fill.
void scene(DrawList &list, const Ink &ink, discord::ArtIndex art, const Rect &r, float radius,
           const std::string &url = {});

// Three or more rounded bars: someone is talking.
void speaking_bars(DrawList &list, float cx, float cy, float height, Color color);

// A coral count pill ("7", "99+") whose right edge is at right; returns its width.
float count_badge(DrawList &list, const Ink &ink, int count, float right, float cy, float height = 34.0f);
// The width count_badge will take, for laying out around it.
float count_badge_width(const Ink &ink, int count, float height = 34.0f);
// A raised card saying why a row or page is empty.
void empty_note(DrawList &list, const Ink &ink, const Rect &r, const std::string &title, const std::string &body);

// "now", "4m ago", "2h ago", "3d ago".
std::string ago(std::int64_t now_ms, std::int64_t then_ms);

// Cuts text to max_width with an ellipsis, on whole UTF-8 letters.
std::string fit(const hui::ui::FontRef &font, const std::string &text, float size, float max_width);

// "In voice", the activity, "Away", "Do not disturb" or "Online".
std::string status_line(const discord::User &user);

// A row heading at the left margin: "Friends online · 48"; count_hot paints the count coral.
void row_label(DrawList &list, const Ink &ink, float baseline, const std::string &text, const std::string &count,
               bool count_hot = false);

// A crossed-out microphone centred on (cx, cy), s across.
void mic_off(DrawList &list, float cx, float cy, float s, Color c);

// The pill at the top right of every screen while in voice: room, count, muted.
void voice_pill(DrawList &list, const Ink &ink, const discord::Voice &voice, float y, float h);

// Up to 99, then "99+".
std::string count_text(int count);

} // namespace screens
