#include "discord/gif.hpp"
#include <cassert>
#include <iostream>

// A GIF of n frames, each a 1x1 image with no colour table; gct adds a 2-entry global table.
static std::string gif(int n, bool gct)
{
    std::string d = "GIF89a";
    d += std::string("\x01\x00\x01\x00", 4) + static_cast<char>(gct ? 0x80 : 0) + std::string("\x00\x00", 2);
    if (gct)
        d += std::string(6, '\0');
    for (int i = 0; i < n; ++i)
    {
        d += std::string("\x21\xf9\x04\x00\x0a\x00\x00\x00", 8); // graphic control: 4 bytes, then the end
        d += std::string("\x2c\x00\x00\x00\x00\x01\x00\x01\x00\x00", 10);
        d += std::string("\x02\x02\x44\x01\x00", 5); // LZW size, one 2-byte block, the end
    }
    return d + "\x3b";
}

int main()
{
    assert(discord::gif_frames(gif(1, false)) == 1);
    assert(discord::gif_frames(gif(3, true)) == 3);
    assert(discord::gif_frames(gif(2000, false)) == 2000);
    assert(discord::gif_frames("") == -1);
    assert(discord::gif_frames("\x89PNG\r\n\x1a\n.....") == -1);
    std::string bad = gif(2, false);
    bad[bad.size() - 1] = 0x7f; // not a block type
    assert(discord::gif_frames(bad) == -1);
    std::cout << "gif frames: counts, colour tables, non-GIF and bad blocks passed\n";
}
