// SPDX-License-Identifier: AGPL-3.0-or-later
// PS5 system alerts (the console's own pop-up, shown over games) and quiet hours.
#pragma once

void notify_system(const char *text); // a line or two; long text is cut
int notify_quiet_now(void);           // inside the quiet hours (prefs quiet_hours)
