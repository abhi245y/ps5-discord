// ps5-discord - Before the account is ready: sign in with the phone (framework
// frame 01) and the waiting / reconnecting card (frame 12).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "screens/signin.hpp"

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

void spinner(DrawList &list, float cx, float cy, float r, float t)
{
    list.ring(cx, cy, r, 7.0f, kRaisedHigh);
    list.arc(cx, cy, r, 7.0f, t * 4.2f, 1.6f, kCoral);
}

} // namespace

void draw_sign_in(DrawList &list, const Ink &ink, const discord::Snapshot &s, float time)
{
    list.rounded_rect({0, 0, 1920, 1080}, 0.0f, kGround);
    const float left = 104.0f;
    ui::text(list, ink.fonts.display, "Sign in with your phone", left, 250.0f, 66.0f, kText);
    const char *steps[] = {"Open Discord on your phone", "Settings  ›  Scan QR Code", "Approve the login"};
    for (int i = 0; i < 3; ++i)
    {
        const float cy = 370.0f + i * 128.0f;
        list.circle(left + 52.0f, cy, 52.0f, kRaised);
        ui::text(list, ink.fonts.semibold, std::to_string(i + 1), left + 52.0f, cy + 16.0f, 44.0f, kDim,
                 gfx::Align::center);
        ui::text(list, ink.fonts.semibold, steps[i], left + 150.0f, cy + 15.0f, 42.0f, kText);
    }
    const Rect status{left, 742.0f, 940.0f, 108.0f};
    list.rounded_rect(status, 22.0f, kRaised);
    spinner(list, status.x + 66.0f, status.cy(), 26.0f, time);
    ui::text(list, ink.fonts.regular, s.login_scanned ? "Scanned. Approve the login on your phone." : "Waiting for scan…",
             status.x + 120.0f, status.cy() + 12.0f, 33.0f, kDim);
    ui::text(list, ink.fonts.regular, "Unofficial client  ·  your password never touches this console", left, 1000.0f,
             25.0f, kFaint);

    // The QR card: the core's code, drawn module by module.
    const Rect card{1144.0f, 128.0f, 700.0f, 700.0f};
    list.rounded_rect(card, 28.0f, Color::rgb(0xffffff));
    const float c = 54.0f, w = 6.0f; // coral corner marks
    for (int k = 0; k < 4; ++k)
    {
        const float x = (k & 1) ? card.x + card.w - 40.0f - c : card.x + 40.0f;
        const float y = (k & 2) ? card.y + card.h - 40.0f - c : card.y + 40.0f;
        const float hx = (k & 1) ? x + c - w : x;
        const float vy = (k & 2) ? y + c - w : y;
        list.rounded_rect({x, vy, c, w}, w * 0.5f, kCoral);
        list.rounded_rect({hx, y, w, c}, w * 0.5f, kCoral);
    }
    const int n = s.login_qr_size;
    if (n > 0 && static_cast<int>(s.login_qr.size()) >= n * n && !s.login_scanned)
    {
        const float area = card.w - 150.0f;
        const float m = std::floor(area / n);
        const float ox = card.x + (card.w - m * n) * 0.5f;
        const float oy = card.y + (card.h - m * n) * 0.5f;
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                if (s.login_qr[static_cast<std::size_t>(y * n + x)] == '1')
                    list.rounded_rect({ox + x * m, oy + y * m, m + 0.5f, m + 0.5f}, 0.0f, Color::rgb(0x111111));
    }
    else
    {
        spinner(list, card.cx(), card.cy(), 46.0f, time);
    }

    const ui::Hint hints[] = {{ui::Button::circle, "Back"}};
    ui::HintLayout layout;
    layout.size = 34.0f;
    layout.text_size = 23.0f;
    layout.cy = kHintsBaseline;
    ui::draw_hints(list, ink.fonts, hint_glyphs(), hints, 1, 1920.0f - kMargin, true, layout);
}

void draw_waiting(DrawList &list, const Ink &ink, const discord::Snapshot &s, float time)
{
    list.rounded_rect({0, 0, 1920, 1080}, 0.0f, kGround);
    const Rect card{560.0f, 330.0f, 800.0f, 400.0f};
    list.shadow(card, 28.0f, 50.0f, Color::rgb(0, 0.45f));
    list.rounded_rect(card, 28.0f, kRaised);
    spinner(list, card.cx(), card.y + 110.0f, 44.0f, time);
    const char *title = "Starting…";
    if (s.connection == discord::Connection::connecting)
        title = "Connecting…";
    else if (s.connection == discord::Connection::reconnecting)
        title = "Reconnecting…";
    ui::text(list, ink.fonts.display, title, card.cx(), card.y + 236.0f, 50.0f, kText, gfx::Align::center);
    const std::string line = !s.status_message.empty() ? s.status_message
                             : s.connection == discord::Connection::reconnecting
                                 ? std::string("Lost connection to Discord. Trying again.")
                                 : std::string("Opening your Discord session.");
    ui::paragraph(list, ink.fonts.regular, line, card.cx(), card.y + 296.0f, 27.0f, card.w - 100.0f, 38.0f, kDim, 3,
                  gfx::Align::center);
}

} // namespace screens
