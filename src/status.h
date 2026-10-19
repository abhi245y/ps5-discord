// Shared session status, written by login/gateway, read by the web UI. Thread-safe.
#pragma once

void status_state(const char *state); // "starting", "login", "scanned", "connecting", "ready", ...
void status_qr(const char *url);      // NULL clears
void status_user(const char *name, int guilds, int dms);
void status_event(void);              // count one gateway dispatch
void status_log(const char *line);    // keeps the last lines for the UI
char *status_json(void);              // caller frees
