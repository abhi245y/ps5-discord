// ps5-discord - The app's own on-screen keyboard, for typing a message with
// the controller (the system keyboard is not reachable from a native title).
// SPDX-License-Identifier: AGPL-3.0-or-later

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
        emoji,  // the Emoji key (or R1): the caller opens the picker, then insert()s
    };
    explicit Keyboard(const Ink &ink) : ink_(ink) {}

    // initial: replaces the draft (editing a message); empty keeps the draft.
    void open(const std::string &title, const std::string &initial = {});
    void insert(const std::string &text); // at the end, where the caret is
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
    void set_text(std::string text)
    {
        text_ = std::move(text);
    }

  private:
    void press(int row, int col, hui::ui::Feedback &feedback);
    void erase();
    bool emoji_asked_ = false;

    const Ink &ink_;
    std::string title_;
    std::string text_;
    int row_ = 2;
    int col_ = 0;
    bool shift_ = false;
    float blink_ = 0.0f;
    float erase_held_ = 0.0f; // Square held: erasing repeats after a moment
    float type_held_ = 0.0f;  // Cross held on a key: it repeats the same way
    bool full() const;        // at Discord's 2000-character limit
};

} // namespace screens
