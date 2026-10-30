// SPDX-License-Identifier: AGPL-3.0-or-later
// The control API's request helpers (src/web.c, included for its statics): ids, query
// strings, %-decoding, the image proxy's allowlist, the REST reply wrapper and the core key.
#include "../src/web.c"

#include <assert.h>
#include <sys/stat.h>

// main.c's, which this test does not link.
int core_sign_out(void)
{
    return -1;
}
void core_quit(int forget_token)
{
    (void)forget_token;
}
void app_alert(const char *context, const char *text, const char *icon, int urgent)
{
    (void)context, (void)text, (void)icon, (void)urgent;
}

static int allowed(const char *url)
{
    char host[128];
    return image_target(url, host) != NULL;
}

// What reply_rest sends for a Discord reply, read back from a socket pair.
static void rest_reply(int status, const char *body, char *out, size_t cap)
{
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    http_response r = {.status = status, .body = body ? strdup(body) : NULL, .body_len = body ? strlen(body) : 0};
    reply_rest(sv[0], r);
    close(sv[0]);
    size_t n = 0;
    for (ssize_t got; n + 1 < cap && (got = read(sv[1], out + n, cap - n - 1)) > 0;)
        n += (size_t)got;
    out[n] = 0;
    close(sv[1]);
}

int main(void)
{
    assert(is_id("1165665327712063540") && !is_id("") && !is_id("12a") && !is_id("1/../2"));
    assert(!is_id("123456789012345678901234")); // 24 digits: longer than any snowflake

    char v[32];
    assert(query_get("channel=11&guild=22", "guild", v, sizeof v) == 0 && strcmp(v, "22") == 0);
    assert(query_get("channel=11&guild=22", "channel", v, sizeof v) == 0 && strcmp(v, "11") == 0);
    assert(query_get("xchannel=1", "channel", v, sizeof v) == -1); // whole keys only
    assert(query_get("v=", "v", v, sizeof v) == 0 && !v[0]);
    assert(query_get("a=1", "b", v, sizeof v) == -1 && query_get(NULL, "a", v, sizeof v) == -1);
    assert(query_get("a=0123456789", "a", v, 4) == -1); // too long for out: refused, not cut

    char s[64] = "https%3A%2F%2Fcdn.discordapp.com%2Fa+b%25";
    url_decode(s);
    assert(strcmp(s, "https://cdn.discordapp.com/a b%") == 0);
    strcpy(s, "end%4");
    url_decode(s); // a cut escape stays as it is
    assert(strcmp(s, "end%4") == 0);

    assert(allowed("https://cdn.discordapp.com/avatars/1/a.png"));
    assert(allowed("https://media.discordapp.net/attachments/1/2/x.png?format=png"));
    assert(allowed("https://images-ext-1.discordapp.net/external/abc"));
    assert(allowed("https://cdn.jsdelivr.net/gh/jdecked/twemoji@15/assets/72x72/1f600.png"));
    assert(!allowed("https://cdn.jsdelivr.net/gh/someone/else@1/x.png"));       // that project only
    assert(!allowed("https://cdn.jsdelivr.net/gh/jdecked/twemoji@15/../../x")); // no climbing out
    assert(!allowed("https://cdn.discordapp.com.evil.example/x"));
    assert(!allowed("https://evil.example/cdn.discordapp.com/x"));
    assert(!allowed("https://images-ext-1.discordapp.net.evil.example/x"));
    assert(!allowed("http://cdn.discordapp.com/x"));              // TLS only
    assert(!allowed("https://cdn.discordapp.com/x\r\nHost: evil")); // no extra headers
    assert(!allowed("https://cdn.discordapp.com/x y"));

    char out[512];
    rest_reply(200, "{\"id\":\"1\"}", out, sizeof out);
    assert(strstr(out, "200 OK") && strstr(out, "{\"status\":200,\"body\":{\"id\":\"1\"}}"));
    rest_reply(502, "<html>Bad gateway</html>", out, sizeof out); // a proxy's page: status kept, body null
    assert(strstr(out, "{\"status\":502,\"body\":null}"));
    rest_reply(-1, NULL, out, sizeof out); // no answer at all
    assert(strstr(out, "{\"status\":-1,\"body\":null}"));

    // The key: none passes before the core made one, then only its 64 characters as a header.
#define REQ_FMT "POST /api/send HTTP/1.0\r\nX-Core-Key: %s\r\nContent-Length: 0\r\n\r\n"
    char req[256];
    snprintf(req, sizeof req, REQ_FMT, "");
    assert(!key_header_ok(req, strstr(req, "\r\n\r\n") + 4) && !core_key_ok("", 0));
    unsigned char raw[32];
    for (int i = 0; i < 32; i++)
        raw[i] = (unsigned char)(i * 37 + 5);
    char dir[] = "/tmp/web-test-XXXXXX";
    assert(mkdtemp(dir));
    assert(core_key_make(dir, raw) == 0);
    char path[64], disk[CORE_KEY_LEN + 1] = "";
    snprintf(path, sizeof path, "%s/control.key", dir);
    struct stat st;
    assert(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600 && st.st_size == CORE_KEY_LEN);
    FILE *f = fopen(path, "r");
    assert(f && fread(disk, 1, CORE_KEY_LEN, f) == CORE_KEY_LEN);
    fclose(f);
    unlink(path);
    core_key_keep(); // a reinstall took the file: written again
    assert(stat(path, &st) == 0 && st.st_size == CORE_KEY_LEN);
    unlink(path);
    rmdir(dir);
    assert(strncmp(disk, "05", 2) == 0); // hex of raw[0]
    snprintf(req, sizeof req, REQ_FMT, disk);
    assert(key_header_ok(req, strstr(req, "\r\n\r\n") + 4));
    char wrong[CORE_KEY_LEN + 1];
    memcpy(wrong, disk, sizeof wrong);
    wrong[CORE_KEY_LEN - 1] ^= 1;
    snprintf(req, sizeof req, REQ_FMT, wrong);
    assert(!key_header_ok(req, strstr(req, "\r\n\r\n") + 4));
    wrong[CORE_KEY_LEN - 1] = 0; // one short
    snprintf(req, sizeof req, REQ_FMT, wrong);
    assert(!key_header_ok(req, strstr(req, "\r\n\r\n") + 4));
    snprintf(req, sizeof req, "POST /api/send HTTP/1.0\r\nX-Core-Key: %sx\r\n\r\n", disk); // one long
    assert(!key_header_ok(req, strstr(req, "\r\n\r\n") + 4));
    snprintf(req, sizeof req, "POST /api/send HTTP/1.0\r\nContent-Length: 77\r\n\r\n\r\nX-Core-Key: %s\r\n", disk);
    assert(!key_header_ok(req, strstr(req, "\r\n\r\n") + 4)); // in the body: not a header
    assert(core_key_ok(disk, CORE_KEY_LEN) && !core_key_ok(disk, CORE_KEY_LEN - 1));
    puts("web: ids, queries, decoding, image allowlist, REST replies, core key passed");
    return 0;
}
