#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace discord
{

// Frames in a GIF, from its block structure (no decoding); -1 when it is not a sound GIF.
// stb decodes every frame at once, a full canvas each, so the count must be known first.
inline int gif_frames(const std::string &d)
{
    const auto at = [&](std::size_t i) { return static_cast<std::uint8_t>(d[i]); };
    // Data sub-blocks: a length byte and that many bytes, until a zero length.
    const auto skip_blocks = [&](std::size_t &i) {
        while (i < d.size() && at(i) != 0)
            i += at(i) + 1u;
        ++i;
    };
    if (d.size() < 13 || d.compare(0, 3, "GIF") != 0)
        return -1;
    std::size_t i = 13;
    if (at(10) & 0x80) // global colour table
        i += 3u << ((at(10) & 7) + 1);
    int frames = 0;
    while (i < d.size())
    {
        const std::uint8_t b = at(i++);
        if (b == 0x3b) // trailer
            return frames;
        if (b == 0x21) // extension: label, then blocks
        {
            ++i;
            skip_blocks(i);
        }
        else if (b == 0x2c) // image: descriptor, local colour table, LZW size, blocks
        {
            if (i + 9 > d.size())
                return -1;
            const std::uint8_t flags = at(i + 8);
            i += 9;
            if (flags & 0x80)
                i += 3u << ((flags & 7) + 1);
            ++i;
            skip_blocks(i);
            ++frames;
        }
        else
            return -1;
    }
    return frames; // cut short: stb reads what is there
}

} // namespace discord
