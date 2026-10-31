// SPDX-License-Identifier: AGPL-3.0-or-later
// Remote Play pairings (rp_pair.h). Per console user, in DATA_DIR:
//   remoteplay-<user>.bin  the ChiakiRegisteredHost (keys; 0600, never logged)
//   remoteplay-<user>.txt  the console's name, the user's name, when (unix seconds)
//   remoteplay-use         the user Go Live runs as, in hex
// The first pairing (remoteplay.bin/.txt, one per console) moves to its user on first use.
#include "rp_pair.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char dir[200];
static char state[16] = "idle", error[160];

static void set_pair(const char *s, const char *why)
{
    pthread_mutex_lock(&lock);
    snprintf(state, sizeof state, "%s", s);
    snprintf(error, sizeof error, "%s", why ? why : "");
    pthread_mutex_unlock(&lock);
}

const char *rp_pair_state(void)
{
    return state;
}

const char *rp_pair_error(void)
{
    return error;
}

void rp_init(const char *data_dir)
{
    snprintf(dir, sizeof dir, "%s", data_dir);
}

static void path(char *out, size_t cap, int32_t user, const char *ext)
{
    snprintf(out, cap, "%s/remoteplay-%08x.%s", dir, (unsigned)user, ext);
}

__attribute__((unused)) static int exists(const char *p) // the console's half only
{
    FILE *f = fopen(p, "rb"); // access() refuses payloads on the console
    if (f)
        fclose(f);
    return f != NULL;
}

// The .txt beside a pairing: console name, user name, time.
__attribute__((unused)) static void read_info(const char *p, char console[64], char user[64], long long *since)
{
    console[0] = user[0] = 0;
    *since = 0;
    FILE *f = fopen(p, "r");
    if (!f)
        return;
    char line[64];
    if (fgets(console, 64, f))
        console[strcspn(console, "\n")] = 0;
    if (fgets(user, 64, f))
        user[strcspn(user, "\n")] = 0;
    if (fgets(line, sizeof line, f))
        *since = atoll(line);
    fclose(f);
}

static int32_t read_use(void)
{
    char p[256];
    snprintf(p, sizeof p, "%s/remoteplay-use", dir);
    FILE *f = fopen(p, "r");
    unsigned v = 0;
    int ok = f && fscanf(f, "%x", &v) == 1;
    if (f)
        fclose(f);
    return ok ? (int32_t)v : -1;
}

static int write_use(int32_t user)
{
    char p[256];
    snprintf(p, sizeof p, "%s/remoteplay-use", dir);
    if (user < 0)
        return remove(p);
    FILE *f = fopen(p, "w");
    if (!f)
        return -1;
    fprintf(f, "%08x\n", (unsigned)user);
    return fclose(f);
}

int rp_unpair(int32_t user)
{
    char p[256];
    path(p, sizeof p, user, "bin");
    int r = remove(p);
    path(p, sizeof p, user, "txt");
    remove(p);
    if (read_use() == user)
        write_use(-1);
    set_pair("idle", "");
    LOG("remote play: pairing of user %#x removed", (unsigned)user);
    return r == 0 ? 0 : -1;
}

#ifndef __PROSPERO__
int rp_users(rp_user *out, int max)
{
    (void)out;
    (void)max;
    return 0;
}

int32_t rp_foreground(void)
{
    return -1;
}

int rp_pair(int32_t user, const char *pin)
{
    (void)user;
    (void)pin;
    set_pair("error", "Pairing needs the console.");
    return -1;
}

int rp_use(int32_t user)
{
    (void)user;
    return -1;
}

int32_t rp_used(void)
{
    return -1;
}

int rp_load(void *host, size_t size, uint64_t *account, int *foreground)
{
    (void)host;
    (void)size;
    (void)account;
    (void)foreground;
    return -1;
}

int rp_chiaki_ready(void)
{
    return 0;
}
#else

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chiaki/regist.h>

int sceUserServiceInitialize(void *);
int sceUserServiceGetForegroundUser(int32_t *);
int sceUserServiceGetUserName(int32_t, char *, size_t);

int rp_chiaki_ready(void)
{
    static int ok;
    if (!ok)
        ok = chiaki_lib_init() == CHIAKI_ERR_SUCCESS;
    return ok;
}

static void chiaki_log_cb(ChiakiLogLevel level, const char *msg, void *user)
{
    (void)level;
    (void)user;
    LOG("remote play: %s", msg);
}

// The user service, started here: the core does not use it otherwise.
static void *user_lib_handle;

static void user_lib_open(void)
{
    sceUserServiceInitialize(NULL); // already started is fine
    user_lib_handle = dlopen("libSceUserService.sprx", RTLD_LAZY);
}

// Once, and a second caller waits for the first (watch_user and a pairing may race here).
static void *user_lib(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, user_lib_open);
    return user_lib_handle;
}

int32_t rp_foreground(void)
{
    user_lib();
    int32_t user = -1;
    return sceUserServiceGetForegroundUser(&user) == 0 ? user : -1;
}

static uint64_t account_of(int32_t user)
{
    void *lib = user_lib();
    int (*acct)(int32_t, uint64_t *) =
        lib ? (int (*)(int32_t, uint64_t *))dlsym(lib, "sceUserServiceGetNpAccountId") : NULL;
    uint64_t id = 0;
    if (acct)
        acct(user, &id);
    return id;
}

// The first pairing (one per console) belongs to the user named in its .txt, or else
// to whoever is on screen.
static void migrate(const int32_t *ids, int n)
{
    char old_bin[256], old_txt[256];
    snprintf(old_bin, sizeof old_bin, "%s/remoteplay.bin", dir);
    snprintf(old_txt, sizeof old_txt, "%s/remoteplay.txt", dir);
    if (!exists(old_bin))
        return;
    char console[64], user[64];
    long long since;
    read_info(old_txt, console, user, &since);
    int32_t owner = rp_foreground();
    for (int i = 0; i < n; i++) {
        char name[64] = "";
        sceUserServiceGetUserName(ids[i], name, sizeof name);
        if (user[0] && strcmp(name, user) == 0)
            owner = ids[i];
    }
    if (owner < 0)
        return;
    char bin[256], txt[256];
    path(bin, sizeof bin, owner, "bin");
    path(txt, sizeof txt, owner, "txt");
    if (rename(old_bin, bin) == 0) {
        rename(old_txt, txt);
        if (read_use() < 0)
            write_use(owner);
        LOG("remote play: the pairing now belongs to user %#x", (unsigned)owner);
    }
}

int rp_users(rp_user *out, int max)
{
    void *lib = user_lib();
    int (*list)(int32_t *) = lib ? (int (*)(int32_t *))dlsym(lib, "sceUserServiceGetRegisteredUserIdList") : NULL;
    int32_t ids[64];
    memset(ids, 0xff, sizeof ids);
    if (!list || list(ids) != 0)
        return 0;
    int n = 0;
    int32_t found[RP_MAX_USERS];
    for (int i = 0; i < RP_MAX_USERS; i++)
        if (ids[i] != -1 && ids[i] != 0)
            found[n++] = ids[i];
    migrate(found, n);
    int k = 0;
    for (int i = 0; i < n && k < max; i++) {
        rp_user *u = &out[k++];
        memset(u, 0, sizeof *u);
        u->id = found[i];
        sceUserServiceGetUserName(u->id, u->name, sizeof u->name);
        u->has_account = account_of(u->id) != 0;
        char p[256], name[64];
        path(p, sizeof p, u->id, "bin");
        u->paired = exists(p);
        path(p, sizeof p, u->id, "txt");
        if (u->paired)
            read_info(p, u->console, name, &u->since);
    }
    return k;
}

int32_t rp_used(void)
{
    char p[256];
    int32_t use = read_use();
    path(p, sizeof p, use, "bin");
    if (use >= 0 && exists(p))
        return use;
    rp_user users[RP_MAX_USERS]; // nobody chosen: the user on screen if paired, else any paired
    int n = rp_users(users, RP_MAX_USERS);
    int32_t fg = rp_foreground(), any = -1;
    for (int i = 0; i < n; i++)
        if (users[i].paired) {
            if (users[i].id == fg)
                return fg;
            if (any < 0)
                any = users[i].id;
        }
    return any;
}

int rp_use(int32_t user)
{
    char p[256];
    path(p, sizeof p, user, "bin");
    if (!exists(p))
        return -1;
    LOG("remote play: Go Live runs as user %#x", (unsigned)user);
    return write_use(user);
}

int rp_load(void *host, size_t size, uint64_t *account, int *foreground)
{
    int32_t user = rp_used();
    if (user < 0)
        return -1;
    char p[256];
    path(p, sizeof p, user, "bin");
    FILE *f = fopen(p, "rb");
    size_t got = f ? fread(host, size, 1, f) : 0;
    if (f)
        fclose(f);
    if (got != 1)
        return -1;
    *account = account_of(user);
    *foreground = user == rp_foreground();
    return 0;
}

// ---- pairing ------------------------------------------------------------------------

// The console's own Remote Play service (what Settings > Remote Play > Link Device uses):
// a PIN made here opens the console for pairing, no screen needed. 0 when it refuses.
static int (*notify_pin_error)(int);

static uint32_t system_pin(void)
{
    void *lib = dlopen("libSceRemoteplay.sprx", RTLD_LAZY);
    int (*init)(void *, size_t) = lib ? (int (*)(void *, size_t))dlsym(lib, "sceRemoteplayInitialize") : NULL;
    int (*gen)(uint32_t *) = lib ? (int (*)(uint32_t *))dlsym(lib, "sceRemoteplayGeneratePinCode") : NULL;
    notify_pin_error = lib ? (int (*)(int))dlsym(lib, "sceRemoteplayNotifyPinCodeError") : NULL;
    if (!init || !gen) {
        LOG("remote play: the console's pairing service is not reachable (%s)", lib ? "symbols" : "library");
        return 0;
    }
    int ir = init(NULL, 0);
    uint32_t pin = 0;
    int gr = gen(&pin);
    LOG("remote play: console pairing service init %#x, PIN %s (%#x)", (unsigned)ir, gr == 0 && pin ? "made" : "refused",
        (unsigned)gr);
    return gr == 0 ? pin : 0;
}

typedef struct {
    int32_t user;
    uint32_t pin;
} pair_job;

static _Atomic int regist_done;
static ChiakiRegisteredHost regist_host;

static void regist_cb(ChiakiRegistEvent *e, void *user)
{
    (void)user;
    if (e->type == CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS)
        regist_host = *e->registered_host;
    regist_done = e->type == CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS ? 1 : 2;
}

static void *pair_main(void *arg)
{
    pair_job job = *(pair_job *)arg;
    free(arg);
    const char *why = NULL;
    int made_pin = job.pin == 0;
    if (made_pin && !(job.pin = system_pin())) {
        set_pair("need_pin", "Open Settings > System > Remote Play > Link Device on the PS5 and enter its PIN.");
        return NULL;
    }
    char name[64] = "";
    sceUserServiceGetUserName(job.user, name, sizeof name);
    uint64_t id = account_of(job.user);
    LOG("remote play: pairing user %#x, account id %s", (unsigned)job.user, id ? "present" : "missing");
    static ChiakiLog log;
    chiaki_log_init(&log, CHIAKI_LOG_ERROR | CHIAKI_LOG_WARNING, chiaki_log_cb, NULL);
    ChiakiRegistInfo info;
    memset(&info, 0, sizeof info);
    info.target = CHIAKI_TARGET_PS5_1;
    info.host = "127.0.0.1";
    info.pin = job.pin;
    for (int i = 0; i < 8; i++) // little-endian, as Sony's account id is sent
        info.psn_account_id[i] = (uint8_t)(id >> (8 * i));
    ChiakiRegist regist;
    regist_done = 0;
    if (!id)
        why = "This console user has no PSN account id: sign in to PSN once on it.";
    else if (chiaki_regist_start(&regist, &log, &info, regist_cb, NULL) != CHIAKI_ERR_SUCCESS)
        why = "Pairing could not start.";
    else {
        for (int i = 0; i < 300 && !regist_done; i++) // 30 s
            usleep(100000);
        if (!regist_done)
            chiaki_regist_stop(&regist);
        chiaki_regist_fini(&regist);
        if (regist_done != 1)
            why = regist_done ? "The console refused the pairing. Is Remote Play on for this user?"
                              : "The console did not answer.";
    }
    if (made_pin && notify_pin_error)
        notify_pin_error(1); // close the console's pairing window again
    if (!why) { // write it whole, then move it into place
        char bin[256], tmp[300], txt[256];
        path(bin, sizeof bin, job.user, "bin");
        path(txt, sizeof txt, job.user, "txt");
        snprintf(tmp, sizeof tmp, "%s.new", bin);
        FILE *f = fopen(tmp, "wb");
        int ok = f && fwrite(&regist_host, sizeof regist_host, 1, f) == 1;
        if (f)
            ok &= fclose(f) == 0;
        if (ok) {
            chmod(tmp, 0600);
            ok = rename(tmp, bin) == 0;
        }
        FILE *t = ok ? fopen(txt, "w") : NULL;
        if (t) {
            fprintf(t, "%.63s\n%.63s\n%lld\n", regist_host.server_nickname, name, (long long)time(NULL));
            fclose(t);
        }
        if (ok && read_use() < 0)
            write_use(job.user); // the first pairing is the one Go Live uses
        if (!ok)
            why = "Could not save the pairing.";
        memset(&regist_host, 0, sizeof regist_host); // the keys stay only in the file
    }
    LOG("remote play: pairing %s%s", why ? "failed: " : "done", why ? why : "");
    set_pair(why ? "error" : "paired", why);
    return NULL;
}

int rp_pair(int32_t user, const char *pin)
{
    char *end = NULL;
    unsigned long n = pin && *pin ? strtoul(pin, &end, 10) : 0;
    if (pin && *pin && (strlen(pin) != 8 || *end)) {
        set_pair("error", "The PIN has 8 digits.");
        return -1;
    }
    if (user < 0)
        user = rp_foreground();
    if (user < 0) {
        set_pair("error", "No console user is signed in.");
        return -1;
    }
    pthread_mutex_lock(&lock);
    int busy = strcmp(state, "pairing") == 0;
    if (!busy)
        snprintf(state, sizeof state, "pairing"), error[0] = 0;
    pthread_mutex_unlock(&lock);
    if (busy)
        return 0;
    pair_job *job = malloc(sizeof *job);
    if (!job || !rp_chiaki_ready()) {
        free(job);
        set_pair("error", "Remote Play could not start.");
        return -1;
    }
    *job = (pair_job){.user = user, .pin = (uint32_t)n};
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, pair_main, job) != 0) {
        free(job);
        set_pair("error", "Pairing could not start.");
        return -1;
    }
    return 0;
}
#endif
