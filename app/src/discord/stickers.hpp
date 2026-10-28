// ps5-discord - Discord stickers as pictures: Lottie ones (most of Discord's own, like
// Wumpus's wave) rendered with ThorVG, GIF ones decoded; both as a sheet of frames.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace discord
{

// Frames side by side, columns to a row, each w/columns by h/rows; played at fps.
struct FrameSheet
{
    int w = 0, h = 0;
    int frames = 1, columns = 1;
    float fps = 0.0f;
    std::vector<std::uint8_t> rgba;
};

// A Lottie animation (JSON) at size px square, up to a couple of seconds' worth of frames.
bool render_lottie(const std::string &json, int size, FrameSheet &out);
// A GIF, every frame (up to the same limit).
bool decode_gif(const std::string &data, FrameSheet &out);

} // namespace discord
