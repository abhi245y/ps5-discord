#include "browser_storage.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sqlite3.h>
#include <cJSON.h>

void storage_wipe(void *p, size_t n)
{
    volatile unsigned char *b = p;
    while (n--)
        *b++ = 0;
}

int browser_storage_token(const char *path, char *out, size_t cap)
{
    if (!out || !cap)
        return -1;
    out[0] = 0;
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > 8 * 1024 * 1024)
        return -1;
    sqlite3 *db = NULL;
    sqlite3_stmt *query = NULL;
    int result = -1;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOFOLLOW, NULL) != SQLITE_OK)
        goto done;
    sqlite3_busy_timeout(db, 100);
    sqlite3_limit(db, SQLITE_LIMIT_LENGTH, 16384);
    if (sqlite3_prepare_v2(db, "SELECT value FROM ItemTable WHERE key='token'", -1, &query, NULL) != SQLITE_OK ||
        sqlite3_step(query) != SQLITE_ROW || sqlite3_column_type(query, 0) != SQLITE_BLOB)
        goto done;
    const unsigned char *blob = sqlite3_column_blob(query, 0);
    int bytes = sqlite3_column_bytes(query, 0);
    // WebKit stores localStorage values as UTF-16LE. Tokens and JSON string
    // syntax are ASCII; reject non-ASCII and embedded NUL rather than truncate.
    char json[1024];
    if (!blob || bytes < 4 || bytes % 2 || bytes / 2 >= (int)sizeof json)
        goto done;
    for (int i = 0; i < bytes / 2; ++i) {
        if (blob[i * 2 + 1] || !blob[i * 2] || blob[i * 2] > 127)
            goto done;
        json[i] = (char)blob[i * 2];
    }
    json[bytes / 2] = 0;
    cJSON *value = cJSON_ParseWithOpts(json, NULL, 1);
    const char *token = cJSON_GetStringValue(value);
    if (token) {
        size_t n = strlen(token);
        if (n >= 20 && n < cap && n < 256 &&
            strspn(token, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") == n) {
            memcpy(out, token, n + 1);
            result = 0;
        }
        storage_wipe(value->valuestring, n);
    }
    cJSON_Delete(value);
    storage_wipe(json, sizeof json);
done:
    sqlite3_finalize(query);
    sqlite3_close(db);
    return result;
}

int session_save(const char *directory, const char *name, const char *token)
{
    struct stat st;
    if (mkdir(directory, 0700) != 0 && errno != EEXIST)
        return -1;
    if (lstat(directory, &st) != 0 || !S_ISDIR(st.st_mode) || chmod(directory, 0700) != 0)
        return -1;
    char temp[512], target[512];
    if (snprintf(temp, sizeof temp, "%s/%s.XXXXXX", directory, name) >= (int)sizeof temp ||
        snprintf(target, sizeof target, "%s/%s", directory, name) >= (int)sizeof target)
        return -1;
    int fd = mkstemp(temp);
    if (fd < 0)
        return -1;
    int ok = fchmod(fd, 0600) == 0;
    size_t left = strlen(token);
    const char *p = token;
    while (ok && left) {
        ssize_t n = write(fd, p, left);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) { ok = 0; break; }
        p += n;
        left -= (size_t)n;
    }
    if (ok && fsync(fd) != 0)
        ok = 0;
    if (close(fd) != 0)
        ok = 0;
    if (!ok || rename(temp, target) != 0) {
        unlink(temp);
        return -1;
    }
    int dir = open(directory, O_RDONLY); // so the rename itself survives a power cut
    if (dir >= 0) {
        fsync(dir);
        close(dir);
    }
    return 0;
}
