// SPDX-License-Identifier: AGPL-3.0-or-later
// The updater (src/update.c, included for its statics, and update_verify.c): version
// order, the manifest, the FIDO signature, the release zip, the swap and its undo, the
// watch over a new version's first launches, and the restore.
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
    assert(update_unzip(zip, out, 1 << 20, why, sizeof why) == 0);
    snprintf(path, sizeof path, "%s/assets/a.txt", out);
    assert(has(path, "art"));
    remove_tree(out);
    const char *bad[][1] = {{"PPSA99072/../x"}, {"PPSA99072//x"}, {"other/x"}, {"PPSA99072/a\\b"}};
    for (int i = 0; i < 4; i++) {
        mkdir(out, 0777);
        make_zip(zip, bad[i], data + 1, 1, 0);
        assert(update_unzip(zip, out, 1 << 20, why, sizeof why) != 0);
        remove_tree(out);
    }
    const char *twice[] = {"PPSA99072/a", "PPSA99072/a"};
    mkdir(out, 0777);
    make_zip(zip, twice, data + 1, 2, 0);
    assert(update_unzip(zip, out, 1 << 20, why, sizeof why) != 0 && strcmp(why, "zip entry twice") == 0);
    remove_tree(out);
    // Deflated, as tools/build.sh writes most entries: unpacked and checked.
    const char *text[] = {"boot boot boot boot boot boot boot boot boot boot", "art"};
    mkdir(out, 0777);
    make_zip(zip, names + 1, text, 2, 8);
    assert(update_unzip(zip, out, 1 << 20, why, sizeof why) == 0);
    snprintf(path, sizeof path, "%s/eboot.bin", out);
    assert(has(path, text[0]));
    remove_tree(out);
    // A damaged deflate stream: refused.
    FILE *z = fopen(zip, "r+b");
    assert(z && fseek(z, 30 + (long)strlen(names[1]) + 2, SEEK_SET) == 0 && fputc(0xff, z) != EOF);
    fclose(z);
    mkdir(out, 0777);
    assert(update_unzip(zip, out, 1 << 20, why, sizeof why) != 0);
    remove_tree(out);
    mkdir(out, 0777);
    make_zip(zip, names + 1, data + 1, 1, 12); // bzip2: refused
    assert(update_unzip(zip, out, 1 << 20, why, sizeof why) != 0);
    remove_tree(out);
    mkdir(out, 0777);
    make_zip(zip, names + 1, data + 1, 1, 0);
    assert(update_unzip(zip, out, 2, why, sizeof why) != 0 && strcmp(why, "zip too big") == 0);
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

static void running(int on)
{
    char p[300];
    snprintf(p, sizeof p, "%s/PPSA99072_000", sandbox_dir), mkdir(p, 0777);
    snprintf(p, sizeof p, "%s/PPSA99072_000/app0", sandbox_dir);
    if (on)
        mkdir(p, 0777);
    else
        rmdir(p);
}

static void watches(void)
{
    // Two launches that never drew: the previous version goes back, the zip marked bad.
    folders("0.4.0", "0.5.0", 0);
    assert(swap_in(0) == 0 && journal_write("done", "0.5.0", "0") == 0);
    int was = 0;
    for (int launch = 0; launch < 2; launch++) {
        running(1), watch_tick(&was);
        update_seen("0.5.0", 0, 0); // polling, never drawn
        running(0), watch_tick(&was);
    }
    assert(app_has("eboot.bin", "old boot") && app_has("version.txt", "0.4.0") && !exists(work));
    assert(has(bad_file, "ab00000000000000000000000000000000000000000000000000000000000000 0.5.0"));
    assert(strcmp(u.state, "rolled_back") == 0);
    unlink(bad_file);

    // One drawn request to the new core: it stays, and previous/ goes.
    folders("0.4.0", "0.5.0", 0);
    assert(swap_in(0) == 0 && journal_write("done", "0.5.0", "0") == 0);
    was = 0;
    running(1), watch_tick(&was);
    update_seen("0.5.0", 1, 0);
    watch_tick(&was);
    assert(!exists(work) && app_has("version.txt", "0.5.0"));
    running(0);

    // Drawn only at the old core (a call kept it): neither proof nor a failed launch.
    folders("0.4.0", "0.5.0", 0);
    assert(swap_in(0) == 0 && journal_write("done", "0.5.0", "0") == 0);
    was = 0;
    running(1), watch_tick(&was);
    snprintf(core_version, sizeof core_version, "0.4.0");
    update_seen("0.5.0", 1, 0);
    snprintf(core_version, sizeof core_version, "0.5.0");
    running(0), watch_tick(&was);
    char state[16], version[64], extra[64];
    assert(journal_read(state, version, extra) == 0 && strcmp(extra, "0") == 0);
    // Drawn on the fallback core: no proof, a failed launch.
    running(1), watch_tick(&was);
    update_seen("0.5.0", 1, 1);
    update_seen("0.5.0", 1, 0);
    running(0), watch_tick(&was);
    assert(journal_read(state, version, extra) == 0 && strcmp(extra, "1") == 0);
    // A proven launch that quits cleanly ends the watch at once, before the core stops.
    running(1), watch_tick(&was);
    update_seen("0.5.0", 1, 0);
    update_bye();
    assert(!exists(work) && app_has("version.txt", "0.5.0"));
    running(0), watch_tick(&was);

    // Another version installed by FTP meanwhile: the watch ends, nothing goes back.
    folders("0.4.0", "0.5.0", 0);
    assert(swap_in(0) == 0 && journal_write("done", "0.5.0", "0") == 0);
    char p[400];
    snprintf(p, sizeof p, "%s/version.txt", app_dir), put_file(p, "0.6.0");
    was = 0;
    running(1), watch_tick(&was), running(0), watch_tick(&was);
    assert(!exists(work) && app_has("version.txt", "0.6.0"));
}

// apply() to the watch: the launch the swap waited out is not the new version's first.
static void applies(void)
{
    folders("0.4.0", "0.5.0", 0);
    assert(journal_write("staged", "0.5.0", "0.4.0") == 0);
    char staged[400];
    snprintf(staged, sizeof staged, "%s/staged", work);
    assert(open_modes(staged, 0) == 0 && open_modes(staged, 1) == 0);
    int was = 1;
    running(1), watch_tick(&was); // staged: nothing to watch yet
    running(0);
    apply(); // waits for the app to be gone (3 s settle)
    was = app_running();
    watch_tick(&was);
    char state[16], version[64], extra[64];
    assert(journal_read(state, version, extra) == 0 && strcmp(state, "done") == 0 && strcmp(extra, "0") == 0);
    assert(app_has("version.txt", "0.5.0"));

    // A restore a power cut ended, finished at start, marks the zip too.
    assert(journal_write("restoring", "0.5.0", "0") == 0);
    mark_bad("0.5.0");
    assert(has(bad_file, "ab00000000000000000000000000000000000000000000000000000000000000 0.5.0"));
    unsigned char sha[32] = {0xab};
    assert(marked_bad(sha));
    unlink(bad_file);
    assert(!marked_bad(sha));

    // A swap that ended before its journal said so: nothing left to move.
    remove_tree(work);
    mkdir(work, 0777);
    assert(swap_in(1) == 0);
    remove_tree(work);
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
    snprintf(core_version, sizeof core_version, "0.5.0");
    swaps();
    watches();
    applies();
    notes_and_urls();
    remove_tree(tmp);
    puts("update: versions, manifest, signature, zip, swap and undo, apply, watch, restore, notes, URLs passed");
    return 0;
}
