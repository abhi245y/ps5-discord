// SPDX-License-Identifier: AGPL-3.0-or-later
// The updater (src/update.c, included for its statics, and update_verify.c): version
// order, the manifest, the FIDO signature, the release zip, the swap and its undo, the
// restore, and the whole of it on the update thread against a fake GitHub.
#define http_get_url fake_get_url // GitHub, served from memory (flow())
#include "../src/update.c"
#include "../src/update_verify.c"

#include <assert.h>

#include <mbedtls/ecdsa.h>

#include "tls.h"

// main.c's, which this test does not link.
int core_sign_out(void)
{
    return -1;
}
void core_quit(int forget_token)
{
    (void)forget_token;
}
void core_end_calls(void)
{
}
void app_alert(const char *context, const char *text, const char *icon, int urgent)
{
    (void)context, (void)text, (void)icon, (void)urgent;
}

static void versions(void)
{
    assert(update_version_cmp("0.5.0-alpha.1", "0.5.0-alpha.1-dev+g86a8258") > 0); // the release after its dev builds
    assert(update_version_cmp("0.5.0-alpha.1-dev+gabc", "0.5.0-alpha.1-dev+gdef") == 0);
    assert(update_version_cmp("0.5.0-alpha.0", "0.4.0-dev") > 0);
    assert(update_version_cmp("0.4.0", "0.4.0-dev") > 0);
    assert(update_version_cmp("0.5.0-alpha.2", "0.5.0-alpha.1") > 0);
    assert(update_version_cmp("0.5.0-alpha.10", "0.5.0-alpha.9") > 0); // numbers, not text
    assert(update_version_cmp("0.5.0-beta.1", "0.5.0-alpha.9") > 0);
    assert(update_version_cmp("0.5.0", "0.5.0-rc.1") > 0);
    assert(update_version_cmp("0.5.0-alpha.1.1", "0.5.0-alpha.1") > 0);
    assert(update_version_cmp("0.10.0", "0.9.9") > 0);
    assert(update_version_cmp("0.4.9", "0.5.0-alpha.1") < 0);
    assert(update_version_ok("1.2.3") && !update_version_ok("1.2") && !update_version_ok("v1.2.3") &&
           !update_version_ok("1.2.3-") && !update_version_ok("1.2.3-a/b"));
    assert(update_prerelease("0.5.0-alpha.1") && update_prerelease("0.5.0-dev") && !update_prerelease("0.5.0"));
}

static const char manifest[] = "ps5-discord-update 1\nversion 0.5.0-alpha.1\nasset ps5-discord-app-v0.5.0-alpha.1.zip\n"
                               "size 1234\nsha256 00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\n"
                               "min_from 0.4.0\nlater a key an older core does not know\n";

static void manifests(void)
{
    update_manifest m;
    assert(update_manifest_parse(manifest, &m) == 0 && strcmp(m.version, "0.5.0-alpha.1") == 0 && m.size == 1234 &&
           m.sha256[1] == 0x11 && strcmp(m.min_from, "0.4.0") == 0);
    assert(update_manifest_parse("ps5-discord-update 2\nversion 0.5.0\n", &m) != 0);
    assert(update_manifest_parse("ps5-discord-update 1\nversion 0.5.0\nasset a.zip\nsize 1\nsha256 00\n", &m) != 0);
    assert(update_manifest_parse("ps5-discord-update 1\nversion 0.5.0\nasset ../a.zip\nsize 1\nsha256 "
                                 "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\n",
                                 &m) != 0);
}

// What tools/sign-release.sh makes from a FIDO key's signature, here from a software key.
static void sign(const char *text, mbedtls_ecdsa_context *k, unsigned char flags, unsigned char out[69])
{
    unsigned char hash[64], blob[128], data[69], digest[32];
    size_t at = 6;
    mbedtls_sha512((const unsigned char *)text, strlen(text), hash, 0);
    memcpy(blob, "SSHSIG", 6);
    put_string(blob, &at, NAMESPACE, sizeof NAMESPACE - 1);
    put_string(blob, &at, "", 0);
    put_string(blob, &at, "sha512", 6);
    put_string(blob, &at, hash, 64);
    mbedtls_sha256((const unsigned char *)APPLICATION, sizeof APPLICATION - 1, data, 0);
    data[32] = flags, data[33] = 0, data[34] = 0, data[35] = 0, data[36] = 7;
    mbedtls_sha256(blob, at, data + 37, 0);
    mbedtls_sha256(data, sizeof data, digest, 0);
    mbedtls_mpi r, s;
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    assert(mbedtls_ecdsa_sign(&k->MBEDTLS_PRIVATE(grp), &r, &s, &k->MBEDTLS_PRIVATE(d), digest, 32, tls_random, NULL) == 0);
    memcpy(out, data + 32, 5);
    assert(mbedtls_mpi_write_binary(&r, out + 5, 32) == 0 && mbedtls_mpi_write_binary(&s, out + 37, 32) == 0);
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
}

static void signatures(void)
{
    assert(tls_global_init() == 0);
    mbedtls_ecdsa_context k, other;
    mbedtls_ecdsa_init(&k);
    mbedtls_ecdsa_init(&other);
    assert(mbedtls_ecdsa_genkey(&k, MBEDTLS_ECP_DP_SECP256R1, tls_random, NULL) == 0);
    assert(mbedtls_ecdsa_genkey(&other, MBEDTLS_ECP_DP_SECP256R1, tls_random, NULL) == 0);
    unsigned char keys[2][65], sig[69];
    size_t n;
    assert(mbedtls_ecp_point_write_binary(&other.MBEDTLS_PRIVATE(grp), &other.MBEDTLS_PRIVATE(Q),
                                          MBEDTLS_ECP_PF_UNCOMPRESSED, &n, keys[0], 65) == 0 && n == 65);
    assert(mbedtls_ecp_point_write_binary(&k.MBEDTLS_PRIVATE(grp), &k.MBEDTLS_PRIVATE(Q), MBEDTLS_ECP_PF_UNCOMPRESSED,
                                          &n, keys[1], 65) == 0);
    sign(manifest, &k, 0x01, sig);
    size_t len = strlen(manifest);
    assert(update_signature_ok(manifest, len, sig, 69, (const unsigned char(*)[65])keys, 2)); // the spare's
    assert(!update_signature_ok(manifest, len, sig, 69, (const unsigned char(*)[65])keys, 1)); // not the main's
    assert(!update_signature_ok(manifest, len - 1, sig, 69, (const unsigned char(*)[65])keys, 2));
    assert(!update_signature_ok(manifest, len, sig, 68, (const unsigned char(*)[65])keys, 2));
    sig[40] ^= 1;
    assert(!update_signature_ok(manifest, len, sig, 69, (const unsigned char(*)[65])keys, 2));
    sign(manifest, &k, 0x00, sig); // not touched
    assert(!update_signature_ok(manifest, len, sig, 69, (const unsigned char(*)[65])keys, 2));
    // A real one: the sample release's manifest, signed by the ZUKEY with ssh-keygen -Y sign and
    // turned into update.sig by tools/sign-release.sh, checked against the project's keys.
    char text[512];
    unsigned char real[80];
    FILE *f = fopen("tests/data/fido-update.txt", "rb");
    size_t text_len = f ? fread(text, 1, sizeof text, f) : 0;
    if (f)
        fclose(f);
    f = fopen("tests/data/fido-update.sig", "rb");
    size_t real_len = f ? fread(real, 1, sizeof real, f) : 0;
    if (f)
        fclose(f);
    assert(text_len > 0 && real_len == 69);
    const int count = (int)(sizeof update_keys / sizeof *update_keys);
    assert(update_signature_ok(text, text_len, real, real_len, update_keys, count));
    real[68] ^= 1;
    assert(!update_signature_ok(text, text_len, real, real_len, update_keys, count));
    real[68] ^= 1;
    text[10] ^= 1;
    assert(!update_signature_ok(text, text_len, real, real_len, update_keys, count));
    // The project's own keys parse as P-256 points.
    mbedtls_ecp_group grp;
    mbedtls_ecp_point q;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&q);
    assert(mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0);
    for (size_t i = 0; i < sizeof update_keys / sizeof *update_keys; i++)
        assert(mbedtls_ecp_point_read_binary(&grp, &q, update_keys[i], 65) == 0 && mbedtls_ecp_check_pubkey(&grp, &q) == 0);
    mbedtls_ecp_point_free(&q);
    mbedtls_ecp_group_free(&grp);
    mbedtls_ecdsa_free(&k);
    mbedtls_ecdsa_free(&other);
}

// ---- A stored zip, written by hand ----

static void le(FILE *f, uint32_t v, int bytes)
{
    for (int i = 0; i < bytes; i++)
        fputc((int)(v >> (8 * i)) & 0xff, f);
}

static void make_zip(const char *path, const char *const *names, const char *const *data, int n, int method)
{
    FILE *f = fopen(path, "wb");
    assert(f);
    uint32_t offs[16], crcs[16], csizes[16];
    for (int i = 0; i < n; i++) {
        offs[i] = (uint32_t)ftell(f);
        size_t len = strlen(data[i]);
        crcs[i] = (uint32_t)crc32(0, (const Bytef *)data[i], (uInt)len);
        unsigned char packed[4096];
        uint32_t clen = (uint32_t)len;
        if (method == 8) { // raw deflate, as zip writes it
            z_stream z = {0};
            assert(deflateInit2(&z, 9, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK);
            z.next_in = (Bytef *)data[i], z.avail_in = (uInt)len;
            z.next_out = packed, z.avail_out = sizeof packed;
            assert(deflate(&z, Z_FINISH) == Z_STREAM_END);
            clen = (uint32_t)z.total_out;
            deflateEnd(&z);
        }
        csizes[i] = clen;
        le(f, 0x04034b50, 4), le(f, 10, 2), le(f, 0, 2), le(f, (uint32_t)method, 2), le(f, 0, 4);
        le(f, crcs[i], 4), le(f, clen, 4), le(f, (uint32_t)len, 4);
        le(f, (uint32_t)strlen(names[i]), 2), le(f, 0, 2);
        fputs(names[i], f);
        fwrite(method == 8 ? (const void *)packed : data[i], 1, clen, f);
    }
    uint32_t cd = (uint32_t)ftell(f);
    for (int i = 0; i < n; i++) {
        size_t len = strlen(data[i]);
        le(f, 0x02014b50, 4), le(f, 0x031e, 2), le(f, 10, 2), le(f, 0, 2), le(f, (uint32_t)method, 2), le(f, 0, 4);
        le(f, crcs[i], 4), le(f, csizes[i], 4), le(f, (uint32_t)len, 4);
        le(f, (uint32_t)strlen(names[i]), 2), le(f, 0, 2), le(f, 0, 2), le(f, 0, 2), le(f, 0, 2);
        le(f, 0x81ff0000u, 4), le(f, offs[i], 4);
        fputs(names[i], f);
    }
    uint32_t cd_size = (uint32_t)ftell(f) - cd;
    le(f, 0x06054b50, 4), le(f, 0, 2), le(f, 0, 2), le(f, (uint32_t)n, 2), le(f, (uint32_t)n, 2);
    le(f, cd_size, 4), le(f, cd, 4), le(f, 0, 2);
    fclose(f);
}

static char tmp[] = "/tmp/update-test-XXXXXX";

static void put_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    assert(f && fputs(text, f) >= 0);
    fclose(f);
}

static int has(const char *path, const char *text)
{
    char line[256];
    return read_line(path, line, sizeof line) == 0 && strcmp(line, text) == 0;
}

static void zips(void)
{
    char zip[300], out[300], path[400], why[96];
    snprintf(zip, sizeof zip, "%s/r.zip", tmp);
    const char *names[] = {"PPSA99072/", "PPSA99072/eboot.bin", "PPSA99072/assets/a.txt"};
    const char *data[] = {"", "boot", "art"};
    make_zip(zip, names, data, 3, 0);
    snprintf(out, sizeof out, "%s/out", tmp);
    mkdir(out, 0777);
    assert(update_unzip(zip, out, 1 << 20, NULL, why, sizeof why) == 0);
    snprintf(path, sizeof path, "%s/assets/a.txt", out);
    assert(has(path, "art"));
    remove_tree(out);
    const char *bad[][1] = {{"PPSA99072/../x"}, {"PPSA99072//x"}, {"other/x"}, {"PPSA99072/a\\b"}};
    for (int i = 0; i < 4; i++) {
        mkdir(out, 0777);
        make_zip(zip, bad[i], data + 1, 1, 0);
        assert(update_unzip(zip, out, 1 << 20, NULL, why, sizeof why) != 0);
        remove_tree(out);
    }
    const char *deep[] = {"PPSA99072/1/2/3/4/5/6/7/8/9/10/11/12/13/14/15/16/17"}; // the walks recurse
    mkdir(out, 0777);
    make_zip(zip, deep, data + 1, 1, 0);
    assert(update_unzip(zip, out, 1 << 20, NULL, why, sizeof why) != 0);
    remove_tree(out);
    const char *twice[] = {"PPSA99072/a", "PPSA99072/a"};
    mkdir(out, 0777);
    make_zip(zip, twice, data + 1, 2, 0);
    assert(update_unzip(zip, out, 1 << 20, NULL, why, sizeof why) != 0 && strcmp(why, "zip entry twice") == 0);
    remove_tree(out);
    // Deflated, as tools/build.sh writes most entries: unpacked and checked.
    const char *text[] = {"boot boot boot boot boot boot boot boot boot boot", "art"};
    mkdir(out, 0777);
    make_zip(zip, names + 1, text, 2, 8);
    assert(update_unzip(zip, out, 1 << 20, NULL, why, sizeof why) == 0);
    snprintf(path, sizeof path, "%s/eboot.bin", out);
    assert(has(path, text[0]));
    remove_tree(out);
    // A damaged deflate stream: refused.
    FILE *z = fopen(zip, "r+b");
    assert(z && fseek(z, 30 + (long)strlen(names[1]) + 2, SEEK_SET) == 0 && fputc(0xff, z) != EOF);
    fclose(z);
    mkdir(out, 0777);
    assert(update_unzip(zip, out, 1 << 20, NULL, why, sizeof why) != 0);
    remove_tree(out);
    mkdir(out, 0777);
    make_zip(zip, names + 1, data + 1, 1, 12); // bzip2: refused
    assert(update_unzip(zip, out, 1 << 20, NULL, why, sizeof why) != 0);
    remove_tree(out);
    mkdir(out, 0777);
    make_zip(zip, names + 1, data + 1, 1, 0);
    assert(update_unzip(zip, out, 2, NULL, why, sizeof why) != 0 && strcmp(why, "zip too big") == 0);
    remove_tree(out);
    unlink(zip);
}

// An app folder with version a, a staged b beside it.
static void folders(const char *installed, const char *staged_version, int with_extra)
{
    char p[400];
    remove_tree(app_dir);
    remove_tree(work);
    mkdir(app_dir, 0777);
    snprintf(p, sizeof p, "%s/assets", app_dir), mkdir(p, 0777);
    snprintf(p, sizeof p, "%s/assets/font", app_dir), put_file(p, "old font");
    snprintf(p, sizeof p, "%s/eboot.bin", app_dir), put_file(p, "old boot");
    snprintf(p, sizeof p, "%s/version.txt", app_dir), put_file(p, installed);
    snprintf(p, sizeof p, "%s/control.key", app_dir), put_file(p, "key");
    mkdir(work, 0777);
    snprintf(p, sizeof p, "%s/staged", work), mkdir(p, 0777);
    snprintf(p, sizeof p, "%s/staged/assets", work), mkdir(p, 0777);
    snprintf(p, sizeof p, "%s/staged/assets/font", work), put_file(p, "new font");
    snprintf(p, sizeof p, "%s/staged/eboot.bin", work), put_file(p, "new boot");
    snprintf(p, sizeof p, "%s/staged/version.txt", work), put_file(p, staged_version);
    if (with_extra)
        snprintf(p, sizeof p, "%s/staged/sce_module", work), mkdir(p, 0777);
    snprintf(p, sizeof p, "%s/sha256", work), put_file(p, "ab00000000000000000000000000000000000000000000000000000000000000");
}

static int app_has(const char *name, const char *text)
{
    char p[400];
    snprintf(p, sizeof p, "%s/%s", app_dir, name);
    return text ? has(p, text) : exists(p);
}

static void swaps(void)
{
    folders("0.4.0", "0.5.0", 1);
    assert(swap_in(0) == 0);
    assert(app_has("eboot.bin", "new boot") && app_has("assets/font", "new font") && app_has("version.txt", "0.5.0"));
    assert(app_has("control.key", "key") && app_has("sce_module", NULL));
    assert(restore() == 0); // previous back; the added sce_module out
    assert(app_has("eboot.bin", "old boot") && app_has("version.txt", "0.4.0") && !app_has("sce_module", NULL) &&
           app_has("control.key", "key"));

    // A failure halfway is undone: eboot.bin (last) cannot go over a folder that is not empty.
    folders("0.4.0", "0.5.0", 0);
    char p[400];
    snprintf(p, sizeof p, "%s/eboot.bin", app_dir), unlink(p), mkdir(p, 0777);
    snprintf(p, sizeof p, "%s/eboot.bin/x", app_dir), put_file(p, "x");
    snprintf(p, sizeof p, "%s/previous", work), mkdir(p, 0777);
    snprintf(p, sizeof p, "%s/previous/eboot.bin", work), put_file(p, "in the way"); // so the old one stays put
    assert(swap_in(0) != 0);
    assert(app_has("assets/font", "old font") && app_has("version.txt", "0.4.0"));
    snprintf(p, sizeof p, "%s/staged/assets/font", work);
    assert(has(p, "new font"));

    // Cut short after assets: carried on.
    folders("0.4.0", "0.5.0", 0);
    snprintf(p, sizeof p, "%s/previous", work), mkdir(p, 0777);
    char from[400], to[400];
    snprintf(from, sizeof from, "%s/assets", app_dir), snprintf(to, sizeof to, "%s/previous/assets", work);
    assert(rename(from, to) == 0);
    snprintf(from, sizeof from, "%s/staged/assets", work), snprintf(to, sizeof to, "%s/assets", app_dir);
    assert(rename(from, to) == 0);
    assert(swap_in(1) == 0 && app_has("eboot.bin", "new boot") && app_has("version.txt", "0.5.0"));
    snprintf(p, sizeof p, "%s/previous/assets/font", work);
    assert(has(p, "old font"));
}

// The journal's pieces a power cut leaves: a restore's zip marked, an empty swap finished.
static void journals(void)
{
    folders("0.4.0", "0.5.0", 0);
    assert(journal_write("restoring", "0.5.0", "0.4.0") == 0);
    mark_bad("0.5.0");
    assert(has(bad_file, "ab00000000000000000000000000000000000000000000000000000000000000 0.5.0"));
    unsigned char sha[32] = {0xab};
    assert(marked_bad(sha));
    unlink(bad_file);
    assert(!marked_bad(sha));
    remove_tree(work);
    mkdir(work, 0777);
    assert(swap_in(1) == 0); // a swap that ended before its journal said so: nothing left to move
    remove_tree(work);
}

// The update thread's work on a stack as small as a console thread's default: copying the
// old core once put a 64 KB buffer there, and the core froze.
static void *copy_small(void *arg)
{
    char *paths = arg;
    return (void *)(intptr_t)copy_file(paths, paths + 300);
}

static void small_stack(void)
{
    char paths[600];
    snprintf(paths, 300, "%s/big", tmp);
    snprintf(paths + 300, 300, "%s/big.copy", tmp);
    FILE *f = fopen(paths, "wb");
    for (int i = 0; f && i < 300000; i++)
        fputc(i & 0xff, f);
    fclose(f);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024);
    pthread_t t;
    void *r = (void *)1;
    assert(pthread_create(&t, &attr, copy_small, paths) == 0 && pthread_join(t, &r) == 0 && r == NULL);
    pthread_attr_destroy(&attr);
    struct stat st;
    assert(stat(paths + 300, &st) == 0 && st.st_size == 300000);
    unlink(paths), unlink(paths + 300);
}

static void notes_and_urls(void)
{
    char body[16400];
    memset(body, 'a', 16383);
    memcpy(body + 16383, "\xc3\xa9", 3); // é across the 16 KB line
    char *n = notes_of(body);
    assert(strlen(n) == 16383);
    free(n);
    n = notes_of("one\r\ntwo");
    assert(strcmp(n, "one\ntwo") == 0);
    free(n);
    char host[256], path[3072], longhost[320] = "https://";
    assert(http_split_url("https://api.github.com/x?y=1", host, path) == 0 && strcmp(host, "api.github.com") == 0 &&
           strcmp(path, "/x?y=1") == 0);
    assert(http_split_url("https://h", host, path) == 0 && strcmp(path, "/") == 0);
    assert(http_split_url("https://u@h/", host, path) != 0 && http_split_url("https://h:443/", host, path) != 0);
    assert(http_split_url("http://h/", host, path) != 0);
    memset(longhost + 8, 'h', 300);
    assert(http_split_url(longhost, host, path) != 0);
}

// ---- The update thread against a fake GitHub ----

static struct {
    const char *url;
    int status;
    char *body;
    size_t len;
} served[4]; // changed only while the thread waits: update_check's lock orders it

int fake_get_url(const char *url, int (*allowed)(const char *), unsigned long long limit, http_sink sink, void *user)
{
    char host[256], path[3072];
    assert(http_split_url(url, host, path) == 0 && allowed(host));
    for (int i = 0; i < 4 && served[i].url; i++) {
        if (strcmp(served[i].url, url) != 0)
            continue;
        if (served[i].status != 200 || served[i].len > limit)
            return served[i].status != 200 ? served[i].status : -1;
        for (size_t at = 0; at < served[i].len; at += 4096)
            if (!sink(user, served[i].body + at, served[i].len - at < 4096 ? served[i].len - at : 4096))
                return -1;
        return 200;
    }
    return 404;
}

static void serve(int i, const char *url, int status, const void *body, size_t len)
{
    free(served[i].body);
    served[i].url = url, served[i].status = status, served[i].len = len;
    served[i].body = malloc(len + 1);
    memcpy(served[i].body, body, len);
}

// Up to 15 s for the thread to get the updater into state (and its check_error set or not).
static void wait_for(const char *state, int with_error)
{
    for (int i = 0; i < 300; i++) {
        pthread_mutex_lock(&lock);
        int there = strcmp(u.state, state) == 0 && !u.want_check && !u.want_start && !u.want_apply &&
                    !u.want_restore && (with_error < 0 || !!u.check_error[0] == with_error);
        pthread_mutex_unlock(&lock);
        if (there)
            return;
        usleep(50000);
    }
    fprintf(stderr, "update: never %s (%s, %s)\n", state, u.state, u.check_error);
    assert(0);
}

static void flow(void)
{
    char p[400], zip[400];
    remove_tree(app_dir);
    remove_tree(work);
    mkdir(app_dir, 0777);
    snprintf(p, sizeof p, "%s/assets", app_dir), mkdir(p, 0777);
    snprintf(p, sizeof p, "%s/assets/ps5-discord.elf", app_dir), put_file(p, "old core");
    snprintf(p, sizeof p, "%s/eboot.bin", app_dir), put_file(p, "old boot");
    snprintf(p, sizeof p, "%s/version.txt", app_dir), put_file(p, "0.4.0");
    // The release: its zip, update.txt signed by a key the test trusts, and the list.
    snprintf(zip, sizeof zip, "%s/release.zip", tmp);
    const char *names[] = {"PPSA99072/eboot.bin", "PPSA99072/sce_sys/param.json", "PPSA99072/assets/ps5-discord.elf",
                           "PPSA99072/version.txt"};
    const char *data[] = {"new boot", "{}", "new core", "0.5.0"};
    make_zip(zip, names, data, 4, 8);
    FILE *f = fopen(zip, "rb");
    static char bytes[8192];
    size_t len = fread(bytes, 1, sizeof bytes, f);
    fclose(f);
    unsigned char sha[32];
    mbedtls_sha256((unsigned char *)bytes, len, sha, 0);
    char text[512], hex[65], list[2048];
    for (int i = 0; i < 32; i++)
        snprintf(hex + 2 * i, 3, "%02x", sha[i]);
    snprintf(text, sizeof text, "ps5-discord-update 1\nversion 0.5.0\nasset app.zip\nsize %zu\nsha256 %s\nmin_from 0.0.0\n",
             len, hex);
    mbedtls_ecdsa_context k;
    mbedtls_ecdsa_init(&k);
    assert(mbedtls_ecdsa_genkey(&k, MBEDTLS_ECP_DP_SECP256R1, tls_random, NULL) == 0);
    static unsigned char mine[1][65];
    size_t n;
    assert(mbedtls_ecp_point_write_binary(&k.MBEDTLS_PRIVATE(grp), &k.MBEDTLS_PRIVATE(Q), MBEDTLS_ECP_PF_UNCOMPRESSED,
                                          &n, mine[0], 65) == 0);
    keys = (const unsigned char(*)[65])mine, key_count = 1;
    unsigned char sig[69];
    sign(text, &k, 0x01, sig);
    mbedtls_ecdsa_free(&k);
    snprintf(list, sizeof list,
             "[{\"tag_name\":\"v0.5.0\",\"draft\":false,\"body\":\"What changed\",\"assets\":["
             "{\"name\":\"update.txt\",\"size\":%zu,\"browser_download_url\":\"https://github.com/r/update.txt\"},"
             "{\"name\":\"update.sig\",\"size\":69,\"browser_download_url\":\"https://github.com/r/update.sig\"},"
             "{\"name\":\"app.zip\",\"size\":%zu,\"browser_download_url\":\"https://github.com/r/app.zip\"}]}]",
             strlen(text), len);
    serve(0, RELEASES, -1, "", 0); // GitHub cannot be reached
    serve(1, "https://github.com/r/update.txt", 200, text, strlen(text));
    serve(2, "https://github.com/r/update.sig", 200, sig, sizeof sig);
    serve(3, "https://github.com/r/app.zip", 200, bytes, len);

    // On the update thread, its own stack: a check that fails says why and is not "Up to date".
    char app[256], to[256], bad[256]; // update_init copies them over these
    snprintf(app, sizeof app, "%s", app_dir), snprintf(to, sizeof to, "%s", work), snprintf(bad, sizeof bad, "%s", bad_file);
    update_init(app, to, bad);
    update_check(1);
    wait_for("none", 1);
    assert(strcmp(u.check_error, "Could not reach GitHub") == 0);
    serve(0, RELEASES, 200, list, strlen(list));
    update_check(1);
    wait_for("available", 0);
    assert(strcmp(u.version, "0.5.0") == 0 && strcmp(u.notes, "What changed") == 0);
    // A check that fails later keeps the offer.
    serve(0, RELEASES, -1, "", 0);
    update_check(1);
    wait_for("available", 1);
    serve(0, RELEASES, 200, list, strlen(list));
    char why[96];
    assert(update_start(why, sizeof why) == 0);
    wait_for("ready", -1);
    char state[16], version[64], from[64];
    assert(journal_read(state, version, from) == 0 && strcmp(state, "staged") == 0 && strcmp(from, "0.4.0") == 0);
    update_check(1); // the staged version again: nothing new, no error
    wait_for("ready", 0);
    // A newer release that cannot be fetched leaves the staged one.
    char newer[2048];
    snprintf(newer, sizeof newer, "%s", list);
    memcpy(strstr(newer, "v0.5.0"), "v0.6.0", 6);
    serve(1, "https://github.com/r/update.txt", 500, "", 0);
    serve(0, RELEASES, 200, newer, strlen(newer));
    update_check(1);
    wait_for("ready", 1);
    // Restart now: the app is closed, the files go in, previous/ keeps 0.4.0.
    assert(update_apply() == 0);
    wait_for("none", -1);
    assert(app_has("eboot.bin", "new boot") && app_has("version.txt", "0.5.0") && app_has("assets/ps5-discord.elf", "new core"));
    assert(app_has("assets/ps5-discord.prev.elf", "old core"));
    assert(journal_read(state, version, from) == 0 && strcmp(state, "done") == 0 && strcmp(u.previous, "0.4.0") == 0);
    // Settings' Restore: 0.4.0 back, the zip kept off the launch check, offered by Check.
    assert(update_restore() == 0);
    wait_for("none", -1);
    assert(app_has("eboot.bin", "old boot") && app_has("version.txt", "0.4.0") && !exists(work) && !u.previous[0]);
    snprintf(p, sizeof p, "%s 0.5.0", hex);
    assert(has(bad_file, p));
    serve(1, "https://github.com/r/update.txt", 200, text, strlen(text));
    serve(0, RELEASES, 200, list, strlen(list));
    pthread_mutex_lock(&lock);
    u.checked = 0; // an hour on
    pthread_mutex_unlock(&lock);
    update_check(0); // the launch check: the zip that was put back is not offered
    wait_for("none", 0);
    update_check(1);
    wait_for("available", 0);
}

int main(void)
{
    versions();
    manifests();
    signatures();
    assert(mkdtemp(tmp));
    snprintf(app_dir, sizeof app_dir, "%s/app", tmp);
    snprintf(work, sizeof work, "%s/.app.update", tmp);
    snprintf(bad_file, sizeof bad_file, "%s/bad", tmp);
    snprintf(sandbox_dir, sizeof sandbox_dir, "%s/sandbox", tmp);
    mkdir(sandbox_dir, 0777);
    zips();
    swaps();
    journals();
    notes_and_urls();
    small_stack();
    flow(); // last: its thread stays, waiting
    remove_tree(tmp);
    puts("update: versions, manifest, signature, zip, swap and undo, restore, notes, URLs, the thread passed");
    return 0;
}
