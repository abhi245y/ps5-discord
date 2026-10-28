#include "voice_audio.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "pcm_stream.h"
#include "prefs.h"
#include "voice_proto.h"

// ponytail: fixed thresholds, tuned by ear. add a knob when a mic needs another level.
#define RMS_THRESHOLD 150 // speech frame RMS (DualSense mic: ~20-50 in quiet, hundreds+ in speech)
#define HANG_FRAMES 40    // 800 ms after the last speech frame: no drop-outs between words
#define MIC_LAG_FRAMES 4  // more mic than this waiting (80 ms): drop the oldest, keep latency low
#define MIC_GAP_MS 100    // no mic data this long while speaking: fill with silence
#define MAX_PEERS 64 // speakers we decode at once; matches the 64 users voice.c tracks
#define MIC_CAP (VAU_RATE / 2) // half a second of mic; older samples go
#define OUT_CAP 8              // playback frames queued for the app (160 ms)
#define OUT_FRAME_BYTES (4 * VAU_FRAME) // one stereo 20 ms frame as bytes
#define SPEAK_MS 300
#define KEEPALIVE_MS 5000
#define REQ_CAP 8
#define TICK_MS 20

// ---- pure pieces ---------------------------------------------------------------

OpusEncoder *vau_encoder_new(void)
{
    int err = OPUS_OK;
    OpusEncoder *e = opus_encoder_create(VAU_RATE, 1, OPUS_APPLICATION_VOIP, &err);
    if (!e || err != OPUS_OK) {
        if (e)
            opus_encoder_destroy(e);
        return NULL;
    }
    opus_encoder_ctl(e, OPUS_SET_BITRATE(64000));
    return e;
}

OpusDecoder *vau_decoder_new(void)
{
    int err = OPUS_OK;
    OpusDecoder *d = opus_decoder_create(VAU_RATE, 2, &err);
    if (!d || err != OPUS_OK) {
        if (d)
            opus_decoder_destroy(d);
        return NULL;
    }
    return d;
}

int vau_vad_frame(vau_vad *v, const int16_t *mono, int n)
{
    int64_t e = 0;
    for (int i = 0; i < n; i++)
        e += (int64_t)mono[i] * mono[i];
    if (n > 0 && e / n > (int64_t)RMS_THRESHOLD * RMS_THRESHOLD)
        v->hang = HANG_FRAMES;
    else if (v->hang > 0)
        v->hang--;
    return v->hang > 0;
}

void vau_rtp_header(uint8_t out[12], uint16_t seq, uint32_t ts, uint32_t ssrc)
{
    out[0] = 0x80;
    out[1] = VAU_RTP_PT;
    out[2] = seq >> 8;
    out[3] = (uint8_t)seq;
    out[4] = ts >> 24;
    out[5] = ts >> 16;
    out[6] = ts >> 8;
    out[7] = (uint8_t)ts;
    out[8] = ssrc >> 24;
    out[9] = ssrc >> 16;
    out[10] = ssrc >> 8;
    out[11] = (uint8_t)ssrc;
}

int vau_rtp_parse(const uint8_t *p, size_t n, vau_rtp *r)
{
    if (n < 12 || (p[0] >> 6) != 2 || (p[1] & 0x7f) != VAU_RTP_PT)
        return -1;
    size_t off = 12 + 4 * (size_t)(p[0] & 0x0f);
    if (p[0] & 0x10) { // extension: profile, length in 32-bit words, then the words
        if (n < off + 4)
            return -1;
        off += 4 + 4 * (size_t)(p[off + 2] << 8 | p[off + 3]);
    }
    size_t end = n;
    if (p[0] & 0x20) { // padding: the last octet counts the padding, itself included
        size_t pad = p[n - 1];
        if (pad == 0 || pad > n)
            return -1;
        end = n - pad;
    }
    if (off > end)
        return -1;
    r->seq = (uint16_t)(p[2] << 8 | p[3]);
    r->ts = (uint32_t)p[4] << 24 | (uint32_t)p[5] << 16 | (uint32_t)p[6] << 8 | p[7];
    r->ssrc = (uint32_t)p[8] << 24 | (uint32_t)p[9] << 16 | (uint32_t)p[10] << 8 | p[11];
    r->off = off;
    r->len = end - off;
    return 0;
}

// Discord's silence frame: sent and received as it is, never DAVE-encrypted (discord.js).
static int is_silence(const uint8_t *p, size_t n)
{
    return n == 3 && p[0] == 0xf8 && p[1] == 0xff && p[2] == 0xfe;
}

size_t vau_build_packet(DAVEEncryptorHandle enc, const uint8_t key[32], uint32_t ssrc, uint16_t seq,
                        uint32_t ts, uint32_t counter, const uint8_t *opus, size_t n, uint8_t *out, size_t cap)
{
    uint8_t body[VAU_MAX_PKT], plain[VAU_MAX_PKT];
    const uint8_t *payload = opus;
    size_t m = n;
    if (enc && !is_silence(opus, n)) {
        size_t room = daveEncryptorGetMaxCiphertextByteSize(enc, DAVE_MEDIA_TYPE_AUDIO, n);
        if (room > sizeof body || daveEncryptorEncrypt(enc, DAVE_MEDIA_TYPE_AUDIO, ssrc, opus, n, body, room,
                                                       &m) != DAVE_ENCRYPTOR_RESULT_CODE_SUCCESS)
            return 0;
        payload = body;
    }
    if (12 + m > sizeof plain)
        return 0;
    vau_rtp_header(plain, seq, ts, ssrc);
    memcpy(plain + 12, payload, m);
    return voice_gcm_seal(key, counter, plain, 12 + m, out, cap);
}

size_t vau_open_packet(DAVEDecryptorHandle dec, const uint8_t key[32], const uint8_t *pkt, size_t n,
                       uint8_t *out, size_t cap, uint32_t *ssrc)
{
    uint8_t plain[VAU_MAX_PKT];
    if (n < 2 || (pkt[1] & 0x7f) != VAU_RTP_PT || n > VAU_MAX_PKT)
        return 0;
    size_t pn = voice_gcm_open(key, pkt, n, plain, sizeof plain);
    vau_rtp r;
    if (!pn || vau_rtp_parse(plain, pn, &r) != 0 || r.len == 0 || r.len > cap)
        return 0;
    if (ssrc)
        *ssrc = r.ssrc;
    const uint8_t *payload = plain + r.off;
    if (!dec || is_silence(payload, r.len)) {
        memcpy(out, payload, r.len);
        return r.len;
    }
    size_t got = 0;
    if (daveDecryptorDecrypt(dec, DAVE_MEDIA_TYPE_AUDIO, payload, r.len, out, cap, &got) !=
        DAVE_DECRYPTOR_RESULT_CODE_SUCCESS)
        return 0;
    return got;
}

void vau_fifo_push(vau_fifo *f, const int16_t *stereo)
{
    if (f->count == VAU_FIFO) { // full: the oldest frame goes
        f->head = (f->head + 1) % VAU_FIFO;
        f->count--;
    }
    memcpy(f->pcm[(f->head + f->count) % VAU_FIFO], stereo, sizeof f->pcm[0]);
    f->count++;
}

int vau_fifo_pop(vau_fifo *f, int16_t *stereo)
{
    if (!f->primed) {
        if (f->count < 2)
            return 0;
        f->primed = 1;
    }
    if (f->count == 0) { // ran dry: wait for two frames again
        f->primed = 0;
        return 0;
    }
    memcpy(stereo, f->pcm[f->head], sizeof f->pcm[0]);
    f->head = (f->head + 1) % VAU_FIFO;
    f->count--;
    return 1;
}

void vau_mix_add(int32_t *acc, const int16_t *stereo)
{
    for (int i = 0; i < 2 * VAU_FRAME; i++)
        acc[i] += stereo[i];
}

void vau_mix_out(const int32_t *acc, int16_t *stereo)
{
    for (int i = 0; i < 2 * VAU_FRAME; i++)
        stereo[i] = acc[i] > INT16_MAX ? INT16_MAX : acc[i] < INT16_MIN ? INT16_MIN : (int16_t)acc[i];
}

int vau_loopback(uint32_t addr_be)
{
    return (ntohl(addr_be) >> 24) == 127;
}

// ---- engine ---------------------------------------------------------------------

typedef struct {
    int used, keyed;
    char user[VAU_ID];
    uint32_t ssrc;
    int64_t heard_ms;
    OpusDecoder *dec;
    DAVEDecryptorHandle dave;
    vau_fifo fifo;
} peer;

typedef struct {
    int speaking;
    uint32_t ssrc;
} speak_req;

// Everything below is guarded by mu. The voice thread and the engine both touch libdave.
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static OpusEncoder *enc_opus;
static int listen_fd = -1, client_fd = -1;

// the connection (set by vau_attach / vau_dave_state)
static int attached, udp = -1;
static uint8_t key[32];
static uint32_t my_ssrc;
static char self_id[VAU_ID];
static DAVESessionHandle session;
static int dave_version, dave_ready, dave_dirty;
static DAVEEncryptorHandle encryptor;
static int mute = 1, deaf = 0;
static peer peers[MAX_PEERS];

// our stream: counters never reset, so a key is never reused with a nonce
static uint16_t seq;
static uint32_t ts, counter, ka_counter;
static int64_t last_tx_ms, last_ka_ms, last_self_ms;
static vau_vad vad;
static int vad_on, audio_ok; // audio_ok: the voice thread sent our SPEAKING start
static speak_req reqs[REQ_CAP];
static int req_head, req_count;

// the mic (bytes from the app, 16-bit little endian mono) and playback (stereo, same format)
static int16_t mic[MIC_CAP];
static int64_t mic_last_ms; // when mic data last arrived from the app
// Counters for the 5-second stats line while in a call (where audio stops, at a glance).
static unsigned st_rx, st_nopeer, st_nokey, st_open_fail, st_decoded, st_mixed, st_out_bytes, st_mic_bytes,
    st_sent, st_seal_fail;
static int64_t st_at;

static int mic_head, mic_count, mic_carry = -1;
static uint8_t out_buf[OUT_CAP * OUT_FRAME_BYTES];
static size_t out_len;

// The external device (vau_ext_*). Its thread must never wait on the engine, which
// holds mu through Opus and DAVE, so the two meet only in these rings under ext_mu.
static pthread_mutex_t ext_mu = PTHREAD_MUTEX_INITIALIZER;
static volatile int ext_on;
static int16_t ext_mic[MIC_CAP];
static int ext_mic_head, ext_mic_count;
static ds5_pcm_stream ext_out; // playback with a 60 ms prefill (tools/ds5-usb-bridge)

static int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void req_push(int speaking)
{
    if (req_count == REQ_CAP) // the voice thread is stuck: keep the newest
        return;
    reqs[(req_head + req_count) % REQ_CAP] = (speak_req){speaking, my_ssrc};
    req_count++;
}

static void peer_free(peer *p)
{
    if (p->dave)
        daveDecryptorDestroy(p->dave);
    if (p->dec)
        opus_decoder_destroy(p->dec);
    memset(p, 0, sizeof *p);
}

static void peers_clear(void)
{
    for (int i = 0; i < MAX_PEERS; i++)
        peer_free(&peers[i]);
}

// Lock held. Gives the decryptor the sender's current ratchet (a copy: ours to destroy).
static void peer_key(peer *p)
{
    if (!p->dave)
        p->dave = daveDecryptorCreate();
    DAVEKeyRatchetHandle r = session && dave_version > 0 ? daveSessionGetKeyRatchet(session, p->user) : NULL;
    if (p->dave && r) {
        daveDecryptorTransitionToKeyRatchet(p->dave, r);
        p->keyed = 1;
    }
    if (r)
        daveKeyRatchetDestroy(r);
}

// Lock held. The epoch moved: our encryptor and every decryptor take the new ratchets.
static void refresh_keys(void)
{
    dave_dirty = 0;
    if (encryptor) {
        DAVEKeyRatchetHandle r = session && dave_version > 0 && dave_ready
                                     ? daveSessionGetKeyRatchet(session, self_id)
                                     : NULL;
        if (r) {
            daveEncryptorSetKeyRatchet(encryptor, r);
            daveKeyRatchetDestroy(r);
        }
    }
    for (int i = 0; i < MAX_PEERS; i++)
        if (peers[i].used && peers[i].dave) {
            peers[i].keyed = 0;
            peer_key(&peers[i]);
        }
}

static peer *peer_by_ssrc(uint32_t ssrc)
{
    for (int i = 0; i < MAX_PEERS; i++)
        if (peers[i].used && peers[i].ssrc == ssrc)
            return &peers[i];
    return NULL;
}

static void mic_push(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (mic_carry < 0) {
            mic_carry = b[i];
            continue;
        }
        mic_last_ms = now_ms();
        mic[(mic_head + mic_count) % MIC_CAP] = (int16_t)(uint16_t)(mic_carry | b[i] << 8);
        mic_carry = -1;
        if (mic_count < MIC_CAP)
            mic_count++;
        else
            mic_head = (mic_head + 1) % MIC_CAP;
    }
}

static void client_drop(void)
{
    if (client_fd >= 0)
        close(client_fd);
    client_fd = -1;
    mic_head = mic_count = 0;
    mic_carry = -1;
    out_len = 0;
}

static void accept_client(void)
{
    if (listen_fd < 0)
        return;
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    int fd = accept(listen_fd, (struct sockaddr *)&a, &l);
    if (fd < 0)
        return;
    if (!vau_loopback(a.sin_addr.s_addr)) {
        close(fd);
        return;
    }
    client_drop(); // one app at a time: the newest replaces the old
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    client_fd = fd;
    LOG("voice audio: app connected");
}

static void read_mic(void)
{
    uint8_t buf[4096];
    for (int i = 0; i < 8 && client_fd >= 0; i++) {
        ssize_t r = recv(client_fd, buf, sizeof buf, MSG_DONTWAIT);
        if (r > 0 && ext_on) // the device is the mic; keep draining the app's socket
            continue;
        if (r > 0) {
            mic_push(buf, (size_t)r);
            st_mic_bytes += (unsigned)r;
        }
        else if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            LOG("voice audio: app disconnected");
            client_drop();
        } else
            break;
    }
}

static void out_flush(void)
{
    while (client_fd >= 0 && out_len) {
        ssize_t w = send(client_fd, out_buf, out_len, MSG_DONTWAIT);
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (w <= 0) {
            LOG("voice audio: playback send to the app failed (%d)", errno);
            client_drop();
            return;
        }
        st_out_bytes += (unsigned)w;
        memmove(out_buf, out_buf + w, out_len - (size_t)w);
        out_len -= (size_t)w;
    }
}

static void out_push(const int16_t *stereo)
{
    if (out_len + OUT_FRAME_BYTES > sizeof out_buf) { // behind: the oldest frame goes
        memmove(out_buf, out_buf + OUT_FRAME_BYTES, out_len - OUT_FRAME_BYTES);
        out_len -= OUT_FRAME_BYTES;
    }
    memcpy(out_buf + out_len, stereo, OUT_FRAME_BYTES);
    out_len += OUT_FRAME_BYTES;
}

// Sends one Opus frame as a UDP packet. Nothing goes out while DAVE is up but not ready.
static void send_opus(const uint8_t *opus, size_t n)
{
    if (udp < 0 || (dave_version > 0 && (!encryptor || !dave_ready)))
        return;
    // The RTP clock runs while we are silent too. Without this a pause looks like late
    // packets to the listener, whose jitter buffer grows by the pause: seconds of delay.
    int64_t now = now_ms();
    if (last_tx_ms && now - last_tx_ms > 100)
        ts += (uint32_t)((now - last_tx_ms) / 20 - 1) * VAU_FRAME;
    uint8_t pkt[VAU_MAX_PKT];
    size_t len = vau_build_packet(dave_version > 0 ? encryptor : NULL, key, my_ssrc, seq, ts, counter, opus, n,
                                  pkt, sizeof pkt);
    if (!len) {
        st_seal_fail++;
        return;
    }
    counter++;
    seq++;
    ts += VAU_FRAME;
    st_sent++;
    if (send(udp, pkt, len, 0) < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        LOG("voice audio: UDP send failed (%d)", errno);
    last_tx_ms = last_self_ms = now_ms();
}

// Speech over: five silence frames (sent as they are), then SPEAKING off.
static void speak_stop(void)
{
    static const uint8_t silence[3] = {0xf8, 0xff, 0xfe};
    vad_on = 0;
    if (audio_ok)
        for (int i = 0; i < 5; i++)
            send_opus(silence, sizeof silence);
    req_push(0);
}

// One 20 ms mic frame. The VAD decides; SPEAKING goes out before the first audio.
// The input volume (prefs mic_gain) comes first, so the VAD hears what the call will.
static void mic_frame(const int16_t *in, uint8_t *scratch)
{
    int16_t pcm[VAU_FRAME];
    int gain = prefs_get("mic_gain"); // 5 = as captured
    for (int i = 0; i < VAU_FRAME; i++) {
        int v = in[i] * gain / 5;
        pcm[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    int sp = vau_vad_frame(&vad, pcm, VAU_FRAME);
    if (sp && !vad_on) {
        vad_on = 1;
        req_push(1);
        return;
    }
    if (!vad_on)
        return;
    if (!sp) {
        speak_stop();
        return;
    }
    if (!audio_ok)
        return;
    int n = opus_encode(enc_opus, pcm, VAU_FRAME, scratch, VAU_MAX_PKT);
    if (n > 0)
        send_opus(scratch, (size_t)n);
}

static void tx(void)
{
    if (mute || deaf) { // muted, or deafened (which implies muted, as in Discord): drop the mic
        if (vad_on)
            speak_stop();
        mic_count = 0;
        return;
    }
    uint8_t scratch[VAU_MAX_PKT];
    int16_t f[VAU_FRAME];
    if (mic_count > MIC_LAG_FRAMES * VAU_FRAME) { // fell behind: the oldest audio goes, not the delay
        int drop = mic_count - 2 * VAU_FRAME;
        mic_head = (mic_head + drop) % MIC_CAP;
        mic_count -= drop;
    }
    for (int k = 0; k < 2 && mic_count >= VAU_FRAME; k++) {
        for (int i = 0; i < VAU_FRAME; i++)
            f[i] = mic[(mic_head + i) % MIC_CAP];
        mic_head = (mic_head + VAU_FRAME) % MIC_CAP;
        mic_count -= VAU_FRAME;
        mic_frame(f, scratch);
    }
    // The app sends 5 ms grains, so a tick often finds a little under a frame: wait for it.
    // Only a real gap (the app stopped sending) is filled with silence, so the hang time runs out.
    if (mic_count < VAU_FRAME && vad_on && now_ms() - mic_last_ms > MIC_GAP_MS) {
        memset(f, 0, sizeof f);
        mic_frame(f, scratch);
    }
}

// Every UDP datagram waiting: transport open, decrypt, decode, queue for the mixer.
static void stats(int64_t now)
{
    if (now - st_at < 5000)
        return;
    if (st_at)
        LOG("voice audio: 5 s rx %u (no peer %u, no key %u, open failed %u) decoded %u mixed %u to app %u B, "
            "mic %u B, sent %u (seal failed %u)", st_rx, st_nopeer, st_nokey, st_open_fail, st_decoded, st_mixed,
            st_out_bytes, st_mic_bytes, st_sent, st_seal_fail);
    st_rx = st_nopeer = st_nokey = st_open_fail = st_decoded = st_mixed = st_out_bytes = st_mic_bytes = st_sent = 0;
    st_seal_fail = 0;
    st_at = now;
}

// Audio from an ssrc nobody announced (op 5 only comes when someone starts speaking, so
// whoever was already talking when we joined stays unknown). Each sender's DAVE key opens
// only their own frames, so try the users still without an ssrc; without DAVE, the only
// such user is it. Fills opus/len on a match. Only RTP audio (payload type 120) is tried.
static peer *identify(uint32_t ssrc, const uint8_t *pkt, size_t n, uint8_t *opus, size_t cap, size_t *len)
{
    if (n < 12 || (pkt[1] & 0x7f) != 0x78)
        return NULL;
    peer *only = NULL;
    int unmapped = 0;
    for (int i = 0; i < MAX_PEERS; i++) {
        peer *p = &peers[i];
        if (!p->used || p->ssrc)
            continue;
        unmapped++;
        only = p;
        if (dave_version <= 0)
            continue;
        if (!p->keyed)
            peer_key(p);
        if (p->keyed && (*len = vau_open_packet(p->dave, key, pkt, n, opus, cap, NULL)) > 0) {
            p->ssrc = ssrc;
            LOG("voice audio: matched an unannounced sender by their DAVE key");
            return p;
        }
    }
    if (dave_version <= 0 && unmapped == 1 && (*len = vau_open_packet(NULL, key, pkt, n, opus, cap, NULL)) > 0) {
        only->ssrc = ssrc;
        return only;
    }
    static uint32_t logged;
    if (ssrc != logged) { // once per sender: why their audio is still unmatched
        int keyed = 0;
        for (int i = 0; i < MAX_PEERS; i++)
            keyed += peers[i].used && !peers[i].ssrc && peers[i].keyed;
        LOG("voice audio: ssrc %u unmatched: %d users without ssrc, %d keyed, DAVE v%d, %zu bytes", ssrc,
            unmapped, keyed, dave_version, n);
        logged = ssrc;
    }
    return NULL;
}

static void rx(void)
{
    uint8_t pkt[VAU_MAX_PKT], opus[VAU_MAX_PKT];
    int16_t pcm[2 * VAU_FRAME];
    for (int i = 0; i < 64 && udp >= 0; i++) {
        ssize_t n = recv(udp, pkt, sizeof pkt, 0);
        if (n < 0)
            break;
        if (n < 12 || deaf || !attached)
            continue;
        st_rx++;
        uint32_t ssrc = (uint32_t)pkt[8] << 24 | (uint32_t)pkt[9] << 16 | (uint32_t)pkt[10] << 8 | pkt[11];
        peer *p = peer_by_ssrc(ssrc);
        size_t m = 0;
        if (!p && !(p = identify(ssrc, pkt, (size_t)n, opus, sizeof opus, &m))) {
            st_nopeer++;
            continue;
        }
        if (!m) {
            if (dave_version > 0) {
                if (!p->keyed)
                    peer_key(p);
                if (!p->keyed) {
                    st_nokey++;
                    continue;
                }
            }
            m = vau_open_packet(dave_version > 0 ? p->dave : NULL, key, pkt, (size_t)n, opus, sizeof opus, NULL);
            if (!m) {
                st_open_fail++;
                continue;
            }
        }
        if (!p->dec && !(p->dec = vau_decoder_new()))
            continue;
        if (opus_decode(p->dec, opus, (opus_int32)m, pcm, VAU_FRAME, 0) != VAU_FRAME)
            continue;
        vau_fifo_push(&p->fifo, pcm);
        st_decoded++;
        p->heard_ms = now_ms();
    }
}

// Mix every speaker's next frame, play it, and flush what the socket takes.
static void play(void)
{
    int32_t acc[2 * VAU_FRAME] = {0};
    int16_t f[2 * VAU_FRAME], out[2 * VAU_FRAME];
    int any = 0;
    for (int i = 0; i < MAX_PEERS; i++)
        if (peers[i].used && vau_fifo_pop(&peers[i].fifo, f)) {
            vau_mix_add(acc, f);
            any = 1;
        }
    if (any && ext_on) {
        vau_mix_out(acc, out);
        pthread_mutex_lock(&ext_mu);
        ds5_pcm_receive(&ext_out, (const uint8_t *)out, sizeof out); // little endian
        pthread_mutex_unlock(&ext_mu);
        st_out_bytes += sizeof out;
        st_mixed++;
    } else if (any && client_fd >= 0) {
        vau_mix_out(acc, out);
        out_push(out);
        st_mixed++;
    }
    out_flush();
}

static void keepalive(int64_t now)
{
    if (udp < 0 || now - last_ka_ms < KEEPALIVE_MS)
        return;
    uint8_t k[8] = {0};
    k[0] = (uint8_t)ka_counter;
    k[1] = (uint8_t)(ka_counter >> 8);
    k[2] = (uint8_t)(ka_counter >> 16);
    k[3] = (uint8_t)(ka_counter >> 24);
    ka_counter++;
    send(udp, k, sizeof k, 0);
    last_ka_ms = now;
}

// Moves the device's mic samples into the mic ring (bytes, as the app sends them).
static void read_ext_mic(void)
{
    uint8_t b[2 * 1024];
    for (;;) {
        pthread_mutex_lock(&ext_mu);
        int n = ext_mic_count < 1024 ? ext_mic_count : 1024;
        for (int i = 0; i < n; i++) {
            uint16_t v = (uint16_t)ext_mic[(ext_mic_head + i) % MIC_CAP];
            b[2 * i] = (uint8_t)v;
            b[2 * i + 1] = (uint8_t)(v >> 8);
        }
        ext_mic_head = (ext_mic_head + n) % MIC_CAP;
        ext_mic_count -= n;
        pthread_mutex_unlock(&ext_mu);
        if (!n)
            return;
        mic_push(b, 2 * (size_t)n);
        st_mic_bytes += 2u * (unsigned)n;
    }
}

static void tick(void)
{
    accept_client();
    read_mic();
    read_ext_mic();
    if (!attached) { // no call: the mic is not wanted
        mic_count = 0;
        return;
    }
    if (dave_dirty)
        refresh_keys();
    rx();
    if (!deaf)
        play();
    tx();
    keepalive(now_ms());
    stats(now_ms());
}

static void *engine_main(void *arg)
{
    (void)arg;
    int64_t next = now_ms();
    for (;;) {
        next += TICK_MS;
        int64_t left = next - now_ms();
        if (left > 0)
            usleep((useconds_t)left * 1000);
        else if (left < -100) // far behind (suspended): start again from now
            next = now_ms();
        pthread_mutex_lock(&mu);
        tick();
        pthread_mutex_unlock(&mu);
    }
    return NULL;
}

static int listen_loopback(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(VAU_PORT)};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    return fd;
}

void vau_init(void)
{
    enc_opus = vau_encoder_new();
    if (!enc_opus)
        LOG("voice audio: Opus encoder failed, no audio");
    listen_fd = listen_loopback();
    if (listen_fd < 0)
        LOG("voice audio: cannot listen on 127.0.0.1:%d (errno %d)", VAU_PORT, errno);
    else
        LOG("voice audio: listening on 127.0.0.1:%d", VAU_PORT);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 1024 * 1024);
    pthread_t th;
    if (pthread_create(&th, &attr, engine_main, NULL) != 0)
        LOG("voice audio: engine thread failed, no audio");
    pthread_attr_destroy(&attr);
}

void vau_lock(void)
{
    pthread_mutex_lock(&mu);
}

void vau_unlock(void)
{
    pthread_mutex_unlock(&mu);
}

void vau_attach(int fd, const uint8_t k[32], uint32_t ssrc, const char *self)
{
    pthread_mutex_lock(&mu);
    peers_clear();
    if (encryptor)
        daveEncryptorDestroy(encryptor);
    encryptor = daveEncryptorCreate();
    if (encryptor) {
        daveEncryptorAssignSsrcToCodec(encryptor, ssrc, DAVE_CODEC_OPUS);
        daveEncryptorSetPassthroughMode(encryptor, false);
    }
    udp = fd;
    if (fd >= 0)
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    memcpy(key, k, sizeof key);
    my_ssrc = ssrc;
    snprintf(self_id, sizeof self_id, "%s", self ? self : "");
    attached = 1;
    dave_dirty = 1;
    vad_on = audio_ok = 0;
    req_head = req_count = 0;
    last_ka_ms = now_ms();
    pthread_mutex_unlock(&mu);
}

void vau_detach(void)
{
    pthread_mutex_lock(&mu);
    attached = 0;
    udp = -1;
    session = NULL;
    dave_version = dave_ready = 0;
    peers_clear();
    if (encryptor)
        daveEncryptorDestroy(encryptor);
    encryptor = NULL;
    vad_on = audio_ok = 0;
    req_head = req_count = 0;
    pthread_mutex_unlock(&mu);
}

void vau_dave_state(DAVESessionHandle s, int version, int ready)
{
    pthread_mutex_lock(&mu);
    session = s;
    dave_version = version;
    dave_ready = ready;
    dave_dirty = 1;
    pthread_mutex_unlock(&mu);
}

void vau_set_flags(int m, int d)
{
    pthread_mutex_lock(&mu);
    mute = m;
    deaf = d;
    pthread_mutex_unlock(&mu);
}

void vau_map_ssrc(uint32_t ssrc, const char *user_id)
{
    pthread_mutex_lock(&mu);
    if (ssrc && user_id && strcmp(user_id, self_id) != 0) {
        peer *p = NULL;
        for (int i = 0; i < MAX_PEERS && !p; i++)
            if (peers[i].used && strcmp(peers[i].user, user_id) == 0)
                p = &peers[i];
        for (int i = 0; i < MAX_PEERS && !p; i++)
            if (!peers[i].used)
                p = &peers[i];
        if (p) {
            if (!p->used)
                memset(&p->fifo, 0, sizeof p->fifo);
            p->used = 1;
            p->ssrc = ssrc;
            snprintf(p->user, sizeof p->user, "%s", user_id);
        }
    }
    pthread_mutex_unlock(&mu);
}

void vau_expect_user(const char *user_id)
{
    pthread_mutex_lock(&mu);
    int known = !user_id || strcmp(user_id, self_id) == 0;
    for (int i = 0; i < MAX_PEERS && !known; i++)
        known = peers[i].used && strcmp(peers[i].user, user_id) == 0;
    for (int i = 0; i < MAX_PEERS && !known; i++)
        if (!peers[i].used) {
            memset(&peers[i].fifo, 0, sizeof peers[i].fifo);
            peers[i].used = 1;
            peers[i].ssrc = 0; // learned from op 5, or from their audio (identify)
            snprintf(peers[i].user, sizeof peers[i].user, "%s", user_id);
            known = 1;
        }
    pthread_mutex_unlock(&mu);
}

void vau_forget_user(const char *user_id)
{
    pthread_mutex_lock(&mu);
    for (int i = 0; user_id && i < MAX_PEERS; i++)
        if (peers[i].used && strcmp(peers[i].user, user_id) == 0)
            peer_free(&peers[i]);
    pthread_mutex_unlock(&mu);
}

int vau_take_speaking(int *speaking, uint32_t *ssrc)
{
    pthread_mutex_lock(&mu);
    int got = req_count > 0;
    if (got) {
        speak_req r = reqs[req_head];
        req_head = (req_head + 1) % REQ_CAP;
        req_count--;
        audio_ok = r.speaking; // audio follows the SPEAKING op that went out
        *speaking = r.speaking;
        *ssrc = r.ssrc;
    }
    pthread_mutex_unlock(&mu);
    return got;
}

int vau_speaking_pending(void)
{
    pthread_mutex_lock(&mu);
    int n = req_count;
    pthread_mutex_unlock(&mu);
    return n > 0;
}

int vau_speaking(char ids[][VAU_ID], int max)
{
    int n = 0;
    int64_t t = now_ms();
    pthread_mutex_lock(&mu);
    if (attached && max > 0 && t - last_self_ms <= SPEAK_MS) {
        snprintf(ids[n++], VAU_ID, "%s", self_id);
    }
    for (int i = 0; i < MAX_PEERS && n < max; i++)
        if (peers[i].used && peers[i].heard_ms && t - peers[i].heard_ms <= SPEAK_MS)
            snprintf(ids[n++], VAU_ID, "%s", peers[i].user);
    pthread_mutex_unlock(&mu);
    return n;
}

void vau_ext_set(int on)
{
    pthread_mutex_lock(&ext_mu);
    ext_mic_head = ext_mic_count = 0;
    memset(&ext_out, 0, sizeof ext_out);
    ext_on = on;
    pthread_mutex_unlock(&ext_mu);
}

void vau_ext_mic(const int16_t *mono, size_t frames)
{
    pthread_mutex_lock(&ext_mu);
    for (size_t i = 0; i < frames; i++) {
        ext_mic[(ext_mic_head + ext_mic_count) % MIC_CAP] = mono[i];
        if (ext_mic_count < MIC_CAP)
            ext_mic_count++;
        else
            ext_mic_head = (ext_mic_head + 1) % MIC_CAP; // behind: the oldest goes
    }
    pthread_mutex_unlock(&ext_mu);
}

size_t vau_ext_playback(int16_t *stereo, size_t frames)
{
    pthread_mutex_lock(&ext_mu);
    size_t n = ds5_pcm_playback(&ext_out, stereo, frames);
    pthread_mutex_unlock(&ext_mu);
    return n;
}

int vau_app_connected(void)
{
    return client_fd >= 0;
}
