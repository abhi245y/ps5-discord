// ps5-discord - Text beyond the baked fonts: HarfBuzz-shaped fallback faces.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "app/text_engine.hpp"

#include <utility>
#include <vector>

#include "core/save_file.hpp"
#include "platform/ps5/system.hpp"

namespace hui::app
{

void attach_text_engine(gfx::Shaper &shaper, gfx::Renderer &renderer, const std::string &fonts_dir,
                        std::initializer_list<gfx::Font *> fonts)
{
    // Shipped faces first (Latin extras, emoji, Indian scripts the console lacks), then the
    // console's own fonts for Japanese, Chinese, Korean, Arabic and Thai. A native title
    // cannot list /preinst/common/font, so they are named (as seen over FTP on 13.00).
    const std::string shipped[] = {"NotoSans-Regular.ttf",        "NotoEmoji-Regular.ttf",
                                   "NotoSansMalayalam-Regular.ttf", "NotoSansDevanagari-VF.ttf",
                                   "NotoSansTamil-VF.ttf",          "NotoSansKannada-VF.ttf",
                                   "NotoSansTelugu-VF.ttf",         "NotoSansBengali-VF.ttf"};
    const char *system[] = {"SSTJpPro-Regular.otf", "DFHEI5-SONY.ttf", "YoonGothicProSIE760.otf",
                            "SSTArabic-Roman.otf", "SSTThai-Roman.otf"};
    std::vector<std::string> paths;
    for (const std::string &name : shipped)
        paths.push_back(fonts_dir + "/" + name);
    for (const char *name : system)
        paths.push_back(std::string("/preinst/common/font/") + name);
    for (const std::string &path : paths)
    {
        std::string data;
        if (!save::read_file(path, &data, 16u << 20) || !shaper.add_face(std::move(data)))
            sys::log("[TEXT] fallback face %s failed", path.c_str());
    }
    shaper.attach(renderer.batch());
    for (gfx::Font *font : fonts)
        font->set_shaper(&shaper);
}

} // namespace hui::app
