// ps5-discord - Text beyond the baked fonts: HarfBuzz-shaped fallback faces.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/text_engine.hpp"

#include <utility>

#include "core/save_file.hpp"
#include "platform/ps5/system.hpp"

namespace hui::app
{

void attach_text_engine(gfx::Shaper &shaper, gfx::Renderer &renderer, const std::string &fonts_dir,
                        std::initializer_list<gfx::Font *> fonts)
{
    // ponytail: CJK and other scripts need the console's own fonts; a native title cannot
    // list /preinst/common/font (no opendir), so they join once their file names are known.
    for (const char *name : {"NotoSans-Regular.ttf", "NotoEmoji-Regular.ttf"})
    {
        std::string data;
        if (!save::read_file(fonts_dir + "/" + name, &data) || !shaper.add_face(std::move(data)))
            sys::log("[TEXT] fallback face %s failed", name);
    }
    shaper.attach(renderer.batch());
    for (gfx::Font *font : fonts)
        font->set_shaper(&shaper);
}

} // namespace hui::app
