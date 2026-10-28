// SPDX-License-Identifier: AGPL-3.0-or-later
// Remote Play pairings for Go Live, one per console user. A pairing belongs to the PSN
// account it was made with, and the Remote Play session logs that user in: Go Live as
// the user on screen takes the USB controller over (it plays the game through the
// session and is the call's headset); Go Live as another console user leaves the
// controller alone (Bluetooth is fine) but has no mic during the game.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define RP_MAX_USERS 16

typedef struct {
    int32_t id;
    char name[64];
    int has_account; // has a PSN account id: Remote Play needs one
    int paired;
    char console[64]; // the paired console's name
    long long since;  // when it was paired (unix seconds), 0 unknown
} rp_user;

void rp_init(const char *data_dir);
int rp_users(rp_user *out, int max);   // the console's users and their pairings; how many
int32_t rp_foreground(void);           // the user on screen, -1 unknown
// Pairs user (-1: the user on screen). pin NULL or "": the core gets a PIN from the
// console's Remote Play service, so the user types nothing; if the console refuses, the
// state turns "need_pin" and the Link Device PIN is asked for. 0 when started.
int rp_pair(int32_t user, const char *pin);
int rp_unpair(int32_t user);           // forgets it here (the console lists it until removed there)
int rp_use(int32_t user);              // the user Go Live runs as; must be paired
int32_t rp_used(void);                 // -1 when nobody is paired
const char *rp_pair_state(void);       // "idle", "pairing", "paired", "need_pin" or "error"
const char *rp_pair_error(void);
// For the session: the Go Live user's pairing (a ChiakiRegisteredHost of size bytes) and
// account id; *foreground is 1 when that user is the one on screen. 0 on success.
int rp_load(void *host, size_t size, uint64_t *account, int *foreground);
int rp_chiaki_ready(void); // chiaki's one-time init, shared with broadcast.c
