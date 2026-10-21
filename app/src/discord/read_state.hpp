#pragma once

#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>

namespace discord
{
// Discord may omit mention_count. Older cores serialized the missing value
// as null; treat missing/invalid counts as zero rather than throwing in the UI.
inline int mention_count(const nlohmann::json &read)
{
    const auto value = read.find("mentions");
    if (value == read.end() || !value->is_number())
        return 0;
    const double count = value->get<double>();
    return std::isfinite(count) && count >= 0 && count <= std::numeric_limits<int>::max()
               ? static_cast<int>(count) : 0;
}
}
