// ps5-discord - Settings (framework frame 11).
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Left: the sections as cards. Right: the chosen section's rows, each a title, a line
// of description and a control (a value, a toggle, a slider, a button or the mic meter).
// Only rows that work are focusable; the rest are drawn faint as what is coming.
// D-pad up/down moves, right (or Cross) enters the rows, left or Circle leaves them;
// on a slider left/right change it. Circle on the sections leaves the screen.

#include "screens/settings.hpp"

#include <algorithm>

#include "core/version.hpp"
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

enum class Act
{
    none,
    pair,
    ui_volume,
    sfx_volume,
    motion,
    swap,
};

enum class Kind
{
    info,   // text only
    value,  // a value at the right, with a chevron when it opens something
    toggle,
    slider, // 0..10, shown as a percentage
    button,
    meter,  // our mic level
};

struct Row
{
    Kind kind = Kind::info;
    std::string title, desc, value;
    int level = 0;
    bool on = false;
    bool soon = false; // not in this build: drawn faint, not focusable
    Act act = Act::none;
};

struct Section
{
    const char *name;
    const char *about;
};

constexpr Section kSections[] = {
    {"Account", "Who is signed in on this console."},
    {"Voice & audio", "Your microphone, the call's sound and Go Live."},
    {"Notifications", "What shows a banner while you are elsewhere."},
    {"Appearance", "How the app sounds and moves."},
    {"Privacy", "What is kept on the console and who can see it."},
    {"About", "This build and where it comes from."},
};
constexpr int kSectionCount = sizeof kSections / sizeof *kSections;

std::vector<Row> rows_for(int section, const discord::Snapshot &s, const hui::Settings &app, const std::string &version)
{
    const discord::Voice &v = s.voice;
    switch (section)
    {
    case 0:
        return {
            {Kind::value, "Signed in as", "Your Discord account on this console.", s.me.name},
            {Kind::button, "Sign out", "Forget this account on the console.", "Sign out", 0, false, true},
        };
    case 1: {
        Row pair;
        pair.title = "Go Live pairing";
        pair.desc = v.pair_state == "error" && !v.pair_error.empty()
                        ? v.pair_error
                        : "Go Live sees your game through Remote Play, paired once with this console.";
        pair.act = Act::pair;
        if (v.paired && v.pair_state != "pairing")
            pair.kind = Kind::value, pair.value = "Paired";
        else
            pair.kind = Kind::button, pair.value = v.pair_state == "pairing" ? "Pairing…" : "Pair";
        return {
            {Kind::meter, "Microphone", v.active() ? "Your mic in the call, live." : "Shows your mic while you are in a call.", ""},
            {Kind::info, "Before pairing",
             "On the PS5, turn Remote Play on (Settings › System › Remote Play) for a user signed in to PSN.", ""},
            pair,
            {Kind::value, "Go Live quality", "The picture your viewers get.", "720p · 30 fps"},
            {Kind::info, "Controller while live",
             "Plug the DualSense in by USB: it plays the game and is your headset while you are live.", ""},
            {Kind::slider, "Input volume", "How loud your microphone is.", "", 7, false, true},
        };
    }
    case 2:
        return {
            {Kind::value, "Direct messages and mentions", "A banner at the top while you are on another screen.", "Banner"},
            {Kind::toggle, "Every server message", "A banner for every message in your servers.", "", 0, false, true},
        };
    case 3:
        return {
            {Kind::slider, "Interface sounds", "Moving around, selecting and going back.", "", app.ui_volume, false, false,
             Act::ui_volume},
            {Kind::slider, "Sound effects", "Calls, banners and other cues.", "", app.sfx_volume, false, false,
             Act::sfx_volume},
            {Kind::toggle, "Reduced motion", "Fewer and shorter animations.", "", 0, app.reduced_motion, false,
             Act::motion},
            {Kind::toggle, "Swap Cross and Circle", "Circle selects, Cross goes back.", "", 0, app.swap_confirm, false,
             Act::swap},
        };
    case 4:
        return {
            {Kind::info, "Your sign-in", "Kept on the console in /data/ps5-discord, not encrypted yet.", ""},
            {Kind::info, "Status page",
             "Anyone on your network can open http://<console>:9041: the app's state and log, never your sign-in.", ""},
            {Kind::info, "Discord's rules", "Clients other than Discord's own break its terms; use an account you can lose.", ""},
        };
    default:
        return {
            {Kind::value, "Version", "The app and its core.", version.empty() ? "development" : version},
            {Kind::value, "License", "Free software: you may share and change it.", "AGPL-3.0"},
            {Kind::value, "Source", "Where the code lives.", "github.com/abhi245y/ps5-discord"},
        };
    }
}

bool focusable(const Row &r)
{
    return !r.soon && r.act != Act::none;
}

// ---- drawing pieces ------------------------------------------------------------

constexpr float kSecX = kMargin, kSecY = 140.0f, kSecW = 500.0f, kSecH = 104.0f, kSecGap = 16.0f;
constexpr float kPanelX = 620.0f, kPanelY = 124.0f, kPanelW = 1920.0f - kMargin - kPanelX, kPanelH = 870.0f;
constexpr float kRowX = kPanelX + 32.0f, kRowW = kPanelW - 64.0f, kRowY = 282.0f, kRowH = 96.0f, kRowGap = 12.0f;

void section_icon(DrawList &list, int section, float cx, float cy, Color c)
{
    switch (section)
    {
    case 0: // a person
        list.circle(cx, cy - 9.0f, 9.0f, c);
        list.rounded_rect({cx - 15.0f, cy + 4.0f, 30.0f, 16.0f}, 8.0f, c);
        break;
    case 1:
        speaking_bars(list, cx, cy, 28.0f, c);
        break;
    case 2: // a bell
        list.rounded_rect({cx - 12.0f, cy - 14.0f, 24.0f, 24.0f}, 10.0f, c);
        list.rounded_rect({cx - 16.0f, cy + 6.0f, 32.0f, 6.0f}, 3.0f, c);
        list.circle(cx, cy + 16.0f, 4.0f, c);
        break;
    case 3: // a brush
        list.line(cx + 12.0f, cy - 14.0f, cx - 4.0f, cy + 4.0f, 6.0f, c);
        list.circle(cx - 8.0f, cy + 9.0f, 7.0f, c);
        break;
    case 4: // a shield
        list.rounded_rect({cx - 13.0f, cy - 15.0f, 26.0f, 22.0f}, 5.0f, c);
        list.line(cx - 13.0f, cy + 4.0f, cx, cy + 17.0f, 6.0f, c);
        list.line(cx + 13.0f, cy + 4.0f, cx, cy + 17.0f, 6.0f, c);
        break;
    default: // i
        list.circle(cx, cy - 11.0f, 3.5f, c);
        list.rounded_rect({cx - 3.0f, cy - 3.0f, 6.0f, 19.0f}, 3.0f, c);
        break;
    }
}

void chevron(DrawList &list, float x, float cy, Color c)
{
    list.line(x - 6.0f, cy - 10.0f, x + 4.0f, cy, 3.0f, c);
    list.line(x + 4.0f, cy, x - 6.0f, cy + 10.0f, 3.0f, c);
}

void toggle(DrawList &list, float right, float cy, bool on)
{
    const Rect r{right - 76.0f, cy - 20.0f, 76.0f, 40.0f};
    list.rounded_rect(r, 20.0f, on ? kCoral : kRaisedHigh);
    list.circle(on ? r.x + r.w - 20.0f : r.x + 20.0f, cy, 15.0f, Color::rgb(0xffffff));
}

void slider(DrawList &list, const Ink &ink, float right, float cy, int level)
{
    const float w = 320.0f, x = right - 110.0f - w, t = static_cast<float>(level) / 10.0f;
    list.rounded_rect({x, cy - 4.0f, w, 8.0f}, 4.0f, kRaisedHigh);
    list.rounded_rect({x, cy - 4.0f, std::max(8.0f, w * t), 8.0f}, 4.0f, kCoral);
    list.circle(x + w * t, cy, 13.0f, Color::rgb(0xffffff));
    list.circle(x + w * t, cy, 7.0f, kCoral);
    ui::text(list, ink.fonts.regular, std::to_string(level * 10) + "%", right, cy + 9.0f, 25.0f, kText,
             gfx::Align::right);
}

} // namespace

SettingsScreen::SettingsScreen(const Ink &ink) : ink_(ink), version_(hui::read_content_version("/app0/sce_sys/param.json")) {}

void SettingsScreen::open()
{
    section_ = 0;
    row_ = 0;
    in_rows_ = false;
    pin_open_ = false;
}

bool SettingsScreen::update_pin(const discord::Snapshot &s, const hui::InputFrame &input, hui::ui::Feedback &feedback,
                          std::vector<discord::Request> &requests)
{
    const bool done = pin_sent_ && s.voice.paired && s.voice.pair_state == "paired";
    if (input.is_pressed(Action::back) || (done && input.is_pressed(Action::confirm)))
    {
        pin_open_ = false;
        feedback.play(hui::audio::Cue::modal_close);
        return true;
    }
    if (pin_sent_ && s.voice.pair_state == "pairing")
        return true; // waiting for the console
    for (char c : input.typed) // a hardware keyboard types the digits straight in
    {
        if (c >= '0' && c <= '9')
        {
            pin_[pin_at_] = c - '0';
            pin_at_ = std::min(7, pin_at_ + 1);
        }
        else if (c == '\b')
        {
            if (pin_[pin_at_] < 0 && pin_at_ > 0)
                --pin_at_;
            pin_[pin_at_] = -1;
        }
    }
    if (input.nav == Direction::up || input.nav == Direction::down)
    {
        const int step = input.nav == Direction::up ? 1 : 9;
        pin_[pin_at_] = pin_[pin_at_] < 0 ? 0 : (pin_[pin_at_] + step) % 10;
        feedback.play(hui::audio::Cue::slider);
    }
    if (input.nav == Direction::left || input.nav == Direction::right)
    {
        const int next = pin_at_ + (input.nav == Direction::right ? 1 : -1);
        if (next >= 0 && next < 8)
        {
            pin_at_ = next;
            feedback.play(hui::audio::Cue::focus);
        }
        else
            feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.5f);
    }
    if (input.is_pressed(Action::confirm))
    {
        std::string pin;
        for (int d : pin_)
            if (d >= 0)
                pin += static_cast<char>('0' + d);
        if (pin.size() == 8)
        {
            discord::Request r;
            r.kind = discord::Request::Kind::remoteplay_pair;
            r.content = pin;
            requests.push_back(r);
            pin_sent_ = true;
            feedback.play(hui::audio::Cue::select);
        }
        else
            feedback.play(hui::audio::Cue::error);
    }
    return true;
}

bool SettingsScreen::update(const discord::Snapshot &s, const hui::InputFrame &input, float, hui::ui::Feedback &feedback,
                      std::vector<discord::Request> &requests, hui::Settings &app, bool &changed)
{
    if (pin_open_)
        return update_pin(s, input, feedback, requests);
    const std::vector<Row> rows = rows_for(section_, s, app, version_);
    auto first_row = [&]() {
        for (int i = 0; i < static_cast<int>(rows.size()); ++i)
            if (focusable(rows[i]))
                return i;
        return -1;
    };
    if (!in_rows_)
    {
        if (input.is_pressed(Action::back))
        {
            feedback.play(hui::audio::Cue::back);
            return false;
        }
        if (input.nav == Direction::up || input.nav == Direction::down)
        {
            const int next = section_ + (input.nav == Direction::down ? 1 : -1);
            if (next >= 0 && next < kSectionCount)
            {
                section_ = next;
                feedback.play(hui::audio::Cue::focus, 1.0f + 0.02f * static_cast<float>(section_));
            }
            else
                feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.5f);
        }
        if (input.nav == Direction::right || input.is_pressed(Action::confirm))
        {
            const int r = first_row();
            if (r >= 0)
            {
                in_rows_ = true;
                row_ = r;
                feedback.play(hui::audio::Cue::focus);
            }
            else
                feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.5f);
        }
        return true;
    }

    if (row_ >= static_cast<int>(rows.size()) || !focusable(rows[row_]))
        row_ = std::max(0, first_row());
    const Row &row = rows[row_];
    const bool is_slider = row.kind == Kind::slider;
    if (input.is_pressed(Action::back) || (input.nav == Direction::left && !is_slider))
    {
        in_rows_ = false;
        feedback.play(hui::audio::Cue::back);
        return true;
    }
    if (input.nav == Direction::up || input.nav == Direction::down)
    {
        const int dir = input.nav == Direction::down ? 1 : -1;
        int next = row_ + dir;
        while (next >= 0 && next < static_cast<int>(rows.size()) && !focusable(rows[next]))
            next += dir;
        if (next >= 0 && next < static_cast<int>(rows.size()))
        {
            row_ = next;
            feedback.play(hui::audio::Cue::focus);
        }
        else
            feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.5f);
    }
    if (is_slider && (input.nav == Direction::left || input.nav == Direction::right))
    {
        int &level = row.act == Act::ui_volume ? app.ui_volume : app.sfx_volume;
        const int next = std::clamp(level + (input.nav == Direction::right ? 1 : -1), 0, 10);
        if (next != level)
        {
            level = next;
            changed = true;
            feedback.play(hui::audio::Cue::slider);
        }
    }
    if (input.is_pressed(Action::confirm))
    {
        switch (row.act)
        {
        case Act::motion:
            app.reduced_motion = !app.reduced_motion;
            changed = true;
            feedback.play(hui::audio::Cue::toggle);
            break;
        case Act::swap:
            app.swap_confirm = !app.swap_confirm;
            changed = true;
            feedback.play(hui::audio::Cue::toggle);
            break;
        case Act::pair:
            pin_open_ = true;
            pin_sent_ = false;
            std::fill(std::begin(pin_), std::end(pin_), -1);
            pin_at_ = 0;
            feedback.play(hui::audio::Cue::modal_open);
            break;
        default:
            break;
        }
    }
    return true;
}

void SettingsScreen::draw(const discord::Snapshot &s, const hui::Settings &app, DrawList &list) const
{
    list.rounded_rect({0, 0, 1920, 1080}, 0.0f, kGround);
    ui::text(list, ink_.fonts.display, "Settings", kMargin, kTitleBaseline, kTitleSize, kText);

    for (int i = 0; i < kSectionCount; ++i)
    {
        const Rect r{kSecX, kSecY + i * (kSecH + kSecGap), kSecW, kSecH};
        const bool chosen = i == section_;
        list.rounded_rect(r, kCardRadius, kRaised);
        list.circle(r.x + 56.0f, r.cy(), 34.0f, chosen ? Color::rgb(0xff7a59, 0.22f) : kRaisedHigh);
        section_icon(list, i, r.x + 56.0f, r.cy(), chosen ? kCoral : kText);
        ui::text(list, ink_.fonts.semibold, kSections[i].name, r.x + 112.0f, r.cy() + 11.0f, 30.0f, kText);
        chevron(list, r.x + r.w - 34.0f, r.cy(), chosen ? kCoral : kDim);
        if (chosen)
            focus_outline(list, r, kCardRadius, in_rows_ ? 0.35f : 1.0f);
    }

    const Rect panel{kPanelX, kPanelY, kPanelW, kPanelH};
    list.bordered_rect(panel, 28.0f, Color::rgb(0x1b1e24), 1.0f, kLine);
    ui::text(list, ink_.fonts.display, kSections[section_].name, kPanelX + 40.0f, kPanelY + 76.0f, 50.0f, kText);
    ui::text(list, ink_.fonts.regular, kSections[section_].about, kPanelX + 42.0f, kPanelY + 122.0f, 26.0f, kDim);

    const std::vector<Row> rows = rows_for(section_, s, app, version_);
    for (int i = 0; i < static_cast<int>(rows.size()); ++i)
    {
        const Row &row = rows[i];
        const Rect r{kRowX, kRowY + i * (kRowH + kRowGap), kRowW, kRowH};
        const float right = r.x + r.w - 32.0f, cy = r.cy();
        list.push_opacity(row.soon ? 0.4f : 1.0f);
        list.rounded_rect(r, kCardRadius, kRaised);
        ui::text(list, ink_.fonts.semibold, row.title, r.x + 28.0f, r.y + 42.0f, 28.0f, kText);
        const std::string desc = row.soon ? "Coming in a later build." : row.desc;
        // The description gets the row's width, less the control at the right.
        const float control = row.kind == Kind::info     ? 0.0f
                              : row.kind == Kind::button ? 260.0f
                              : row.kind == Kind::value  ? r.w * 0.36f
                                                         : 480.0f;
        ui::text(list, ink_.fonts.regular, fit(ink_.fonts.regular, desc, 22.0f, r.w - 56.0f - control), r.x + 28.0f,
                 r.y + 74.0f, 22.0f, kDim);
        switch (row.kind)
        {
        case Kind::value: {
            const bool opens = row.act != Act::none;
            ui::text(list, ink_.fonts.regular, fit(ink_.fonts.regular, row.value, 26.0f, r.w * 0.38f),
                     opens ? right - 32.0f : right, cy + 9.0f, 26.0f, kText, gfx::Align::right);
            if (opens)
                chevron(list, right - 6.0f, cy, kDim);
            break;
        }
        case Kind::toggle:
            toggle(list, right, cy, row.on);
            break;
        case Kind::slider:
            slider(list, ink_, right, cy, row.level);
            break;
        case Kind::button: {
            const float w = ink_.fonts.semibold.measure(row.value, 26.0f) + 72.0f;
            const Rect b{right - w, cy - 28.0f, w, 56.0f};
            list.rounded_rect(b, 28.0f, kCoral);
            ui::text(list, ink_.fonts.semibold, row.value, b.cx(), cy + 9.0f, 26.0f, kOnCoral, gfx::Align::center);
            break;
        }
        case Kind::meter:
            mic_meter(list, right - 320.0f, cy, 320.0f, s.voice.mic_level, s.voice.muted || !s.voice.active());
            break;
        case Kind::info:
            break;
        }
        list.pop_opacity();
        if (in_rows_ && i == row_)
            focus_outline(list, r, kCardRadius, 1.0f);
    }

    const bool on_slider = in_rows_ && row_ < static_cast<int>(rows.size()) && rows[row_].kind == Kind::slider;
    const ui::Hint hints[] = {{ui::Button::dpad, on_slider ? "Adjust" : "Move"},
                              {ui::Button::cross, "Select"},
                              {ui::Button::circle, "Back"}};
    ui::HintLayout layout;
    layout.size = 34.0f;
    layout.text_size = 23.0f;
    layout.cy = kHintsBaseline;
    layout.item_gap = 38.0f;
    ui::draw_hints(list, ink_.fonts, hint_glyphs(), hints, 3, 1920.0f - kMargin, true, layout);

    if (pin_open_)
        draw_pin(s, list);
}

void SettingsScreen::draw_pin(const discord::Snapshot &s, DrawList &list) const
{
    list.rounded_rect({0, 0, 1920, 1080}, 0.0f, Color::rgb(0x0d0e12, 0.78f));
    const Rect card{460.0f, 230.0f, 1000.0f, 600.0f};
    list.shadow(card, 28.0f, 50.0f, Color::rgb(0, 0.5f));
    list.rounded_rect(card, 28.0f, kRaised);
    ui::text(list, ink_.fonts.display, "Pair Go Live", card.x + 56.0f, card.y + 92.0f, 50.0f, kText);
    ui::text(list, ink_.fonts.regular, "On the PS5: Settings  ›  System  ›  Remote Play  ›  Link Device.",
             card.x + 58.0f, card.y + 150.0f, 26.0f, kDim);
    ui::text(list, ink_.fonts.regular, "Enter the 8-digit number it shows. Up and down change a digit.",
             card.x + 58.0f, card.y + 190.0f, 26.0f, kDim);

    constexpr float kBox = 84.0f, kGap = 20.0f;
    const float x0 = card.cx() - (8 * kBox + 7 * kGap + 24.0f) * 0.5f;
    for (int i = 0; i < 8; ++i)
    {
        const float x = x0 + i * (kBox + kGap) + (i >= 4 ? 24.0f : 0.0f);
        const Rect b{x, card.y + 250.0f, kBox, 108.0f};
        list.rounded_rect(b, 16.0f, kRaisedHigh);
        if (pin_[i] >= 0)
            ui::text(list, ink_.fonts.display, std::string(1, static_cast<char>('0' + pin_[i])), b.cx(),
                     b.y + 76.0f, 56.0f, kText, gfx::Align::center);
        if (i == pin_at_ && !pin_sent_)
            focus_outline(list, b, 16.0f, 1.0f);
    }

    std::string status;
    Color tone = kDim;
    if (pin_sent_)
    {
        if (s.voice.pair_state == "paired" && s.voice.paired)
            status = "Paired. Go Live is ready.", tone = kOnline;
        else if (s.voice.pair_state == "error")
            status = s.voice.pair_error.empty() ? "Pairing failed." : s.voice.pair_error, tone = kDnd;
        else
            status = "Pairing with the console…";
    }
    if (!status.empty())
        ui::paragraph(list, ink_.fonts.regular, status, card.x + 58.0f, card.y + 420.0f, 26.0f, card.w - 116.0f, 34.0f,
                      tone, 2, gfx::Align::left);

    const bool done = pin_sent_ && s.voice.paired && s.voice.pair_state == "paired";
    const ui::Hint hints[] = {{ui::Button::cross, done ? "Done" : "Pair"}, {ui::Button::circle, "Cancel"}};
    ui::HintLayout layout;
    layout.size = 34.0f;
    layout.text_size = 23.0f;
    layout.cy = card.y + card.h - 44.0f;
    ui::draw_hints(list, ink_.fonts, hint_glyphs(), hints, 2, card.x + card.w - 48.0f, true, layout);
}

} // namespace screens
