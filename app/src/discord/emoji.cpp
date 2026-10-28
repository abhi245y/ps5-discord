// ps5-discord - Unicode emoji lookup and detection.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "discord/emoji.hpp"

#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace discord
{

namespace
{

// The text without variation selector 16: the key both spellings share.
std::string bare(std::string_view s)
{
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i)
        if (s.compare(i, 3, "\xEF\xB8\x8F") == 0)
            i += 2;
        else
            out += s[i];
    return out;
}

const std::unordered_map<std::string, int> &table()
{
    static const std::unordered_map<std::string, int> map = [] {
        std::unordered_map<std::string, int> m;
        for (int i = 0; i < kEmojiCount; ++i)
            m.emplace(bare(kEmojiTable[i].text), i);
        return m;
    }();
    return map;
}

// One code point decoded from text[at]; its byte length in len.
std::uint32_t decode(std::string_view s, std::size_t at, std::size_t &len)
{
    const unsigned char c = static_cast<unsigned char>(s[at]);
    const int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    len = static_cast<std::size_t>(n);
    if (at + len > s.size())
        return len = 1, c;
    std::uint32_t cp = n == 1 ? c : c & (0x3F >> (n - 1));
    for (int k = 1; k < n; ++k)
        cp = (cp << 6) | (static_cast<unsigned char>(s[at + k]) & 0x3F);
    return cp;
}

bool skin(std::uint32_t cp)
{
    return cp >= 0x1F3FB && cp <= 0x1F3FF;
}

bool regional(std::uint32_t cp)
{
    return cp >= 0x1F1E6 && cp <= 0x1F1FF;
}

} // namespace

int emoji_index(std::string_view text)
{
    const auto it = table().find(bare(text));
    return it == table().end() ? -1 : it->second;
}

std::size_t emoji_length(std::string_view s, std::size_t at)
{
    if (at >= s.size())
        return 0;
    std::size_t len = 0;
    const std::uint32_t first = decode(s, at, len);
    std::size_t end = at + len;
    if (regional(first)) // a flag: two letters
    {
        std::size_t n2 = 0;
        return end < s.size() && regional(decode(s, end, n2)) ? len + n2 : 0;
    }
    bool selector = false, joined = false;
    for (;;) // the sequence: selectors, a skin tone, a keycap, tags, joined parts
    {
        if (end >= s.size())
            break;
        std::size_t n = 0;
        const std::uint32_t cp = decode(s, end, n);
        if (cp == 0xFE0F || cp == 0x20E3 || skin(cp) || (cp >= 0xE0020 && cp <= 0xE007F))
        {
            selector = selector || cp == 0xFE0F || cp == 0x20E3;
            end += n;
        }
        else if (cp == 0x200D && end + n < s.size())
        {
            std::size_t n2 = 0;
            decode(s, end + n, n2);
            end += n + n2;
            joined = true;
        }
        else
            break;
    }
    // Digits, # and *, and the old text symbols (©, ®) only as emoji with their selector.
    if (first < 0x2000 && !selector)
        return 0;
    const std::string_view seq = s.substr(at, end - at);
    if (emoji_index(seq) >= 0)
        return seq.size();
    // A skin tone or a joined sequence the table lacks: the first part decides.
    if (emoji_index(s.substr(at, len)) >= 0 && (joined || seq.size() > len))
        return seq.size();
    return first >= 0x1F000 && first <= 0x1FAFF ? seq.size() : 0;
}

std::string twemoji_url(std::string_view text)
{
    // Twemoji names files by code points in hex; FE0F is left out unless a ZWJ joins the sequence.
    std::vector<std::uint32_t> points;
    for (std::size_t i = 0, n = 0; i < text.size(); i += n)
        points.push_back(decode(text, i, n));
    bool zwj = false;
    for (const std::uint32_t cp : points)
        zwj = zwj || cp == 0x200D;
    std::string name;
    for (const std::uint32_t cp : points)
    {
        if (cp == 0xFE0F && !zwj)
            continue;
        char hex[12];
        std::snprintf(hex, sizeof hex, "%s%x", name.empty() ? "" : "-", cp);
        name += hex;
    }
    return name.empty() ? std::string() : "https://cdn.jsdelivr.net/gh/jdecked/twemoji@15.1.0/assets/72x72/" + name + ".png";
}

} // namespace discord
