// The game in front on this console, for Discord's "Playing ..." (gateway.c sends it).
#pragma once
#include <stddef.h>

// Discord application whose image proxy shows game covers, and whose icon shows when a
// game has none. "" = name only, no pictures.
#define PLAYING_APP_ID "1565829739241807882" // ps5-discord on discord.com/developers

// The running game: its title id (10 bytes) and name, or "" (none, this app, a system app,
// or no name found). Gateway thread only: it keeps the last title's name.
void playing_now(char title[16], char *name, size_t cap);
// The game's cover as a Discord asset ("mp:external/..."), or "" while it is looked up (a
// thread of its own: prosperopatches.com or orbispatches.com, then Discord's proxy) or when
// there is none. Kept for the last title asked about.
void playing_cover(const char *title, const char *token, char *out, size_t cap);
// The two replies, read (for the tests): the lookup's icon URL, Discord's asset path.
int playing_icon_url(const char *lookup_json, char *out, size_t cap);
int playing_asset(const char *assets_json, char *out, size_t cap);
// A title's name from the console's metadata folder: dir/<title>/param.json (PS5), else
// param.sfo (PS4). English when there is one, else the title's own language. 0 when found.
int playing_title_name(const char *dir, const char *title, char *name, size_t cap);
