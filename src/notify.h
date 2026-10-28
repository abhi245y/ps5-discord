// SPDX-License-Identifier: AGPL-3.0-or-later
// PS5 system alerts (the console's own notification, shown over games and kept in its
// notification list) and quiet hours.
#pragma once

// title: who and where; body: what; icon: an image URL (the sender's avatar), NULL for none.
void notify_system(const char *title, const char *body, const char *icon);
// An alert for a message, paced: a burst becomes one "N new messages" alert with the
// latest, at most one every few seconds.
void notify_message(const char *title, const char *body, const char *icon);
int notify_quiet_now(void); // inside the quiet hours (prefs quiet_hours)
