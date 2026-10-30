// SPDX-License-Identifier: AGPL-3.0-or-later
// The updater's I/O (docs/updater-plan.md). One thread does everything in order: a check,
// a download, a swap, the watch over the first launches of a new version, a restore.
//
// The work folder sits beside the app folder (same filesystem, so renames are atomic) and
// holds a one-line journal: "staged V installed", "swapping V 0", "done V fails" or
// "restoring V 0". A core of any version reads it at start and carries on.
#include "update.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include <mbedtls/sha256.h>

#include "broadcast.h"
#include "http.h"
#include "log.h"
#include "notify.h"
#include "update_keys.h"
#include "voice.h"
#include "web.h"

#define RELEASES "https://api.github.com/repositories/" UPDATE_REPO_ID "/releases?per_page=10"
#define MAX_UNPACKED (1ull << 30)

static char app_dir[256], work[256], bad_file[256], sandbox_dir[64] = "/mnt/sandbox";
static char core_version[64]; // this core's: only a core of the new version takes its proof
static const char *registered[] = {"/user/appmeta/PPSA99072", "/user/app/PPSA99072/sce_sys"};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static struct {
    const char *state; // none checking available downloading unpacking ready applying failed rolled_back
    char version[64], installed[64], error[96];
    char *notes;
    uint64_t done, total;
    int want_check, force, want_start, want_apply;
    time_t checked;
    // The release found: its manifest, checked, and where its zip is.
    update_manifest manifest;
    char zip_url[1024];
    // The watch: the launch now running.
    char watch[64];
    int64_t proof_first; // the new version's app drew its first 60 frames, talking to its own core
    int fallback;
    int drawn_old; // ... drew, but only at the old core (a call kept the app from replacing it)
} u = {.state = "none"};

static int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void set_state(const char *state, const char *error)
{
    pthread_mutex_lock(&lock);
    u.state = state;
    snprintf(u.error, sizeof u.error, "%s", error ? error : "");
    pthread_mutex_unlock(&lock);
    if (error)
        LOG("update: %s", error);
}

// ---- Files ----

static int read_line(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    int ok = fgets(out, (int)cap, f) != NULL;
    fclose(f);
    out[ok ? strcspn(out, "\r\n") : 0] = 0;
    return ok && out[0] ? 0 : -1;
}

static void installed_version(char *out, size_t cap)
{
    char path[300];
    snprintf(path, sizeof path, "%s/version.txt", app_dir);
    if (read_line(path, out, cap) != 0)
        out[0] = 0;
}

static int sync_dir(const char *path)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    int r = fd >= 0 && fsync(fd) == 0 ? 0 : -1;
    if (fd >= 0)
        close(fd);
    return r;
}

// The journal: written to a new file and renamed over, so a power cut leaves one or the other.
static int journal_write(const char *state, const char *version, const char *extra)
{
    char path[300], tmp[300];
    snprintf(path, sizeof path, "%s/journal", work);
    snprintf(tmp, sizeof tmp, "%s/journal.new", work);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return -1;
    int ok = fprintf(f, "%s %s %s\n", state, version, extra) > 0;
    ok = fflush(f) == 0 && fsync(fileno(f)) == 0 && ok;
    ok = fclose(f) == 0 && ok;
    return ok && rename(tmp, path) == 0 && sync_dir(work) == 0 ? 0 : -1;
}

static int journal_read(char state[16], char version[64], char extra[64])
{
    char path[300], line[160];
    snprintf(path, sizeof path, "%s/journal", work);
    if (read_line(path, line, sizeof line) != 0 || sscanf(line, "%15s %63s %63s", state, version, extra) != 3)
        return -1;
    return 0;
}

static void remove_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        for (struct dirent *e; d && (e = readdir(d));) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            char child[1024];
            snprintf(child, sizeof child, "%s/%s", path, e->d_name);
            remove_tree(child);
        }
        if (d)
            closedir(d);
        rmdir(path);
    } else {
        unlink(path);
    }
}

// Every file and folder 0777 and root's: the console refuses to start an app otherwise
// (CE-107750-0). check: only look, 0 when all are so.
static int open_modes(const char *path, int check)
{
    struct stat st;
    if (lstat(path, &st) != 0 || !(S_ISDIR(st.st_mode) || S_ISREG(st.st_mode)))
        return -1;
    if (!check && (chmod(path, 0777) != 0
#ifdef __PROSPERO__
                   || chown(path, 0, 0) != 0
#endif
                   ))
        return -1;
    if (check && ((st.st_mode & 07777) != 0777
#ifdef __PROSPERO__
                  || st.st_uid != 0 || st.st_gid != 0
#endif
                  ))
        return -1;
    if (!S_ISDIR(st.st_mode))
        return 0;
    DIR *d = opendir(path);
    int r = d ? 0 : -1;
    for (struct dirent *e; d && !r && (e = readdir(d));) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char child[1024];
        snprintf(child, sizeof child, "%s/%s", path, e->d_name);
        r = open_modes(child, check);
    }
    if (d)
        closedir(d);
    return r;
}

static int copy_file(const char *from, const char *to)
{
    int in = open(from, O_RDONLY), out = in >= 0 ? open(to, O_WRONLY | O_CREAT | O_TRUNC, 0777) : -1;
    char *buf = malloc(1 << 20); // not on the stack: a thread's is small on the console
    ssize_t n = buf ? 0 : -1;
    while (buf && out >= 0 && (n = read(in, buf, 1 << 20)) > 0)
        if (write(out, buf, (size_t)n) != n)
            n = -1;
    free(buf);
    int ok = in >= 0 && out >= 0 && n == 0 && fsync(out) == 0;
    if (in >= 0)
        close(in);
    if (out >= 0)
        close(out);
    return ok ? 0 : -1;
}

// The console keeps its own copies of an app's sce_sys files: refresh the ones it has.
static void refresh_registered(void)
{
    char src[300];
    snprintf(src, sizeof src, "%s/sce_sys", app_dir);
    DIR *d = opendir(src);
    for (struct dirent *e; d && (e = readdir(d));)
        for (size_t i = 0; i < sizeof registered / sizeof *registered; i++) {
            char from[600], to[600];
            struct stat st;
            snprintf(from, sizeof from, "%s/%s", src, e->d_name);
            snprintf(to, sizeof to, "%s/%s", registered[i], e->d_name);
            char tmp[620];
            snprintf(tmp, sizeof tmp, "%s.new", to); // beside it: the rename is atomic, no half file
            if (stat(from, &st) == 0 && S_ISREG(st.st_mode) && stat(to, &st) == 0 && S_ISREG(st.st_mode) &&
                (copy_file(from, tmp) != 0 || rename(tmp, to) != 0))
                unlink(tmp);
        }
    if (d)
        closedir(d);
}

// ---- The swap: whole top-level entries, assets first and eboot.bin last ----

static const char *first[] = {"assets", "version.txt", "sce_sys", "sce_module"};

static int entry_rank(const char *name)
{
    for (int i = 0; i < 4; i++)
        if (strcmp(name, first[i]) == 0)
            return i;
    return strcmp(name, "eboot.bin") == 0 ? 6 : 5;
}

static int by_rank(const void *a, const void *b)
{
    const char *x = *(const char *const *)a, *y = *(const char *const *)b;
    int r = entry_rank(x) - entry_rank(y);
    return r ? r : strcmp(x, y);
}

// The top-level names in dir, in swap order. Caller frees each and the list.
static int entries(const char *dir, char ***out)
{
    DIR *d = opendir(dir);
    if (!d)
        return -1;
    char **list = NULL;
    int n = 0;
    for (struct dirent *e; (e = readdir(d));) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char **more = realloc(list, sizeof *list * (size_t)(n + 1));
        if (!more)
            break;
        list = more;
        list[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(list, (size_t)n, sizeof *list, by_rank);
    *out = list;
    return n;
}

static void free_entries(char **list, int n)
{
    for (int i = 0; i < n; i++)
        free(list[i]);
    free(list);
}

static int exists(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

// Moves the staged entries in; the replaced ones go to previous/, and the names that had
// none are listed in "added" (a restore moves those out). Undone on any failure. finish:
// a swap cut short, carried on (an entry is either still staged or in place).
static int swap_in(int finish)
{
    char staged[300], previous[300], added[300];
    snprintf(staged, sizeof staged, "%s/staged", work);
    snprintf(previous, sizeof previous, "%s/previous", work);
    snprintf(added, sizeof added, "%s/added", work);
    mkdir(previous, 0777);
    char **list;
    int n = entries(staged, &list), done = 0, r = n >= 0 ? 0 : -1;
    if (n < 0 && finish && errno == ENOENT) // the swap ended before the journal said so
        n = 0, r = 0, list = NULL;
    FILE *add = r == 0 ? fopen(added, "a") : NULL;
    for (; r == 0 && done < n; done++) {
        char from[600], to[600], keep[600];
        snprintf(from, sizeof from, "%s/%s", staged, list[done]);
        snprintf(to, sizeof to, "%s/%s", app_dir, list[done]);
        snprintf(keep, sizeof keep, "%s/%s", previous, list[done]);
        if (exists(to) && !exists(keep)) {
            if (rename(to, keep) != 0)
                r = -1;
        } else if (!exists(to) && !exists(keep) && add) {
            fprintf(add, "%s\n", list[done]);
        }
        if (r == 0 && rename(from, to) != 0) {
            if (exists(keep) && !exists(to))
                rename(keep, to);
            r = -1;
        }
        if (r == 0 && (sync_dir(app_dir) != 0 || sync_dir(previous) != 0))
            LOG("update: fsync after %s failed", list[done]);
    }
    if (add)
        fclose(add);
    if (r != 0 && !finish) // back as it was: each moved entry to staged/, its old one back
        for (int i = done - 2; i >= 0; i--) {
            char from[600], to[600], keep[600];
            snprintf(from, sizeof from, "%s/%s", staged, list[i]);
            snprintf(to, sizeof to, "%s/%s", app_dir, list[i]);
            snprintf(keep, sizeof keep, "%s/%s", previous, list[i]);
            rename(to, from);
            if (exists(keep))
                rename(keep, to);
        }
    if (r != 0)
        LOG("update: swap failed at %s (%s)", done && done <= n ? list[done - 1] : "?", strerror(errno));
    if (n > 0)
        free_entries(list, n);
    return r;
}

// previous/ back in place; what the new version added goes out. Carried on if cut short.
static int restore(void)
{
    char previous[300], added[300], discard[300];
    snprintf(previous, sizeof previous, "%s/previous", work);
    snprintf(added, sizeof added, "%s/added", work);
    snprintf(discard, sizeof discard, "%s/discard", work);
    mkdir(discard, 0777);
    int r = 0;
    FILE *f = fopen(added, "r");
    for (char name[256]; f && fgets(name, sizeof name, f);) {
        name[strcspn(name, "\n")] = 0;
        char from[600], to[600];
        snprintf(from, sizeof from, "%s/%s", app_dir, name);
        snprintf(to, sizeof to, "%s/%s", discard, name);
        if (name[0] && !strchr(name, '/') && exists(from) && rename(from, to) != 0)
            r = -1;
    }
    if (f)
        fclose(f);
    char **list;
    int n = entries(previous, &list);
    for (int i = 0; i < n; i++) {
        char from[600], to[600], out[600];
        snprintf(from, sizeof from, "%s/%s", previous, list[i]);
        snprintf(to, sizeof to, "%s/%s", app_dir, list[i]);
        snprintf(out, sizeof out, "%s/%s.new", discard, list[i]);
        if ((exists(to) && rename(to, out) != 0) || rename(from, to) != 0)
            r = -1;
    }
    if (n > 0)
        free_entries(list, n);
    sync_dir(app_dir);
    return n < 0 ? -1 : r;
}

// ---- Is the app running? ----

// A running title has /mnt/sandbox/<TITLE>_NNN/app0; the folder outlives the app with only
// its data mount inside (as Prospero Store's updater found).
static int app_running(void)
{
    DIR *d = opendir(sandbox_dir);
    int running = 0;
    for (struct dirent *e; d && !running && (e = readdir(d));) {
        if (strncmp(e->d_name, "PPSA99072_", 10) != 0)
            continue;
        char path[340];
        snprintf(path, sizeof path, "%s/%s/app0", sandbox_dir, e->d_name);
        running = exists(path);
    }
    if (d)
        closedir(d);
    return running;
}

// ---- Check ----

static int github_host(const char *host)
{
    return strcmp(host, "api.github.com") == 0 || strcmp(host, "github.com") == 0 ||
           strcmp(host, "objects.githubusercontent.com") == 0 ||
           strcmp(host, "release-assets.githubusercontent.com") == 0;
}

typedef struct {
    char *data;
    size_t len, cap;
} buffer;

static int to_buffer(void *user, const void *data, size_t len)
{
    buffer *b = user;
    if (b->len + len + 1 > b->cap) {
        size_t cap = (b->len + len + 1) * 2;
        char *p = realloc(b->data, cap);
        if (!p)
            return 0;
        b->data = p, b->cap = cap;
    }
    memcpy(b->data + b->len, data, len);
    b->len += len;
    b->data[b->len] = 0;
    return 1;
}

static int fetch(const char *url, unsigned long long limit, buffer *b)
{
    memset(b, 0, sizeof *b);
    int status = http_get_url(url, github_host, limit, to_buffer, b);
    if (status != 200 || !b->data) {
        free(b->data);
        b->data = NULL;
        return status;
    }
    return 200;
}

// A zip that rolled back is not offered by the launch check again; Check for updates in
// Settings (force) offers it anyway: a person who presses it after a rollback wants a retry.
static int marked_bad(const unsigned char sha[32])
{
    char hex[65], line[80];
    for (int i = 0; i < 32; i++)
        snprintf(hex + 2 * i, 3, "%02x", sha[i]);
    FILE *f = fopen(bad_file, "r");
    int bad = 0;
    while (f && !bad && fgets(line, sizeof line, f))
        bad = strncmp(line, hex, 64) == 0;
    if (f)
        fclose(f);
    return bad;
}

static const char *asset_url(cJSON *release, const char *name, double *size)
{
    cJSON *a;
    cJSON_ArrayForEach(a, cJSON_GetObjectItem(release, "assets"))
        if (strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(a, "name")) ?: "", name) == 0) {
            if (size)
                *size = cJSON_GetNumberValue(cJSON_GetObjectItem(a, "size"));
            return cJSON_GetStringValue(cJSON_GetObjectItem(a, "browser_download_url"));
        }
    return NULL;
}

// The release's text for the app: at most 16 KB, cut on a character (the app's JSON parser
// drops a reply with broken UTF-8), \r\n as \n. Caller frees.
static char *notes_of(const char *body)
{
    size_t n = body ? strlen(body) : 0;
    if (n > 16384)
        for (n = 16384; n > 0 && (body[n] & 0xC0) == 0x80; n--)
            ;
    char *notes = strndup(body ? body : "", n);
    for (char *p = notes, *q = notes; notes; p++)
        if (*p != '\r' && !(*q++ = *p))
            break;
    return notes;
}

// One release, checked: signed by our keys, newer than installed, for it, not marked bad.
static int try_release(cJSON *rel, const char *installed, int force)
{
    const char *txt_url = asset_url(rel, "update.txt", NULL), *sig_url = asset_url(rel, "update.sig", NULL);
    const char *tag = cJSON_GetStringValue(cJSON_GetObjectItem(rel, "tag_name"));
    if (!txt_url || !sig_url || !tag || tag[0] != 'v')
        return -1;
    buffer txt, sig;
    if (fetch(txt_url, 4096, &txt) != 200)
        return -1;
    int ok = fetch(sig_url, 128, &sig) == 200;
    update_manifest m;
    double size = 0;
    const char *zip = NULL;
    if (ok && !update_signature_ok(txt.data, txt.len, (unsigned char *)sig.data, sig.len, update_keys,
                                   (int)(sizeof update_keys / sizeof *update_keys))) {
        LOG("update: %s is not signed by the project's keys", tag);
        ok = 0;
    }
    ok = ok && update_manifest_parse(txt.data, &m) == 0 && strcmp(m.version, tag + 1) == 0 &&
         update_version_cmp(m.version, installed) > 0 && update_version_cmp(installed, m.min_from) >= 0 &&
         (force || !marked_bad(m.sha256)) && (zip = asset_url(rel, m.asset, &size)) && size == (double)m.size &&
         strlen(zip) < sizeof u.zip_url;
    free(txt.data);
    free(sig.data);
    if (!ok)
        return -1;
    char *notes = notes_of(cJSON_GetStringValue(cJSON_GetObjectItem(rel, "body")));
    pthread_mutex_lock(&lock);
    u.manifest = m;
    snprintf(u.zip_url, sizeof u.zip_url, "%s", zip);
    snprintf(u.version, sizeof u.version, "%s", m.version);
    free(u.notes);
    u.notes = notes;
    u.total = m.size;
    u.done = 0;
    u.state = "available";
    pthread_mutex_unlock(&lock);
    LOG("update: %s is available", m.version);
    return 0;
}

static void check(int force)
{
    char installed[64];
    installed_version(installed, sizeof installed);
    if (!update_version_ok(installed)) {
        set_state("none", "no installed version to update");
        return;
    }
    set_state("checking", NULL);
    buffer list;
    int status = fetch(RELEASES, 2u << 20, &list);
    cJSON *rels = status == 200 ? cJSON_Parse(list.data) : NULL;
    free(list.data);
    if (!cJSON_IsArray(rels)) {
        set_state("none", status == 403 || status == 429 ? "GitHub is busy; try later" : "Could not reach GitHub");
        cJSON_Delete(rels);
        return;
    }
    // Newest first, prereleases only for a prerelease: up to three tries.
    int tried = 0, found = -1;
    for (const char *best = NULL; tried < 3 && found != 0; tried++, best = NULL) {
        cJSON *r, *pick = NULL;
        cJSON_ArrayForEach(r, rels)
        {
            const char *tag = cJSON_GetStringValue(cJSON_GetObjectItem(r, "tag_name"));
            if (cJSON_IsTrue(cJSON_GetObjectItem(r, "draft")) || cJSON_IsTrue(cJSON_GetObjectItem(r, "tried")) ||
                !tag || tag[0] != 'v' || !update_version_ok(tag + 1) || update_version_cmp(tag + 1, installed) <= 0 ||
                (update_prerelease(tag + 1) && !update_prerelease(installed)))
                continue;
            if (!best || update_version_cmp(tag + 1, best) > 0)
                best = tag + 1, pick = r;
        }
        if (!pick)
            break;
        cJSON_AddTrueToObject(pick, "tried");
        found = try_release(pick, installed, force);
    }
    cJSON_Delete(rels);
    pthread_mutex_lock(&lock);
    snprintf(u.installed, sizeof u.installed, "%s", installed);
    u.checked = time(NULL);
    if (found != 0)
        u.state = "none";
    pthread_mutex_unlock(&lock);
}

// ---- Download and stage ----

typedef struct {
    int fd;
    mbedtls_sha256_context sha;
} download;

static int to_file(void *user, const void *data, size_t len)
{
    download *d = user;
    mbedtls_sha256_update(&d->sha, data, len);
    pthread_mutex_lock(&lock);
    u.done += len;
    pthread_mutex_unlock(&lock);
    return write(d->fd, data, len) == (ssize_t)len;
}

static void unpacked(uint64_t done, uint64_t total)
{
    pthread_mutex_lock(&lock);
    u.done = done, u.total = total;
    pthread_mutex_unlock(&lock);
}

static int stage(void)
{
    update_manifest m;
    char url[1024], installed[64], path[300], staged[300], why[96] = "";
    pthread_mutex_lock(&lock);
    m = u.manifest;
    snprintf(url, sizeof url, "%s", u.zip_url);
    u.done = 0;
    u.total = m.size;
    u.state = "downloading";
    pthread_mutex_unlock(&lock);
    installed_version(installed, sizeof installed);
    remove_tree(work);
    if (mkdir(work, 0777) != 0)
        return set_state("failed", "Cannot write next to the app"), -1;
    snprintf(path, sizeof path, "%s/new.zip", work);
    download d = {.fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600)};
    mbedtls_sha256_init(&d.sha);
    mbedtls_sha256_starts(&d.sha, 0);
    int status = d.fd >= 0 ? http_get_url(url, github_host, m.size, to_file, &d) : -1;
    unsigned char sha[32];
    mbedtls_sha256_finish(&d.sha, sha);
    mbedtls_sha256_free(&d.sha);
    int ok = d.fd >= 0 && fsync(d.fd) == 0;
    if (d.fd >= 0)
        close(d.fd);
    struct stat st;
    if (status != 200 || !ok || stat(path, &st) != 0 || (uint64_t)st.st_size != m.size ||
        memcmp(sha, m.sha256, 32) != 0) {
        remove_tree(work);
        return set_state("failed", status != 200 ? "Download failed" : "Download does not match the release"), -1;
    }
    set_state("unpacking", NULL);
    snprintf(staged, sizeof staged, "%s/staged", work);
    char from[600], to[600], v[64] = "";
    ok = mkdir(staged, 0777) == 0 && update_unzip(path, staged, MAX_UNPACKED, unpacked, why, sizeof why) == 0;
    unlink(path);
    const char *need[] = {"eboot.bin", "sce_sys/param.json", "assets/ps5-discord.elf"};
    for (int i = 0; ok && i < 3; i++) {
        snprintf(to, sizeof to, "%s/%s", staged, need[i]);
        if (!exists(to))
            ok = 0, snprintf(why, sizeof why, "release lacks %s", need[i]);
    }
    snprintf(to, sizeof to, "%s/version.txt", staged);
    if (ok && (read_line(to, v, sizeof v) != 0 || strcmp(v, m.version) != 0))
        ok = 0, snprintf(why, sizeof why, "release is not %s", m.version);
    // The installed core, for an app whose new core will not start (client.cpp start_core).
    snprintf(from, sizeof from, "%s/assets/ps5-discord.elf", app_dir);
    snprintf(to, sizeof to, "%s/assets/ps5-discord.prev.elf", staged);
    if (ok && exists(from) && copy_file(from, to) != 0)
        ok = 0, snprintf(why, sizeof why, "cannot keep the old core");
    if (ok && (open_modes(staged, 0) != 0 || open_modes(staged, 1) != 0))
        ok = 0, snprintf(why, sizeof why, "cannot set file modes");
    // The zip's hash, so a restore by any core can mark this zip bad.
    snprintf(path, sizeof path, "%s/sha256", work);
    FILE *f = ok ? fopen(path, "w") : NULL;
    for (int i = 0; f && i < 32; i++)
        fprintf(f, "%02x", m.sha256[i]);
    if (f)
        ok = fclose(f) == 0;
    if (!ok || journal_write("staged", m.version, installed[0] ? installed : "-") != 0) {
        remove_tree(work);
        char msg[128];
        snprintf(msg, sizeof msg, "Unpacking failed: %s", why[0] ? why : "cannot write");
        return set_state("failed", msg), -1;
    }
    set_state("ready", NULL);
    LOG("update: %s staged", m.version);
    return 0;
}

// ---- Apply, the watch, restore ----

static int wait_closed(int seconds)
{
    for (int i = 0; i < seconds; i++) {
        if (!app_running()) {
            sleep(3); // let the console finish with its files
            if (!app_running())
                return 0;
        }
        sleep(1);
    }
    return -1;
}

static void start_watch(const char *version)
{
    pthread_mutex_lock(&lock);
    snprintf(u.watch, sizeof u.watch, "%s", version);
    u.proof_first = 0;
    u.fallback = u.drawn_old = 0;
    pthread_mutex_unlock(&lock);
}

static void apply(void)
{
    char state[16], version[64], from[64], installed[64];
    if (journal_read(state, version, from) != 0 || strcmp(state, "staged") != 0)
        return set_state("failed", "Nothing staged");
    set_state("applying", NULL);
    // A clean state first: the new app replaces this core at once only outside a call, and
    // it proves itself only to the new core.
    if (voice_wanted() || strcmp(bc_state(), "off") != 0) {
        LOG("update: leaving the call and Go Live first");
        core_end_calls();
    }
    if (wait_closed(60) != 0)
        return set_state("ready", "The app is still open");
    installed_version(installed, sizeof installed);
    char staged[300];
    snprintf(staged, sizeof staged, "%s/staged", work);
    if (strcmp(installed[0] ? installed : "-", from) != 0) { // installed by FTP meanwhile
        remove_tree(work);
        return set_state("none", "Another version was installed; update dropped");
    }
    if (app_running()) // opened again in the settle
        return set_state("ready", "The app is still open");
    if (open_modes(staged, 1) != 0 || journal_write("swapping", version, "0") != 0)
        return set_state("failed", "Staged files are not ready");
    if (swap_in(0) != 0) {
        journal_write("staged", version, from);
        return set_state("failed", "Could not replace the app's files; nothing changed");
    }
    journal_write("done", version, "0");
    refresh_registered();
    start_watch(version);
    set_state("none", NULL);
    LOG("update: %s in place", version);
    char text[128];
    snprintf(text, sizeof text, "Discord updated to %s. Open it again.", version);
    notify_system("Discord", text, NULL);
}

static void clear_watch(const char *why)
{
    LOG("update: %s; the previous version is removed", why);
    remove_tree(work);
    pthread_mutex_lock(&lock);
    u.watch[0] = 0;
    pthread_mutex_unlock(&lock);
}

// The zip of this version, not its number, is kept off the launch check: a fixed one may
// come under a new tag.
static void mark_bad(const char *version)
{
    char sha_hex[80] = "", path[300];
    snprintf(path, sizeof path, "%s/sha256", work);
    FILE *f = read_line(path, sha_hex, sizeof sha_hex) == 0 && strlen(sha_hex) == 64 ? fopen(bad_file, "a") : NULL;
    if (f) {
        fprintf(f, "%s %s\n", sha_hex, version);
        fclose(f);
    }
}

static void roll_back(const char *version)
{
    journal_write("restoring", version, "0");
    if (restore() != 0) {
        LOG("update: restore of the previous version failed");
        set_state("failed", "Going back failed: install a release by FTP");
        return;
    }
    refresh_registered();
    pthread_mutex_lock(&lock);
    u.watch[0] = 0;
    pthread_mutex_unlock(&lock);
    mark_bad(version);
    remove_tree(work);
    set_state("rolled_back", NULL);
    LOG("update: %s did not start; the previous version is back", version);
    notify_system("Discord", "The update did not start; the previous version is back.", NULL);
}

// One look at the watch: a launch of the new version must draw its first 60 frames while it
// talks to the new core; two launches without that and the previous version goes back.
static void watch_tick(int *was_running)
{
    char state[16], version[64], extra[64];
    if (journal_read(state, version, extra) != 0 || strcmp(state, "done") != 0)
        return;
    int running = app_running();
    pthread_mutex_lock(&lock);
    if (strcmp(u.watch, version) != 0) { // a core started while the watch is on
        snprintf(u.watch, sizeof u.watch, "%s", version);
        u.proof_first = 0;
        u.fallback = u.drawn_old = 0;
    }
    if (running && !*was_running)
        u.proof_first = 0, u.fallback = u.drawn_old = 0;
    int proven = !u.fallback && u.proof_first, unsure = !u.fallback && u.drawn_old;
    pthread_mutex_unlock(&lock);
    int ended = *was_running && !running;
    *was_running = running;
    if (proven)
        return clear_watch("the new version runs");
    if (!ended)
        return;
    char installed[64];
    installed_version(installed, sizeof installed);
    if (strcmp(installed, version) != 0)
        return clear_watch("another version was installed");
    if (unsure) { // the new app ran; whether the new core does, the next launch tells
        LOG("update: %s ran on the old core; not counted", version);
        return;
    }
    int fails = atoi(extra) + 1;
    LOG("update: %s stopped without starting (%d of 2)", version, fails);
    if (fails >= 2)
        return roll_back(version);
    snprintf(extra, sizeof extra, "%d", fails);
    journal_write("done", version, extra);
}

static void *run(void *unused)
{
    (void)unused;
    int was_running = app_running(); // the core may start under a running app: that launch counts
    for (;;) {
        pthread_mutex_lock(&lock);
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        t.tv_sec += 1;
        if (!u.want_check && !u.want_start && !u.want_apply)
            pthread_cond_timedwait(&wake, &lock, &t);
        int do_check = u.want_check, do_start = u.want_start, do_apply = u.want_apply, force = u.force;
        u.want_check = u.want_start = u.want_apply = u.force = 0;
        int showing = strcmp(u.state, "checking") == 0;
        time_t since = time(NULL) - u.checked;
        pthread_mutex_unlock(&lock);
        char state[16], version[64], extra[64];
        int journal = journal_read(state, version, extra) == 0;
        if (do_check && !journal && (force || since >= 3600))
            check(force);
        else if (do_check && showing)
            set_state("none", NULL); // Check shows "checking" at once; nothing to do this time
        if (do_start)
            stage();
        if (do_apply) {
            apply();
            was_running = app_running(); // the launch the swap waited out is no launch of the new version
        }
        watch_tick(&was_running);
    }
    return NULL;
}

// ---- At start, and the control API ----

void update_init(const char *app, const char *work_dir, const char *bad, const char *self)
{
    snprintf(core_version, sizeof core_version, "%s", self);
    snprintf(app_dir, sizeof app_dir, "%s", app);
    snprintf(work, sizeof work, "%s", work_dir);
    snprintf(bad_file, sizeof bad_file, "%s", bad);
    char state[16], version[64], extra[64];
    if (journal_read(state, version, extra) != 0) {
        remove_tree(work); // a download a power cut ended
    } else if (strcmp(state, "swapping") == 0) {
        LOG("update: finishing the swap to %s", version);
        if (swap_in(1) == 0 && journal_write("done", version, "0") == 0)
            refresh_registered();
    } else if (strcmp(state, "restoring") == 0) {
        LOG("update: finishing going back from %s", version);
        restore();
        mark_bad(version);
        remove_tree(work);
    } else if (strcmp(state, "staged") == 0) {
        pthread_mutex_lock(&lock);
        u.state = "ready";
        snprintf(u.version, sizeof u.version, "%s", version);
        pthread_mutex_unlock(&lock);
    }
    installed_version(u.installed, sizeof u.installed);
    // A stack of its own size: the console's default is small (a 64 KB buffer overflowed it).
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    if (pthread_create(&t, &attr, run, NULL) != 0)
        LOG("update: no thread");
    pthread_attr_destroy(&attr);
}

void update_check(int force)
{
    pthread_mutex_lock(&lock);
    if (strcmp(u.state, "rolled_back") == 0 && !force)
        u.state = "none"; // the app has shown it
    u.want_check = 1;
    u.force |= force;
    if (force && (strcmp(u.state, "none") == 0 || strcmp(u.state, "failed") == 0))
        u.state = "checking"; // at once: the next /api/state says so
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
}

int update_start(char *why, size_t cap)
{
    struct statvfs fs;
    const char *no = NULL;
    pthread_mutex_lock(&lock);
    uint64_t size = u.manifest.size;
    if (strcmp(u.state, "available") != 0 && strcmp(u.state, "failed") != 0)
        no = "No update to start";
    else if (!size)
        no = "No update to start";
    pthread_mutex_unlock(&lock);
    if (!no && statvfs(app_dir, &fs) == 0 && (uint64_t)fs.f_bavail * fs.f_frsize < 3 * size)
        no = "Not enough free space";
    else if (!no && exists(work)) {
        char state[16], version[64], extra[64];
        if (journal_read(state, version, extra) == 0 && strcmp(state, "staged") != 0)
            no = "An update is still being checked";
    }
    if (no) {
        snprintf(why, cap, "%s", no);
        return -1;
    }
    pthread_mutex_lock(&lock);
    u.want_start = 1;
    u.state = "downloading";
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
    return 0;
}

int update_apply(void)
{
    pthread_mutex_lock(&lock);
    int ok = strcmp(u.state, "ready") == 0;
    if (ok) {
        u.want_apply = 1;
        u.state = "applying";
        pthread_cond_signal(&wake);
    }
    pthread_mutex_unlock(&lock);
    return ok ? 0 : -1;
}

void update_seen(const char *app_version, int drawn, int fallback)
{
    pthread_mutex_lock(&lock);
    if (u.watch[0]) {
        u.fallback |= fallback;
        // Not at the old core the new app talks to until it replaces it: the new core may not start.
        if (drawn && app_version && strcmp(app_version, u.watch) == 0) {
            if (strcmp(core_version, u.watch) != 0)
                u.drawn_old = 1;
            else if (!u.proof_first)
                u.proof_first = now_ms();
        }
    }
    pthread_mutex_unlock(&lock);
}

// The app's own Quit, which stops this core next: a proven launch ends the watch now (no tick
// would see it after the quit), so previous/ does not wait for the next start.
void update_bye(void)
{
    pthread_mutex_lock(&lock);
    int proven = u.watch[0] && u.proof_first && !u.fallback;
    pthread_mutex_unlock(&lock);
    char state[16], version[64], extra[64];
    if (proven && journal_read(state, version, extra) == 0 && strcmp(state, "done") == 0)
        clear_watch("the new version quit cleanly");
}

cJSON *update_json(void)
{
    cJSON *o = cJSON_CreateObject();
    pthread_mutex_lock(&lock);
    cJSON_AddStringToObject(o, "state", u.state);
    cJSON_AddStringToObject(o, "version", u.version);
    cJSON_AddStringToObject(o, "installed", u.installed);
    cJSON_AddStringToObject(o, "notes", u.notes ? u.notes : "");
    cJSON_AddNumberToObject(o, "done", (double)u.done);
    cJSON_AddNumberToObject(o, "total", (double)u.total);
    cJSON_AddStringToObject(o, "error", u.error);
    pthread_mutex_unlock(&lock);
    return o;
}
