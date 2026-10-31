// ps5-discord - The update dialog.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "screens/update.hpp"

#include <algorithm>

#include "screens/style.hpp"

namespace screens
{

namespace
{

namespace ui = hui::ui;
using hui::Action;
using hui::Direction;
using discord::Update;

constexpr std::size_t kNotesMax = 1500; // bytes drawn; the core keeps up to 16 KB

// text cut to at most kNotesMax bytes on a UTF-8 boundary.
std::string trimmed(const std::string &text)
{
    if (text.size() <= kNotesMax)
        return text;
    std::size_t n = kNotesMax;
    while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80)
        --n;
    return text.substr(0, n);
}

} // namespace

UpdateDialog::Card UpdateDialog::card(const discord::Snapshot &s, bool idle) const
{
    const Update &u = s.update;
    if (!idle)
        return Card::none;
    if (u.state == "failed")
        return u.error == failed_seen_ ? Card::none : Card::failed;
    if (u.state == "available")
    {
        if (started_ && u.error.empty())
            return Card::progress; // the core has not said "downloading" yet
        return later_ ? Card::none : notes_ ? Card::notes : Card::offer;
    }
    if (u.state == "downloading" || u.state == "unpacking")
        return open_ || started_ ? Card::progress : Card::none;
    if (u.state == "ready" || u.state == "applying")
        return ready_later_ ? Card::none : Card::ready;
    return Card::none;
}

bool UpdateDialog::update(const discord::Snapshot &s, const hui::InputFrame &input, ui::Feedback &feedback,
                          std::vector<discord::Request> &requests, bool idle)
{
    const Update &u = s.update;
    if (u.state != "failed")
        failed_seen_.clear(); // the same text on a later failure is news again
    if (u.state != "available" && u.state != "downloading" && u.state != "unpacking")
        started_ = false;
    if (u.state != "available")
        notes_ = false;
    idle_ = idle;
    const Card c = card(s, idle);
    if (c == Card::none)
        return false;
    auto request = [&](discord::Request::Kind kind) {
        discord::Request r;
        r.kind = kind;
        requests.push_back(r);
        feedback.play(hui::audio::Cue::select);
    };
    const bool yes = input.is_pressed(Action::confirm), no = input.is_pressed(Action::back);
    switch (c)
    {
    case Card::offer:
    case Card::notes:
        if (yes)
        {
            request(discord::Request::Kind::update_start);
            started_ = true;
        }
        else if (c == Card::offer && input.is_pressed(Action::west))
            notes_ = true;
        else if (no && c == Card::notes)
            notes_ = false;
        else if (no)
            later_ = true;
        break;
    case Card::progress: // Circle hides it; the core carries on
        if (no)
            started_ = open_ = false;
        break;
    case Card::ready:
        if (yes && u.state == "ready")
            request(discord::Request::Kind::update_apply); // the client closes the app on 200
        else if (no)
            ready_later_ = true;
        break;
    default: // failed
        if (yes || no)
            failed_seen_ = u.error;
        break;
    }
    return true;
}

void UpdateDialog::draw(const discord::Snapshot &s, DrawList &list) const
{
    const Update &u = s.update;
    const Card c = card(s, idle_);
    if (c == Card::none)
        return;
    const std::string version = u.version.empty() ? "the new version" : "Version " + u.version;
    std::string title, body;
    std::vector<ui::Hint> hints;
    float progress = -1.0f;
    int lines = 4;
    switch (c)
    {
    case Card::offer:
        title = version + " is available";
        body = u.error.empty() ? "You have " + (u.installed.empty() ? std::string("an older version") : u.installed) +
                                     ". The update downloads in the background; you restart to finish."
                               : u.error;
        hints = {{ui::Button::square, "What's new"}, {ui::Button::cross, "Update"}, {ui::Button::circle, "Later"}};
        break;
    case Card::notes:
        title = "What's new in " + (u.version.empty() ? std::string("this version") : u.version);
        body = u.notes.empty() ? "No release notes." : trimmed(u.notes);
        hints = {{ui::Button::cross, "Update"}, {ui::Button::circle, "Back"}};
        lines = 11;
        break;
    case Card::progress: {
        const bool unpacking = u.state == "unpacking";
        title = unpacking ? "Unpacking" : "Downloading";
        // Both report bytes: downloaded of the zip, then unpacked of the app's files.
        const bool known = u.total > 0 && !(started_ && u.state == "available");
        progress = known ? std::clamp(static_cast<float>(u.done) / static_cast<float>(u.total), 0.0f, 1.0f) : 0.0f;
        body = known ? std::to_string(u.done >> 20) + " of " + std::to_string(u.total >> 20) + " MB" +
                           (unpacking ? ". Then you restart to finish." : "")
                     : "Starting...";
        hints = {{ui::Button::circle, "Hide"}};
        break;
    }
    case Card::ready:
        title = "Restart to finish";
        body = u.state == "applying" ? "Restarting..."
                                     : version + " is ready. The app closes; open it again to use it.";
        if (u.state == "ready")
            hints = {{ui::Button::cross, "Restart now"}, {ui::Button::circle, "Later"}};
        break;
    default: // failed
        title = "The update failed";
        body = u.error.empty() ? "Something went wrong." : u.error;
        hints = {{ui::Button::cross, "OK"}};
        break;
    }
    const float h = c == Card::notes ? 720.0f : 400.0f;
    list.rounded_rect({0, 0, 1920, 1080}, 0.0f, hui::gfx::Color::rgb(0x0d0e12, 0.78f));
    const Rect card{c == Card::notes ? 460.0f : 560.0f, (1080.0f - h) / 2, c == Card::notes ? 1000.0f : 800.0f, h};
    list.shadow(card, 28.0f, 50.0f, hui::gfx::Color::rgb(0, 0.5f));
    list.rounded_rect(card, 28.0f, style::kRaised);
    ui::text(list, ink_.fonts.display, fit(ink_.fonts.display, title, 50.0f, card.w - 112.0f), card.x + 56.0f,
             card.y + 92.0f, 50.0f, style::kText);
    float y = card.y + 150.0f;
    if (progress >= 0.0f)
    {
        const Rect bar{card.x + 56.0f, y + 6.0f, card.w - 112.0f, 12.0f};
        list.rounded_rect(bar, 6.0f, style::kRaisedHigh);
        list.rounded_rect({bar.x, bar.y, std::max(12.0f, bar.w * progress), bar.h}, 6.0f, style::kCoral);
        y += 56.0f;
    }
    ui::paragraph(list, ink_.fonts.regular, body, card.x + 58.0f, y, 26.0f, card.w - 116.0f, 36.0f, style::kDim, lines);
    ui::HintLayout layout;
    layout.size = 34.0f;
    layout.text_size = 23.0f;
    layout.cy = card.y + card.h - 44.0f;
    ui::draw_hints(list, ink_.fonts, hint_glyphs(), hints.data(), static_cast<int>(hints.size()), card.x + card.w - 48.0f,
                   true, layout);
}

} // namespace screens
