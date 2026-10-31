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

// What a close code means for the session: GATEWAY_STOP it is over (bad token, a request
// Discord will never accept), GATEWAY_FRESH identify again (invalid seq, timed out),
// GATEWAY_KEEP resume it when there is one.
enum { GATEWAY_KEEP, GATEWAY_FRESH, GATEWAY_STOP };
static inline int gateway_close_action(int code)
{
    if (code == 4004 || (code >= 4010 && code <= 4014))
        return GATEWAY_STOP;
    return code == 4007 || code == 4009 ? GATEWAY_FRESH : GATEWAY_KEEP;
}
// The wait (seconds) after waiting `last`: doubling, at most 15 (back soon once the network is).
static inline int gateway_next_wait(int last)
{
    return last * 2 < 15 ? last * 2 : 15;
}

// Ask for live events of a guild (op 37), as the web client does when one is opened.
// Callable from any thread; the gateway thread sends it within about a second.
int gateway_subscribe(const char *guild_id);
