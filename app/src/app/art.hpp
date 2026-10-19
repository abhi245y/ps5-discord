// ps5-discord - Loads the shipped placeholder art (assets/art) into textures.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

#include "app/concept.hpp"
#include "gfx/renderer.hpp"

namespace hui::app
{

inline constexpr int kAvatarArt = 12; // assets/art/avatar-00.jpg ...
inline constexpr int kSceneArt = 8;   // assets/art/scene-00.jpg ...

// Missing files leave a 0 texture; screens then draw a flat placeholder.
ArtSet load_art(gfx::Renderer &renderer, const std::string &dir);

} // namespace hui::app
