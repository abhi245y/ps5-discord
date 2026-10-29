// ps5-discord - Settings (framework frame 11).
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Left: the sections as cards. Right: the chosen section's rows, each a title, a line
// of description and a control (a value, a choice, a toggle, a slider or a button).
// Only controls are cards and focusable; facts (who is signed in, the version) are
// plain text. D-pad up/down moves, right (or Cross) enters the rows, left or Circle
// leaves them; on a slider or a choice left/right change it. Circle on the sections
// leaves the screen.

#include "screens/settings.hpp"

#include <algorithm>
#include <ctime>

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
    sign_out,
    quit,
    pref, // a console-side setting (Row::key, discord::Prefs)
    ui_volume,
    sfx_volume,
    motion,
    swap,
};

enum class Kind
{
    info,   // text only
    value,  // a value at the right, with a chevron when it opens something
    choice, // one of a few values, left/right picks
    toggle,
    slider, // 0..10, shown as a percentage
    button,
};

struct Row
{
    Kind kind = Kind::info;
    std::string title, desc, value;
    int level = 0;
    bool on = false;
    bool soon = false; // not in this build: drawn faint, not focusable
    Act act = Act::none;
    int percent = 10;   // a slider's step in percent
    bool meter = false; // the mic level in place of the description, while in a call
    const char *key = nullptr;             // Act::pref: the setting
    const char *const *options = nullptr;  // a choice's values, level indexes them
    int count = 0;
};

// Prefs::golive_quality. Remote Play streams 1080p at most; Discord wants H.264.
constexpr const char *kQualities[] = {"720p · 30 fps", "720p · 60 fps", "1080p · 30 fps", "1080p · 60 fps"};
// Prefs::notify_server and quiet_hours (the core's notify.c has the same hours).
constexpr const char *kServerMessages[] = {"Off", "As in Discord", "Every message"};
constexpr const char *kQuiet[] = {"Off", "22:00 – 07:00", "23:00 – 08:00", "00:00 – 08:00", "21:00 – 06:00"};

template <int N>
Row choice_row(const char *title, const char *desc, const char *key, const char *const (&options)[N], int level)
{
    Row r{Kind::choice, title, desc, options[std::clamp(level, 0, N - 1)], std::clamp(level, 0, N - 1)};
    r.act = Act::pref;
    r.key = key;
    r.options = options;
    r.count = N;
    return r;
}

Row toggle_row(const char *title, const char *desc, const char *key, bool on)
{
    Row r{Kind::toggle, title, desc, "", 0, on};
    r.act = Act::pref;
    r.key = key;
    return r;
}

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

std::string day_text(std::int64_t unix_s, int utc_offset_minutes);

std::vector<Row> rows_for(int section, const discord::Snapshot &s, const hui::Settings &app, const std::string &version)
{
    const discord::Voice &v = s.voice;
    switch (section)
    {
    case 0:
        return {
            {Kind::value, "Signed in as", "Your Discord account on this console.", s.me.name},
            {Kind::button, "Sign out",
             s.sign_out_error.empty() ? "Ends this session with Discord and forgets it here." : s.sign_out_error,
             "Sign out", 0, false, false, Act::sign_out},
            {Kind::button, "Quit Discord", "Stops calls and alerts. Closing with the PS button keeps them on.", "Quit",
             0, false, false, Act::quit},
        };
    case 1: {
        Row pair;
        pair.title = "Go Live pairing";
        pair.act = Act::pair;
        pair.kind = Kind::value;
        const discord::RemotePlayUser *used = nullptr;
        for (const discord::RemotePlayUser &u : v.rp_users)
            if (u.id == v.rp_use)
                used = &u;
        if (used)
        {
            pair.value = "Go Live as " + used->name;
            pair.desc = "Paired with " + (used->console.empty() ? std::string("this console") : used->console) +
                        (used->since > 0 ? " since " + day_text(used->since, s.utc_offset_minutes) : std::string()) +
                        ". Select to manage.";
        }
        else
        {
            pair.value = "Not paired";
            pair.desc = "Pair a console user once so Go Live can show your game.";
        }
        Row mic{Kind::slider, "Input volume", "How loud you are in calls. Join one to see your level here.", "",
                s.prefs.mic_gain, false, false, Act::pref};
        mic.key = "mic_gain";
        mic.percent = 20;
        mic.meter = v.active();
        return {
            mic,
            pair,
            choice_row("Go Live quality", "From the next Go Live on. Above 720p · 30 fps may need Nitro.",
                       "golive_quality", kQualities, s.prefs.golive_quality),
        };
    }
    case 2:
        return {
            toggle_row("Direct messages and mentions", "DMs and @mentions. Chats muted in Discord stay quiet.", "notify_dm", s.prefs.notify_dm),
            choice_row("Server messages", "As in Discord follows each server's notification setting.",
                       "notify_server", kServerMessages, s.prefs.notify_server),
            toggle_row("PS5 alerts while playing", "When a game covers the app, the PS5 shows who wrote.",
                       "notify_system", s.prefs.notify_system),
            toggle_row("Alerts during Go Live", "Off keeps your messages off your stream.", "notify_live",
                       s.prefs.notify_live),
            choice_row("Quiet hours", "No sounds and no PS5 alerts; banners in the app still show.", "quiet_hours",
                       kQuiet, s.prefs.quiet_hours),
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
            {Kind::info, "Your sign-in", "Kept on this console, encrypted: a copy of the file works nowhere else.", ""},
            {Kind::info, "Network", "The core answers only this app, on this console; nothing on your network can reach it.",
             ""},
        };
    default:
        return {
            {Kind::value, "Version", "The app and its core.", version.empty() ? "development" : version},
            {Kind::value, "License", "Free software: you may share and change it.", "AGPL-3.0"},
            {Kind::value, "Source", "Where the code lives.", "github.com/abhi245y/ps5-discord"},
            {Kind::value, "Emoji", "Twemoji graphics by Twitter and contributors.", "CC-BY 4.0"},
        };
    }
}

// "Oct 28, 2026" for a unix time in the console's time zone.
std::string day_text(std::int64_t unix_s, int utc_offset_minutes)
{
    static const char *kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const std::time_t t = static_cast<std::time_t>(unix_s + static_cast<std::int64_t>(utc_offset_minutes) * 60);
    std::tm tm{};
    gmtime_r(&t, &tm);
    return std::string(kMonths[tm.tm_mon % 12]) + " " + std::to_string(tm.tm_mday) + ", " +
           std::to_string(tm.tm_year + 1900);
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

void slider(DrawList &list, const Ink &ink, float right, float cy, int level, int percent)
{
    const float w = 320.0f, x = right - 110.0f - w, t = static_cast<float>(level) / 10.0f;
    list.rounded_rect({x, cy - 4.0f, w, 8.0f}, 4.0f, kRaisedHigh);
    list.rounded_rect({x, cy - 4.0f, std::max(8.0f, w * t), 8.0f}, 4.0f, kCoral);
    list.circle(x + w * t, cy, 13.0f, Color::rgb(0xffffff));
    list.circle(x + w * t, cy, 7.0f, kCoral);
    ui::text(list, ink.fonts.regular, std::to_string(level * percent) + "%", right, cy + 9.0f, 25.0f, kText,
             gfx::Align::right);
}

} // namespace

SettingsScreen::SettingsScreen(const Ink &ink) : ink_(ink), version_(hui::read_content_version("/app0/sce_sys/param.json")) {}

void SettingsScreen::open()
{
    section_ = 0;
    row_ = 0;
    in_rows_ = false;
    confirm_ = Confirm::none;
    pair_ = Pair::none;
}

void SettingsScreen::start_pin()
{
    pair_ = Pair::pin;
    pin_sent_ = false;
    std::fill(std::begin(pin_), std::end(pin_), -1);
    pin_at_ = 0;
}

namespace
{

enum class Do
{
    use,
    pair,
    remove,
};

struct Option
{
    const char *label;
    Do act;
};

// What can be done for one console user.
std::vector<Option> options_for(const discord::Voice &v, const std::string &id)
{
    for (const discord::RemotePlayUser &u : v.rp_users)
        if (u.id == id)
        {
            if (!u.paired)
                return {{"Pair", Do::pair}};
            std::vector<Option> o;
            if (v.rp_use != u.id)
                o.push_back({"Use for Go Live", Do::use});
            if (u.on_screen) // the console pairs only the user signed in on screen
                o.push_back({"Pair again", Do::pair});
            o.push_back({"Remove pairing", Do::remove});
            return o;
        }
    return {};
}

} // namespace

// The pairing dialog. After a request the snapshot shows the old state until the next
// poll, so the first second and a half counts as "pairing".
void SettingsScreen::update_pair(const discord::Snapshot &s, const hui::InputFrame &input,
                                 hui::ui::Feedback &feedback, std::vector<discord::Request> &requests)
{
    const discord::Voice &v = s.voice;
    auto send = [&](discord::Request::Kind kind, const std::string &pin = {}) {
        discord::Request r;
        r.kind = kind;
        r.user_id = target_;
        r.content = pin;
        requests.push_back(r);
        sent_time_ = 0.0f;
    };
    const bool fresh = sent_time_ < 1.5f;
    const std::string &st = v.pair_state;
    const bool done = !fresh && st == "paired";
    if (pair_ == Pair::users)
    {
        const int n = static_cast<int>(v.rp_users.size());
        if (input.is_pressed(Action::back))
        {
            pair_ = Pair::none;
            feedback.play(hui::audio::Cue::modal_close);
        }
        else if ((input.nav == Direction::up || input.nav == Direction::down) && n > 0)
        {
            user_at_ = (user_at_ + (input.nav == Direction::down ? 1 : n - 1)) % n;
            feedback.play(hui::audio::Cue::focus);
        }
        else if (input.is_pressed(Action::confirm) && user_at_ < n)
        {
            const discord::RemotePlayUser &u = v.rp_users[user_at_];
            target_ = u.id;
            if (!u.account || (!u.paired && !u.on_screen))
                feedback.play(hui::audio::Cue::error); // needs a PSN account, and to be the user on screen
            else if (!u.paired)
            {
                pair_ = Pair::wait; // the core gets the PIN itself; the boxes only if it cannot
                send(discord::Request::Kind::remoteplay_pair);
                feedback.play(hui::audio::Cue::select);
            }
            else
            {
                pair_ = Pair::menu;
                menu_at_ = 0;
                feedback.play(hui::audio::Cue::select);
            }
        }
        return;
    }
    if (input.is_pressed(Action::back) || (done && input.is_pressed(Action::confirm)))
    {
        pair_ = Pair::users;
        feedback.play(hui::audio::Cue::back);
        return;
    }
    if (pair_ == Pair::menu)
    {
        const std::vector<Option> o = options_for(v, target_);
        const int n = static_cast<int>(o.size());
        if ((input.nav == Direction::up || input.nav == Direction::down) && n > 0)
        {
            menu_at_ = (menu_at_ + (input.nav == Direction::down ? 1 : n - 1)) % n;
            feedback.play(hui::audio::Cue::focus);
        }
        if (input.is_pressed(Action::confirm) && menu_at_ < n)
        {
            switch (o[menu_at_].act)
            {
            case Do::use:
                send(discord::Request::Kind::remoteplay_use);
                pair_ = Pair::users;
                feedback.play(hui::audio::Cue::toggle);
                break;
            case Do::pair:
                send(discord::Request::Kind::remoteplay_pair);
                pair_ = Pair::wait;
                feedback.play(hui::audio::Cue::select);
                break;
            case Do::remove:
                send(discord::Request::Kind::remoteplay_unpair);
                pair_ = Pair::users;
                feedback.play(hui::audio::Cue::favorite_off);
                break;
            }
        }
        return;
    }
    if (pair_ == Pair::wait)
    {
        if (!fresh && st == "need_pin")
            start_pin(); // the console wants its own PIN typed
        else if (!fresh && st == "error" && input.is_pressed(Action::confirm))
            send(discord::Request::Kind::remoteplay_pair);
        else if (!fresh && st == "error" && input.is_pressed(Action::west))
            start_pin();
        return;
    }
    // The PIN boxes.
    if (pin_sent_ && (fresh || st == "pairing"))
        return; // waiting for the console
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
            send(discord::Request::Kind::remoteplay_pair, pin);
            pin_sent_ = true;
            feedback.play(hui::audio::Cue::select);
        }
        else
            feedback.play(hui::audio::Cue::error);
    }
}

bool SettingsScreen::update(const discord::Snapshot &s, const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback,
                      std::vector<discord::Request> &requests, hui::Settings &app, bool &changed)
{
    sent_time_ += dt;
    if (pair_ != Pair::none)
    {
        update_pair(s, input, feedback, requests);
        return true;
    }
    auto set_pref = [&](const char *key, int value) {
        discord::Request r;
        r.kind = discord::Request::Kind::set_pref;
        r.content = key;
        r.value = value;
        requests.push_back(r);
    };
    if (confirm_ != Confirm::none)
    {
        if (input.is_pressed(Action::confirm))
        {
            discord::Request r;
            r.kind = confirm_ == Confirm::quit ? discord::Request::Kind::quit : discord::Request::Kind::sign_out;
            requests.push_back(r);
            feedback.play(hui::audio::Cue::select);
            open();
        }
        else if (input.is_pressed(Action::back))
        {
            confirm_ = Confirm::none;
            feedback.play(hui::audio::Cue::modal_close);
        }
        return true;
    }
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
    const bool is_slider = row.kind == Kind::slider || row.kind == Kind::choice;
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
        const int top = row.kind == Kind::choice ? row.count - 1 : 10;
        const int next = std::clamp(row.level + (input.nav == Direction::right ? 1 : -1), 0, top);
        if (next == row.level)
            feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.5f);
        else
        {
            if (row.act == Act::ui_volume || row.act == Act::sfx_volume)
            {
                (row.act == Act::ui_volume ? app.ui_volume : app.sfx_volume) = next;
                changed = true;
            }
            else
                set_pref(row.key, next);
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
        case Act::pref:
            if (row.kind == Kind::toggle)
            {
                set_pref(row.key, row.on ? 0 : 1);
                feedback.play(hui::audio::Cue::toggle);
            }
            else if (row.kind == Kind::choice) // Cross steps through them too
            {
                set_pref(row.key, (row.level + 1) % row.count);
                feedback.play(hui::audio::Cue::slider);
            }
            break;
        case Act::sign_out:
        case Act::quit:
            confirm_ = row.act == Act::quit ? Confirm::quit : Confirm::sign_out;
            feedback.play(hui::audio::Cue::modal_open);
            break;
        case Act::pair:
            feedback.play(hui::audio::Cue::modal_open);
            pair_ = Pair::users;
            user_at_ = 0; // start on the Go Live user, else the one on screen
            for (int i = 0; i < static_cast<int>(s.voice.rp_users.size()); ++i)
                if (s.voice.rp_users[i].id == s.voice.rp_use ||
                    (s.voice.rp_use.empty() && s.voice.rp_users[i].on_screen))
                    user_at_ = i;
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
        // Controls are cards; facts are text on the panel, with a rule under them.
        if (focusable(row) || row.soon)
            list.rounded_rect(r, kCardRadius, kRaised);
        else
            list.rounded_rect({r.x + 28.0f, r.y + r.h + kRowGap * 0.5f - 1.0f, r.w - 56.0f, 1.0f}, 0.0f, kLine);
        ui::text(list, ink_.fonts.semibold, row.title, r.x + 28.0f, r.y + 42.0f, 28.0f, kText);
        const std::string desc = row.soon ? "Coming in a later build." : row.desc;
        // The description gets the row's width, less the control at the right.
        const float control = row.kind == Kind::info     ? 0.0f
                              : row.kind == Kind::button ? 260.0f
                              : row.kind == Kind::value || row.kind == Kind::choice ? r.w * 0.36f
                                                                                     : 480.0f;
        if (row.meter)
            mic_meter(list, r.x + 28.0f, r.y + 67.0f, 280.0f, // as the call hears it: after the volume
                      std::min(1.0f, s.voice.mic_level * static_cast<float>(row.level) / 5.0f), s.voice.muted);
        else
            ui::text(list, ink_.fonts.regular, fit(ink_.fonts.regular, desc, 22.0f, r.w - 56.0f - control),
                     r.x + 28.0f, r.y + 74.0f, 22.0f, kDim);
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
        case Kind::choice: { // ‹ value ›, each arrow faint at its end
            const float w = 300.0f;
            ui::text(list, ink_.fonts.regular, row.value, right - w * 0.5f, cy + 9.0f, 26.0f, kText,
                     gfx::Align::center);
            chevron(list, right - 6.0f, cy, row.level < row.count - 1 ? kDim : kLine);
            list.line(right - w + 6.0f, cy - 10.0f, right - w - 4.0f, cy, 3.0f, row.level > 0 ? kDim : kLine);
            list.line(right - w - 4.0f, cy, right - w + 6.0f, cy + 10.0f, 3.0f, row.level > 0 ? kDim : kLine);
            break;
        }
        case Kind::toggle:
            toggle(list, right, cy, row.on);
            break;
        case Kind::slider:
            slider(list, ink_, right, cy, row.level, row.percent);
            break;
        case Kind::button: {
            const float w = ink_.fonts.semibold.measure(row.value, 26.0f) + 72.0f;
            const Rect b{right - w, cy - 28.0f, w, 56.0f};
            list.rounded_rect(b, 28.0f, kCoral);
            ui::text(list, ink_.fonts.semibold, row.value, b.cx(), cy + 9.0f, 26.0f, kOnCoral, gfx::Align::center);
            break;
        }
        case Kind::info:
            break;
        }
        list.pop_opacity();
        if (in_rows_ && i == row_)
            focus_outline(list, r, kCardRadius, 1.0f);
    }

    const bool on_slider = in_rows_ && row_ < static_cast<int>(rows.size()) &&
                           (rows[row_].kind == Kind::slider || rows[row_].kind == Kind::choice);
    const ui::Hint hints[] = {{ui::Button::dpad, on_slider ? "Adjust" : "Move"},
                              {ui::Button::cross, "Select"},
                              {ui::Button::circle, "Back"}};
    ui::HintLayout layout;
    layout.size = 34.0f;
    layout.text_size = 23.0f;
    layout.cy = kHintsBaseline;
    layout.item_gap = 38.0f;
    ui::draw_hints(list, ink_.fonts, hint_glyphs(), hints, 3, 1920.0f - kMargin, true, layout);

    if (pair_ != Pair::none)
        draw_pair(s, list);
    if (confirm_ == Confirm::sign_out)
        confirm_card(list, ink_, "Sign out?",
                     "Discord ends this console's session and the app forgets it. A call or Go Live stops. To "
                     "come back, scan the QR code again.",
                     "Sign out");
    else if (confirm_ == Confirm::quit)
        confirm_card(list, ink_, "Quit Discord?", kQuitText, "Quit");
}

void SettingsScreen::draw_pair(const discord::Snapshot &s, DrawList &list) const
{
    list.rounded_rect({0, 0, 1920, 1080}, 0.0f, Color::rgb(0x0d0e12, 0.78f));
    const Rect card{410.0f, 150.0f, 1100.0f, 780.0f};
    list.shadow(card, 28.0f, 50.0f, Color::rgb(0, 0.5f));
    list.rounded_rect(card, 28.0f, kRaised);
    const discord::Voice &v = s.voice;
    const bool fresh = sent_time_ < 1.5f;
    const bool done = !fresh && v.pair_state == "paired";
    std::string who;
    for (const discord::RemotePlayUser &u : v.rp_users)
        if (u.id == target_)
            who = u.name;
    ui::text(list, ink_.fonts.display, pair_ == Pair::users || who.empty() ? "Go Live pairing" : "Go Live as " + who,
             card.x + 56.0f, card.y + 92.0f, 50.0f, kText);
    const float left = card.x + 58.0f, width = card.w - 116.0f;

    std::vector<ui::Hint> hints;
    if (pair_ == Pair::users)
    {
        ui::text(list, ink_.fonts.regular, "Each console user pairs once. Go Live runs as the one marked.", left,
                 card.y + 146.0f, 25.0f, kDim);
        const int n = static_cast<int>(v.rp_users.size());
        if (n == 0)
            ui::text(list, ink_.fonts.regular, "Reading the console's users…", left, card.y + 230.0f, 26.0f, kDim);
        for (int i = 0; i < n && i < 4; ++i)
        {
            const discord::RemotePlayUser &u = v.rp_users[i];
            const Rect r{card.x + 56.0f, card.y + 180.0f + i * 104.0f, card.w - 112.0f, 90.0f};
            list.push_opacity(u.account && (u.paired || u.on_screen) ? 1.0f : 0.45f);
            list.rounded_rect(r, kCardRadius, kRaisedHigh);
            ui::text(list, ink_.fonts.semibold, u.name + (u.on_screen ? "  · on screen" : ""), r.x + 28.0f,
                     r.y + 40.0f, 28.0f, kText);
            const std::string line = !u.account  ? "No PSN account: Remote Play cannot pair this user."
                                     : !u.paired && u.on_screen ? "Not paired. Select to pair."
                                     : !u.paired ? "Not paired. To pair, switch to this user on the PS5."
                                     : u.on_screen ? "Paired · controller on USB, with voice"
                                                   : "Paired · your controller stays yours, no voice in games";
            ui::text(list, ink_.fonts.regular, fit(ink_.fonts.regular, line, 22.0f, r.w - 300.0f), r.x + 28.0f,
                     r.y + 72.0f, 22.0f, kDim);
            if (u.id == v.rp_use)
            {
                const Rect tag{r.x + r.w - 220.0f, r.cy() - 22.0f, 196.0f, 44.0f};
                list.rounded_rect(tag, 22.0f, kCoral);
                ui::text(list, ink_.fonts.semibold, "Go Live", tag.cx(), tag.cy() + 8.0f, 22.0f, kOnCoral,
                         gfx::Align::center);
            }
            list.pop_opacity();
            if (i == user_at_)
                focus_outline(list, r, kCardRadius, 1.0f);
        }
        ui::paragraph(list, ink_.fonts.regular,
                      "Only the user on screen can pair: for another, switch to them on the PS5 and open the app "
                      "there. Turn Remote Play on first (PS5 Settings › System › Remote Play). Removing a pairing "
                      "here leaves the PS5's own list of linked devices as it is.",
                      left, card.y + 600.0f, 22.0f, width, 30.0f, kFaint, 3, gfx::Align::left);
        hints = {{ui::Button::cross, "Select"}, {ui::Button::circle, "Close"}};
    }
    else if (pair_ == Pair::menu)
    {
        const std::vector<Option> o = options_for(v, target_);
        for (int i = 0; i < static_cast<int>(o.size()); ++i)
        {
            const Rect b{card.x + 56.0f, card.y + 170.0f + i * 96.0f, card.w - 112.0f, 80.0f};
            list.rounded_rect(b, kCardRadius, kRaisedHigh);
            ui::text(list, ink_.fonts.semibold, o[i].label, b.x + 28.0f, b.cy() + 10.0f, 28.0f,
                     o[i].act == Do::remove ? kDnd : kText);
            if (i == menu_at_)
                focus_outline(list, b, kCardRadius, 1.0f);
        }
        hints = {{ui::Button::cross, "Select"}, {ui::Button::circle, "Back"}};
    }
    else if (pair_ == Pair::wait)
    {
        std::string status = "Pairing with the console…";
        Color tone = kDim;
        if (done)
            status = "Paired.", tone = kOnline;
        else if (!fresh && v.pair_state == "error")
            status = v.pair_error.empty() ? "Pairing failed." : v.pair_error, tone = kDnd;
        ui::paragraph(list, ink_.fonts.regular, status, left, card.y + 180.0f, 28.0f, width, 38.0f, tone, 4,
                      gfx::Align::left);
        ui::paragraph(list, ink_.fonts.regular,
                      "The app asks the PS5 for a pairing code itself; nothing to type.", left, card.y + 340.0f, 24.0f,
                      width, 32.0f, kFaint, 2, gfx::Align::left);
        if (done)
            hints = {{ui::Button::cross, "Done"}};
        else if (!fresh && v.pair_state == "error")
            hints = {{ui::Button::cross, "Try again"}, {ui::Button::square, "Enter a PIN"}, {ui::Button::circle, "Back"}};
        else
            hints = {{ui::Button::circle, "Back"}};
    }
    else
    {
        ui::text(list, ink_.fonts.regular, "On the PS5: Settings  ›  System  ›  Remote Play  ›  Link Device.", left,
                 card.y + 150.0f, 26.0f, kDim);
        ui::text(list, ink_.fonts.regular, "Enter the 8-digit number it shows. Up and down change a digit.", left,
                 card.y + 190.0f, 26.0f, kDim);
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
            if (done)
                status = "Paired.", tone = kOnline;
            else if (!fresh && v.pair_state == "error")
                status = v.pair_error.empty() ? "Pairing failed." : v.pair_error, tone = kDnd;
            else
                status = "Pairing with the console…";
        }
        if (!status.empty())
            ui::paragraph(list, ink_.fonts.regular, status, left, card.y + 420.0f, 26.0f, width, 34.0f, tone, 2,
                          gfx::Align::left);
        hints = {{ui::Button::cross, done ? "Done" : "Pair"}, {ui::Button::circle, "Back"}};
    }
    ui::HintLayout layout;
    layout.size = 34.0f;
    layout.text_size = 23.0f;
    layout.cy = card.y + card.h - 44.0f;
    ui::draw_hints(list, ink_.fonts, hint_glyphs(), hints.data(), static_cast<int>(hints.size()),
                   card.x + card.w - 48.0f, true, layout);
}

} // namespace screens
