// SPDX-License-Identifier: AGPL-3.0-or-later
// The updater's pure checks: versions, the signed manifest, the release zip.
#include "update.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mbedtls/ecdsa.h>
#include <mbedtls/sha256.h>
#include <mbedtls/sha512.h>
#include <zlib.h>

// ---- Versions ----

typedef struct {
    long n[3];
    char pre[64];
    int dev; // a development build of this number: older than its release
} version;

static int parse_version(const char *s, version *v)
{
    memset(v, 0, sizeof *v);
    for (int i = 0; i < 3; i++) {
        if (!isdigit((unsigned char)*s))
            return -1;
        char *end;
        v->n[i] = strtol(s, &end, 10);
        s = end;
        if (i < 2 && *s++ != '.')
            return -1;
    }
    size_t n = strcspn(s, "+");
    if (n && *s != '-')
        return -1;
    if (n) {
        if (n - 1 >= sizeof v->pre || n == 1)
            return -1;
        memcpy(v->pre, s + 1, n - 1);
        v->pre[n - 1] = 0;
    }
    size_t p = strlen(v->pre);
    if (strcmp(v->pre, "dev") == 0)
        v->pre[0] = 0, v->dev = 1;
    else if (p > 4 && strcmp(v->pre + p - 4, "-dev") == 0)
        v->pre[p - 4] = 0, v->dev = 1;
    for (const char *c = v->pre; *c; c++)
        if (!isalnum((unsigned char)*c) && *c != '.' && *c != '-')
            return -1;
    return 0;
}

int update_version_ok(const char *v)
{
    version x;
    return parse_version(v, &x) == 0;
}

int update_prerelease(const char *v)
{
    version x;
    return parse_version(v, &x) == 0 && (x.pre[0] || x.dev);
}

static int numeric(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (!isdigit((unsigned char)s[i]))
            return 0;
    return n > 0;
}

static int cmp_pre(const char *a, const char *b)
{
    if (!*a || !*b)
        return !*a - !*b; // none is newer than any
    for (;;) {
        size_t na = strcspn(a, "."), nb = strcspn(b, ".");
        int r;
        if (numeric(a, na) && numeric(b, nb))
            r = na != nb ? (na > nb ? 1 : -1) : strncmp(a, b, na);
        else if (numeric(a, na) != numeric(b, nb))
            r = numeric(a, na) ? -1 : 1;
        else {
            r = strncmp(a, b, na < nb ? na : nb);
            if (!r && na != nb)
                r = na > nb ? 1 : -1;
        }
        if (r)
            return r > 0 ? 1 : -1;
        a += na, b += nb;
        if (!*a || !*b)
            return !*b - !*a; // more identifiers: newer
        a++, b++;
    }
}

int update_version_cmp(const char *a, const char *b)
{
    version x, y;
    if (parse_version(a, &x) != 0 || parse_version(b, &y) != 0)
        return 0;
    for (int i = 0; i < 3; i++)
        if (x.n[i] != y.n[i])
            return x.n[i] > y.n[i] ? 1 : -1;
    int r = cmp_pre(x.pre, y.pre);
    return r ? r : y.dev - x.dev;
}

// ---- The manifest and its signature ----

static int hex_bytes(const char *s, unsigned char *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (!isxdigit((unsigned char)s[2 * i]) || !isxdigit((unsigned char)s[2 * i + 1]) ||
            sscanf(s + 2 * i, "%2x", &v) != 1)
            return -1;
        out[i] = (unsigned char)v;
    }
    return s[2 * n] ? -1 : 0;
}

int update_manifest_parse(const char *text, update_manifest *m)
{
    memset(m, 0, sizeof *m);
    snprintf(m->min_from, sizeof m->min_from, "0.0.0");
    if (strncmp(text, "ps5-discord-update 1\n", 21) != 0)
        return -1;
    int have_sha = 0;
    for (const char *line = text + 21; *line;) {
        size_t n = strcspn(line, "\n");
        char key[32], value[160];
        if (n < sizeof value && sscanf(line, "%31s %159[^\n]", key, value) == 2) {
            if (strcmp(key, "version") == 0)
                snprintf(m->version, sizeof m->version, "%.63s", value);
            else if (strcmp(key, "asset") == 0)
                snprintf(m->asset, sizeof m->asset, "%.127s", value);
            else if (strcmp(key, "size") == 0)
                m->size = strtoull(value, NULL, 10);
            else if (strcmp(key, "sha256") == 0)
                have_sha = hex_bytes(value, m->sha256, 32) == 0;
            else if (strcmp(key, "min_from") == 0)
                snprintf(m->min_from, sizeof m->min_from, "%.63s", value);
        }
        line += n + (line[n] == '\n');
    }
    int asset_ok = m->asset[0] && strspn(m->asset, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") ==
                                      strlen(m->asset);
    return update_version_ok(m->version) && update_version_ok(m->min_from) && asset_ok && m->size && have_sha ? 0 : -1;
}

#define NAMESPACE "ps5-discord-release"
#define APPLICATION "ssh:" NAMESPACE

static void put_string(unsigned char *out, size_t *at, const void *s, size_t n)
{
    out[(*at)++] = (unsigned char)(n >> 24), out[(*at)++] = (unsigned char)(n >> 16);
    out[(*at)++] = (unsigned char)(n >> 8), out[(*at)++] = (unsigned char)n;
    memcpy(out + *at, s, n);
    *at += n;
}

int update_signature_ok(const char *text, size_t len, const unsigned char *sig, size_t sig_len,
                        const unsigned char (*keys)[65], int key_count)
{
    if (sig_len != 69 || !(sig[0] & 0x01)) // the user-presence flag: the key was touched
        return 0;
    // What ssh-keygen -Y sign signs (PROTOCOL.sshsig): the SSHSIG blob with the file's SHA-512.
    unsigned char hash[64], blob[128];
    size_t at = 0;
    mbedtls_sha512((const unsigned char *)text, len, hash, 0);
    memcpy(blob, "SSHSIG", 6);
    at = 6;
    put_string(blob, &at, NAMESPACE, sizeof NAMESPACE - 1);
    put_string(blob, &at, "", 0);
    put_string(blob, &at, "sha512", 6);
    put_string(blob, &at, hash, 64);
    // A FIDO key signs SHA-256(application) || flags || counter || SHA-256(message) (PROTOCOL.u2f).
    unsigned char data[69], digest[32];
    mbedtls_sha256((const unsigned char *)APPLICATION, sizeof APPLICATION - 1, data, 0);
    memcpy(data + 32, sig, 5);
    mbedtls_sha256(blob, at, data + 37, 0);
    mbedtls_sha256(data, sizeof data, digest, 0);

    int ok = 0;
    mbedtls_ecp_group grp;
    mbedtls_ecp_point q;
    mbedtls_mpi r, s;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
        mbedtls_mpi_read_binary(&r, sig + 5, 32) == 0 && mbedtls_mpi_read_binary(&s, sig + 37, 32) == 0)
        for (int i = 0; i < key_count && !ok; i++)
            ok = mbedtls_ecp_point_read_binary(&grp, &q, keys[i], 65) == 0 &&
                 mbedtls_ecp_check_pubkey(&grp, &q) == 0 &&
                 mbedtls_ecdsa_verify(&grp, digest, sizeof digest, &q, &r, &s) == 0;
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&q);
    mbedtls_ecp_group_free(&grp);
    return ok;
}

// ---- The release zip ----

static unsigned u16(const unsigned char *p)
{
    return p[0] | p[1] << 8;
}

static uint32_t u32(const unsigned char *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static int fail(char *why, size_t cap, const char *what)
{
    snprintf(why, cap, "%s", what);
    return -1;
}

// A path inside PPSA99072/: no "..", no empty part, no backslash; "" for the folder itself.
static const char *inside(const char *name)
{
    if (strncmp(name, "PPSA99072/", 10) != 0 || strchr(name, '\\'))
        return NULL;
    const char *rest = name + 10;
    for (const char *p = rest; *p;) {
        size_t n = strcspn(p, "/");
        if (n == 0 || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.'))
            return NULL;
        p += n + (p[n] == '/');
    }
    return rest;
}

static int make_dirs(char *path) // every parent of path
{
    for (char *p = strchr(path + 1, '/'); p; p = strchr(p + 1, '/')) {
        *p = 0;
        int r = mkdir(path, 0777);
        *p = '/';
        if (r != 0 && errno != EEXIST)
            return -1;
    }
    return 0;
}

// One entry's data (csize bytes at f's position) to out: stored as is, or deflated (raw
// deflate, as zip writes it). buf: 2 MiB, half for each side. 0 on success.
static int unpack(FILE *f, int out, unsigned method, uint32_t csize, uint32_t usize, unsigned char *buf,
                  uLong *sum, uint64_t *wrote)
{
    const size_t half = 1u << 20;
    z_stream z = {0};
    if (method == 8 && inflateInit2(&z, -MAX_WBITS) != Z_OK)
        return -1;
    int r = 0, zr = Z_OK;
    for (uint32_t left = csize; !r && (left || (method == 8 && zr != Z_STREAM_END));) {
        size_t n = left < half ? left : half;
        if (n && fread(buf, 1, n, f) != n) {
            r = -1;
            break;
        }
        left -= (uint32_t)n;
        if (method == 0) {
            r = write(out, buf, n) == (ssize_t)n ? 0 : -1;
            *sum = crc32(*sum, buf, (uInt)n);
            *wrote += n;
            continue;
        }
        z.next_in = buf, z.avail_in = (uInt)n;
        do { // all the output this input gives
            z.next_out = buf + half, z.avail_out = (uInt)half;
            zr = inflate(&z, Z_NO_FLUSH);
            size_t got = half - z.avail_out;
            if ((zr != Z_OK && zr != Z_STREAM_END && zr != Z_BUF_ERROR) || (*wrote += got) > usize ||
                write(out, buf + half, got) != (ssize_t)got)
                r = -1;
            *sum = crc32(*sum, buf + half, (uInt)got);
        } while (!r && z.avail_out == 0 && zr != Z_STREAM_END);
        if (!r && !left && zr != Z_STREAM_END)
            r = -1; // the input ended before the stream did
    }
    if (method == 8)
        inflateEnd(&z);
    return r;
}

int update_unzip(const char *zip_path, const char *dir, uint64_t max_total, void (*progress)(uint64_t done, uint64_t total),
                 char *why, size_t cap)
{
    FILE *f = fopen(zip_path, "rb");
    if (!f)
        return fail(why, cap, "no archive");
    // The end record sits in the last 22 bytes (the release zip has no comment).
    unsigned char end[22];
    if (fseeko(f, -22, SEEK_END) != 0 || fread(end, 1, 22, f) != 22 || u32(end) != 0x06054b50) {
        fclose(f);
        return fail(why, cap, "not a release zip");
    }
    unsigned count = u16(end + 10);
    uint32_t cd_size = u32(end + 12), cd_at = u32(end + 16);
    unsigned char *cd = count && count <= 4096 && cd_size <= (8u << 20) ? malloc(cd_size) : NULL;
    if (!cd || fseeko(f, cd_at, SEEK_SET) != 0 || fread(cd, 1, cd_size, f) != cd_size) {
        free(cd);
        fclose(f);
        return fail(why, cap, "bad zip directory");
    }
    uint64_t all = 0, unpacked = 0; // for progress: the sizes the directory states
    for (size_t at = 0, i = 0; i < count && at + 46 <= cd_size && u32(cd + at) == 0x02014b50; i++) {
        all += u32(cd + at + 24);
        at += 46 + u16(cd + at + 28) + u16(cd + at + 30) + u16(cd + at + 32);
    }
    char **seen = calloc(count, sizeof *seen);
    int r = seen ? 0 : fail(why, cap, "out of memory");
    uint64_t total = 0;
    unsigned char *buf = malloc(2u << 20);
    if (!buf && !r)
        r = fail(why, cap, "out of memory");
    size_t at = 0;
    for (unsigned i = 0; i < count && !r; i++) {
        const unsigned char *e = cd + at;
        if (at + 46 > cd_size || u32(e) != 0x02014b50) {
            r = fail(why, cap, "bad zip entry");
            break;
        }
        unsigned method = u16(e + 10), name_len = u16(e + 28);
        uint32_t crc = u32(e + 16), csize = u32(e + 20), usize = u32(e + 24), local = u32(e + 42);
        if (at + 46 + name_len > cd_size || name_len == 0 || name_len > 1024) {
            r = fail(why, cap, "bad zip entry");
            break;
        }
        char name[1025];
        memcpy(name, e + 46, name_len);
        name[name_len] = 0;
        at += 46 + name_len + u16(e + 30) + u16(e + 32);
        const char *rest = inside(name);
        if (!rest || memchr(name, 0, name_len) || (method != 0 && method != 8) || (method == 0 && csize != usize) ||
            csize == 0xffffffffu || usize == 0xffffffffu ||
            local == 0xffffffffu) {
            r = fail(why, cap, "zip entry not allowed");
            break;
        }
        for (unsigned k = 0; k < i && !r; k++)
            if (strcmp(seen[k], name) == 0)
                r = fail(why, cap, "zip entry twice");
        if (r || !(seen[i] = strdup(name)) || (total += usize) > max_total) {
            r = r ? r : fail(why, cap, "zip too big");
            break;
        }
        char path[1400];
        snprintf(path, sizeof path, "%s/%s", dir, rest);
        if (make_dirs(path) != 0) {
            r = fail(why, cap, "cannot write");
            break;
        }
        if (!*rest || name[name_len - 1] == '/') {
            if (usize || (mkdir(path, 0777) != 0 && errno != EEXIST))
                r = fail(why, cap, "cannot write");
            continue;
        }
        unsigned char lh[30];
        if (fseeko(f, local, SEEK_SET) != 0 || fread(lh, 1, 30, f) != 30 || u32(lh) != 0x04034b50 ||
            fseeko(f, (off_t)local + 30 + u16(lh + 26) + u16(lh + 28), SEEK_SET) != 0) {
            r = fail(why, cap, "bad zip entry");
            break;
        }
        int out = open(path, O_WRONLY | O_CREAT | O_EXCL, 0777);
        uLong sum = crc32(0, Z_NULL, 0);
        uint64_t wrote = 0;
        r = out >= 0 ? unpack(f, out, method, csize, usize, buf, &sum, &wrote) : r;
        if (r)
            fail(why, cap, "cannot unpack");
        if (out < 0 || (!r && (sum != crc || wrote != usize || fsync(out) != 0)))
            r = fail(why, cap, out < 0 ? "cannot write" : "zip entry damaged");
        if (out >= 0)
            close(out);
        if (!r && progress)
            progress(unpacked += usize, all);
    }
    for (unsigned k = 0; seen && k < count; k++)
        free(seen[k]);
    free(seen);
    free(buf);
    free(cd);
    fclose(f);
    return r;
}
