// ps5-discord - Text beyond the baked fonts: HarfBuzz-shaped fallback faces.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <initializer_list>
#include <string>

#include "gfx/font.hpp"
#include "gfx/renderer.hpp"
#include "gfx/shaper.hpp"

namespace hui::app
{

// Loads the fallback faces (assets/fonts: Noto Sans for Cyrillic, Greek and
// extended Latin; Noto Emoji) into shaper, makes its atlas and lets every
// baked font fall back to it. Call shaper.upload(renderer.batch()) once per
// frame after composing and before presenting.
void attach_text_engine(gfx::Shaper &shaper, gfx::Renderer &renderer, const std::string &fonts_dir,
                        std::initializer_list<gfx::Font *> fonts);

} // namespace hui::app
