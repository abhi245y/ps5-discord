// ps5-discord - Before the account is ready: sign in, waiting, reconnecting.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "screens/draw.hpp"

namespace screens
{

// The QR sign-in (framework frame 01), from the core's login QR.
void draw_sign_in(DrawList &list, const Ink &ink, const discord::Snapshot &snapshot, float time);
// Starting, connecting or reconnecting (framework frame 12), with the reason.
void draw_waiting(DrawList &list, const Ink &ink, const discord::Snapshot &snapshot, float time);

} // namespace screens
