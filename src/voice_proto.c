#include "voice_proto.h"

#include <string.h>

#include <cJSON.h>
#include <mbedtls/gcm.h>

#define GCM_TAG 16
#define GCM_COUNTER 4
#define GCM_NONCE 12

size_t voice_rtp_header_len(const uint8_t *p, size_t len)
{
    if (len < 12 || (p[0] >> 6) != 2)
        return 0;
    size_t n = 12 + 4 * (size_t)(p[0] & 0x0f);
    if (p[0] & 0x10) // extension: only its 4-byte preamble is clear; the words are encrypted (rtpsize)
        n += 4;
    return n <= len ? n : 0;
}

// Nonce is the counter big-endian in the first 4 bytes, zero after (songbird, discord.py).
static void gcm_nonce(uint8_t nonce[GCM_NONCE], uint32_t counter)
{
    memset(nonce, 0, GCM_NONCE);
    nonce[0] = counter >> 24;
    nonce[1] = counter >> 16;
    nonce[2] = counter >> 8;
    nonce[3] = counter;
}

size_t voice_gcm_seal(const uint8_t key[32], uint32_t counter, const uint8_t *pkt, size_t len,
                      uint8_t *out, size_t cap)
{
    size_t hlen = voice_rtp_header_len(pkt, len);
    if (!hlen || cap < len + GCM_TAG + GCM_COUNTER)
        return 0;
    uint8_t nonce[GCM_NONCE], tag[GCM_TAG];
    gcm_nonce(nonce, counter);
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    int ok = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
             mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, len - hlen, nonce, GCM_NONCE, pkt, hlen,
                                       pkt + hlen, out + hlen, GCM_TAG, tag) == 0;
    mbedtls_gcm_free(&g);
    if (!ok)
        return 0;
    memcpy(out, pkt, hlen);
    memcpy(out + len, tag, GCM_TAG);
    out[len + GCM_TAG] = counter >> 24;
    out[len + GCM_TAG + 1] = counter >> 16;
    out[len + GCM_TAG + 2] = counter >> 8;
    out[len + GCM_TAG + 3] = counter;
    return len + GCM_TAG + GCM_COUNTER;
}

size_t voice_gcm_open(const uint8_t key[32], const uint8_t *pkt, size_t len, uint8_t *out, size_t cap)
{
    if (len < GCM_TAG + GCM_COUNTER)
        return 0;
    const size_t plain_len = len - GCM_TAG - GCM_COUNTER;
    const size_t hlen = voice_rtp_header_len(pkt, plain_len);
    if (!hlen || cap < plain_len)
        return 0;
    const uint8_t *tail = pkt + len - GCM_COUNTER;
    uint32_t counter = (uint32_t)tail[0] << 24 | (uint32_t)tail[1] << 16 | (uint32_t)tail[2] << 8 | tail[3];
    uint8_t nonce[GCM_NONCE];
    gcm_nonce(nonce, counter);
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    int ok = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
             mbedtls_gcm_auth_decrypt(&g, plain_len - hlen, nonce, GCM_NONCE, pkt, hlen, pkt + plain_len,
                                      GCM_TAG, pkt + hlen, out + hlen) == 0;
    mbedtls_gcm_free(&g);
    if (!ok)
        return 0;
    memcpy(out, pkt, hlen);
    return plain_len;
}

size_t voice_ip_discovery_request(uint8_t out[74], uint32_t ssrc)
{
    memset(out, 0, 74);
    out[1] = 0x01; // type: request
    out[3] = 70;   // length of what follows the type and length fields
    out[4] = ssrc >> 24;
    out[5] = ssrc >> 16;
    out[6] = ssrc >> 8;
    out[7] = ssrc;
    return 74;
}

int voice_ip_discovery_parse(const uint8_t *in, size_t len, char address[64], uint16_t *port)
{
    if (len != 74 || in[0] != 0 || in[1] != 0x02 || in[2] != 0 || in[3] != 70)
        return -1;
    if (strnlen((const char *)in + 8, 64) == 64) // the address must end inside its field
        return -1;
    memcpy(address, in + 8, 64);
    *port = (uint16_t)(in[72] << 8 | in[73]);
    return 0;
}

// {"op":op,"d":d}, d is consumed.
static char *op_json(int op, cJSON *d)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddNumberToObject(m, "op", op);
    cJSON_AddItemToObject(m, "d", d);
    char *s = cJSON_PrintUnformatted(m);
    cJSON_Delete(m);
    return s;
}

char *voice_json_identify(const char *server_id, const char *user_id, const char *session_id,
                          const char *token, int dave_max_version)
{
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "server_id", server_id);
    cJSON_AddStringToObject(d, "user_id", user_id);
    cJSON_AddStringToObject(d, "session_id", session_id);
    cJSON_AddStringToObject(d, "token", token);
    cJSON_AddNumberToObject(d, "max_dave_protocol_version", dave_max_version);
    return op_json(0, d);
}

char *voice_json_resume(const char *server_id, const char *session_id, const char *token, int64_t seq_ack)
{
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "server_id", server_id);
    cJSON_AddStringToObject(d, "session_id", session_id);
    cJSON_AddStringToObject(d, "token", token);
    cJSON_AddNumberToObject(d, "seq_ack", (double)seq_ack);
    return op_json(7, d);
}

char *voice_json_heartbeat(int64_t nonce, int64_t seq_ack)
{
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "t", (double)nonce);
    cJSON_AddNumberToObject(d, "seq_ack", (double)seq_ack);
    return op_json(3, d);
}

char *voice_json_select_udp(const char *address, uint16_t port, const char *mode)
{
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "address", address);
    cJSON_AddNumberToObject(data, "port", port);
    cJSON_AddStringToObject(data, "mode", mode);
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "protocol", "udp");
    cJSON_AddItemToObject(d, "data", data);
    return op_json(1, d);
}

char *voice_json_speaking(int speaking, uint32_t ssrc)
{
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "speaking", speaking); // 1 voice, 2 soundshare (Go Live), 0 off
    cJSON_AddNumberToObject(d, "delay", 0);
    cJSON_AddNumberToObject(d, "ssrc", (double)ssrc);
    return op_json(5, d);
}

char *voice_json_transition(int op, int64_t transition_id)
{
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "transition_id", (double)transition_id);
    return op_json(op, d);
}

int voice_bin_parse(const uint8_t *msg, size_t len, voice_bin *out)
{
    if (len < 3)
        return -1;
    out->seq = msg[0] << 8 | msg[1];
    out->op = msg[2];
    out->payload = msg + 3;
    out->len = len - 3;
    return 0;
}

size_t voice_bin_build(uint8_t *out, size_t cap, int op, const uint8_t *payload, size_t len)
{
    if (cap < len + 1)
        return 0;
    out[0] = (uint8_t)op;
    memcpy(out + 1, payload, len);
    return len + 1;
}

const uint8_t *h264_next_nal(const uint8_t *b, size_t n, size_t *pos, size_t *len)
{
    size_t i = *pos;
    while (i + 3 <= n && !(b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1))
        i++;
    if (i + 3 > n)
        return NULL;
    size_t start = i + 3, j = start;
    while (j + 3 <= n && !(b[j] == 0 && b[j + 1] == 0 && (b[j + 2] == 1 || (b[j + 2] == 0 && j + 3 < n && b[j + 3] == 1))))
        j++;
    if (j + 3 > n)
        j = n;
    *pos = j;
    *len = j - start;
    return b + start;
}

void h264_packetize(const uint8_t *au, size_t n, size_t mtu,
                    void (*emit)(void *user, const uint8_t *payload, size_t len, int marker), void *user)
{
    size_t pos = 0, len;
    const uint8_t *nal = h264_next_nal(au, n, &pos, &len);
    uint8_t frag[1500 + 2];
    if (mtu > 1500)
        mtu = 1500;
    while (nal) {
        size_t nlen = len;
        const uint8_t *next = h264_next_nal(au, n, &pos, &len);
        const int last_nal = next == NULL;
        if (nlen <= mtu) {
            emit(user, nal, nlen, last_nal);
        } else {
            frag[0] = (uint8_t)((nal[0] & 0xe0) | 28); // FU indicator: the NAL's NRI, type 28
            for (size_t off = 1; off < nlen;) {
                size_t chunk = nlen - off > mtu ? mtu : nlen - off;
                frag[1] = (uint8_t)((off == 1 ? 0x80 : 0) | (off + chunk == nlen ? 0x40 : 0) | (nal[0] & 0x1f));
                memcpy(frag + 2, nal + off, chunk);
                off += chunk;
                emit(user, frag, chunk + 2, last_nal && off == nlen);
            }
        }
        nal = next;
    }
}
