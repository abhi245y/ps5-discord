// SPDX-License-Identifier: AGPL-3.0-or-later
// PS5 system notifications (the console's own, shown over games and kept in its
// notification list) and quiet hours.
#pragma once
#include <stdint.h>

// context: the small grey line on top (who and where); text: the main line (what);
// icon: an image URL (the sender's avatar), NULL for Discord's own.
void notify_system(const char *context, const char *text, const char *icon);
// For a message, paced: a burst becomes one "N new messages" notification with the
// latest, at most one every couple of seconds.
void notify_message(const char *context, const char *text, const char *icon);
int notify_quiet_now(void); // inside the quiet hours (prefs quiet_hours)
// The console user whose Discord account runs: notifications go to them, and only while
// they are the one on screen.
void notify_set_owner(int32_t user);

// main.c: any of the app's notifications, shown by the PS5 only while the app is not in
// front (with it in front the app shows its own). urgent: now, unpaced (a ring).
void app_alert(const char *context, const char *text, const char *icon, int urgent);
