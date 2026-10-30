// The game in front on this console, for Discord's "Playing ..." (gateway.c sends it).
#pragma once
#include <stddef.h>

// The running game's name, or "" (none, this app, a system app, or no name found).
// Gateway thread only: it keeps the last title's name.
void playing_now(char *name, size_t cap);
// A title's name from the console's metadata folder: dir/<title>/param.json (PS5), else
// param.sfo (PS4). English when there is one, else the title's own language. 0 when found.
int playing_title_name(const char *dir, const char *title, char *name, size_t cap);
