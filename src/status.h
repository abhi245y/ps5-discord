// Shared session status, written by login/gateway, read by the web UI. Thread-safe.
#pragma once

// The control API's version (ROADMAP.md, "Core API version"). Raise it on any
// change an older app could misread; the app refuses a core outside its range.
#define CORE_API_VERSION 1

void status_state(const char *state); // "starting", "login", "scanned", "connecting", "ready", ...
void status_qr(const char *url);      // NULL clears
void status_user(const char *name, int guilds, int dms);
void status_event(void);              // count one gateway dispatch
void status_log(const char *line);    // keeps the last lines for the UI
void status_note(const char *note);   // why sign-in is stuck, for the app; NULL clears
char *status_json(void);              // caller frees
