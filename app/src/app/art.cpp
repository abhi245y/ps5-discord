// ps5-discord - Loads the shipped placeholder art into textures.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/art.hpp"

#include <cstdio>

#include "core/save_file.hpp"
#include "third_party/stb/stb_image.h"

namespace hui::app
{

namespace
{

std::uint32_t load_image(gfx::Renderer &renderer, const std::string &path, int *out_w = nullptr,
                         int *out_h = nullptr)
{
    std::string data;
    if (!save::read_file(path, &data, 16u << 20)) // the emoji atlas is past the 4 MB default
        return 0;
    int w = 0, h = 0, n = 0;
    std::uint8_t *px = stbi_load_from_memory(reinterpret_cast<const std::uint8_t *>(data.data()),
                                             static_cast<int>(data.size()), &w, &h, &n, 4);
    if (!px)
        return 0;
    const std::uint32_t texture = renderer.batch().create_texture(w, h, px);
    stbi_image_free(px);
    if (out_w)
        *out_w = w, *out_h = h;
    return texture;
}

} // namespace

ArtSet load_art(gfx::Renderer &renderer, const std::string &dir)
{
    ArtSet art;
    char name[64];
    for (int i = 0; i < kAvatarArt; ++i)
    {
        std::snprintf(name, sizeof name, "/avatar-%02d.jpg", i);
        art.avatars.push_back(load_image(renderer, dir + name));
    }
    for (int i = 0; i < kSceneArt; ++i)
    {
        std::snprintf(name, sizeof name, "/scene-%02d.jpg", i);
        art.scenes.push_back(load_image(renderer, dir + name));
    }
    art.emoji = load_image(renderer, dir + "/emoji.png", &art.emoji_w, &art.emoji_h);
    return art;
}

} // namespace hui::app
