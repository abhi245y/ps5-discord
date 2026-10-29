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
#include <mbedtls/gcm.h>
#include <mbedtls/sha256.h>
#ifdef __PROSPERO__
int sceKernelGetOpenPsId(void *out); // 16 bytes, this console's
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen); // platform/entropy_ps5.c
#else
#include <sys/random.h>
#endif

// The session on disk: "PDS1", a 12-byte nonce, the 16-byte GCM tag, then the token
// encrypted under a key made from this console's OpenPSID. A copy of the file is no use
// anywhere else, nor under another name (the name is authenticated). Code running on the
// console itself can still make the key: this guards copies, not the console.
#define SESSION_MAGIC "PDS1"
enum { MAGIC_LEN = 4, NONCE_LEN = 12, TAG_LEN = 16, HEAD_LEN = MAGIC_LEN + NONCE_LEN + TAG_LEN };

static int console_key(unsigned char key[32])
{
    unsigned char id[16] = {0};
#ifdef __PROSPERO__
    if (sceKernelGetOpenPsId(id) != 0)
        return -1;
#else
    memcpy(id, "host build only", 16); // the self-checks; no console to bind to
#endif
    unsigned char any = 0;
    for (size_t i = 0; i < sizeof id; ++i)
        any |= id[i];
    if (!any)
        return -1;
    unsigned char in[32 + sizeof id] = "ps5-discord session key v1";
    memcpy(in + 32, id, sizeof id);
    int r = mbedtls_sha256(in, sizeof in, key, 0);
    storage_wipe(in, sizeof in);
    storage_wipe(id, sizeof id);
    return r == 0 ? 0 : -1;
}

static int random_bytes(unsigned char *out, size_t n)
{
#ifdef __PROSPERO__
    size_t got = 0;
    return mbedtls_hardware_poll(NULL, out, n, &got) == 0 && got == n ? 0 : -1;
#else
    return getrandom(out, n, 0) == (ssize_t)n ? 0 : -1;
#endif
}

// Encrypts (encrypt != 0) or decrypts n bytes; -1 when the key is missing or the tag is wrong.
static int seal(int encrypt, const char *name, const unsigned char *nonce, unsigned char *tag,
                const unsigned char *in, unsigned char *out, size_t n)
{
    unsigned char key[32];
    if (console_key(key) != 0)
        return -1;
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int r = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
    storage_wipe(key, sizeof key);
    if (r == 0)
        r = encrypt ? mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, n, nonce, NONCE_LEN, (const unsigned char *)name,
                                                strlen(name), in, out, TAG_LEN, tag)
                    : mbedtls_gcm_auth_decrypt(&gcm, n, nonce, NONCE_LEN, (const unsigned char *)name, strlen(name),
                                               tag, TAG_LEN, in, out);
    mbedtls_gcm_free(&gcm);
    return r == 0 ? 0 : -1;
}

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
    const size_t len = strlen(token);
    if (len == 0 || len > 512)
        return -1;
    unsigned char sealed[HEAD_LEN + 512];
    memcpy(sealed, SESSION_MAGIC, MAGIC_LEN);
    if (random_bytes(sealed + MAGIC_LEN, NONCE_LEN) != 0 ||
        seal(1, name, sealed + MAGIC_LEN, sealed + MAGIC_LEN + NONCE_LEN, (const unsigned char *)token,
             sealed + HEAD_LEN, len) != 0)
        return -1; // never written in the clear
    int fd = mkstemp(temp);
    if (fd < 0)
        return -1;
    int ok = fchmod(fd, 0600) == 0;
    size_t left = HEAD_LEN + len;
    const unsigned char *p = sealed;
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
    storage_wipe(sealed, sizeof sealed);
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

int session_load(const char *directory, const char *name, char *out, size_t cap)
{
    if (!out || cap < 2)
        return -1;
    out[0] = 0;
    char path[512];
    if (snprintf(path, sizeof path, "%s/%s", directory, name) >= (int)sizeof path)
        return -1;
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    unsigned char data[HEAD_LEN + 512];
    ssize_t n = read(fd, data, sizeof data);
    close(fd);
    int result = -1;
    if (n > HEAD_LEN && memcmp(data, SESSION_MAGIC, MAGIC_LEN) == 0) {
        const size_t len = (size_t)n - HEAD_LEN;
        if (len < cap && seal(0, name, data + MAGIC_LEN, data + MAGIC_LEN + NONCE_LEN, data + HEAD_LEN,
                              (unsigned char *)out, len) == 0) {
            out[len] = 0;
            result = 0;
        }
    } else if (n > 0) { // written by an older build, in the clear: the caller saves it again
        size_t len = (size_t)n;
        while (len > 0 && (data[len - 1] == '\n' || data[len - 1] == '\r' || data[len - 1] == ' '))
            len--;
        if (len > 0 && len < cap) {
            memcpy(out, data, len);
            out[len] = 0;
            result = 1;
        }
    }
    storage_wipe(data, sizeof data);
    if (result < 0)
        storage_wipe(out, cap);
    return result;
}
