// ps5-discord - The design framework's tokens (design/framework, "TV Media Rows").
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// One quiet world: charcoal ground, raised cards, large round avatars, and a
// single coral accent that only ever means focus, live or unread. Sizes are in
// the kit's 1920x1080 virtual canvas, measured from the approved comps.

#pragma once

#include "gfx/draw_list.hpp"

namespace screens::style
{

using hui::gfx::Color;

inline const Color kGround = Color::rgb(0x16181d);
inline const Color kRaised = Color::rgb(0x22252c);
inline const Color kRaisedHigh = Color::rgb(0x2c3039); // selected chip, hover plate
inline const Color kLine = Color::rgb(0x2f333c);
inline const Color kText = Color::rgb(0xf2f3f5);
inline const Color kDim = Color::rgb(0xa3a9b6);
inline const Color kFaint = Color::rgb(0x6f7684);
inline const Color kCoral = Color::rgb(0xff7a59); // focus, live, unread: nothing else
inline const Color kOnCoral = Color::rgb(0x1d0f0a);
inline const Color kOnline = Color::rgb(0x3ddc84);
inline const Color kIdle = Color::rgb(0xf5b544);
inline const Color kDnd = Color::rgb(0xed4245);
inline const Color kMentionTint = Color::rgb(0x3a2724); // a mention card's ground

// Layout.
inline constexpr float kMargin = 72.0f;      // left and right screen edges
inline constexpr float kTitleBaseline = 84.0f;
inline constexpr float kTitleSize = 64.0f;   // Nunito Sans ExtraBold
inline constexpr float kLabelSize = 30.0f;   // row headings, Nunito Sans Bold
inline constexpr float kNameSize = 26.0f;
inline constexpr float kBodySize = 25.0f;
inline constexpr float kMetaSize = 21.0f;
inline constexpr float kCardRadius = 18.0f;
inline constexpr float kFocusWidth = 4.0f;   // the coral outline
inline constexpr float kHintsBaseline = 1040.0f;

} // namespace screens::style
