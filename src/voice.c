#include "voice.h"
#include "json_num.h"

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
#include "log.h"
#include "tls.h"
#include "voice_audio.h"
#include "voice_proto.h"
#include "ws.h"

// Voice gateway version 8: the recommended one (docs/voice-connections).
#define VOICE_QUERY "/?v=8"
#define JOIN_TIMEOUT_MS 20000
#define MAX_RESUMES 5
// The voice server could not be reached at all (no network, no DNS): not the server's fault,
// so it does not count against MAX_RESUMES; the call waits up to UNREACHABLE_MS for it.
#define VOICE_UNREACHABLE -4
#define UNREACHABLE_MS (10 * 60 * 1000)
#define MAX_USERS 99 // a voice channel's limit
#define IDS 32

enum vstate { V_IDLE, V_JOINING, V_CONNECTING, V_SECURING, V_CONNECTED, V_ERROR };
static const char *state_names[] = {"idle", "joining", "connecting", "securing", "connected", "error"};

// One voice connection, owned by its thread. Detached: the thread frees it on exit.
typedef struct {
    // copied from the gateway when the thread starts
    char endpoint[256], token[128], session_id[64], server_id[IDS], channel_id[IDS], user_id[IDS];
    _Atomic int stop;   // set (under lock) when a newer state replaces this connection
    ws_conn ws;
    int udp;            // -1 until READY
    uint32_t ssrc;
    char mode[40];
    uint8_t key[32];
    int64_t seq_ack;    // last sequence number seen, -1 none
    int ready, healthy; // READY seen (resume is possible); a session description arrived
    int64_t hb_ms, next_hb, hb_sent;
    int hb_acked;
    int dave_version, pending_version, dave_ready;
    DAVESessionHandle dave;
    uint8_t ext_sender[512]; // op 25, sent once per call: a reset session needs it again
    size_t ext_sender_len;
    char users[MAX_USERS][IDS]; // who is in the call, for DAVE
    int readds;                 // op 31 re-add requests in a row
    int n_users;
    char err[256];
} vthread;

// Everything below is guarded by lock. Other threads only read or queue requests.
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char self_id[IDS];
static char gw_session[64]; // this gateway session's id (READY): voice states of ours carry it
static enum vstate state = V_IDLE;
static char err_text[256];
static char want_guild[IDS], want_channel[IDS]; // the call asked for (or current)
static char guild[IDS], channel[IDS];         // the call Discord confirmed (VOICE_STATE_UPDATE)
static char session_id[64], token[128], endpoint[256];
static int mute = 1, deaf = 0;
static int ping_ms, secure;
static char refused[160];
static int64_t joined_ms, since_ms;
static int op4_due;
static char listed[32]; // the channel Discord last said we are in (our VOICE_STATE_UPDATE); "" none
static voice_op4 op4;
static vthread *cur; // the connection that may report; NULL when none
static vthread *att_owner; // the connection the audio engine serves (voice_audio.c)

static int64_t mono_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int64_t wall_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void copy(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src ? src : "");
}

static const char *str(cJSON *o, const char *key)
{
    return cJSON_GetStringValue(cJSON_GetObjectItem(o, key));
}

static int64_t num(cJSON *o, const char *key)
{
    return json_int(o, key, 0);
}

// Lock held. Replaces the current connection: its thread exits without reporting.
int voice_wanted(void)
{
    pthread_mutex_lock(&lock);
    int w = want_channel[0] != 0;
    pthread_mutex_unlock(&lock);
    return w;
}

int voice_current(char guild_id[32], char channel_id[32], char user_id[32], char session[64])
{
    pthread_mutex_lock(&lock);
    int ok = state == V_CONNECTED && channel[0] && session_id[0];
    if (ok) {
        copy(guild_id, IDS, guild);
        copy(channel_id, IDS, channel);
        copy(user_id, IDS, self_id);
        copy(session, 64, session_id);
    }
    pthread_mutex_unlock(&lock);
    return ok;
}

static void stop_locked(void)
{
    if (cur) {
        cur->stop = 1;
        cur = NULL;
    }
}

static void set_state_locked(enum vstate s)
{
    if (s == V_CONNECTED && state != V_CONNECTED)
        since_ms = wall_ms();
    if (s != V_CONNECTED)
        since_ms = 0;
    if (s == V_JOINING && state != V_JOINING)
        joined_ms = mono_ms(); // the join timeout counts from entering this state
    if (s == V_IDLE || s == V_ERROR)
        secure = ping_ms = 0; // the next call starts without the last one's
    state = s;
}

// Lock held: sends a leave (op 4 with null channel) for the gateway to make.
static void queue_leave_locked(void)
{
    op4 = (voice_op4){.mute = mute, .deaf = deaf};
    op4_due = 1;
}

// Lock held: starts the voice thread once the gateway has given us everything it needs.
static void start_locked(void);
static void maybe_start_locked(void)
{
    if (!cur && session_id[0] && token[0] && endpoint[0] && channel[0])
        start_locked();
}

void voice_set_self(const char *user_id)
{
    pthread_mutex_lock(&lock);
    if (strcmp(self_id, user_id ? user_id : "") != 0)
        listed[0] = 0; // another account: where Discord listed the last one is not ours
    copy(self_id, sizeof self_id, user_id);
    pthread_mutex_unlock(&lock);
}

void voice_join(const char *guild_id, const char *channel_id)
{
    pthread_mutex_lock(&lock);
    int here = (state == V_CONNECTING || state == V_SECURING || state == V_CONNECTED) &&
               strcmp(channel, channel_id) == 0;
    if (!here) { // already in it: nothing to send
        copy(want_guild, sizeof want_guild, guild_id);
        copy(want_channel, sizeof want_channel, channel_id);
        op4 = (voice_op4){.mute = mute, .deaf = deaf, .leave_first = !cur && listed[0]};
        copy(op4.guild, sizeof op4.guild, guild_id);
        copy(op4.channel, sizeof op4.channel, channel_id);
        op4_due = 1;
        err_text[0] = 0;
        joined_ms = mono_ms();
        set_state_locked(V_JOINING);
    }
    pthread_mutex_unlock(&lock);
}

static char session_file[256]; // our last gateway session id, to spot our own ghost

void voice_init(const char *data_dir)
{
    snprintf(session_file, sizeof session_file, "%s/voice_session", data_dir);
    vau_init();
}

// The guild where READY shows us in voice under session prev (our previous run), or NULL.
static const char *ghost_guild(cJSON *ready, const char *prev)
{
    cJSON *g, *vs;
    cJSON_ArrayForEach(g, cJSON_GetObjectItem(ready, "guilds"))
        cJSON_ArrayForEach(vs, cJSON_GetObjectItem(g, "voice_states")) {
            const char *user = str(vs, "user_id"), *sid = str(vs, "session_id");
            if (user && sid && strcmp(user, self_id) == 0 && strcmp(sid, prev) == 0)
                return str(g, "id");
        }
    return NULL;
}

// A fresh gateway session (READY): our old voice connection cannot be resumed. Rejoin
// the call we want, or leave a ghost our previous run left behind (rejoining that same
// channel would change nothing for Discord). A voice state owned by another session
// (the same account on a phone or PC) is left alone: Discord allows one call per account.
void voice_on_ready(cJSON *ready)
{
    char prev[64] = "";
    FILE *f = session_file[0] ? fopen(session_file, "r") : NULL;
    if (f) {
        if (!fgets(prev, sizeof prev, f))
            prev[0] = 0;
        fclose(f);
    }
    const char *sid = str(ready, "session_id");
    if (sid && session_file[0] && (f = fopen(session_file, "w"))) {
        fputs(sid, f);
        fclose(f);
    }
    pthread_mutex_lock(&lock);
    const char *ghost = prev[0] ? ghost_guild(ready, prev) : NULL;
    stop_locked();
    session_id[0] = token[0] = endpoint[0] = channel[0] = guild[0] = 0;
    copy(gw_session, sizeof gw_session, sid);
    if (want_channel[0] || ghost) {
        op4 = (voice_op4){.mute = mute, .deaf = deaf};
        copy(op4.guild, sizeof op4.guild, want_channel[0] ? want_guild : ghost);
        copy(op4.channel, sizeof op4.channel, want_channel);
        op4_due = 1;
    }
    if (want_channel[0]) {
        joined_ms = mono_ms();
        set_state_locked(V_JOINING);
    } else if (ghost)
        LOG("voice: leaving the call our previous run left behind");
    pthread_mutex_unlock(&lock);
}

void voice_refuse(const char *why)
{
    pthread_mutex_lock(&lock);
    copy(refused, sizeof refused, why);
    pthread_mutex_unlock(&lock);
}

void voice_leave(void)
{
    pthread_mutex_lock(&lock);
    stop_locked();
    queue_leave_locked();
    want_guild[0] = want_channel[0] = guild[0] = channel[0] = 0;
    session_id[0] = token[0] = endpoint[0] = 0;
    err_text[0] = 0;
    set_state_locked(V_IDLE);
    pthread_mutex_unlock(&lock);
}

int voice_set_flags(int m, int d)
{
    pthread_mutex_lock(&lock);
    int now_mute, now_deaf;
    int in_call = state != V_IDLE && state != V_ERROR;
    if (in_call) {
        if (m >= 0)
            mute = m;
        if (d >= 0)
            deaf = d;
        op4 = (voice_op4){.mute = mute, .deaf = deaf};
        copy(op4.guild, sizeof op4.guild, want_guild);
        copy(op4.channel, sizeof op4.channel, want_channel);
        op4_due = 1;
    }
    now_mute = mute;
    now_deaf = deaf;
    pthread_mutex_unlock(&lock);
    vau_set_flags(now_mute, now_deaf);
    return in_call ? 0 : -1;
}

int voice_request_pending(void)
{
    pthread_mutex_lock(&lock);
    int due = op4_due;
    pthread_mutex_unlock(&lock);
    return due;
}

int voice_take_request(voice_op4 *out)
{
    pthread_mutex_lock(&lock);
    int due = op4_due;
    if (due)
        *out = op4;
    if (due && state == V_JOINING)
        joined_ms = mono_ms(); // the join timeout counts from when Discord gets it, not from the queue
    op4_due = 0;
    pthread_mutex_unlock(&lock);
    return due;
}

void voice_on_state(cJSON *d)
{
    const char *uid = str(d, "user_id"), *cid = str(d, "channel_id");
    pthread_mutex_lock(&lock);
    if (!uid || strcmp(uid, self_id) != 0) {
        pthread_mutex_unlock(&lock);
        return;
    }
    const char *sid = str(d, "session_id");
    // Our account in voice from another client (a phone): Discord moved the call there, so
    // ours ended; it is not a call to join here.
    const int elsewhere = cid && sid && gw_session[0] && strcmp(sid, gw_session) != 0;
    if (elsewhere)
        LOG("voice: this account joined voice on another device");
    copy(listed, sizeof listed, elsewhere ? NULL : cid);
    if (!cid || elsewhere) { // we left (or were moved out by the server, or to another device)
        stop_locked();
        guild[0] = channel[0] = session_id[0] = token[0] = endpoint[0] = 0;
        if (state != V_JOINING && state != V_ERROR) { // a join we just sent stays pending
            want_guild[0] = want_channel[0] = 0;
            set_state_locked(V_IDLE);
        }
    } else {
        const char *gid = str(d, "guild_id");
        if (strcmp(channel, cid) != 0 || strcmp(session_id, sid ? sid : "") != 0) {
            stop_locked();
            if (session_id[0]) // server details of the old call; on a first join Discord may
                token[0] = endpoint[0] = 0; // send the server update first, and it is ours
            copy(session_id, sizeof session_id, sid);
            if (state != V_ERROR)
                set_state_locked(V_JOINING);
        }
        copy(channel, sizeof channel, cid);
        copy(guild, sizeof guild, gid);
        copy(want_channel, sizeof want_channel, cid);
        copy(want_guild, sizeof want_guild, gid);
        maybe_start_locked();
    }
    pthread_mutex_unlock(&lock);
}

void voice_on_server(cJSON *d)
{
    const char *tok = str(d, "token"), *ep = str(d, "endpoint");
    pthread_mutex_lock(&lock);
    if (!want_channel[0]) { // not in a call: a stale server update
        pthread_mutex_unlock(&lock);
        return;
    }
    if (!ep) { // the server is moving us: wait for the next update
        stop_locked();
        token[0] = endpoint[0] = 0;
        set_state_locked(V_CONNECTING);
    } else {
        char bare[256];
        copy(bare, sizeof bare, strncmp(ep, "wss://", 6) == 0 ? ep + 6 : ep);
        if (!cur || strcmp(token, tok ? tok : "") != 0 || strcmp(endpoint, bare) != 0) { // a re-send changes nothing
            stop_locked();
            copy(token, sizeof token, tok);
            copy(endpoint, sizeof endpoint, bare);
            maybe_start_locked();
        }
    }
    pthread_mutex_unlock(&lock);
}

// Lock held: a join Discord did not confirm in time ends (or, for a switch, keeps the call).
static void join_timeout_locked(void)
{
    if (op4_due) // not sent yet (the gateway is reconnecting): the wait starts when it goes
        return;
    if (state == V_JOINING && mono_ms() - joined_ms > JOIN_TIMEOUT_MS && cur && channel[0]) {
        // A switch Discord never confirmed: we are still in the old channel, so stay there.
        LOG("voice: the switch was not confirmed, staying in the call");
        copy(want_guild, sizeof want_guild, guild);
        copy(want_channel, sizeof want_channel, channel);
        set_state_locked(V_CONNECTED);
    } else if (state == V_JOINING && mono_ms() - joined_ms > JOIN_TIMEOUT_MS) {
        // Discord never confirmed the channel (not a voice room, or no permission).
        copy(err_text, sizeof err_text, "Discord did not confirm the voice channel.");
        stop_locked();
        guild[0] = channel[0] = 0;
        queue_leave_locked();
        set_state_locked(V_ERROR);
    }
}

void voice_tick(void)
{
    pthread_mutex_lock(&lock);
    join_timeout_locked();
    pthread_mutex_unlock(&lock);
}

char *voice_state_json(void)
{
    char ids[8][VAU_ID];
    int ns = vau_speaking(ids, 8); // before our lock: the engine never takes ours
    pthread_mutex_lock(&lock);
    join_timeout_locked();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", state_names[state]);
    cJSON_AddStringToObject(o, "channel", channel[0] ? channel : want_channel);
    cJSON_AddStringToObject(o, "guild", channel[0] ? guild : want_guild);
    cJSON_AddBoolToObject(o, "mute", mute);
    cJSON_AddBoolToObject(o, "deaf", deaf);
    cJSON_AddNumberToObject(o, "ping_ms", ping_ms);
    cJSON_AddBoolToObject(o, "secure", secure);
    cJSON_AddNumberToObject(o, "since_ms", (double)since_ms);
    cJSON_AddStringToObject(o, "error", err_text);
    cJSON_AddStringToObject(o, "refused", refused);
    cJSON *speaking = cJSON_AddArrayToObject(o, "speaking");
    for (int i = 0; i < ns; i++)
        cJSON_AddItemToArray(speaking, cJSON_CreateString(ids[i]));
    pthread_mutex_unlock(&lock);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

// ---- voice thread ----------------------------------------------------------

static int send_json(vthread *t, char *s)
{
    int r = s ? ws_send(&t->ws, WS_TEXT, s, strlen(s)) : -1;
    free(s);
    return r;
}

static int send_bin(vthread *t, int op, const uint8_t *p, size_t n)
{
    uint8_t *buf = malloc(n + 1);
    if (!buf)
        return -1;
    size_t k = voice_bin_build(buf, n + 1, op, p, n);
    int r = k ? ws_send(&t->ws, WS_BINARY, buf, k) : -1;
    free(buf);
    return r;
}

// Sets the error for the thread's end and returns the fatal code.
static int fatal(vthread *t, const char *msg)
{
    copy(t->err, sizeof t->err, msg);
    return -2;
}

// The audio engine reads this connection's session: give it the current state, or take it back.
static void audio_dave(vthread *t, int detach)
{
    pthread_mutex_lock(&lock);
    // The owner alone: a stopped thread (cur moved on) must still take its session back
    // before destroying it.
    if (att_owner == t && (detach || cur == t)) {
        if (detach) // keep the version: above 0 and not ready, the engine sends nothing in the clear
            vau_dave_state(NULL, t->dave_version, 0);
        else
            vau_dave_state(t->dave, t->dave_version, t->dave_ready);
    }
    pthread_mutex_unlock(&lock);
}

// The connection's socket ends: the engine must let go of its UDP socket and keys.
static void audio_release(vthread *t)
{
    pthread_mutex_lock(&lock);
    if (att_owner == t) {
        att_owner = NULL;
        vau_detach();
    }
    pthread_mutex_unlock(&lock);
}

static void update_state(vthread *t)
{
    pthread_mutex_lock(&lock);
    if (cur == t) {
        // Present once the session is up; alone in a channel Discord forms no MLS group
        // until someone else joins, so "secure" follows the group, not the state.
        secure = t->dave_version > 0 && t->dave_ready;
        set_state_locked(V_CONNECTED);
    }
    pthread_mutex_unlock(&lock);
    audio_dave(t, 0);
}

static int udp_open(vthread *t, const char *ip, int port)
{
    if (t->udp >= 0)
        close(t->udp);
    t->udp = -1;
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1)
        return -1;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    struct timeval tv = {.tv_sec = 1};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        close(fd);
        return -1;
    }
    t->udp = fd;
    return 0;
}

static int discover(vthread *t, char address[64], uint16_t *port)
{
    uint8_t req[74], rep[74];
    voice_ip_discovery_request(req, t->ssrc);
    for (int i = 0; i < 5; i++) {
        if (send(t->udp, req, sizeof req, 0) != (ssize_t)sizeof req)
            return -1;
        ssize_t n = recv(t->udp, rep, sizeof rep, 0);
        if (n == (ssize_t)sizeof rep && voice_ip_discovery_parse(rep, sizeof rep, address, port) == 0)
            return 0;
    }
    return -1;
}

// libdave's own reasons (a bad welcome, an unrecognized member, ...).
static void dave_failed(const char *source, const char *reason, void *user)
{
    (void)user;
    LOG("voice: DAVE %s: %s", source ? source : "?", reason ? reason : "?");
}

static void dave_start(vthread *t, int version)
{
    t->dave_version = t->pending_version = version; // a commit's transition keeps the version
    if (version <= 0)
        return;
    if (!t->dave)
        t->dave = daveSessionCreate(NULL, "", dave_failed, NULL);
    if (t->dave) {
        vau_lock();
        daveSessionInit(t->dave, (uint16_t)version, strtoull(t->channel_id, NULL, 10), t->user_id);
        // Discord sends the external sender once per call; after a reset (a new epoch when
        // the others left) the session has lost it and would refuse every proposal and welcome.
        if (t->ext_sender_len)
            daveSessionSetExternalSender(t->dave, t->ext_sender, t->ext_sender_len);
        vau_unlock();
    }
}

static void dave_reset(vthread *t, int version)
{
    audio_dave(t, 1); // the engine lets go of the old session before it is destroyed
    if (t->dave) {
        vau_lock();
        daveSessionDestroy(t->dave);
        vau_unlock();
    }
    t->dave = NULL;
    t->dave_ready = 0;
    t->readds = 0; // a new group gets its own re-adds
    dave_start(t, version);
    audio_dave(t, 0);
}

static void users_add(vthread *t, const char *id)
{
    for (int i = 0; i < t->n_users; i++)
        if (strcmp(t->users[i], id) == 0)
            return;
    if (t->n_users < MAX_USERS)
        copy(t->users[t->n_users++], IDS, id);
}

static void users_remove(vthread *t, const char *id)
{
    for (int i = 0; i < t->n_users; i++)
        if (strcmp(t->users[i], id) == 0) {
            memmove(t->users[i], t->users[i + 1], (size_t)(t->n_users - i - 1) * IDS);
            t->n_users--;
            return;
        }
}

// Opcode 1 (READY): pick the mode, open UDP, discover our address, then select it.
static int on_ready(vthread *t, cJSON *d)
{
    t->ready = 1;
    t->ssrc = (uint32_t)num(d, "ssrc");
    int chosen = 0;
    cJSON *m;
    cJSON_ArrayForEach(m, cJSON_GetObjectItem(d, "modes"))
        if (cJSON_IsString(m) && strcmp(m->valuestring, VOICE_MODE_GCM) == 0)
            chosen = 1;
    if (!chosen)
        return fatal(t, "This voice server offers no encryption mode this build supports.");
    copy(t->mode, sizeof t->mode, VOICE_MODE_GCM);
    const char *ip = str(d, "ip");
    char address[64];
    uint16_t port = 0;
    if (!ip || udp_open(t, ip, (int)num(d, "port")) != 0 || discover(t, address, &port) != 0)
        return -1; // transport trouble: the caller retries
    LOG("voice: discovered our address, selecting %s", VOICE_MODE_GCM);
    return send_json(t, voice_json_select_udp(address, port, t->mode)) == 0 ? 0 : -1;
}

static int send_key_package(vthread *t);

// The audio engine takes this connection's socket and key; again after a resume (the
// engine let go when the websocket dropped; the UDP socket and key live on). Then the state
// and the DAVE session follow.
static void attach_audio(vthread *t)
{
    t->healthy = 1;
    pthread_mutex_lock(&lock);
    if (cur == t) {
        att_owner = t;
        vau_attach(t->udp, t->key, t->ssrc, t->user_id, t->dave_version);
        // op 11 (who is already here) arrives before this and attach starts a clean peer
        // table: list them again so their audio can be matched to them.
        for (int i = 0; i < t->n_users; i++)
            vau_expect_user(t->users[i]);
    }
    pthread_mutex_unlock(&lock);
    update_state(t);
}

// Opcode 4 (SESSION_DESCRIPTION): the transport key and the DAVE version to use.
static int on_session(vthread *t, cJSON *d)
{
    const char *mode = str(d, "mode");
    if (!mode || strcmp(mode, t->mode) != 0)
        return fatal(t, "The voice server chose an encryption mode this build does not use.");
    cJSON *k = cJSON_GetObjectItem(d, "secret_key");
    if (cJSON_GetArraySize(k) != 32)
        return fatal(t, "The voice server sent a bad encryption key.");
    for (int i = 0; i < 32; i++)
        t->key[i] = (uint8_t)json_item_int(cJSON_GetArrayItem(k, i), 0);
    dave_start(t, (int)num(d, "dave_protocol_version"));
    attach_audio(t);
    LOG("voice: session up, DAVE version %d", t->dave_version);
    return send_key_package(t) == 0 ? 0 : -1; // Discord's clients send it at once, not only on op 24
}

// Opcode 26 (MLS_KEY_PACKAGE): the server adds us to the group with it.
static int send_key_package(vthread *t)
{
    if (!t->dave)
        return 0;
    uint8_t *kp = NULL;
    size_t len = 0;
    vau_lock();
    daveSessionGetMarshalledKeyPackage(t->dave, &kp, &len);
    vau_unlock();
    LOG("voice: DAVE key package %zu bytes", len);
    int r = kp ? send_bin(t, 26, kp, len) : 0;
    daveFree(kp);
    return r;
}

// Opcode 24 (PREPARE_EPOCH): epoch 1 starts a new MLS group, so send our key package.
static int dave_prepare_epoch(vthread *t, cJSON *d)
{
    int version = (int)num(d, "protocol_version");
    if (num(d, "epoch") != 1)
        return 0; // a version change of a running group: nothing to send yet
    dave_reset(t, version);
    return send_key_package(t);
}

// Opcode 21/22/23-style JSON: the transitions.
static int on_dave_json(vthread *t, int op, cJSON *d)
{
    int64_t tid = num(d, "transition_id");
    LOG("voice: DAVE op %d, transition %lld, version %d", op, (long long)tid, (int)num(d, "protocol_version"));
    switch (op) {
    case 21: // PREPARE_TRANSITION
        t->pending_version = (int)num(d, "protocol_version");
        if (tid == 0) {
            t->dave_version = t->pending_version;
            update_state(t);
        } else {
            return send_json(t, voice_json_transition(23, tid));
        }
        return 0;
    case 22: // EXECUTE_TRANSITION
        // A transition announced by a commit or welcome (no op 21) keeps the version:
        // pending_version is only changed by op 21, and starts as the current one.
        t->dave_version = t->pending_version;
        update_state(t);
        return 0;
    case 24:
        return dave_prepare_epoch(t, d);
    }
    return 0;
}

// The users libdave may accept in the group: everyone op 11/13 reported, and us
// (libdave checks every member of a welcome's roster, ourselves included).
static size_t recognized(vthread *t, const char *ids[MAX_USERS + 1])
{
    size_t n = 0;
    int self = 0;
    for (int i = 0; i < t->n_users; i++) {
        ids[n++] = t->users[i];
        self |= strcmp(t->users[i], t->user_id) == 0;
    }
    if (!self)
        ids[n++] = t->user_id;
    return n;
}

// Opcode 27 (MLS_PROPOSALS): libdave reads the optype byte (append/revoke) itself.
static void dave_proposals(vthread *t, const uint8_t *p, size_t n)
{
    if (!t->dave || n < 2)
        return;
    const char *ids[MAX_USERS + 1];
    size_t n_ids = recognized(t, ids);
    uint8_t *out = NULL;
    size_t out_len = 0;
    vau_lock();
    daveSessionProcessProposals(t->dave, p, n, ids, n_ids, &out, &out_len);
    vau_unlock();
    LOG("voice: DAVE proposals with %zu recognized users, commit %zu bytes", n_ids, out_len);
    if (out && out_len)
        send_bin(t, 28, out, out_len);
    if (out)
        daveFree(out);
    if (out_len) {
        t->dave_ready = 1;
        update_state(t);
    }
}


// Opcodes 29 (commit) and 30 (welcome): u16 transition id, then the MLS message.
static void dave_commit_or_welcome(vthread *t, int op, const uint8_t *p, size_t n)
{
    if (!t->dave || n < 2)
        return;
    int64_t tid = p[0] << 8 | p[1];
    const char *ids[MAX_USERS + 1];
    int failed;
    if (op == 29) {
        vau_lock();
        DAVECommitResultHandle r = daveSessionProcessCommit(t->dave, p + 2, n - 2);
        vau_unlock();
        failed = !r || daveCommitResultIsFailed(r);
        if (r)
            daveCommitResultDestroy(r);
    } else {
        size_t n_ids = recognized(t, ids);
        vau_lock();
        DAVEWelcomeResultHandle r = daveSessionProcessWelcome(t->dave, p + 2, n - 2, ids, n_ids);
        vau_unlock();
        failed = !r;
        if (r)
            daveWelcomeResultDestroy(r);
    }
    LOG("voice: DAVE %s %s, transition %lld", op == 29 ? "commit" : "welcome", failed ? "failed" : "ok",
        (long long)tid);
    if (failed) { // ask for a re-add, a few times: a roster we cannot match fails every time
        if (++t->readds <= 3)
            send_json(t, voice_json_transition(31, tid));
        else
            LOG("voice: DAVE keeps failing, no more re-adds (nothing is sent until a group forms)");
        return;
    }
    t->readds = 0;
    int first = !t->dave_ready;
    t->dave_ready = 1;
    if (tid)
        send_json(t, voice_json_transition(23, tid));
    // The new epoch's keys are used once Discord executes the transition (op 22, which updates
    // the state); a transition 0 or our first group takes effect now.
    if (!tid || first)
        update_state(t);
}

// Binary frame from the server: [seq][op][payload].
static void on_binary(vthread *t, const uint8_t *msg, size_t len)
{
    voice_bin b;
    if (voice_bin_parse(msg, len, &b) != 0)
        return;
    if (b.seq >= 0)
        t->seq_ack = b.seq;
    LOG("voice: DAVE op %d, %zu bytes", b.op, b.len);
    switch (b.op) {
    case 25: // MLS_EXTERNAL_SENDER
        if (b.len <= sizeof t->ext_sender) {
            memcpy(t->ext_sender, b.payload, b.len);
            t->ext_sender_len = b.len;
        }
        if (t->dave) {
            vau_lock();
            daveSessionSetExternalSender(t->dave, b.payload, b.len);
            vau_unlock();
        }
        break;
    case 27:
        dave_proposals(t, b.payload, b.len);
        break;
    case 29:
    case 30:
        dave_commit_or_welcome(t, b.op, b.payload, b.len);
        break;
    }
}

// Text frame: a JSON opcode. Returns 0, -1 on transport trouble, -2 fatal (t->err set).
static int on_text(vthread *t, const char *json)
{
    cJSON *m = cJSON_Parse(json);
    if (!m)
        return 0;
    if (cJSON_IsNumber(cJSON_GetObjectItem(m, "seq")))
        t->seq_ack = num(m, "seq");
    int op = (int)json_int(m, "op", 0);
    cJSON *d = cJSON_GetObjectItem(m, "d");
    int r = 0;
    switch (op) {
    case 8: // HELLO
        t->hb_ms = num(d, "heartbeat_interval");
        if (t->hb_ms < 1000) // a bad interval must not turn into a heartbeat storm
            t->hb_ms = 1000;
        t->next_hb = mono_ms() + t->hb_ms;
        break;
    case 2:
        r = on_ready(t, d);
        break;
    case 4:
        r = on_session(t, d);
        break;
    case 6: { // HEARTBEAT_ACK
        t->hb_acked = 1;
        int ping = (int)(mono_ms() - t->hb_sent);
        pthread_mutex_lock(&lock);
        if (cur == t)
            ping_ms = ping;
        pthread_mutex_unlock(&lock);
        break;
    }
    case 9: // RESUMED: Discord sends no new op 4, so the engine comes back here
        attach_audio(t);
        LOG("voice: resumed");
        break;
    case 11: { // CLIENTS_CONNECT
        cJSON *u;
        cJSON_ArrayForEach(u, cJSON_GetObjectItem(d, "user_ids"))
            if (cJSON_IsString(u)) {
                users_add(t, u->valuestring);
                vau_expect_user(u->valuestring);
            }
        break;
    }
    case 5: // SPEAKING: who an ssrc belongs to
        vau_map_ssrc((uint32_t)num(d, "ssrc"), str(d, "user_id"));
        break;
    case 13: // CLIENT_DISCONNECT
        if (str(d, "user_id")) {
            users_remove(t, str(d, "user_id"));
            vau_forget_user(str(d, "user_id"));
        }
        break;
    case 21:
    case 22:
    case 24:
        r = on_dave_json(t, op, d);
        break;
    default: { // once per opcode: what the server sends that we do not use yet
        static uint64_t seen;
        if (op >= 0 && op < 64 && !(seen >> op & 1)) {
            seen |= 1ull << op;
            LOG("voice: op %d not handled", op);
        }
    }
    }
    cJSON_Delete(m);
    return r;
}

// One voice websocket: identify or resume, then serve it until it ends.
// Returns the close code, 0 when stopped, -1 for a dropped connection, -2 fatal.
static int run_session(vthread *t)
{
    char *first = t->ready
        ? voice_json_resume(t->server_id, t->session_id, t->token, t->seq_ack)
        : voice_json_identify(t->server_id, t->user_id, t->session_id, t->token,
                              (int)daveMaxSupportedProtocolVersion());
    if (send_json(t, first) != 0)
        return -1;
    t->hb_acked = 1;
    t->next_hb = 0;
    for (;;) {
        if (t->stop)
            return 0;
        int speaking;
        uint32_t ssrc;
        while (vau_take_speaking(&speaking, &ssrc))
            if (send_json(t, voice_json_speaking(speaking, ssrc)) != 0)
                return -1;
        int64_t now = mono_ms();
        if (t->next_hb && now >= t->next_hb) {
            if (!t->hb_acked) {
                LOG("voice: heartbeat not acked, reconnecting");
                return -1;
            }
            t->hb_sent = now;
            t->hb_acked = 0;
            if (send_json(t, voice_json_heartbeat(now, t->seq_ack)) != 0)
                return -1;
            t->next_hb = now + t->hb_ms;
            continue;
        }
        pthread_mutex_lock(&lock);
        int dead = att_owner == t && vau_udp_dead();
        pthread_mutex_unlock(&lock);
        if (dead) { // a resume would keep the dead socket: identify again, for a new one
            t->ready = 0;
            return -1;
        }
        int wait = vau_speaking_pending() ? 5 : 500;
        if (t->next_hb && t->next_hb - now < wait)
            wait = (int)(t->next_hb - now);
        int m = ws_recv(&t->ws, wait);
        if (m == 0)
            continue;
        if (m == WS_CLOSE)
            return t->ws.close_code;
        if (m < 0)
            return -1;
        if (m == WS_BINARY) {
            on_binary(t, t->ws.msg, t->ws.msg_len);
            continue;
        }
        int r = on_text(t, (const char *)t->ws.msg);
        if (r != 0)
            return r;
    }
}

static int connect_once(vthread *t)
{
    char url[400];
    t->healthy = 0;
    snprintf(url, sizeof url, "wss://%s%s", t->endpoint, VOICE_QUERY);
    if (ws_open(&t->ws, url) != 0) {
        tls_close(&t->ws.tls);
        return VOICE_UNREACHABLE;
    }
    int code = run_session(t);
    audio_release(t);
    ws_close(&t->ws, 1000);
    return code;
}

// The voice thread's end: reports s (or asks for a rejoin) unless it was already replaced.
static void finish(vthread *t, enum vstate s, const char *msg, int rejoin)
{
    pthread_mutex_lock(&lock);
    if (cur == t) {
        cur = NULL;
        if (rejoin) { // the server dropped our session: ask again, it sends new details
            token[0] = endpoint[0] = 0;
            op4 = (voice_op4){.mute = mute, .deaf = deaf, .leave_first = listed[0] != 0};
            copy(op4.guild, sizeof op4.guild, want_guild);
            copy(op4.channel, sizeof op4.channel, want_channel);
            op4_due = 1;
            joined_ms = mono_ms();
            set_state_locked(V_JOINING);
        } else {
            guild[0] = channel[0] = session_id[0] = token[0] = endpoint[0] = 0;
            copy(err_text, sizeof err_text, msg);
            want_guild[0] = want_channel[0] = 0; // an error too: READY must not rejoin it in a loop
            queue_leave_locked(); // keep the gateway's view in step: we are not in the channel
            set_state_locked(s);
        }
    }
    pthread_mutex_unlock(&lock);
}

static _Atomic int retry_now; // voice_retry: the app's Retry now

void voice_retry(void)
{
    retry_now = 1;
}

static int sleep_unless_stopped(vthread *t, int seconds)
{
    retry_now = 0;
    for (int i = 0; i < seconds * 10 && !t->stop && !retry_now; i++)
        usleep(100000);
    return !t->stop;
}

static void *voice_main(void *arg)
{
    vthread *t = arg;
    int resumes = 0, backoff = 1;
    int64_t unreachable_since = 0;
    for (;;) {
        int code = connect_once(t);
        if (t->stop)
            break;
        if (t->healthy) // it worked this time: the next drop starts the count again
            resumes = 0, backoff = 1;
        char msg[160];
        if (code == -2) {
            finish(t, V_ERROR, t->err, 0);
            break;
        }
        if (code == 4006 || code == 4009) { // session no longer valid: a fresh join
            finish(t, V_JOINING, "", 1);
            break;
        }
        if (code == 4014 || code == 4022) { // kicked, or the call ended
            finish(t, V_IDLE, "", 0);
            break;
        }
        if (code == 4017) {
            finish(t, V_ERROR, "This channel needs end-to-end encryption, which this build does not support yet.", 0);
            break;
        }
        if (code == 4021) {
            finish(t, V_ERROR, "Discord rate limited the voice connection. Wait a minute and try again.", 0);
            break;
        }
        if ((code >= 4001 && code <= 4005) || code == 4011 || code == 4012 || code == 4016 || code == 4020) {
            snprintf(msg, sizeof msg, "Discord closed the voice connection (%d).", code);
            finish(t, V_ERROR, msg, 0);
            break;
        }
        // A dropped socket or 4015 (voice server crashed): resume, with a limit.
        LOG("voice: connection ended (%d), resume in %d s", code, backoff);
        if (code == VOICE_UNREACHABLE) {
            if (!unreachable_since)
                unreachable_since = mono_ms();
            if (mono_ms() - unreachable_since > UNREACHABLE_MS) {
                finish(t, V_ERROR, "Lost the voice server.", 0);
                break;
            }
        } else {
            unreachable_since = 0;
            if (++resumes > MAX_RESUMES) {
                finish(t, V_ERROR, "Lost the voice server.", 0);
                break;
            }
        }
        pthread_mutex_lock(&lock);
        if (cur == t)
            set_state_locked(V_CONNECTING);
        pthread_mutex_unlock(&lock);
        if (!sleep_unless_stopped(t, backoff))
            break;
        backoff = backoff < 16 ? backoff * 2 : 16;
    }
    if (t->dave)
        daveSessionDestroy(t->dave);
    if (t->udp >= 0)
        close(t->udp);
    free(t);
    return NULL;
}

// lock held. Starts a voice thread from the current details; the caller checked them.
static void start_locked(void)
{
    vthread *t = calloc(1, sizeof *t);
    if (!t)
        return;
    t->udp = -1;
    t->seq_ack = -1;
    copy(t->endpoint, sizeof t->endpoint, endpoint);
    copy(t->token, sizeof t->token, token);
    copy(t->session_id, sizeof t->session_id, session_id);
    copy(t->server_id, sizeof t->server_id, guild[0] ? guild : channel);
    copy(t->channel_id, sizeof t->channel_id, channel);
    copy(t->user_id, sizeof t->user_id, self_id);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    pthread_t th;
    if (pthread_create(&th, &attr, voice_main, t) == 0) {
        cur = t;
        set_state_locked(V_CONNECTING);
    } else {
        free(t);
        copy(err_text, sizeof err_text, "Could not start the voice connection.");
        set_state_locked(V_ERROR);
    }
    pthread_attr_destroy(&attr);
}
