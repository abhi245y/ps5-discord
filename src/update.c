// SPDX-License-Identifier: AGPL-3.0-or-later
// The updater's I/O (docs/updater-plan.md). One thread does everything in order: a check,
// a download, a swap, and going back to the previous version when asked.
//
// The work folder sits beside the app folder (same filesystem, so renames are atomic) and
// holds a one-line journal: "staged V F", "swapping V F", "done V F" or "restoring V F",
// F the version before V. A core of any version reads it at start and carries on. After a
// swap, previous/ keeps F until the next download: Settings can put it back.
#include "update.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
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
static const char *registered[] = {"/user/appmeta/PPSA99072", "/user/app/PPSA99072/sce_sys"};
// The keys a release must be signed by: the tests put their own in.
static const unsigned char (*keys)[65] = update_keys;
static int key_count = (int)(sizeof update_keys / sizeof *update_keys);

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static struct {
    const char *state; // none checking available downloading unpacking ready applying restoring failed
    char version[64], installed[64], error[96];
    char check_error[96]; // why the last check found nothing ("" when it worked)
    char previous[64];    // the version previous/ holds, which Settings can put back
    char *notes;
    uint64_t done, total;
    int want_check, force, want_start, want_apply, want_restore;
    time_t checked;
    // The release found: its manifest, checked, and where its zip is.
    update_manifest manifest;
    char zip_url[1024];
} u = {.state = "none"};

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

// The newest release, checked: signed by our keys, for this version, not marked bad. 0 and
// it is offered; else why (empty: nothing to say, a zip that was put back).
static int try_release(cJSON *rel, const char *installed, int force, char *why, size_t cap)
{
    const char *txt_url = asset_url(rel, "update.txt", NULL), *sig_url = asset_url(rel, "update.sig", NULL);
    const char *tag = cJSON_GetStringValue(cJSON_GetObjectItem(rel, "tag_name"));
    snprintf(why, cap, "%s is not a valid release", tag);
    if (!txt_url || !sig_url)
        return -1;
    buffer txt, sig = {0};
    int status = fetch(txt_url, 4096, &txt);
    if (status == 200)
        status = fetch(sig_url, 128, &sig);
    update_manifest m;
    double size = 0;
    const char *zip = NULL;
    int ok = status == 200 && update_signature_ok(txt.data, txt.len, (unsigned char *)sig.data, sig.len, keys, key_count);
    if (status == 200 && !ok)
        LOG("update: %s is not signed by the project's keys", tag);
    ok = ok && update_manifest_parse(txt.data, &m) == 0 && strcmp(m.version, tag + 1) == 0 &&
         (zip = asset_url(rel, m.asset, &size)) && size == (double)m.size && strlen(zip) < sizeof u.zip_url;
    free(txt.data);
    free(sig.data);
    if (status != 200)
        snprintf(why, cap, "Could not reach GitHub");
    else if (ok && update_version_cmp(installed, m.min_from) < 0)
        snprintf(why, cap, "%s must be installed by hand", tag), ok = 0;
    else if (ok && !force && marked_bad(m.sha256))
        why[0] = 0, ok = 0;
    if (!ok)
        return -1;
    why[0] = 0;
    char *notes = notes_of(cJSON_GetStringValue(cJSON_GetObjectItem(rel, "body")));
    pthread_mutex_lock(&lock);
    u.manifest = m;
    snprintf(u.zip_url, sizeof u.zip_url, "%s", zip);
    snprintf(u.version, sizeof u.version, "%s", m.version);
    free(u.notes);
    u.notes = notes;
    u.total = m.size;
    u.done = 0;
    u.error[0] = 0;
    u.state = "available";
    pthread_mutex_unlock(&lock);
    LOG("update: %s is available", m.version);
    return 0;
}

// A check never takes away what is there: an offer or a staged update stays when GitHub
// cannot be reached, and why goes to check_error.
static void check(int force)
{
    char installed[64], why[96] = "", staged[64] = "";
    installed_version(installed, sizeof installed);
    pthread_mutex_lock(&lock);
    const char *keep = strcmp(u.state, "available") == 0 || strcmp(u.state, "ready") == 0 ? u.state : "none";
    if (strcmp(keep, "ready") == 0)
        snprintf(staged, sizeof staged, "%s", u.version);
    if (strcmp(keep, "none") == 0)
        u.state = "checking";
    pthread_mutex_unlock(&lock);
    cJSON *rels = NULL, *pick = NULL;
    if (!update_version_ok(installed)) {
        snprintf(why, sizeof why, "No installed version to update");
    } else {
        buffer list;
        int status = fetch(RELEASES, 2u << 20, &list);
        rels = status == 200 ? cJSON_Parse(list.data) : NULL;
        free(list.data);
        if (!cJSON_IsArray(rels))
            snprintf(why, sizeof why, status == 403 || status == 429 ? "GitHub is busy; try later" : "Could not reach GitHub");
    }
    // The newest only, prereleases only for a prerelease: if it fails, that is the news.
    cJSON *r, *all = cJSON_IsArray(rels) ? rels : NULL;
    const char *best = NULL;
    cJSON_ArrayForEach(r, all)
    {
        const char *tag = cJSON_GetStringValue(cJSON_GetObjectItem(r, "tag_name"));
        if (cJSON_IsTrue(cJSON_GetObjectItem(r, "draft")) || !tag || tag[0] != 'v' || !update_version_ok(tag + 1) ||
            update_version_cmp(tag + 1, installed) <= 0 || (update_prerelease(tag + 1) && !update_prerelease(installed)))
            continue;
        if (!best || update_version_cmp(tag + 1, best) > 0)
            best = tag + 1, pick = r;
    }
    int found = -1;
    if (pick && !(staged[0] && update_version_cmp(best, staged) <= 0))
        found = try_release(pick, installed, force, why, sizeof why);
    if (found == 0 && staged[0]) { // a newer one than the staged update: that goes
        LOG("update: %s replaces the staged %s", best, staged);
        remove_tree(work);
    }
    cJSON_Delete(rels);
    pthread_mutex_lock(&lock);
    snprintf(u.installed, sizeof u.installed, "%s", installed);
    snprintf(u.check_error, sizeof u.check_error, "%s", why);
    if (!why[0])
        u.checked = time(NULL);
    if (found != 0) // a list without a newer release ends an offer; a staged update stays
        u.state = why[0] || strcmp(keep, "ready") == 0 ? keep : "none";
    pthread_mutex_unlock(&lock);
    if (why[0])
        LOG("update: check: %s", why);
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
    u.previous[0] = 0; // previous/ goes with the old work folder
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

// ---- Apply and going back ----

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

// A clean state first: the app that opens next replaces this core, which it does at once
// only outside a call. Then the app gone, and still the version the journal is about.
static int ready_to_swap(const char *version)
{
    if (voice_wanted() || strcmp(bc_state(), "off") != 0) {
        LOG("update: leaving the call and Go Live first");
        core_end_calls();
    }
    char installed[64];
    if (wait_closed(60) != 0 || app_running())
        return -1;
    installed_version(installed, sizeof installed);
    if (strcmp(installed[0] ? installed : "-", version) != 0) { // installed by FTP meanwhile
        remove_tree(work);
        return -2;
    }
    return 0;
}

static void apply(void)
{
    char state[16], version[64], from[64], staged[300];
    if (journal_read(state, version, from) != 0 || strcmp(state, "staged") != 0)
        return set_state("failed", "Nothing staged");
    set_state("applying", NULL);
    int r = ready_to_swap(from);
    if (r == -2)
        return set_state("none", "Another version was installed; update dropped");
    if (r != 0)
        return set_state("ready", "The app is still open");
    snprintf(staged, sizeof staged, "%s/staged", work);
    if (open_modes(staged, 1) != 0 || journal_write("swapping", version, from) != 0)
        return set_state("failed", "Staged files are not ready");
    if (swap_in(0) != 0) {
        journal_write("staged", version, from);
        return set_state("failed", "Could not replace the app's files; nothing changed");
    }
    journal_write("done", version, from);
    refresh_registered();
    pthread_mutex_lock(&lock);
    snprintf(u.previous, sizeof u.previous, "%s", strcmp(from, "-") ? from : "");
    snprintf(u.installed, sizeof u.installed, "%s", version);
    pthread_mutex_unlock(&lock);
    set_state("none", NULL);
    LOG("update: %s in place", version);
    char text[128];
    snprintf(text, sizeof text, "Discord updated to %s. Open it again.", version);
    notify_system("Discord", text, NULL);
}

// The zip of a version put back, not its number, is kept off the launch check: a fixed one
// may come under a new tag.
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

// Settings' Restore: previous/ back in place once the app has closed.
static void go_back(void)
{
    char state[16], version[64], from[64];
    if (journal_read(state, version, from) != 0 || strcmp(state, "done") != 0)
        return set_state("none", "No previous version to go back to");
    int r = ready_to_swap(version);
    if (r == -2) {
        pthread_mutex_lock(&lock);
        u.previous[0] = 0;
        pthread_mutex_unlock(&lock);
        return set_state("none", "Another version was installed");
    }
    if (r != 0)
        return set_state("none", "The app is still open");
    journal_write("restoring", version, from);
    if (restore() != 0) {
        LOG("update: restore of the previous version failed");
        return set_state("failed", "Going back failed: install a release by FTP");
    }
    refresh_registered();
    mark_bad(version);
    remove_tree(work);
    pthread_mutex_lock(&lock);
    u.previous[0] = 0;
    snprintf(u.installed, sizeof u.installed, "%s", from);
    pthread_mutex_unlock(&lock);
    set_state("none", NULL);
    LOG("update: back to %s from %s", from, version);
    char text[128];
    snprintf(text, sizeof text, "Discord went back to %s. Open it again.", from);
    notify_system("Discord", text, NULL);
}

// What the journal says previous/ holds, if it still matches the installed version.
static void load_previous(void)
{
    char state[16], version[64], from[64], installed[64], path[300];
    installed_version(installed, sizeof installed);
    snprintf(path, sizeof path, "%s/previous", work);
    if (journal_read(state, version, from) != 0 || strcmp(state, "done") != 0)
        return;
    if (strcmp(installed, version) != 0 || !exists(path) || strcmp(from, "-") == 0) {
        remove_tree(work); // another version came by FTP: previous/ is not what is behind it
        return;
    }
    pthread_mutex_lock(&lock);
    snprintf(u.previous, sizeof u.previous, "%s", from);
    pthread_mutex_unlock(&lock);
}

static void *run(void *unused)
{
    (void)unused;
    // A stack of its own for the crash handler, so an overflow here is logged (main.c on_crash).
    stack_t alt = {.ss_sp = malloc(SIGSTKSZ * 4), .ss_size = SIGSTKSZ * 4};
    if (alt.ss_sp)
        sigaltstack(&alt, NULL);
    for (;;) {
        pthread_mutex_lock(&lock);
        while (!u.want_check && !u.want_start && !u.want_apply && !u.want_restore)
            pthread_cond_wait(&wake, &lock);
        int do_check = u.want_check, do_start = u.want_start, do_apply = u.want_apply, do_restore = u.want_restore;
        int force = u.force;
        u.want_check = u.want_start = u.want_apply = u.want_restore = u.force = 0;
        time_t since = time(NULL) - u.checked;
        pthread_mutex_unlock(&lock);
        // The asked-for work first: a check then sees what it left.
        if (do_start)
            stage();
        if (do_apply)
            apply();
        if (do_restore)
            go_back();
        if (do_check && (force || since >= 3600))
            check(force);
    }
    return NULL;
}

// ---- At start, and the control API ----

void update_init(const char *app, const char *work_dir, const char *bad)
{
    snprintf(app_dir, sizeof app_dir, "%s", app);
    snprintf(work, sizeof work, "%s", work_dir);
    snprintf(bad_file, sizeof bad_file, "%s", bad);
    char state[16], version[64], extra[64];
    if (journal_read(state, version, extra) != 0) {
        remove_tree(work); // a download a power cut ended
    } else if (strcmp(state, "swapping") == 0) {
        LOG("update: finishing the swap to %s", version);
        if (swap_in(1) == 0 && journal_write("done", version, extra) == 0)
            refresh_registered();
    } else if (strcmp(state, "restoring") == 0) {
        LOG("update: finishing going back from %s", version);
        restore();
        refresh_registered();
        mark_bad(version);
        remove_tree(work);
    } else if (strcmp(state, "staged") == 0) {
        pthread_mutex_lock(&lock);
        u.state = "ready";
        snprintf(u.version, sizeof u.version, "%s", version);
        pthread_mutex_unlock(&lock);
    }
    load_previous();
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
    if ((strcmp(u.state, "available") != 0 && strcmp(u.state, "failed") != 0) || !size)
        no = "No update to start";
    pthread_mutex_unlock(&lock);
    if (!no && statvfs(app_dir, &fs) == 0 && (uint64_t)fs.f_bavail * fs.f_frsize < 3 * size)
        no = "Not enough free space";
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

// The two that close the app: accepted only from a state where nothing else is going on.
static int ask(int *want, const char *from, const char *state)
{
    pthread_mutex_lock(&lock);
    int ok = strcmp(u.state, from) == 0 && (want != &u.want_restore || u.previous[0]);
    if (ok) {
        *want = 1;
        u.state = state;
        pthread_cond_signal(&wake);
    }
    pthread_mutex_unlock(&lock);
    return ok ? 0 : -1;
}

int update_apply(void)
{
    return ask(&u.want_apply, "ready", "applying");
}

int update_restore(void)
{
    return ask(&u.want_restore, "none", "restoring");
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
    cJSON_AddStringToObject(o, "check_error", u.check_error);
    cJSON_AddStringToObject(o, "previous", u.previous);
    pthread_mutex_unlock(&lock);
    return o;
}
