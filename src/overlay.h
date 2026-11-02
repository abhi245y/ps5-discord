// SPDX-License-Identifier: AGPL-3.0-or-later
// The in-game overlay's feed (overlay/, injected into the system UI): five times a second
// the call's state goes into its text channel as one short line (overlay/src/app/
// discord_app.inc has the format), and the call panel's presses come back as commands
// (cmd.bin). With Settings' overlay on, the core starts the overlay service through elfldr.
// Console only; the host build writes nothing.
#pragma once

void overlay_init(void);     // starts the feed thread; once, at startup
void overlay_shutdown(void); // the core is quitting: the overlay hides (it stays loaded)

// The line for these inputs, at most 127 bytes (exposed for the selftest). people_flags[i]
// is 't' talking, 'm' muted, 'd' deafened or '-'; talking people go first.
int overlay_line(char *out, int cap, char state, char me, int glance, const char *live, const char *call,
                 const char names[][64], const char *flags, int count);
