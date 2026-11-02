// SPDX-License-Identifier: GPL-3.0-or-later
// The overlay UI the engine builds in (src/core/worker.cpp includes this file): the
// Discord call overlay, or the demo bar for the engine's own host tests.
#include "overlay_ui.hpp"

#ifdef OVERLAY_APP_DEMO
#include "demo_app.inc"
#else
#include "discord_app.inc"
#endif
