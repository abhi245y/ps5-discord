// ps5-discord - Unicode emoji: the baked picker set (assets/art/emoji.png, Twemoji) and
// finding emoji in text, so they draw in colour as Discord draws them.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace discord
{

struct EmojiEntry
{
    int group; // into kEmojiGroups
    const char *text;
    const char *name; // Unicode's (CLDR) name: "grinning face"
};

#include "discord/emoji_table.inc"

constexpr int kEmojiCount = static_cast<int>(sizeof kEmojiTable / sizeof *kEmojiTable);
constexpr int kEmojiGroupCount = static_cast<int>(sizeof kEmojiGroups / sizeof *kEmojiGroups);

// The emoji's cell in the atlas (kEmojiTable order), -1 when it is not baked (a skin tone,
// a newer one): those come from twemoji_url. Variation selectors do not matter.
int emoji_index(std::string_view text);
// Bytes of the emoji sequence that starts at text[at] (with its skin tone, joined parts,
// keycap or flag), 0 when none starts there. Plain digits, # and * are not emoji.
std::size_t emoji_length(std::string_view text, std::size_t at);
// Twemoji's picture of a Unicode emoji, as Discord draws it (through the core's proxy).
std::string twemoji_url(std::string_view text);

} // namespace discord
