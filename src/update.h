// The in-app updater (docs/updater-plan.md): finds a signed release on GitHub, unpacks it
// beside the app, swaps the app folder's entries once the app has closed, and puts the
// previous version back when Settings asks.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <cJSON.h>

// The project's repository by id, so a rename or transfer keeps updates coming.
#define UPDATE_REPO_ID "1425363066"

// At core start: finishes a swap or a restore cut short, and starts the update thread.
void update_init(const char *app_dir, const char *work_dir, const char *bad_file);
void update_check(int force);           // asks GitHub (at most once an hour unless forced)
int update_start(char *why, size_t cap); // download and unpack the release found; 0 started
int update_apply(void);                 // ready: swap once the app has closed; 0 accepted
int update_restore(void);               // the previous version back once the app has closed; 0 accepted
cJSON *update_json(void); // the "update" object of /api/state

// ---- The pure parts (update_verify.c), for the tests ----

// >0 when a is newer than b, 0 same, <0 older. Semver with prereleases; a "-dev" build
// comes just before the release of the same number.
int update_version_cmp(const char *a, const char *b);
int update_version_ok(const char *v); // 1 when it parses
int update_prerelease(const char *v);

typedef struct {
    char version[64];
    char asset[128];
    uint64_t size;
    unsigned char sha256[32];
    char min_from[64];
} update_manifest;
// update.txt: "ps5-discord-update 1" then key value lines; unknown keys skipped. 0 on success.
int update_manifest_parse(const char *text, update_manifest *m);
// update.sig (flags, counter, r, s: 69 bytes) over update.txt by one of keys (65-byte
// points), as ssh-keygen -Y sign makes it with a FIDO key. 1 when valid and touched.
int update_signature_ok(const char *text, size_t len, const unsigned char *sig, size_t sig_len,
                        const unsigned char (*keys)[65], int key_count);

// The release zip (stored or deflated entries, one PPSA99072/ folder) into dir: 0 on success.
// progress (may be NULL): bytes unpacked of all, after each entry. why: short.
int update_unzip(const char *zip_path, const char *dir, uint64_t max_total, void (*progress)(uint64_t done, uint64_t total),
                 char *why, size_t cap);
