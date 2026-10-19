// Discord main gateway session: connect, identify/resume, heartbeat, dispatch.
#pragma once

// Runs until the session is unrecoverable (bad token, disallowed intents...).
// Returns the close code that ended it, or -1.
int gateway_run(const char *token);

// Ask for live events of a guild (op 37), as the web client does when one is opened.
// Callable from any thread; the gateway thread sends it within about a second.
int gateway_subscribe(const char *guild_id);
