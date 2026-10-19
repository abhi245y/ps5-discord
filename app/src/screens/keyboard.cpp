// ps5-discord - The on-screen keyboard.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rows of ten keys (an emoji row, digits, three letter rows) and a bottom row
// of wide keys. Cross types the focused key; Square erases, Triangle is a
// space, L1 shifts, R2 or Options sends, Circle closes and keeps the draft.

#include "screens/keyboard.hpp"

#include <algorithm>
#include <cctype>
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
using hui::Action;
using hui::Direction;

const char *const kRows[5][10] = {
    {"\xF0\x9F\x98\x80", "\xF0\x9F\x98\x82", "\xE2\x9D\xA4\xEF\xB8\x8F", "\xF0\x9F\x91\x8D", "\xF0\x9F\x94\xA5",
     "\xF0\x9F\x8E\xAE", "\xF0\x9F\x98\xAD", "\xF0\x9F\x99\x8F", "\xF0\x9F\x98\xAE", "\xF0\x9F\x8E\x89"},
    {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
    {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"},
    {"a", "s", "d", "f", "g", "h", "j", "k", "l", "'"},
    {"z", "x", "c", "v", "b", "n", "m", ",", ".", "?"},
};
constexpr int kKeyRows = 5;
// The bottom row: label and width in key units (ten in all).
struct Wide
{
    const char *label;
    float units;
};
const Wide kBottom[] = {{"Shift", 1.5f}, {"@", 1.0f}, {"!", 1.0f}, {"Space", 3.5f}, {"Erase", 1.5f}, {"Send", 1.5f}};
constexpr int kBottomCount = 6;
constexpr int kRowCount = kKeyRows + 1;

constexpr float kPanelY = 470.0f;
constexpr float kKeysY = 640.0f;
constexpr float kKeyH = 54.0f;
constexpr float kGap = 8.0f;
constexpr float kLeft = kMargin;
constexpr float kRowW = 1920.0f - 2 * kMargin;
constexpr float kUnit = (kRowW - 9 * kGap) / 10.0f;

Rect key_rect(int row, int col)
{
    const float y = kKeysY + row * (kKeyH + kGap);
    if (row < kKeyRows)
        return {kLeft + col * (kUnit + kGap), y, kUnit, kKeyH};
    float x = kLeft;
    for (int i = 0; i < col; ++i)
        x += kBottom[i].units * kUnit + (kBottom[i].units - 1.0f) * kGap + kGap;
    const float w = kBottom[col].units * kUnit + (kBottom[col].units - 1.0f) * kGap;
    return {x, y, w, kKeyH};
}

int columns(int row)
{
    return row < kKeyRows ? 10 : kBottomCount;
}

std::string shifted(const char *key, bool shift)
{
    std::string out = key;
    if (shift && out.size() == 1 && std::isalpha(static_cast<unsigned char>(out[0])))
        out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

} // namespace

void Keyboard::open(const std::string &title, const std::string &initial)
{
    title_ = title;
    if (!initial.empty())
        text_ = initial;
    row_ = 2;
    col_ = 0;
    shift_ = text_.empty(); // the first letter of a message starts capitalised
    blink_ = 0.0f;
}

bool Keyboard::full() const
{
    std::size_t letters = 0;
    for (const unsigned char c : text_)
        letters += (c & 0xc0) != 0x80;
    return letters >= 2000;
}

void Keyboard::erase()
{
    while (!text_.empty() && (static_cast<unsigned char>(text_.back()) & 0xc0) == 0x80)
        text_.pop_back(); // continuation bytes of the last letter
    if (!text_.empty())
        text_.pop_back();
    // An emoji with a variation selector leaves the base behind: take it too.
    if (text_.size() >= 3 && text_.compare(text_.size() - 3, 3, "\xEF\xB8\x8F") == 0)
        erase();
}

void Keyboard::press(int row, int col, hui::ui::Feedback &feedback)
{
    if (full() && !(row == kKeyRows && (col == 0 || col == 4)))
    {
        feedback.play(hui::audio::Cue::error); // Discord's limit; Shift and Erase still work
        return;
    }
    if (row < kKeyRows)
    {
        text_ += shifted(kRows[row][col], shift_);
        if (shift_ && row >= 2)
            shift_ = false; // one capital, as phone keyboards do
        feedback.play(hui::audio::Cue::type, 1.0f + 0.01f * static_cast<float>(col));
        return;
    }
    switch (col)
    {
    case 0:
        shift_ = !shift_;
        feedback.play(hui::audio::Cue::toggle);
        break;
    case 1:
    case 2:
        text_ += kBottom[col].label;
        feedback.play(hui::audio::Cue::type);
        break;
    case 3:
        text_ += ' ';
        feedback.play(hui::audio::Cue::type, 0.9f);
        break;
    case 4:
        erase();
        feedback.play(hui::audio::Cue::erase);
        break;
    default:
        break; // Send is handled by the caller (it ends the keyboard)
    }
}

Keyboard::Result Keyboard::update(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback)
{
    blink_ += dt;
    if (input.nav == Direction::left || input.nav == Direction::right)
    {
        const int step = input.nav == Direction::right ? 1 : -1;
        col_ = (col_ + step + columns(row_)) % columns(row_);
        feedback.play(hui::audio::Cue::tick);
    }
    else if (input.nav == Direction::up || input.nav == Direction::down)
    {
        const int next = row_ + (input.nav == Direction::down ? 1 : -1);
        if (next < 0 || next >= kRowCount)
            feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.5f);
        else
        {
            // Keep to the key under the same spot when rows differ in width.
            const float x = key_rect(row_, col_).cx();
            int best = 0;
            for (int c = 1; c < columns(next); ++c)
                if (std::fabs(key_rect(next, c).cx() - x) < std::fabs(key_rect(next, best).cx() - x))
                    best = c;
            row_ = next;
            col_ = best;
            feedback.play(hui::audio::Cue::tick);
        }
    }
    const bool send_key = input.is_pressed(Action::confirm) && row_ == kKeyRows && col_ == kBottomCount - 1;
    if (send_key || input.is_pressed(Action::jump_next) || input.is_pressed(Action::menu))
    {
        if (text_.find_first_not_of(" \n") == std::string::npos)
        {
            feedback.play(hui::audio::Cue::error);
            return Result::none;
        }
        return Result::send;
    }
    if (input.is_pressed(Action::confirm))
    {
        press(row_, col_, feedback);
        type_held_ = 0.0f;
    }
    else if (input.is_held(Action::confirm) && !(row_ == kKeyRows && col_ == 0))
    {
        // Held: the key repeats after a beat, as TV keyboards do (not Shift).
        const float before = type_held_;
        type_held_ += dt;
        if (type_held_ > 0.45f && std::floor(before / 0.05f) != std::floor(type_held_ / 0.05f))
            press(row_, col_, feedback);
    }
    if (input.is_pressed(Action::west))
    {
        erase();
        erase_held_ = 0.0f;
        feedback.play(hui::audio::Cue::erase);
    }
    else if (input.is_held(Action::west) && !text_.empty())
    {
        // Held: after a beat, erase a letter every few frames.
        const float before = erase_held_;
        erase_held_ += dt;
        if (erase_held_ > 0.45f && std::floor(before / 0.06f) != std::floor(erase_held_ / 0.06f))
        {
            erase();
            feedback.play(hui::audio::Cue::tick, 0.9f, 0.0f, 0.6f);
        }
    }
    if (input.is_pressed(Action::north) && full())
        feedback.play(hui::audio::Cue::error);
    else if (input.is_pressed(Action::north))
    {
        text_ += ' ';
        feedback.play(hui::audio::Cue::type, 0.9f);
    }
    if (input.is_pressed(Action::page_prev))
    {
        shift_ = !shift_;
        feedback.play(hui::audio::Cue::toggle);
    }
    if (input.is_pressed(Action::back))
    {
        feedback.play(hui::audio::Cue::modal_close);
        return Result::cancel;
    }
    if (input.is_pressed(Action::confirm) || input.is_pressed(Action::west) || input.is_pressed(Action::north))
        blink_ = 0.0f; // the caret stays lit while typing
    return Result::none;
}

void Keyboard::draw(DrawList &list) const
{
    list.rounded_rect({0, 0, 1920, 1080}, 0.0f, Color::rgb(0x0d0e12, 0.62f));
    const Rect panel{0, kPanelY, 1920, 1080 - kPanelY};
    list.shadow(panel, 0.0f, 40.0f, Color::rgb(0, 0.5f));
    list.rounded_rect(panel, 0.0f, kGround);

    // The text being written, last lines first if it runs long.
    ui::text(list, ink_.fonts.semibold, title_, kLeft, kPanelY + 52.0f, 26.0f, kDim);
    const Rect field{kLeft, kPanelY + 72.0f, kRowW, 84.0f};
    list.rounded_rect(field, 18.0f, kRaised);
    std::vector<std::string> lines = ink_.fonts.regular.font->wrap(text_.empty() ? std::string(" ") : text_, 27.0f,
                                                                   field.w - 60.0f);
    if (lines.size() > 2)
        lines.erase(lines.begin(), lines.end() - 2);
    float ly = field.y + (lines.size() > 1 ? 34.0f : 52.0f);
    float caret_x = field.x + 24.0f;
    for (const std::string &line : lines)
    {
        caret_x = field.x + 24.0f + ui::text(list, ink_.fonts.regular, line, field.x + 24.0f, ly, 27.0f, kText);
        ly += 34.0f;
    }
    if (text_.empty())
        ui::text(list, ink_.fonts.regular, "Type with Cross. Square erases, Triangle adds a space.", field.x + 24.0f,
                 field.y + 52.0f, 25.0f, kFaint);
    if (std::fmod(blink_, 1.0f) < 0.6f)
        list.rounded_rect({caret_x + 2.0f, ly - 34.0f - 26.0f, 3.0f, 32.0f}, 1.5f, kCoral);

    for (int row = 0; row < kRowCount; ++row)
        for (int col = 0; col < columns(row); ++col)
        {
            const Rect r = key_rect(row, col);
            const bool hot = row == row_ && col == col_;
            const bool send = row == kKeyRows && col == kBottomCount - 1;
            const bool shift_on = row == kKeyRows && col == 0 && shift_;
            list.rounded_rect(r, 12.0f, send ? kCoral : shift_on ? kRaisedHigh : kRaised);
            const std::string label = row < kKeyRows ? shifted(kRows[row][col], shift_) : kBottom[col].label;
            ui::text(list, row < kKeyRows ? ink_.fonts.regular : ink_.fonts.semibold, label, r.cx(), r.cy() + 10.0f,
                     row < kKeyRows ? 30.0f : 25.0f, send ? kOnCoral : kText, gfx::Align::center);
            if (hot)
                focus_outline(list, r, 12.0f, 1.0f);
        }

    const ui::Hint hints[] = {{ui::Button::cross, "Type"},
                              {ui::Button::square, "Erase"},
                              {ui::Button::triangle, "Space"},
                              {ui::Button::l1, "Shift"},
                              {ui::Button::r2, "Send"},
                              {ui::Button::circle, "Close"}};
    ui::HintLayout layout;
    layout.size = 30.0f;
    layout.text_size = 22.0f;
    layout.cy = kHintsBaseline;
    ui::draw_hints(list, ink_.fonts, ui::GlyphStyle::dark(), hints, 6, 1920.0f - kMargin, true, layout);
}

} // namespace screens
