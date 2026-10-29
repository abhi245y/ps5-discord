// ps5-discord - Sticker frames: ThorVG for Lottie, stb_image for GIF.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ponytail: a fixed budget of frames (48, a sheet of 8 columns) at a fixed size, all
// rendered at once on the image thread; stream frames if long stickers ever matter.

#include "discord/stickers.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>

#include "third_party/stb/stb_image.h"
#include "third_party/thorvg/thorvg.h"

namespace discord
{

namespace
{

constexpr int kMaxFrames = 48;
constexpr int kColumns = 8;

void sheet(FrameSheet &out, int cell_w, int cell_h, int frames)
{
    out.frames = frames;
    out.columns = std::min(frames, kColumns);
    out.w = cell_w * out.columns;
    out.h = cell_h * ((frames + kColumns - 1) / kColumns);
    out.rgba.assign(static_cast<std::size_t>(out.w) * out.h * 4, 0);
}

void place(FrameSheet &out, int index, const std::uint8_t *frame, int cell_w, int cell_h)
{
    const int x = (index % kColumns) * cell_w, y = (index / kColumns) * cell_h;
    for (int row = 0; row < cell_h; ++row)
        std::memcpy(&out.rgba[(static_cast<std::size_t>(y + row) * out.w + x) * 4],
                    frame + static_cast<std::size_t>(row) * cell_w * 4, static_cast<std::size_t>(cell_w) * 4);
}

} // namespace

bool render_lottie(const std::string &json, int size, FrameSheet &out, int max_frames)
{
    // One at a time: ThorVG's renderer keeps shared buffers (tvgRender.cpp), and the
    // image threads are several.
    static std::mutex one;
    std::lock_guard<std::mutex> lock(one);
    if (json.size() > (1u << 20)) // Discord's are tens of KB; ThorVG's parser is not for hostile input
        return false;
    static const bool ready = tvg::Initializer::init(0) == tvg::Result::Success; // no threads of its own
    if (!ready)
        return false;
    std::unique_ptr<tvg::Animation> animation(tvg::Animation::gen());
    tvg::Picture *picture = animation->picture();
    if (picture->load(json.data(), static_cast<std::uint32_t>(json.size()), "lottie", nullptr, true) !=
        tvg::Result::Success)
        return false;
    float w = 0.0f, h = 0.0f;
    picture->size(&w, &h);
    if (w <= 0.0f || h <= 0.0f)
        return false;
    const float scale = static_cast<float>(size) / std::max(w, h);
    picture->size(w * scale, h * scale);
    picture->translate((size - w * scale) * 0.5f, (size - h * scale) * 0.5f);

    std::unique_ptr<tvg::SwCanvas> canvas(tvg::SwCanvas::gen());
    std::vector<std::uint32_t> frame(static_cast<std::size_t>(size) * size);
    if (!canvas || canvas->target(frame.data(), static_cast<std::uint32_t>(size), static_cast<std::uint32_t>(size),
                                  static_cast<std::uint32_t>(size), tvg::ColorSpace::ABGR8888S) != tvg::Result::Success ||
        canvas->add(picture) != tvg::Result::Success)
        return false;
    const float total = animation->totalFrame(), seconds = animation->duration();
    const int frames = std::clamp(static_cast<int>(std::lround(seconds * 20.0f)), 1, std::min(max_frames, kMaxFrames));
    sheet(out, size, size, frames);
    out.fps = seconds > 0.0f ? static_cast<float>(frames) / seconds : 0.0f;
    for (int i = 0; i < frames; ++i)
    {
        animation->frame(frames == 1 ? total * 0.5f : total * static_cast<float>(i) / static_cast<float>(frames));
        std::fill(frame.begin(), frame.end(), 0u);
        canvas->update();
        canvas->draw(true);
        canvas->sync();
        place(out, i, reinterpret_cast<const std::uint8_t *>(frame.data()), size, size); // ABGR words: RGBA bytes
    }
    canvas->remove(); // the animation owns the picture
    return true;
}

bool decode_gif(const std::string &data, FrameSheet &out)
{
    int *delays = nullptr, w = 0, h = 0, frames = 0, n = 0;
    // stb decodes every frame at once: stickers are 320 px, so refuse big ones before that.
    // ponytail: the frame count is known only after decoding; a cap needs stb's frame loop.
    if (!stbi_info_from_memory(reinterpret_cast<const std::uint8_t *>(data.data()), static_cast<int>(data.size()), &w,
                               &h, &n) ||
        static_cast<long>(w) * h > 512 * 512)
        return false;
    std::uint8_t *px = stbi_load_gif_from_memory(reinterpret_cast<const std::uint8_t *>(data.data()),
                                                 static_cast<int>(data.size()), &delays, &w, &h, &frames, &n, 4);
    if (!px)
        return false;
    const int keep = std::min(frames, kMaxFrames);
    sheet(out, w, h, keep);
    int ms = 0;
    for (int i = 0; i < keep; ++i)
    {
        place(out, i, px + static_cast<std::size_t>(i) * w * h * 4, w, h);
        ms += delays && delays[i] > 0 ? delays[i] : 100;
    }
    out.fps = ms > 0 ? keep * 1000.0f / static_cast<float>(ms) : 0.0f;
    stbi_image_free(px);
    stbi_image_free(delays);
    return true;
}

} // namespace discord
