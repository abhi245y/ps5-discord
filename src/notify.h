// SPDX-License-Identifier: AGPL-3.0-or-later
// PS5 system alerts (the console's own pop-up, shown over games) and quiet hours.
#pragma once

void notify_system(const char *text); // a line or two; long text is cut
// An alert for a message, paced: the pop-ups stack and stay, so a burst becomes one
// "N new messages" alert with the latest, at most one every few seconds.
void notify_message(const char *text);
int notify_quiet_now(void);           // inside the quiet hours (prefs quiet_hours)
