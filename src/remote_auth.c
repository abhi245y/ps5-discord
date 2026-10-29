#include "remote_auth.h"
#include "json_num.h"
#include "browser_auth.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cJSON.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/sha256.h>
#include <qrcodegen.h>
#ifdef __PROSPERO__
#include <ps5/klog.h>
#endif

#include "http.h"
#include "gateway.h"
#include "log.h"
#include "status.h"
#include "ws.h"

#define REMOTE_AUTH_URL "wss://remote-auth-gateway.discord.gg/?v=2"
#define QR_PREFIX "https://discord.com/ra/"

static int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// Standard base64 into out; url=1 gives base64url without padding.
static int b64(char *out, size_t cap, const unsigned char *in, size_t len, int url)
{
    size_t olen;
    if (mbedtls_base64_encode((unsigned char *)out, cap, &olen, in, len) != 0)
        return -1;
    if (url) {
        for (char *p = out; *p; p++)
            *p = *p == '+' ? '-' : *p == '/' ? '_' : *p;
        while (olen > 0 && out[olen - 1] == '=')
            out[--olen] = 0;
    }
    return 0;
}

// RSA-OAEP-SHA256 decrypt of a base64 string. Returns plaintext length or -1.
static int decrypt_b64(mbedtls_pk_context *pk, const char *in, unsigned char *out, size_t cap)
{
    unsigned char ct[512];
    size_t ctlen, olen;
    if (!in || mbedtls_base64_decode(ct, sizeof ct, &ctlen, (const unsigned char *)in, strlen(in)) != 0)
        return -1;
    if (ctlen != mbedtls_pk_get_len(pk))
        return -1;
    if (mbedtls_rsa_rsaes_oaep_decrypt(mbedtls_pk_rsa(*pk), tls_random, NULL, NULL, 0, &olen, ct, out,
                                       cap) != 0)
        return -1;
    return (int)olen;
}

static void klog_line(const char *line)
{
#ifdef __PROSPERO__
    klog_printf("[discord] %s\n", line);
#else
    fprintf(stderr, "[discord] %s\n", line);
#endif
}

// One QR row per line, two characters per module, plain ASCII so any klog viewer
// shows it. Light modules are drawn, so it reads dark-on-light on a dark terminal.
static void log_qr(const char *text)
{
    static uint8_t qr[qrcodegen_BUFFER_LEN_MAX], tmp[qrcodegen_BUFFER_LEN_MAX];
    if (!qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN,
                              qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true)) {
        LOG("qr: encode failed");
        return;
    }
    int size = qrcodegen_getSize(qr), quiet = 2;
    char line[512];
    for (int y = -quiet; y < size + quiet; y++) {
        size_t n = 0;
        for (int x = -quiet; x < size + quiet && n + 2 < sizeof line; x++) {
            char c = qrcodegen_getModule(qr, x, y) ? ' ' : '#';
            line[n++] = c;
            line[n++] = c;
        }
        line[n] = 0;
        klog_line(line);
    }
}

static int send_op(ws_conn *ws, const char *op, const char *key, const char *value)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "op", op);
    if (key)
        cJSON_AddStringToObject(m, key, value);
    char *s = cJSON_PrintUnformatted(m);
    cJSON_Delete(m);
    int r = s ? ws_send(ws, WS_TEXT, s, strlen(s)) : -1;
    free(s);
    return r;
}

// Exchange the ticket for the encrypted token over REST.
static int redeem_ticket(mbedtls_pk_context *pk, const char *ticket, char *out, size_t cap)
{
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "ticket", ticket);
    char *body = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);
    http_response res = http_post_json("/users/@me/remote-auth/login", body, NULL);
    free(body);
    int r = -1;
    cJSON *j = res.status == 200 ? cJSON_Parse(res.body) : NULL;
    if (!j) {
        // The error body says why (a captcha, an expired ticket); it holds no token.
        LOG("remote auth: ticket redeem failed (HTTP %d) %.300s", res.status, res.body ? res.body : "");
        // Discord wants a captcha this console cannot show (often right after a
        // device was logged out). Never worked around: the person waits it out.
        if (res.body && strstr(res.body, "captcha-required"))
            status_note("Discord asked for a captcha, which the QR sign-in cannot show. "
                        "Press Cross to sign in through the browser instead.");
    } else {
        unsigned char tok[256];
        int n = decrypt_b64(pk, cJSON_GetStringValue(cJSON_GetObjectItem(j, "encrypted_token")), tok,
                            sizeof tok - 1);
        if (n > 0 && (size_t)n < cap) {
            memcpy(out, tok, (size_t)n);
            out[n] = 0;
            r = 0;
        } else {
            LOG("remote auth: token decrypt failed");
        }
        cJSON_Delete(j);
    }
    free(res.body);
    return r;
}

int remote_auth_login(char *out, size_t cap)
{
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    unsigned char der[600], *spki;
    char pub_b64[800], fingerprint[64];
    int spki_len, result = -1;

    LOG("remote auth: generating key");
    if (mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) != 0 ||
        mbedtls_rsa_gen_key(mbedtls_pk_rsa(pk), tls_random, NULL, 2048, 65537) != 0 ||
        mbedtls_rsa_set_padding(mbedtls_pk_rsa(pk), MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA256) != 0 ||
        (spki_len = mbedtls_pk_write_pubkey_der(&pk, der, sizeof der)) <= 0) {
        LOG("remote auth: key setup failed");
        mbedtls_pk_free(&pk);
        return -1;
    }
    spki = der + sizeof der - spki_len; // pk_write_*_der writes at the end of the buffer
    unsigned char hash[32];
    mbedtls_sha256(spki, (size_t)spki_len, hash, 0);
    b64(pub_b64, sizeof pub_b64, spki, (size_t)spki_len, 0);
    b64(fingerprint, sizeof fingerprint, hash, sizeof hash, 1);

    ws_conn ws;
    if (ws_open(&ws, REMOTE_AUTH_URL) != 0) {
        tls_close(&ws.tls);
        mbedtls_pk_free(&pk);
        return -1;
    }
    int64_t next_beat = 0, deadline = now_ms() + 30000; // until HELLO sets the real one
    char qr_url[128] = "";
    int interval = 0, acked = 1;
    char ticket[512] = "";
    for (;;) {
        if (browser_auth_pending() || gateway_switching())
            break;
        int64_t now = now_ms(), wake = next_beat && next_beat < deadline ? next_beat : deadline;
        if (now >= deadline) {
            LOG("remote auth: QR expired");
            break;
        }
        int wait_ms = (int)(wake - now);
        int r = ws_recv(&ws, wait_ms > 1000 ? 1000 : wait_ms);
        if (r == 0) {
            if (next_beat && now_ms() >= next_beat) {
                if (!acked) {
                    LOG("remote auth: heartbeat not acked");
                    break;
                }
                acked = 0;
                send_op(&ws, "heartbeat", NULL, NULL);
                next_beat += interval;
            }
            continue;
        }
        if (r != WS_TEXT)
            break; // closed or error; pending_login is handled before the server closes

        cJSON *m = cJSON_Parse((const char *)ws.msg);
        const char *op = cJSON_GetStringValue(cJSON_GetObjectItem(m, "op"));
        if (!op) {
            cJSON_Delete(m);
            continue;
        }
        if (strcmp(op, "hello") == 0) {
            interval = (int)json_int(m, "heartbeat_interval", 0);
            deadline = now_ms() + (int64_t)json_int(m, "timeout_ms", 0);
            next_beat = now_ms() + (int64_t)(interval * (rand() / (RAND_MAX + 1.0)));
            send_op(&ws, "init", "encoded_public_key", pub_b64);
        } else if (strcmp(op, "nonce_proof") == 0) {
            unsigned char nonce[256];
            char proof[400];
            int n = decrypt_b64(&pk, cJSON_GetStringValue(cJSON_GetObjectItem(m, "encrypted_nonce")),
                                nonce, sizeof nonce);
            if (n < 0 || b64(proof, sizeof proof, nonce, (size_t)n, 1) != 0) {
                LOG("remote auth: nonce decrypt failed");
                cJSON_Delete(m);
                break;
            }
            send_op(&ws, "nonce_proof", "nonce", proof);
        } else if (strcmp(op, "pending_remote_init") == 0) {
            const char *fp = cJSON_GetStringValue(cJSON_GetObjectItem(m, "fingerprint"));
            if (!fp || strcmp(fp, fingerprint) != 0) {
                LOG("remote auth: fingerprint mismatch");
                cJSON_Delete(m);
                break;
            }
            snprintf(qr_url, sizeof qr_url, QR_PREFIX "%s", fp);
            status_qr(qr_url);
            status_state("login");
            LOG("remote auth: QR ready, open the web UI on port 9041 or scan below "
                "(Discord app: Settings > Scan QR Code)");
            log_qr(qr_url);
        } else if (strcmp(op, "pending_ticket") == 0) {
            // "id:discriminator:avatar:username"
            unsigned char user[512];
            int n = decrypt_b64(&pk, cJSON_GetStringValue(cJSON_GetObjectItem(m, "encrypted_user_payload")),
                                user, sizeof user - 1);
            if (n > 0) {
                user[n] = 0;
                const char *name = strrchr((char *)user, ':');
                LOG("remote auth: scanned by %s, approve on the phone", name ? name + 1 : "?");
            }
            status_qr(NULL);
            status_state("scanned");
        } else if (strcmp(op, "pending_login") == 0) {
            const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(m, "ticket"));
            snprintf(ticket, sizeof ticket, "%s", t ? t : "");
            cJSON_Delete(m);
            break;
        } else if (strcmp(op, "cancel") == 0) {
            LOG("remote auth: cancelled on the phone");
            cJSON_Delete(m);
            break;
        } else if (strcmp(op, "heartbeat_ack") == 0) {
            acked = 1;
        }
        cJSON_Delete(m);
    }
    ws_close(&ws, 1000);
    status_qr(NULL);

    if (!browser_auth_pending() && ticket[0] && redeem_ticket(&pk, ticket, out, cap) == 0) {
        LOG("remote auth: logged in");
        status_note(NULL);
        result = 0;
    }
    mbedtls_pk_free(&pk);
    return result;
}
