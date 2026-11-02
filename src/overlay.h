// SPDX-License-Identifier: AGPL-3.0-or-later
// The in-game overlay's feed (overlay/, injected into the system UI): five times a second
// the call's state goes into its text channel as one short line (overlay/src/app/
// discord_app.inc has the format), and the call panel's presses come back as commands
// (cmd.bin). With Settings' overlay on, the core starts the overlay service through elfldr.
// Console only; the host build writes nothing.
#pragma once

void overlay_init(void);     // starts the feed thread; once, at startup
void overlay_shutdown(void); // the core is quitting: the overlay hides (it stays loaded)
// Settings: off, starting, running, stopped (needs a reload), reloading.
const char *overlay_state(void);
// Restart the system UI and load the overlay again (about 45 s, in the background). 0 when
// started; -1 when one runs already or the overlay is off.
int overlay_reload(void);

// The line for these inputs, at most 127 bytes (exposed for the selftest). people_flags[i]
// is 't' talking, 'm' muted, 'd' deafened or '-'; talking people go first.
int overlay_line(char *out, int cap, char state, char me, int glance, const char *live, const char *call,
                 const char names[][64], const char *flags, int count);
