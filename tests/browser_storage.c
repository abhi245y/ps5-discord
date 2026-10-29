#define _GNU_SOURCE // memmem
#include "browser_storage.h"
#include <assert.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void set_value(sqlite3 *db, const char *value)
{
    unsigned char blob[2048];
    size_t n = strlen(value);
    assert(n * 2 < sizeof blob);
    for (size_t i = 0; i < n; ++i) { blob[i * 2] = (unsigned char)value[i]; blob[i * 2 + 1] = 0; }
    sqlite3_stmt *q;
    assert(sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO ItemTable VALUES('token',?)", -1, &q, NULL) == SQLITE_OK);
    assert(sqlite3_bind_blob(q, 1, blob, (int)n * 2, SQLITE_TRANSIENT) == SQLITE_OK);
    assert(sqlite3_step(q) == SQLITE_DONE);
    sqlite3_finalize(q);
}

int main(void)
{
    char directory[] = "/tmp/discord-storage-test-XXXXXX";
    assert(mkdtemp(directory));
    char path[512], out[256], target[512], link[512];
    snprintf(path, sizeof path, "%s/browser.db", directory);
    assert(browser_storage_token(path, out, sizeof out) == -1);
    assert(access(path, F_OK) != 0); // missing browser DB is never created
    sqlite3 *db;
    assert(sqlite3_open(path, &db) == SQLITE_OK);
    assert(sqlite3_exec(db, "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0; CREATE TABLE ItemTable(key TEXT UNIQUE,value BLOB)", NULL, NULL, NULL) == SQLITE_OK);
    const char *valid = "test_session.ABCDEFGHIJKLMNOPQRSTUVWXYZ_123456789";
    char quoted[256];
    snprintf(quoted, sizeof quoted, "\"%s\"", valid);
    set_value(db, quoted);
    assert(browser_storage_token(path, out, sizeof out) == 0 && strcmp(out, valid) == 0);
    // Uncommitted replacement must not be imported; committed WAL change must be.
    assert(sqlite3_exec(db, "BEGIN", NULL, NULL, NULL) == SQLITE_OK);
    set_value(db, "null");
    assert(browser_storage_token(path, out, sizeof out) == 0 && strcmp(out, valid) == 0);
    assert(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    assert(browser_storage_token(path, out, sizeof out) == -1 && out[0] == 0);
    const char *bad[] = {"{}", "\"short\"", "\"AAAAAAAAAAAAAAAAAAAA\\n\"", "\"AAAAAAAAAAAAAAAAAAAA\"junk"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        set_value(db, bad[i]);
        assert(browser_storage_token(path, out, sizeof out) == -1);
    }
    set_value(db, quoted);
    assert(browser_storage_token(path, out, 10) == -1);
    sqlite3_close(db);
    assert(browser_storage_token(path, out, sizeof out) == 0); // checkpointed WAL DB
    snprintf(link, sizeof link, "%s/link.db", directory);
    assert(symlink(path, link) == 0);
    assert(browser_storage_token(link, out, sizeof out) == -1);
    assert(session_save(directory, "token", valid) == 0);
    snprintf(target, sizeof target, "%s/token", directory);
    struct stat st;
    assert(stat(target, &st) == 0 && (st.st_mode & 0777) == 0600);
    assert(stat(directory, &st) == 0 && (st.st_mode & 0777) == 0700);
    // On disk only sealed: the token is not in the file, and it reads back only under its name.
    unsigned char raw[1024];
    FILE *f = fopen(target, "rb");
    assert(f);
    size_t n = fread(raw, 1, sizeof raw, f); fclose(f);
    assert(n == 32 + strlen(valid) && memcmp(raw, "PDS1", 4) == 0);
    assert(!memmem(raw, n, "test_session", 12));
    assert(session_load(directory, "token", out, sizeof out) == 0 && strcmp(out, valid) == 0);
    snprintf(path, sizeof path, "%s/token-copy", directory);
    f = fopen(path, "wb"); fwrite(raw, 1, n, f); fclose(f);
    assert(session_load(directory, "token-copy", out, sizeof out) == -1 && out[0] == 0); // the name is sealed in
    raw[n - 1] ^= 1;
    f = fopen(target, "wb"); fwrite(raw, 1, n, f); fclose(f);
    assert(session_load(directory, "token", out, sizeof out) == -1); // tampered
    // An older build's file, in the clear: read once (1), for the caller to save again.
    f = fopen(target, "w"); fprintf(f, "%s\n", valid); fclose(f);
    assert(session_load(directory, "token", out, sizeof out) == 1 && strcmp(out, valid) == 0);
    assert(session_save(directory, "token", out) == 0);
    assert(session_load(directory, "token", out, sizeof out) == 0 && strcmp(out, valid) == 0);
    unlink(path);
    snprintf(path, sizeof path, "%s/browser.db", directory);
    assert(session_save(link, "token", "replacement") == -1); // never follows a destination symlink
    unlink(target); unlink(link); unlink(path);
    snprintf(path, sizeof path, "%s/browser.db-wal", directory); unlink(path);
    snprintf(path, sizeof path, "%s/browser.db-shm", directory); unlink(path);
    rmdir(directory);
    puts("browser storage: WAL isolation, malformed values, symlinks, atomic permissions, sealed session passed");
}
