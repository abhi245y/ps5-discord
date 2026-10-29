// Discord main gateway session: connect, identify/resume, heartbeat, dispatch.
#pragma once

// Runs until the session is unrecoverable (bad token, disallowed intents...), or another
// console user's account is wanted (GATEWAY_SWITCH). Returns the close code, or -1.
#define GATEWAY_SWITCH -2
int gateway_run(const char *token);
// Set when the user on screen changed (main.c); the gateway and the logins stop within a
// second or two. Cleared before the next account starts.
void gateway_switch(int on);
int gateway_switching(void);
// Retry now: ends the wait between reconnect attempts (the app's Retry now).
void gateway_retry(void);

// Ask for live events of a guild (op 37), as the web client does when one is opened.
// Callable from any thread; the gateway thread sends it within about a second.
int gateway_subscribe(const char *guild_id);
