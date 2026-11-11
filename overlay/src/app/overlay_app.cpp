// SPDX-License-Identifier: GPL-3.0-or-later
// The overlay UI the engine builds in (src/core/worker.cpp includes this file): the
// Discord call overlay, standalone pill lab, or the engine host-test demo.
#include "overlay_ui.hpp"

#if defined(OVERLAY_APP_LAB)
#include "pill_lab.inc"
#elif defined(OVERLAY_APP_DEMO)
#include "demo_app.inc"
#else
#include "discord_app.inc"
#endif
