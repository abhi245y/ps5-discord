// ps5-discord - The app's own on-screen keyboard, for typing a message with
// the controller (the system keyboard is not reachable from a native title).
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

#include "core/input.hpp"
#include "screens/draw.hpp"
#include "ui/feedback.hpp"

namespace screens
{

class Keyboard
{
  public:
    enum class Result
    {
        none,
        send,   // text() is ready to post
        cancel, // closed; the text stays as a draft
    };
    explicit Keyboard(const Ink &ink) : ink_(ink) {}

    void open(const std::string &title);
    Result update(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void draw(DrawList &list) const;
    const std::string &text() const
    {
        return text_;
    }
    void clear()
    {
        text_.clear();
    }

  private:
    void press(int row, int col, hui::ui::Feedback &feedback);
    void erase();

    const Ink &ink_;
    std::string title_;
    std::string text_;
    int row_ = 2;
    int col_ = 0;
    bool shift_ = false;
    float blink_ = 0.0f;
    float erase_held_ = 0.0f; // Square held: erasing repeats after a moment
    bool full() const;        // at Discord's 2000-character limit
};

} // namespace screens
