// ps5-discord - The system keyboard (IME dialog) for typing a message.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace hui::ps5
{

class Ime
{
  public:
    enum class State
    {
        idle,
        open,
        done,      // text() holds what was typed
        cancelled,
    };
    // Shows the keyboard over the app; false if the system refused.
    bool open(int user_id, const std::string &title, const std::string &initial);
    // Advances the dialog; call once a frame while open.
    State poll();
    const std::string &text() const
    {
        return text_;
    }

  private:
    State state_ = State::idle;
    std::string text_;
    char16_t buffer_[2048 + 1] = {};
    char16_t title_[128] = {};
};

} // namespace hui::ps5
