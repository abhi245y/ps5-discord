// SPDX-License-Identifier: AGPL-3.0-or-later
// Go Live (stream.h, docs/go-live.md). Two halves meet in `conn`:
//   - the stream thread runs the stream's voice websocket: identify, UDP setup, the
//     transport key and the DAVE group (group id rtc_server_id - 1), and asks the
//     console for a keyframe every second;
//   - the broadcast's frames arrive on Remote Play's threads and go out from there:
//     DAVE-encrypt the whole frame, then RTP (H.264 FU-A at 90 kHz, Opus at 48 kHz),
//     then the transport seal with one nonce counter for both.
// Everything in conn that both halves touch is guarded by mlock.
// ponytail: no resume; a dropped stream connection ends the broadcast with an error.
#include "stream.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <dave/dave.h>
#include "broadcast.h"
#include "log.h"
#include "tls.h"
#include "voice.h"
#include "voice_proto.h"
#include "ws.h"

#define IDS 32
#define MAX_USERS 64
#define PT_OPUS 120
#define PT_H264 101
#define PT_H264_RTX 102
#define RTP_MTU 1200     // largest RTP payload
#define WIDTH 1280       // broadcast.c asks Remote Play for 720p30
#define HEIGHT 720
#define FPS 30
// The playout-delay header extension the reference clients put on video: with it Discord's
// stream server drops every video packet (2026-10-28, error 2012 on viewers).
#define VIDEO_EXT 0

typedef struct {
    char endpoint[256], token[256], server_id[IDS], user_id[IDS], session_id[64];
    ws_conn ws;
    int udp;
    uint32_t audio_ssrc, video_ssrc, rtx_ssrc;
    uint8_t key[32];
    int64_t seq_ack, hb_ms, next_hb, hb_sent;
    int hb_acked, healthy, announced;
    int dave_version, pending_version, dave_ready;
    DAVESessionHandle dave;
    DAVEEncryptorHandle enc;
    uint8_t ext_sender[512];
    size_t ext_sender_len;
    char users[MAX_USERS][IDS];
    int n_users;
    // the media half (mlock)
    int media_ok, need_idr; // need_idr: viewers start from a keyframe, so P-frames wait
    uint16_t seq_a, seq_v;
    uint32_t ts_a, counter;
    int64_t t0;
    unsigned sent_frames, sent_packets, sent_audio, dropped;
    char err[200];
} conn;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;  // the gateway side below
static pthread_mutex_t mlock = PTHREAD_MUTEX_INITIALIZER; // conn's shared part, DAVE
static char state[16] = "off", error[200];
static int want, running, viewers;
static char guild[IDS], channel[IDS], self[IDS], session[64], key_[160];
static char rtc_server[IDS], endpoint[256], token[256];
static cJSON *gw_queue[4];
static int gw_count;
static conn *media; // the live connection, for the frame senders (mlock)
static uint8_t sps_pps[512];
static size_t sps_pps_len; // the latest parameter sets (mlock)

static int64_t mono_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void copy(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src ? src : "");
}

static const char *str(cJSON *o, const char *k)
{
    cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static int64_t num(cJSON *o, const char *k)
{
    cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? (int64_t)v->valuedouble : cJSON_IsString(v) ? strtoll(v->valuestring, NULL, 10) : 0;
}

static void set_state(const char *s, const char *why)
{
    pthread_mutex_lock(&lock);
    copy(state, sizeof state, s);
    if (why)
        copy(error, sizeof error, why);
    pthread_mutex_unlock(&lock);
}

const char *stream_state(void)
{
    return state;
}

const char *stream_error(void)
{
    return error;
}

int stream_viewers(void)
{
    return viewers;
}

// lock held. Queues a gateway op for the gateway thread.
static void gw_push(int op, cJSON *d)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddNumberToObject(m, "op", op);
    cJSON_AddItemToObject(m, "d", d);
    if (gw_count < 4)
        gw_queue[gw_count++] = m;
    else
        cJSON_Delete(m);
}

cJSON *stream_take_gateway(void)
{
    pthread_mutex_lock(&lock);
    cJSON *m = NULL;
    if (gw_count) {
        m = gw_queue[0];
        memmove(gw_queue, gw_queue + 1, (size_t)--gw_count * sizeof *gw_queue);
    }
    pthread_mutex_unlock(&lock);
    return m;
}

// ---- media: frames from the broadcast -------------------------------------------

// mlock held. One RTP packet: header (+ the playout-delay extension on video), payload,
// sealed with the connection's transport key.
static void send_rtp(conn *c, int pt, uint32_t ssrc, uint16_t seq, uint32_t ts, int marker, int ext,
                     const uint8_t *p, size_t n)
{
    uint8_t plain[12 + 8 + RTP_MTU + 2], out[sizeof plain + 32];
    size_t h = 12;
    if (h + 8 + n > sizeof plain)
        return;
    plain[0] = ext ? 0x90 : 0x80;
    plain[1] = (uint8_t)(pt | (marker ? 0x80 : 0));
    plain[2] = (uint8_t)(seq >> 8), plain[3] = (uint8_t)seq;
    plain[4] = (uint8_t)(ts >> 24), plain[5] = (uint8_t)(ts >> 16), plain[6] = (uint8_t)(ts >> 8), plain[7] = (uint8_t)ts;
    plain[8] = (uint8_t)(ssrc >> 24), plain[9] = (uint8_t)(ssrc >> 16), plain[10] = (uint8_t)(ssrc >> 8),
    plain[11] = (uint8_t)ssrc;
    if (ext) { // one-byte header extension: ID 5 playout delay, min = max = 0
        static const uint8_t e[8] = {0xbe, 0xde, 0x00, 0x01, 0x52, 0x00, 0x00, 0x00};
        memcpy(plain + 12, e, 8);
        h += 8;
    }
    memcpy(plain + h, p, n);
    size_t len = voice_gcm_seal(c->key, c->counter++, plain, h + n, out, sizeof out);
    if (len && send(c->udp, out, len, 0) == (ssize_t)len)
        c->sent_packets++;
    else
        c->dropped++;
}

// The next NAL unit in Annex-B data from *pos: returns its start (after the start code)
// and length, or NULL at the end.
static const uint8_t *next_nal(const uint8_t *b, size_t n, size_t *pos, size_t *len)
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

// mlock held. RFC 6184: a NAL up to RTP_MTU goes whole, a larger one as FU-A fragments.
static void send_frame_rtp(conn *c, const uint8_t *f, size_t n, uint32_t ts)
{
    size_t pos = 0, len;
    const uint8_t *nal, *next;
    nal = next_nal(f, n, &pos, &len);
    while (nal) {
        size_t nlen = len;
        next = next_nal(f, n, &pos, &len);
        int last_nal = next == NULL;
        if (nlen <= RTP_MTU) {
            send_rtp(c, PT_H264, c->video_ssrc, c->seq_v++, ts, last_nal, VIDEO_EXT, nal, nlen);
        } else {
            uint8_t frag[RTP_MTU + 2];
            frag[0] = (uint8_t)((nal[0] & 0xe0) | 28);
            for (size_t off = 1; off < nlen;) {
                size_t chunk = nlen - off > RTP_MTU ? RTP_MTU : nlen - off;
                frag[1] = (uint8_t)((off == 1 ? 0x80 : 0) | (off + chunk == nlen ? 0x40 : 0) | (nal[0] & 0x1f));
                memcpy(frag + 2, nal + off, chunk);
                off += chunk;
                send_rtp(c, PT_H264, c->video_ssrc, c->seq_v++, ts, last_nal && off == nlen, VIDEO_EXT, frag, chunk + 2);
            }
        }
        nal = next;
    }
}

static void on_video(const uint8_t *au, size_t n, int keyframe)
{
    (void)keyframe;
    // Which NAL types the access unit holds; AUDs (9) are left out of what is sent.
    int has_slice = 0, has_idr = 0, has_ps = 0;
    size_t pos = 0, len, nals = 0, payload = 0;
    const uint8_t *nal;
    while ((nal = next_nal(au, n, &pos, &len)) && len) {
        int t = nal[0] & 0x1f;
        nals++;
        payload += len;
        has_slice |= t == 1 || t == 5;
        has_idr |= t == 5;
        has_ps |= t == 7 || t == 8;
    }
    pthread_mutex_lock(&mlock);
    if (has_ps && !has_slice) { // Remote Play sends SPS and PPS as a frame of their own
        if (n <= sizeof sps_pps) {
            memcpy(sps_pps, au, n);
            sps_pps_len = n;
        }
        pthread_mutex_unlock(&mlock);
        return;
    }
    conn *c = media;
    if (!c || !c->media_ok || !has_slice || (c->need_idr && !has_idr)) {
        pthread_mutex_unlock(&mlock);
        return;
    }
    c->need_idr = 0;
    // Viewers who join late need SPS and PPS in front of every IDR.
    size_t extra = has_idr && !has_ps ? sps_pps_len : 0;
    // Rebuilt with 4-byte start codes; Remote Play puts 3-byte ones between slices.
    uint8_t *frame = malloc(extra + payload + 4 * nals);
    size_t m = 0;
    if (frame) {
        memcpy(frame, sps_pps, extra);
        m = extra;
        pos = 0;
        while ((nal = next_nal(au, n, &pos, &len)) && len) {
            if ((nal[0] & 0x1f) == 9)
                continue;
            static const uint8_t sc[4] = {0, 0, 0, 1};
            memcpy(frame + m, sc, 4);
            memcpy(frame + m + 4, nal, len);
            m += 4 + len;
        }
    }
    uint8_t *out = NULL;
    size_t out_len = m;
    if (frame && c->dave_version > 0) {
        // libdave's estimate leaves 64 bytes for the trailer, but the trailer lists every
        // NAL's clear range (~4 bytes each) and Remote Play's IDRs carry ~25 slices.
        size_t cap = daveEncryptorGetMaxCiphertextByteSize(c->enc, DAVE_MEDIA_TYPE_VIDEO, m) + 8 * (nals + 2);
        out = malloc(cap);
        if (!out || daveEncryptorEncrypt(c->enc, DAVE_MEDIA_TYPE_VIDEO, c->video_ssrc, frame, m, out, cap,
                                         &out_len) != DAVE_ENCRYPTOR_RESULT_CODE_SUCCESS) {
            free(out);
            out = NULL;
            c->dropped++;
        }
    }
    const uint8_t *send_buf = c->dave_version > 0 ? out : frame;
    if (send_buf) {
        send_frame_rtp(c, send_buf, out_len, (uint32_t)((mono_ms() - c->t0) * 90));
        c->sent_frames++;
    }
    pthread_mutex_unlock(&mlock);
    free(frame);
    free(out);
}

static void on_audio(const uint8_t *opus, size_t n)
{
    pthread_mutex_lock(&mlock);
    conn *c = media;
    if (c && c->media_ok) {
        uint8_t body[1500];
        const uint8_t *p = opus;
        size_t m = n;
        if (c->dave_version > 0) {
            size_t cap = daveEncryptorGetMaxCiphertextByteSize(c->enc, DAVE_MEDIA_TYPE_AUDIO, n);
            if (cap > sizeof body || daveEncryptorEncrypt(c->enc, DAVE_MEDIA_TYPE_AUDIO, c->audio_ssrc, opus, n, body,
                                                          cap, &m) != DAVE_ENCRYPTOR_RESULT_CODE_SUCCESS)
                p = NULL;
            else
                p = body;
        }
        if (p) {
            send_rtp(c, PT_OPUS, c->audio_ssrc, c->seq_a++, c->ts_a, 0, 0, p, m);
            c->sent_audio++;
        }
        c->ts_a += 480; // 10 ms at 48 kHz
    }
    pthread_mutex_unlock(&mlock);
}

static const bc_sink sink = {.video = on_video, .audio = on_audio};

void stream_init(void)
{
    bc_set_sink(&sink);
}

// ---- the stream connection ------------------------------------------------------

static int send_text(conn *c, cJSON *m)
{
    char *s = cJSON_PrintUnformatted(m);
    cJSON_Delete(m);
    int r = s ? ws_send(&c->ws, WS_TEXT, s, strlen(s)) : -1;
    free(s);
    return r;
}

static int send_plain(conn *c, char *s)
{
    int r = s ? ws_send(&c->ws, WS_TEXT, s, strlen(s)) : -1;
    free(s);
    return r;
}

static cJSON *op(int n, cJSON **d)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddNumberToObject(m, "op", n);
    *d = cJSON_AddObjectToObject(m, "d");
    return m;
}

static int send_bin(conn *c, int code, const uint8_t *p, size_t n)
{
    uint8_t *buf = malloc(n + 1);
    if (!buf)
        return -1;
    size_t k = voice_bin_build(buf, n + 1, code, p, n);
    int r = k ? ws_send(&c->ws, WS_BINARY, buf, k) : -1;
    free(buf);
    return r;
}

static int identify(conn *c)
{
    cJSON *d, *m = op(0, &d);
    cJSON_AddStringToObject(d, "server_id", c->server_id);
    cJSON_AddStringToObject(d, "user_id", c->user_id);
    cJSON_AddStringToObject(d, "session_id", c->session_id);
    cJSON_AddStringToObject(d, "token", c->token);
    cJSON_AddTrueToObject(d, "video");
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "screen");
    cJSON_AddStringToObject(s, "rid", "100");
    cJSON_AddNumberToObject(s, "quality", 100);
    cJSON_AddItemToArray(cJSON_AddArrayToObject(d, "streams"), s);
    cJSON_AddNumberToObject(d, "max_dave_protocol_version", daveMaxSupportedProtocolVersion());
    return send_text(c, m);
}

// Op 12: the video stream on (with its size) or off.
static int video_attributes(conn *c, int on)
{
    cJSON *d, *m = op(12, &d);
    cJSON_AddNumberToObject(d, "audio_ssrc", c->audio_ssrc);
    cJSON_AddNumberToObject(d, "video_ssrc", on ? c->video_ssrc : 0);
    cJSON_AddNumberToObject(d, "rtx_ssrc", on ? c->rtx_ssrc : 0);
    cJSON *streams = cJSON_AddArrayToObject(d, "streams");
    if (on) {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "type", "video");
        cJSON_AddStringToObject(s, "rid", "100");
        cJSON_AddNumberToObject(s, "ssrc", c->video_ssrc);
        cJSON_AddTrueToObject(s, "active");
        cJSON_AddNumberToObject(s, "quality", 100);
        cJSON_AddNumberToObject(s, "rtx_ssrc", c->rtx_ssrc);
        cJSON_AddNumberToObject(s, "max_bitrate", 10000000);
        cJSON_AddNumberToObject(s, "max_framerate", FPS);
        cJSON *r = cJSON_AddObjectToObject(s, "max_resolution");
        cJSON_AddStringToObject(r, "type", "fixed");
        cJSON_AddNumberToObject(r, "width", WIDTH);
        cJSON_AddNumberToObject(r, "height", HEIGHT);
        cJSON_AddItemToArray(streams, s);
    }
    return send_text(c, m);
}

static int select_protocol(conn *c, const char *address, uint16_t port)
{
    cJSON *d, *m = op(1, &d);
    cJSON_AddStringToObject(d, "protocol", "udp");
    cJSON *data = cJSON_AddObjectToObject(d, "data");
    cJSON_AddStringToObject(data, "address", address);
    cJSON_AddNumberToObject(data, "port", port);
    cJSON_AddStringToObject(data, "mode", VOICE_MODE_GCM);
    cJSON *codecs = cJSON_AddArrayToObject(d, "codecs");
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "name", "opus");
    cJSON_AddStringToObject(a, "type", "audio");
    cJSON_AddNumberToObject(a, "priority", 1000);
    cJSON_AddNumberToObject(a, "payload_type", PT_OPUS);
    cJSON_AddItemToArray(codecs, a);
    cJSON *v = cJSON_CreateObject();
    cJSON_AddStringToObject(v, "name", "H264");
    cJSON_AddStringToObject(v, "type", "video");
    cJSON_AddNumberToObject(v, "priority", 1000);
    cJSON_AddNumberToObject(v, "payload_type", PT_H264);
    cJSON_AddNumberToObject(v, "rtx_payload_type", PT_H264_RTX);
    cJSON_AddTrueToObject(v, "encode");
    cJSON_AddTrueToObject(v, "decode");
    cJSON_AddItemToArray(codecs, v);
    return send_text(c, m);
}

static int udp_open(conn *c, const char *ip, int port)
{
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1)
        return -1;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    struct timeval tv = {.tv_sec = 1};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int buf = 1 << 20; // a keyframe goes out as one burst of packets
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof buf);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        close(fd);
        return -1;
    }
    c->udp = fd;
    return 0;
}

static int discover(conn *c, char address[64], uint16_t *port)
{
    uint8_t req[74], rep[74];
    voice_ip_discovery_request(req, c->audio_ssrc);
    for (int i = 0; i < 5; i++) {
        if (send(c->udp, req, sizeof req, 0) != (ssize_t)sizeof req)
            return -1;
        ssize_t n = recv(c->udp, rep, sizeof rep, 0);
        if (n == (ssize_t)sizeof rep && voice_ip_discovery_parse(rep, sizeof rep, address, port) == 0)
            return 0;
    }
    return -1;
}

static int fatal(conn *c, const char *msg)
{
    copy(c->err, sizeof c->err, msg);
    return -2;
}

// mlock held. Media may flow once the transport key is in and, with DAVE, our key is.
static void update_media(conn *c)
{
    int ok = c->healthy;
    if (ok && c->dave_version > 0) {
        DAVEKeyRatchetHandle r = c->dave_ready ? daveSessionGetKeyRatchet(c->dave, c->user_id) : NULL;
        if (r) {
            daveEncryptorSetKeyRatchet(c->enc, r);
            daveKeyRatchetDestroy(r);
        }
        // A new epoch or a failed commit clears dave_ready: the old key must not be used.
        ok = c->dave_ready && (r != NULL || daveEncryptorHasKeyRatchet(c->enc));
    }
    if (!c->enc)
        ok = 0;
    if (ok && !c->media_ok) {
        LOG("stream: media on (DAVE %d)", c->dave_version);
        c->need_idr = 1;
    }
    c->media_ok = ok;
}

static void dave_failed(const char *source, const char *reason, void *user)
{
    (void)user;
    LOG("stream: DAVE %s: %s", source ? source : "?", reason ? reason : "?");
}

// mlock held.
static void dave_start(conn *c, int version)
{
    c->dave_version = c->pending_version = version;
    if (version <= 0)
        return;
    if (!c->dave)
        c->dave = daveSessionCreate(NULL, "", dave_failed, NULL);
    if (c->dave) {
        // The stream's MLS group is rtc_server_id - 1 (docs/go-live.md).
        daveSessionInit(c->dave, (uint16_t)version, strtoull(c->server_id, NULL, 10) - 1, c->user_id);
        if (c->ext_sender_len)
            daveSessionSetExternalSender(c->dave, c->ext_sender, c->ext_sender_len);
    }
}

static int send_key_package(conn *c)
{
    uint8_t *kp = NULL;
    size_t len = 0;
    pthread_mutex_lock(&mlock);
    if (c->dave)
        daveSessionGetMarshalledKeyPackage(c->dave, &kp, &len);
    pthread_mutex_unlock(&mlock);
    int r = kp ? send_bin(c, 26, kp, len) : 0;
    daveFree(kp);
    return r;
}

static int on_ready(conn *c, cJSON *d)
{
    c->audio_ssrc = (uint32_t)num(d, "ssrc");
    cJSON *s = cJSON_GetArrayItem(cJSON_GetObjectItem(d, "streams"), 0);
    c->video_ssrc = (uint32_t)num(s, "ssrc");
    c->rtx_ssrc = (uint32_t)num(s, "rtx_ssrc");
    int gcm = 0;
    cJSON *m;
    cJSON_ArrayForEach(m, cJSON_GetObjectItem(d, "modes"))
        gcm |= cJSON_IsString(m) && strcmp(m->valuestring, VOICE_MODE_GCM) == 0;
    if (!gcm || !c->video_ssrc)
        return fatal(c, "Discord's stream server offered no setup this build supports.");
    LOG("stream: ready, audio ssrc %u, video ssrc %u, rtx %u", c->audio_ssrc, c->video_ssrc, c->rtx_ssrc);
    const char *ip = str(d, "ip");
    char address[64];
    uint16_t port = 0;
    if (video_attributes(c, 0) != 0)
        return -1;
    if (!ip || udp_open(c, ip, (int)num(d, "port")) != 0 || discover(c, address, &port) != 0)
        return fatal(c, "Could not reach Discord's stream server.");
    return select_protocol(c, address, port) == 0 ? 0 : -1;
}

static int on_session(conn *c, cJSON *d)
{
    const char *video = str(d, "video_codec");
    if (!video || strcmp(video, "H264") != 0)
        return fatal(c, "Discord's stream server did not take H.264.");
    cJSON *k = cJSON_GetObjectItem(d, "secret_key");
    if (cJSON_GetArraySize(k) != 32)
        return fatal(c, "Discord's stream server sent a bad key.");
    pthread_mutex_lock(&mlock);
    for (int i = 0; i < 32; i++)
        c->key[i] = (uint8_t)cJSON_GetNumberValue(cJSON_GetArrayItem(k, i));
    dave_start(c, (int)num(d, "dave_protocol_version"));
    c->enc = daveEncryptorCreate();
    if (c->enc) {
        daveEncryptorAssignSsrcToCodec(c->enc, c->audio_ssrc, DAVE_CODEC_OPUS);
        daveEncryptorAssignSsrcToCodec(c->enc, c->video_ssrc, DAVE_CODEC_H264);
        daveEncryptorSetPassthroughMode(c->enc, false);
    }
    c->t0 = mono_ms();
    c->healthy = 1;
    update_media(c);
    media = c;
    pthread_mutex_unlock(&mlock);
    LOG("stream: session up, video H264, DAVE version %d", c->dave_version);
    // Soundshare speaking, then the video on, before any media.
    if (send_plain(c, voice_json_speaking(2, c->audio_ssrc)) != 0 || video_attributes(c, 1) != 0)
        return -1;
    c->announced = 1;
    set_state("live", "");
    return send_key_package(c);
}

static size_t recognized(conn *c, const char *ids[MAX_USERS + 1])
{
    size_t n = 0;
    int me = 0;
    for (int i = 0; i < c->n_users; i++) {
        ids[n++] = c->users[i];
        me |= strcmp(c->users[i], c->user_id) == 0;
    }
    if (!me)
        ids[n++] = c->user_id;
    return n;
}

static void users_add(conn *c, const char *id)
{
    for (int i = 0; i < c->n_users; i++)
        if (strcmp(c->users[i], id) == 0)
            return;
    if (c->n_users < MAX_USERS)
        copy(c->users[c->n_users++], IDS, id);
}

static void users_remove(conn *c, const char *id)
{
    for (int i = 0; i < c->n_users; i++)
        if (strcmp(c->users[i], id) == 0) {
            memmove(c->users[i], c->users[i + 1], (size_t)(c->n_users - i - 1) * IDS);
            c->n_users--;
            return;
        }
}

static void dave_ready(conn *c)
{
    pthread_mutex_lock(&mlock);
    c->dave_ready = 1;
    update_media(c);
    pthread_mutex_unlock(&mlock);
    bc_request_keyframe(); // a new viewer needs a picture to start from
}

static void on_binary(conn *c, const uint8_t *msg, size_t len)
{
    voice_bin b;
    if (voice_bin_parse(msg, len, &b) != 0)
        return;
    if (b.seq >= 0)
        c->seq_ack = b.seq;
    LOG("stream: DAVE op %d, %zu bytes", b.op, b.len);
    if (b.op == 25) {
        if (b.len <= sizeof c->ext_sender) {
            memcpy(c->ext_sender, b.payload, b.len);
            c->ext_sender_len = b.len;
        }
        pthread_mutex_lock(&mlock);
        if (c->dave)
            daveSessionSetExternalSender(c->dave, b.payload, b.len);
        pthread_mutex_unlock(&mlock);
    } else if (b.op == 27 && c->dave && b.len >= 2) {
        const char *ids[MAX_USERS + 1];
        size_t n_ids = recognized(c, ids);
        uint8_t *out = NULL;
        size_t out_len = 0;
        pthread_mutex_lock(&mlock);
        daveSessionProcessProposals(c->dave, b.payload, b.len, ids, n_ids, &out, &out_len);
        pthread_mutex_unlock(&mlock);
        if (out && out_len)
            send_bin(c, 28, out, out_len);
        if (out)
            daveFree(out);
        if (out_len)
            dave_ready(c);
    } else if ((b.op == 29 || b.op == 30) && c->dave && b.len >= 2) {
        int64_t tid = b.payload[0] << 8 | b.payload[1];
        int failed;
        pthread_mutex_lock(&mlock);
        if (b.op == 29) {
            DAVECommitResultHandle r = daveSessionProcessCommit(c->dave, b.payload + 2, b.len - 2);
            failed = !r || daveCommitResultIsFailed(r);
            if (r)
                daveCommitResultDestroy(r);
        } else {
            const char *ids[MAX_USERS + 1];
            size_t n_ids = recognized(c, ids);
            DAVEWelcomeResultHandle r = daveSessionProcessWelcome(c->dave, b.payload + 2, b.len - 2, ids, n_ids);
            failed = !r;
            if (r)
                daveWelcomeResultDestroy(r);
        }
        pthread_mutex_unlock(&mlock);
        LOG("stream: DAVE %s %s", b.op == 29 ? "commit" : "welcome", failed ? "failed" : "ok");
        if (failed) {
            send_plain(c, voice_json_transition(31, tid));
            return;
        }
        if (tid)
            send_plain(c, voice_json_transition(23, tid));
        dave_ready(c);
    }
}

static int on_text(conn *c, const char *json)
{
    cJSON *m = cJSON_Parse(json);
    if (!m)
        return 0;
    if (cJSON_IsNumber(cJSON_GetObjectItem(m, "seq")))
        c->seq_ack = num(m, "seq");
    int code = (int)num(m, "op");
    cJSON *d = cJSON_GetObjectItem(m, "d");
    int r = 0;
    switch (code) {
    case 8: // HELLO
        c->hb_ms = num(d, "heartbeat_interval");
        if (c->hb_ms < 1000)
            c->hb_ms = 1000;
        c->next_hb = mono_ms() + c->hb_ms;
        break;
    case 2:
        r = on_ready(c, d);
        break;
    case 4:
        r = on_session(c, d);
        break;
    case 6:
        c->hb_acked = 1;
        break;
    case 11: {
        cJSON *u;
        cJSON_ArrayForEach(u, cJSON_GetObjectItem(d, "user_ids"))
            if (cJSON_IsString(u))
                users_add(c, u->valuestring);
        break;
    }
    case 13:
        if (str(d, "user_id"))
            users_remove(c, str(d, "user_id"));
        break;
    case 21: { // PREPARE_TRANSITION
        int64_t tid = num(d, "transition_id");
        c->pending_version = (int)num(d, "protocol_version");
        if (tid == 0) {
            pthread_mutex_lock(&mlock);
            c->dave_version = c->pending_version;
            update_media(c);
            pthread_mutex_unlock(&mlock);
        } else
            r = send_plain(c, voice_json_transition(23, tid));
        break;
    }
    case 22: // EXECUTE_TRANSITION
        pthread_mutex_lock(&mlock);
        c->dave_version = c->pending_version;
        update_media(c);
        pthread_mutex_unlock(&mlock);
        break;
    case 24: // PREPARE_EPOCH; epoch 1 starts a new group
        if (num(d, "epoch") == 1) {
            pthread_mutex_lock(&mlock);
            if (c->dave)
                daveSessionDestroy(c->dave);
            c->dave = NULL;
            c->dave_ready = 0;
            dave_start(c, (int)num(d, "protocol_version"));
            update_media(c);
            pthread_mutex_unlock(&mlock);
            r = send_key_package(c);
        }
        break;
    default: { // once per opcode, with its payload: what the server tells a streamer
        static uint64_t seen;
        if (code >= 0 && code < 64 && !(seen >> code & 1)) {
            seen |= 1ull << code;
            char *p = d ? cJSON_PrintUnformatted(d) : NULL;
            LOG("stream: op %d not handled: %.300s", code, p ? p : "");
            free(p);
        }
    }
    }
    cJSON_Delete(m);
    return r;
}

static int still_wanted(void)
{
    pthread_mutex_lock(&lock);
    int w = want;
    pthread_mutex_unlock(&lock);
    return w && voice_wanted();
}

// The websocket's life. Returns the close code, 0 when stopped, -1 dropped, -2 fatal.
static int serve(conn *c)
{
    if (identify(c) != 0)
        return -1;
    c->hb_acked = 1;
    int64_t next_key = 0, next_log = mono_ms() + 5000;
    for (;;) {
        if (!still_wanted())
            return 0;
        int64_t now = mono_ms();
        if (c->next_hb && now >= c->next_hb) {
            if (!c->hb_acked)
                return -1;
            c->hb_sent = now;
            c->hb_acked = 0;
            if (send_plain(c, voice_json_heartbeat(now, c->seq_ack)) != 0)
                return -1;
            c->next_hb = now + c->hb_ms;
        }
        if (c->media_ok && now >= next_key) { // late viewers need a keyframe each second
            bc_request_keyframe();
            next_key = now + 1000;
        }
        if (now >= next_log) {
            pthread_mutex_lock(&mlock);
            LOG("stream: sent %u frames, %u audio, %u packets, %u dropped; media %d, DAVE %d ready %d, %d users",
                c->sent_frames, c->sent_audio, c->sent_packets, c->dropped, c->media_ok, c->dave_version,
                c->dave_ready, c->n_users);
            pthread_mutex_unlock(&mlock);
            next_log = now + 5000;
        }
        int m = ws_recv(&c->ws, 200);
        if (m == 0)
            continue;
        if (m == WS_CLOSE)
            return c->ws.close_code;
        if (m < 0)
            return -1;
        if (m == WS_BINARY) {
            on_binary(c, c->ws.msg, c->ws.msg_len);
            continue;
        }
        int r = on_text(c, (const char *)c->ws.msg);
        if (r)
            return r;
    }
}

static void *stream_main(void *arg)
{
    conn *c = arg;
    char url[400];
    snprintf(url, sizeof url, "wss://%s/?v=8", c->endpoint);
    int code = -1;
    if (ws_open(&c->ws, url) == 0) {
        code = serve(c);
        if (c->announced) {
            send_plain(c, voice_json_speaking(0, c->audio_ssrc));
            video_attributes(c, 0);
        }
        ws_close(&c->ws, 1000);
    } else
        tls_close(&c->ws.tls);
    pthread_mutex_lock(&mlock);
    media = NULL;
    pthread_mutex_unlock(&mlock);
    LOG("stream: connection ended (%d): %u frames, %u audio, %u packets", code, c->sent_frames, c->sent_audio,
        c->sent_packets);
    const char *why = code == -2 ? c->err
                      : code == 4017 ? "Discord asked for end-to-end encryption the stream did not set up."
                      : code != 0    ? "Lost Discord's stream server."
                                     : NULL;
    pthread_mutex_lock(&lock);
    if (want) { // ended by itself: take the stream down at Discord too
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "stream_key", key_);
        gw_push(19, d);
        want = 0;
    }
    running = 0;
    rtc_server[0] = endpoint[0] = token[0] = 0;
    pthread_mutex_unlock(&lock);
    set_state(why ? "error" : "off", why ? why : "");
    bc_stop(); // the picture has nowhere to go
    if (c->enc)
        daveEncryptorDestroy(c->enc);
    if (c->dave)
        daveSessionDestroy(c->dave);
    if (c->udp >= 0)
        close(c->udp);
    free(c);
    return NULL;
}

// lock held. Both halves of the server's answer are in: open the connection.
static void connect_locked(void)
{
    if (running || !rtc_server[0] || !endpoint[0] || !token[0])
        return;
    conn *c = calloc(1, sizeof *c);
    if (!c)
        return;
    c->udp = -1;
    c->seq_ack = -1;
    copy(c->endpoint, sizeof c->endpoint, endpoint);
    copy(c->token, sizeof c->token, token);
    copy(c->server_id, sizeof c->server_id, rtc_server);
    copy(c->user_id, sizeof c->user_id, self);
    copy(c->session_id, sizeof c->session_id, session);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    pthread_t th;
    if (pthread_create(&th, &attr, stream_main, c) == 0)
        running = 1;
    else
        free(c);
}

int stream_start(void)
{
    char g[IDS], ch[IDS], me[IDS], sess[64];
    if (!voice_current(g, ch, me, sess)) {
        set_state("error", "Join a call first.");
        return -1;
    }
    pthread_mutex_lock(&lock);
    if (want) {
        pthread_mutex_unlock(&lock);
        return 0;
    }
    want = 1;
    viewers = 0;
    copy(guild, sizeof guild, g);
    copy(channel, sizeof channel, ch);
    copy(self, sizeof self, me);
    copy(session, sizeof session, sess);
    if (g[0])
        snprintf(key_, sizeof key_, "guild:%s:%s:%s", g, ch, me);
    else
        snprintf(key_, sizeof key_, "call:%s:%s", ch, me);
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "type", g[0] ? "guild" : "call");
    cJSON_AddItemToObject(d, "guild_id", g[0] ? cJSON_CreateString(g) : cJSON_CreateNull());
    cJSON_AddStringToObject(d, "channel_id", ch);
    cJSON_AddNullToObject(d, "preferred_region");
    gw_push(18, d);
    d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "stream_key", key_);
    cJSON_AddFalseToObject(d, "paused");
    gw_push(22, d);
    pthread_mutex_unlock(&lock);
    set_state("starting", "");
    LOG("stream: asked for %s", key_);
    return 0;
}

void stream_stop(void)
{
    pthread_mutex_lock(&lock);
    if (want) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "stream_key", key_);
        gw_push(19, d);
    }
    want = 0;
    pthread_mutex_unlock(&lock);
    if (!running)
        set_state("off", "");
}

void stream_on_dispatch(const char *type, cJSON *d)
{
    const char *k = str(d, "stream_key");
    pthread_mutex_lock(&lock);
    if (!want || !k || strcmp(k, key_) != 0) {
        pthread_mutex_unlock(&lock);
        return;
    }
    if (strcmp(type, "STREAM_CREATE") == 0) {
        copy(rtc_server, sizeof rtc_server, str(d, "rtc_server_id"));
        connect_locked();
    } else if (strcmp(type, "STREAM_SERVER_UPDATE") == 0) {
        copy(endpoint, sizeof endpoint, str(d, "endpoint"));
        copy(token, sizeof token, str(d, "token"));
        connect_locked();
    } else if (strcmp(type, "STREAM_UPDATE") == 0) {
        viewers = cJSON_GetArraySize(cJSON_GetObjectItem(d, "viewer_ids"));
        LOG("stream: %d watching", viewers);
    } else if (strcmp(type, "STREAM_DELETE") == 0 && !cJSON_IsTrue(cJSON_GetObjectItem(d, "unavailable"))) {
        LOG("stream: Discord ended the stream (%s)", str(d, "reason") ? str(d, "reason") : "?");
        want = 0;
    }
    pthread_mutex_unlock(&lock);
}
