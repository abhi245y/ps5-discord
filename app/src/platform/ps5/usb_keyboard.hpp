// ps5-discord - Keys from a keyboard plugged into the console. The core reads
// the keyboard (a title cannot reach libSceKeyboard) and sends each key as a
// "KEY" event; these turn one into what the screens read.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>

#include "core/input.hpp"

namespace hui::ps5
{

// A key as the console's keyboard reports it: a USB HID usage code, the
// modifier bits (1/16 Ctrl, 2/32 Shift) and the lock lights (2 Caps Lock).
struct Key
{
    std::uint16_t usage = 0;
    std::uint32_t mods = 0;
    std::uint32_t led = 0;
};

// Adds one key to input: text in typed ('\b' erase, '\n' Enter, '\r'
// Shift+Enter, '\x1b' Escape), arrows as navigation, Enter as Cross, Escape as
// Circle, Tab and Shift+Tab as R1 and L1.
void apply_key(InputFrame &input, const Key &key);

// The text one key gives (US layout), "" for none.
std::string key_text(std::uint16_t usage, bool shift, bool caps);

} // namespace hui::ps5
