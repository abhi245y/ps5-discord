// ps5-discord - The voice room and the incoming call (framework frames 10a, 10).
// SPDX-License-Identifier: AGPL-3.0-or-later
//
//   - the title (room or friend), who is in it and for how long, and the
//     connection pill (state, then the ping);
//   - the faces: speaking ring, muted and deafened badges, Ringing for a call
//     that has not been answered yet;
//   - Mute, Deafen, Share screen (not yet available) and Leave.
// Square mutes, Triangle deafens, Circle goes back and leaves the call running.

#include "screens/voice.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
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

constexpr float kCellW = (1920.0f - 2.0f * kMargin) / 6.0f;
constexpr int kCols = 6;
constexpr float kRowY = 316.0f; // first row's centre
constexpr float kRowGap = 262.0f; // room for a face, its name and its state
constexpr float kFaceR = 76.0f;
constexpr std::size_t kMaxCells = 12; // up to 11 faces, then a "+N" chip
constexpr float kCtrlY = 860.0f;
constexpr float kCtrlR = 62.0f;
constexpr float kCtrlX[] = {600.0f, 840.0f, 1080.0f, 1320.0f};

enum Control : int
{
    kMute,
    kDeafen,
    kShare, // drawn, never focused: screen share is not in this milestone
    kLeave,
    kControls,
};

struct Face
{
    discord::User user;
    bool ringing = false;
    bool muted = false;
    bool deaf = false;
};

// A person in the room; a stranger shows as "Someone" until their name is known.
discord::User person_for(const discord::Snapshot &s, const std::string &id)
{
    if (const discord::User *u = discord::find_user(s, id))
        return *u;
    discord::User u;
    u.id = id;
    u.name = "Someone";
    return u;
}

const discord::Call *call_in(const discord::Snapshot &s, const std::string &channel)
{
    for (const discord::Call &call : s.calls)
        if (call.channel_id == channel)
            return &call;
    return nullptr;
}

// Everyone in the room: those in voice, then those a call still rings.
std::vector<Face> faces_in(const discord::Snapshot &s, const std::string &channel)
{
    std::vector<Face> out;
    if (const auto seats = s.seats.find(channel); seats != s.seats.end())
        for (const discord::VoiceSeat &seat : seats->second)
        {
            discord::User who = person_for(s, seat.user_id);
            who.speaking = seat.speaking; // the seat knows (strangers have no User entry to flag)
            out.push_back({who, false, seat.muted, seat.deafened});
        }
    if (const discord::Call *call = call_in(s, channel))
        for (const std::string &id : call->ringing)
            if (std::none_of(out.begin(), out.end(), [&](const Face &f) { return f.user.id == id; }))
                out.push_back({person_for(s, id), true, false, false});
    return out;
}

// The other person, while a call we started is still ringing them; empty otherwise.
std::string calling_who(const discord::Snapshot &s, const std::string &channel)
{
    const discord::Call *call = call_in(s, channel);
    if (!call)
        return {};
    for (const std::string &id : call->ringing)
        if (id != s.me.id)
            return person_for(s, id).name;
    return {};
}

// 42:10, or 1:02:03 past an hour.
std::string elapsed_text(std::int64_t ms)
{
    const long total = static_cast<long>(std::max<std::int64_t>(0, ms) / 1000);
    char out[16];
    if (total >= 3600)
        std::snprintf(out, sizeof out, "%ld:%02ld:%02ld", total / 3600, (total % 3600) / 60, total % 60);
    else
        std::snprintf(out, sizeof out, "%ld:%02ld", total / 60, total % 60);
    return out;
}

// The connection pill's words.
std::string connection_text(const discord::Voice &v)
{
    switch (v.state)
    {
    case discord::VoiceState::idle:
        return "Not connected";
    case discord::VoiceState::joining:
        return "Joining…";
    case discord::VoiceState::connecting:
        return "Connecting…";
    case discord::VoiceState::securing:
        return "Securing…";
    case discord::VoiceState::connected:
        return v.ping_ms > 0 ? "Connected · " + std::to_string(v.ping_ms) + " ms" : std::string("Connected");
    case discord::VoiceState::error:
        return "Connection lost";
    }
    return {};
}

Color connection_dot(const discord::Voice &v)
{
    switch (v.state)
    {
    case discord::VoiceState::connected:
        return kOnline;
    case discord::VoiceState::error:
        return kDnd;
    case discord::VoiceState::idle:
        return kFaint;
    default:
        return kIdle;
    }
}

// Our own mic as a bar: the level of the last grain (square root, so quiet speech shows).
// Empty while muted.
void mic_meter(DrawList &list, float x, float cy, float w, float level, bool muted)
{
    constexpr float kH = 10.0f;
    list.rounded_rect({x, cy - kH * 0.5f, w, kH}, kH * 0.5f, kRaisedHigh);
    if (!muted && level > 0.0f)
        list.rounded_rect({x, cy - kH * 0.5f, std::max(kH, w * std::sqrt(std::min(1.0f, level))), kH}, kH * 0.5f,
                          kOnline);
}

// A small round badge on a face: the icon drawn by icon on a ground-coloured disc.
template <typename Icon>
void badge(DrawList &list, float cx, float cy, float r, Icon icon)
{
    list.circle(cx, cy, r + 5.0f, kGround);
    list.circle(cx, cy, r, kRaisedHigh);
    icon(list, cx, cy, r * 1.25f);
}

// A curve of the circle round (cx, cy) from angle a0 to a1 (radians, anticlockwise on screen), in short lines.
void curve(DrawList &list, float cx, float cy, float r, float a0, float a1, float thickness, Color c)
{
    constexpr int kSteps = 16;
    float px = cx + r * std::cos(a0);
    float py = cy - r * std::sin(a0);
    for (int i = 1; i <= kSteps; ++i)
    {
        const float a = a0 + (a1 - a0) * static_cast<float>(i) / kSteps;
        const float x = cx + r * std::cos(a);
        const float y = cy - r * std::sin(a);
        list.line(px, py, x, y, thickness, c);
        px = x;
        py = y;
    }
}

// Headphones: a band over the top and a cup each side; slashed when off.
void headphones(DrawList &list, float cx, float cy, float s, Color c, bool slash)
{
    curve(list, cx, cy + s * 0.05f, s * 0.42f, 0.0f, 3.14159f, s * 0.08f, c);
    list.rounded_rect({cx - s * 0.48f, cy + s * 0.02f, s * 0.18f, s * 0.36f}, s * 0.08f, c);
    list.rounded_rect({cx + s * 0.30f, cy + s * 0.02f, s * 0.18f, s * 0.36f}, s * 0.08f, c);
    if (slash)
        list.line(cx - s * 0.42f, cy - s * 0.5f, cx + s * 0.42f, cy + s * 0.42f, s * 0.08f, c);
}

// A microphone, the same shape mic_off slashes.
void mic(DrawList &list, float cx, float cy, float s, Color c)
{
    list.rounded_rect({cx - s * 0.18f, cy - s * 0.5f, s * 0.36f, s * 0.62f}, s * 0.18f, c);
    list.arc(cx, cy - s * 0.05f, s * 0.34f, s * 0.07f, 0.0f, 3.14159f, c);
    list.line(cx, cy + s * 0.3f, cx, cy + s * 0.48f, s * 0.07f, c);
}

// The handset: an arch with its two ends turned down.
void hang_up(DrawList &list, float cx, float cy, float s, Color c)
{
    curve(list, cx, cy + s * 0.12f, s * 0.42f, 0.0f, 3.14159f, s * 0.16f, c);
    list.rounded_rect({cx - s * 0.56f, cy + s * 0.04f, s * 0.22f, s * 0.2f}, s * 0.08f, c);
    list.rounded_rect({cx + s * 0.34f, cy + s * 0.04f, s * 0.22f, s * 0.2f}, s * 0.08f, c);
}

// A monitor with its stand: the screen share the milestone does not have yet.
void monitor(DrawList &list, float cx, float cy, float s, Color c)
{
    list.bordered_rect({cx - s * 0.5f, cy - s * 0.4f, s, s * 0.66f}, s * 0.08f, Color::rgb(0, 0.0f), s * 0.07f, c);
    list.line(cx, cy + s * 0.26f, cx, cy + s * 0.44f, s * 0.07f, c);
    list.line(cx - s * 0.25f, cy + s * 0.44f, cx + s * 0.25f, cy + s * 0.44f, s * 0.07f, c);
}

void draw_face(DrawList &list, const Ink &ink, const Face &f, float cx, float cy)
{
    if (f.ringing)
        list.push_opacity(0.5f);
    const bool speaking = f.user.speaking && !f.ringing;
    if (speaking) // the speaking ring: audio will drive it; the hook is the User's flag
        list.ring(cx, cy, kFaceR + 7.0f, 5.0f, kCoral);
    discord::User face = f.user;
    face.voice_room.clear(); // the room's own ring says who is talking, not the green "in voice"
    avatar(list, ink, face, cx, cy, kFaceR, false);
    if (f.muted)
        badge(list, cx + kFaceR * 0.72f, cy + kFaceR * 0.72f, 22.0f,
              [](DrawList &l, float x, float y, float s) { mic_off(l, x, y, s, kDim); });
    if (f.deaf)
        badge(list, cx - kFaceR * 0.72f, cy + kFaceR * 0.72f, 22.0f,
              [](DrawList &l, float x, float y, float s) { headphones(l, x, y, s, kDim, true); });
    ui::text(list, ink.fonts.semibold, fit(ink.fonts.semibold, f.user.name, 28.0f, kCellW - 30.0f), cx,
             cy + kFaceR + 46.0f, 28.0f, kText, gfx::Align::center);
    const char *status = f.ringing ? "Ringing…"
                         : speaking ? "Speaking"
                         : f.deaf   ? "Deafened"
                         : f.muted  ? "Muted"
                                    : "In voice";
    ui::text(list, ink.fonts.regular, status, cx, cy + kFaceR + 78.0f, 23.0f, kDim, gfx::Align::center);
    if (f.ringing)
        list.pop_opacity();
}

} // namespace

std::string room_title(const discord::Snapshot &s, const std::string &channel)
{
    if (s.voice.active() && s.voice.channel_id == channel && !s.voice.room_name.empty())
        return s.voice.room_name;
    for (const discord::Guild &g : s.guilds)
        for (const discord::Room &r : g.rooms)
            if (r.id == channel)
                return r.name;
    for (const auto &[user_id, dm] : s.dm_by_user)
        if (dm == channel)
            if (const discord::User *u = discord::find_user(s, user_id))
                return u->name;
    if (const discord::Call *call = call_in(s, channel); call && !call->peer_id.empty())
        return person_for(s, call->peer_id).name;
    return "Voice";
}

void VoiceRoom::open(const std::string &channel_id)
{
    channel_ = channel_id;
    focus_ = kMute;
}

bool VoiceRoom::update(const discord::Snapshot &s, const hui::InputFrame &input, float, hui::ui::Feedback &feedback,
                       std::vector<discord::Request> &requests)
{
    const discord::Voice &v = s.voice;
    const bool live = v.active();
    auto set = [&](bool mute, bool deaf) {
        discord::Request r;
        r.kind = discord::Request::Kind::voice_set;
        r.mute = mute;
        r.deaf = deaf;
        requests.push_back(r);
        feedback.play(hui::audio::Cue::toggle);
    };
    if (input.is_pressed(Action::back))
    {
        feedback.play(hui::audio::Cue::back);
        return false; // the call goes on, on the screen behind
    }
    if (input.nav == Direction::left || input.nav == Direction::right)
    {
        int next = focus_ + (input.nav == Direction::right ? 1 : -1);
        if (next == kShare) // drawn but not focusable
            next += input.nav == Direction::right ? 1 : -1;
        if (next >= 0 && next < kControls && next != kShare)
        {
            focus_ = next;
            feedback.play(hui::audio::Cue::focus, 1.0f + 0.02f * static_cast<float>(focus_));
        }
        else
            feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.5f);
    }
    if (input.is_pressed(Action::west) && live)
        set(!v.muted, v.deafened);
    if (input.is_pressed(Action::north) && live)
        set(v.muted, !v.deafened);
    if (input.is_pressed(Action::confirm))
    {
        if (focus_ == kMute && live)
            set(!v.muted, v.deafened);
        else if (focus_ == kDeafen && live)
            set(v.muted, !v.deafened);
        else if (focus_ == kLeave)
        {
            discord::Request r;
            r.kind = discord::Request::Kind::voice_leave;
            requests.push_back(r);
            feedback.play(hui::audio::Cue::back);
            return false;
        }
    }
    return true;
}

void VoiceRoom::draw(const discord::Snapshot &s, DrawList &list) const
{
    const discord::Voice &v = s.voice;
    const std::string channel = v.active() ? v.channel_id : channel_;
    const bool dm = v.active() ? v.dm() : call_in(s, channel) != nullptr;
    list.rounded_rect({0, 0, 1920, 1080}, 0.0f, kGround);

    // Title and what the room is doing.
    ui::text(list, ink_.fonts.display, fit(ink_.fonts.display, room_title(s, channel), 60.0f, 1200.0f), kMargin,
             kTitleBaseline, 60.0f, kText);
    const std::string calling = calling_who(s, channel);
    const bool here = v.active() && v.channel_id == channel;
    if (!calling.empty())
        ui::text(list, ink_.fonts.regular, "Calling " + calling + "…", kMargin, 150.0f, 28.0f, kDim);
    else if (here)
    {
        list.circle(kMargin + 9.0f, 140.0f, 9.0f, kCoral);
        const std::string line = std::to_string(v.member_count) + (dm ? " in call" : " in voice") +
                                 (v.state == discord::VoiceState::connected ? " · " + elapsed_text(s.now_ms - v.since_ms)
                                                                              : std::string());
        ui::text(list, ink_.fonts.regular, line, kMargin + 28.0f, 150.0f, 28.0f, kDim);
        if (v.state == discord::VoiceState::connected)
        {
            // The meter sits after the line: "Mic" then the bar.
            const float x = kMargin + 28.0f + ink_.fonts.regular.measure(line, 28.0f) + 40.0f;
            ui::text(list, ink_.fonts.regular, "Mic", x, 150.0f, 24.0f, kDim);
            mic_meter(list, x + 66.0f, 143.0f, 200.0f, v.mic_level, v.muted);
        }
    }
    else
        ui::text(list, ink_.fonts.regular, "Not in voice", kMargin, 150.0f, 28.0f, kDim);
    if (here && v.state == discord::VoiceState::error)
        ui::text(list, ink_.fonts.regular, fit(ink_.fonts.regular, v.error, 26.0f, 1300.0f), kMargin, 196.0f, 26.0f,
                 kDnd);

    // The connection, always in the top right.
    const std::string link = here ? connection_text(v) : std::string("Not connected");
    const float right = 1920.0f - kMargin;
    const float pw = ink_.fonts.regular.measure(link, 24.0f) + 84.0f;
    const Rect pill{right - pw, 46.0f, pw, 64.0f};
    list.rounded_rect(pill, 32.0f, kRaised);
    list.circle(pill.x + 36.0f, pill.cy(), 9.0f, here ? connection_dot(v) : kFaint);
    ui::text(list, ink_.fonts.regular, link, pill.x + 60.0f, pill.cy() + 9.0f, 24.0f, kText);

    // Faces.
    const std::vector<Face> people = faces_in(s, channel);
    const bool chip = people.size() > kMaxCells;
    const std::size_t shown = chip ? kMaxCells - 1 : people.size();
    for (std::size_t i = 0; i < shown; ++i)
    {
        const float cx = kMargin + kFaceR + static_cast<float>(i % kCols) * kCellW;
        const float cy = kRowY + static_cast<float>(i / kCols) * kRowGap;
        draw_face(list, ink_, people[i], cx, cy);
    }
    if (chip)
    {
        const std::size_t i = shown;
        const float cx = kMargin + kFaceR + static_cast<float>(i % kCols) * kCellW;
        const float cy = kRowY + static_cast<float>(i / kCols) * kRowGap;
        list.circle(cx, cy, kFaceR, kRaised);
        ui::text(list, ink_.fonts.semibold, "+" + std::to_string(people.size() - shown), cx, cy + 14.0f, 40.0f, kText,
                 gfx::Align::center);
    }

    // Controls. Mute and Deafen need a connection; Share is drawn faint.
    const bool usable = v.active();
    for (int c = 0; c < kControls; ++c)
    {
        const float x = kCtrlX[c];
        const bool share = c == kShare;
        const bool lit = c == kMute ? v.muted : c == kDeafen ? v.deafened : false;
        const float alpha = share || (!usable && c != kLeave) ? 0.4f : 1.0f;
        list.push_opacity(alpha);
        Color fill = kRaised;
        if (c == kLeave)
            fill = kDnd;
        else if (lit)
            fill = kRaisedHigh;
        list.circle(x, kCtrlY, kCtrlR, fill);
        switch (c)
        {
        case kMute:
            if (v.muted)
                mic_off(list, x, kCtrlY, 56.0f, kText);
            else
                mic(list, x, kCtrlY, 56.0f, kText);
            break;
        case kDeafen:
            headphones(list, x, kCtrlY, 56.0f, kText, v.deafened);
            break;
        case kShare:
            monitor(list, x, kCtrlY, 52.0f, kFaint);
            break;
        case kLeave:
            hang_up(list, x, kCtrlY, 52.0f, kText);
            break;
        }
        const char *label = c == kMute ? (v.muted ? "Unmute" : "Mute")
                            : c == kDeafen ? (v.deafened ? "Undeafen" : "Deafen")
                            : c == kShare ? "Share screen"
                                          : "Leave";
        ui::text(list, ink_.fonts.semibold, label, x, kCtrlY + kCtrlR + 50.0f, 26.0f, kText, gfx::Align::center);
        list.pop_opacity();
        if (focus_ == c)
            focus_outline(list, {x - kCtrlR - 6.0f, kCtrlY - kCtrlR - 6.0f, 2 * kCtrlR + 12.0f, 2 * kCtrlR + 12.0f},
                          kCtrlR + 6.0f, 1.0f);
    }

    const ui::Hint hints[] = {{ui::Button::cross, "Select"},
                              {ui::Button::circle, "Back"},
                              {ui::Button::square, "Mute"},
                              {ui::Button::triangle, "Deafen"}};
    ui::HintLayout layout;
    layout.size = 34.0f;
    layout.text_size = 23.0f;
    layout.cy = kHintsBaseline;
    layout.item_gap = 38.0f;
    ui::draw_hints(list, ink_.fonts, hint_glyphs(), hints, 4, right, true, layout);
}

void draw_incoming(DrawList &list, const Ink &ink, const discord::Snapshot &s, const discord::Call &call)
{
    list.rounded_rect({0, 0, 1920, 1080}, 0.0f, Color::rgb(0x0d0e12, 0.72f));
    const Rect card{560.0f, 268.0f, 800.0f, 520.0f};
    list.shadow(card, 28.0f, 50.0f, Color::rgb(0, 0.5f));
    list.rounded_rect(card, 28.0f, kRaised);
    const discord::User caller = person_for(s, call.peer_id);
    const float cx = card.cx(), cy = card.y + 136.0f;
    list.ring(cx, cy, 104.0f, 5.0f, kCoral);
    discord::User face = caller;
    face.voice_room.clear();
    avatar(list, ink, face, cx, cy, 94.0f, false);
    ui::text(list, ink.fonts.semibold, "Incoming call", cx, card.y + 292.0f, 26.0f, kCoral, gfx::Align::center);
    ui::text(list, ink.fonts.display, fit(ink.fonts.display, caller.name, 52.0f, card.w - 80.0f), cx, card.y + 364.0f,
             52.0f, kText, gfx::Align::center);
    ui::text(list, ink.fonts.regular, "Direct voice call", cx, card.y + 410.0f, 25.0f, kDim, gfx::Align::center);
    const ui::Hint hints[] = {{ui::Button::cross, "Answer"}, {ui::Button::circle, "Decline"}};
    ui::HintLayout layout;
    layout.size = 32.0f;
    layout.text_size = 22.0f;
    layout.cy = card.y + card.h - 50.0f;
    layout.item_gap = 34.0f;
    ui::draw_hints(list, ink.fonts, hint_glyphs(), hints, 2, card.x + card.w - 40.0f, true, layout);
}

} // namespace screens
