// SPDX-License-Identifier: AGPL-3.0-or-later
// Controller audio: during a call, the DualSense plugged in by USB can be the headset
// (its mic and its speaker or headphones). The console keeps the controller's buttons,
// so games play as usual. The call uses the controller when the user picks it, and
// whenever the app is not on screen (a game covers it), since then the TV cannot play
// the call (payload audio is muted by the system).
#pragma once

void cv_init(void);
void cv_set_tv(int tv); // the user's pick while the app is on screen: 1 TV, 0 controller
int cv_tv(void);
const char *cv_state(void); // "off", "on" (the controller is the call's audio) or "missing"
