// ps5-discord - Keys from a keyboard plugged into the console.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "platform/ps5/usb_keyboard.hpp"

namespace hui::ps5
{

std::string key_text(std::uint16_t usage, bool shift, bool caps)
{
    if (usage >= 4 && usage <= 29) // a..z
        return std::string(1, static_cast<char>((shift != caps ? 'A' : 'a') + (usage - 4)));
    if (usage >= 30 && usage <= 39) // 1..9, 0
    {
        static const char plain[] = "1234567890";
        static const char shifted[] = "!@#$%^&*()";
        return std::string(1, (shift ? shifted : plain)[usage - 30]);
    }
    if (usage == 44)
        return " ";
    if (usage >= 45 && usage <= 56 && usage != 50) // 50 is the non-US # key
    {
        static const char plain[] = "-=[]\\#;'`,./";
        static const char shifted[] = "_+{}|~:\"~<>?";
        return std::string(1, (shift ? shifted : plain)[usage - 45]);
    }
    if (usage >= 89 && usage <= 98) // keypad 1..9, 0
        return std::string(1, usage == 98 ? '0' : static_cast<char>('1' + (usage - 89)));
    return {};
}

void apply_key(InputFrame &input, const Key &key)
{
    const bool shift = (key.mods & (0x02 | 0x20)) != 0;
    const bool ctrl = (key.mods & (0x01 | 0x10)) != 0;
    switch (key.usage)
    {
    case 40: // Enter
    case 88: // keypad Enter
        input.typed += shift ? '\r' : '\n';
        input.pressed |= action_bit(Action::confirm);
        break;
    case 41: // Escape
        input.typed += '\x1b';
        input.pressed |= action_bit(Action::back);
        break;
    case 42: // Backspace
        input.typed += '\b';
        break;
    case 43: // Tab: the next channel, Shift+Tab the previous
        input.pressed |= action_bit(shift ? Action::page_prev : Action::page_next);
        break;
    case 79:
        input.nav = Direction::right;
        break;
    case 80:
        input.nav = Direction::left;
        break;
    case 81:
        input.nav = Direction::down;
        break;
    case 82:
        input.nav = Direction::up;
        break;
    default:
        if (!ctrl)
            input.typed += key_text(key.usage, shift, (key.led & 2) != 0);
    }
    input.held |= input.pressed;
    input.connected = true;
}

} // namespace hui::ps5
