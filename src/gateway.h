// Discord main gateway session: connect, identify/resume, heartbeat, dispatch.
#pragma once

// Runs until the session is unrecoverable (bad token, disallowed intents...).
// Returns the close code that ended it, or -1.
int gateway_run(const char *token);
