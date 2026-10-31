#include "gateway.h"
#include "json_num.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>

#include <cJSON.h>

#include "chat.h"
#include "log.h"
#include "playing.h"
#include "prefs.h"
#include "props.h"
#include "status.h"
#include "voice.h"
#include "stream.h"
#include "ws.h"
#include "zstream.h"

#define GATEWAY_URL "wss://gateway.discord.gg"
#define GATEWAY_QUERY "/?encoding=json&v=9&compress=zlib-stream"
// Same value the web client sent in build 641004 (docs/gateway.md).
#define CAPABILITIES 1734653

enum { OP_DISPATCH = 0, OP_HEARTBEAT = 1, OP_IDENTIFY = 2, OP_PRESENCE = 3, OP_VOICE_STATE = 4, OP_RESUME = 6, OP_RECONNECT = 7,
       OP_INVALID_SESSION = 9, OP_HELLO = 10, OP_HEARTBEAT_ACK = 11, OP_GUILD_SUBSCRIPTIONS_BULK = 37 };

typedef struct {
    const char *token;
    ws_conn ws;
    zstream z;
    int64_t seq; // -1 until the first dispatch
    char session_id[128];
    char resume_url[256];
    int heartbeat_ms;
    int ready; // READY or RESUMED on this connection: Discord took us
    int64_t next_heartbeat; // monotonic ms, 0 = not started
    int acked;
    int reconnect; // set by handlers: leave the read loop
    char game[128]; // the game Discord shows for this session ("" none)
    char cover[512]; // ... and its picture (playing_cover)
    int64_t game_check; // monotonic ms of the next look at the console's game
} gateway;

// READY: the user's linked PlayStation account already posts the game (Sony's own
// integration), so this session sends none.
static int sony_shows_game;

// All socket I/O stays on the gateway thread (an mbedTLS context is not safe for a
// concurrent read and write). Other threads leave requests here.
static pthread_mutex_t pending_lock = PTHREAD_MUTEX_INITIALIZER;
static char pending_subscribe[32];

static int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int send_json(gateway *g, cJSON *msg)
{
    char *s = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (!s)
        return -1;
    int r = ws_send(&g->ws, WS_TEXT, s, strlen(s));
    free(s);
    return r;
}

static cJSON *op_msg(int op, cJSON *d)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddNumberToObject(m, "op", op);
    cJSON_AddItemToObject(m, "d", d ? d : cJSON_CreateNull());
    return m;
}

static int send_heartbeat(gateway *g)
{
    g->acked = 0;
    return send_json(g, op_msg(OP_HEARTBEAT, g->seq >= 0 ? cJSON_CreateNumber((double)g->seq) : NULL));
}

static int send_identify(gateway *g)
{
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "token", g->token);
    cJSON_AddNumberToObject(d, "capabilities", CAPABILITIES);
    cJSON_AddItemToObject(d, "properties", props_create());
    cJSON *pr = cJSON_AddObjectToObject(d, "presence");
    cJSON_AddStringToObject(pr, "status", "unknown");
    cJSON_AddNumberToObject(pr, "since", 0);
    cJSON_AddArrayToObject(pr, "activities");
    cJSON_AddFalseToObject(pr, "afk");
    cJSON_AddFalseToObject(d, "compress");
    cJSON *cs = cJSON_AddObjectToObject(d, "client_state");
    cJSON_AddObjectToObject(cs, "guild_versions");
    LOG("gateway: identify");
    return send_json(g, op_msg(OP_IDENTIFY, d));
}

static int send_resume(gateway *g)
{
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "token", g->token);
    cJSON_AddStringToObject(d, "session_id", g->session_id);
    cJSON_AddNumberToObject(d, "seq", (double)g->seq);
    LOG("gateway: resume seq %lld", (long long)g->seq);
    return send_json(g, op_msg(OP_RESUME, d));
}

static void drop_session(gateway *g)
{
    g->session_id[0] = 0;
    g->resume_url[0] = 0;
    g->seq = -1;
}

static void on_dispatch(gateway *g, const char *t, cJSON *d)
{
    chat_dispatch(t, d);
    if (strcmp(t, "READY") == 0) {
        const char *sid = cJSON_GetStringValue(cJSON_GetObjectItem(d, "session_id"));
        const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(d, "resume_gateway_url"));
        const char *name = cJSON_GetStringValue(
            cJSON_GetObjectItem(cJSON_GetObjectItem(d, "user"), "username"));
        const char *me = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(d, "user"), "id"));
        voice_set_self(me ? me : "");
        voice_on_ready(d);
        snprintf(g->session_id, sizeof g->session_id, "%s", sid ? sid : "");
        snprintf(g->resume_url, sizeof g->resume_url, "%s", url ? url : "");
        sony_shows_game = 0;
        cJSON *acct;
        cJSON_ArrayForEach(acct, cJSON_GetObjectItem(d, "connected_accounts"))
            sony_shows_game |= strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(acct, "type")) ?: "", "playstation") == 0 &&
                               cJSON_IsTrue(cJSON_GetObjectItem(acct, "show_activity"));
        g->game[0] = g->cover[0] = 0; // IDENTIFY sent no activities
        g->game_check = 0;
        status_user(name, cJSON_GetArraySize(cJSON_GetObjectItem(d, "guilds")),
                    cJSON_GetArraySize(cJSON_GetObjectItem(d, "private_channels")));
        status_state("ready");
        status_note(NULL);
        g->ready = 1;
        status_attempt(0);
        LOG("gateway: READY as %s, %d guilds, %d DMs, resume via %s", name ? name : "?",
            cJSON_GetArraySize(cJSON_GetObjectItem(d, "guilds")),
            cJSON_GetArraySize(cJSON_GetObjectItem(d, "private_channels")), g->resume_url);
    } else if (strcmp(t, "RESUMED") == 0) {
        status_state("ready");
        status_note(NULL);
        g->ready = 1;
        status_attempt(0);
        LOG("gateway: RESUMED");
    } else if (strcmp(t, "VOICE_STATE_UPDATE") == 0) {
        LOG("gateway: VOICE_STATE_UPDATE %s mute=%d", cJSON_GetObjectItem(d, "guild_id") &&
            !cJSON_IsNull(cJSON_GetObjectItem(d, "guild_id")) ? "guild" : "call",
            cJSON_IsTrue(cJSON_GetObjectItem(d, "self_mute")));
        voice_on_state(d);
    } else if (strcmp(t, "VOICE_SERVER_UPDATE") == 0) {
        voice_on_server(d);
    } else if (strncmp(t, "STREAM_", 7) == 0) {
        LOG("gateway: %s", t);
        stream_on_dispatch(t, d);
    } else {
        // Event names only; message contents stay out of the console log.
        status_event();
        LOG("gateway: %s s=%lld", t, (long long)g->seq);
    }
}

static int on_message(gateway *g, const char *json)
{
    cJSON *m = cJSON_Parse(json);
    if (!m)
        return LOG("gateway: bad json"), -1;
    int op = (int)json_int(m, "op", 0);
    cJSON *s = cJSON_GetObjectItem(m, "s"), *d = cJSON_GetObjectItem(m, "d");
    if (cJSON_IsNumber(s))
        g->seq = (int64_t)s->valuedouble;

    int r = 0;
    switch (op) {
    case OP_HELLO:
        g->heartbeat_ms = (int)json_int(d, "heartbeat_interval", 41250);
        if (g->heartbeat_ms < 1000) // 0 would be a heartbeat on every turn of the loop
            g->heartbeat_ms = 1000;
        g->next_heartbeat = now_ms() + (int64_t)(g->heartbeat_ms * (rand() / (RAND_MAX + 1.0)));
        g->acked = 1;
        LOG("gateway: HELLO, heartbeat %d ms", g->heartbeat_ms);
        r = g->session_id[0] ? send_resume(g) : send_identify(g);
        break;
    case OP_HEARTBEAT_ACK:
        g->acked = 1;
        break;
    case OP_HEARTBEAT:
        r = send_heartbeat(g);
        break;
    case OP_RECONNECT:
        LOG("gateway: server asked to reconnect");
        g->reconnect = 1;
        break;
    case OP_INVALID_SESSION:
        LOG("gateway: invalid session (resumable: %s)", cJSON_IsTrue(d) ? "yes" : "no");
        if (!cJSON_IsTrue(d)) {
            drop_session(g);
            sleep(1 + rand() % 5); // docs: wait 1-5 s before a fresh identify
        }
        g->reconnect = 1;
        break;
    case OP_DISPATCH: {
        const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(m, "t"));
        if (t)
            on_dispatch(g, t, d);
        break;
    }
    default:
        LOG("gateway: unhandled op %d", op);
    }
    cJSON_Delete(m);
    return r;
}

static int send_pending(gateway *g);

static _Atomic int switching; // gateway_switch: another console user's account is wanted

void gateway_switch(int on)
{
    switching = on;
}

static _Atomic int retry_now;

void gateway_retry(void)
{
    retry_now = 1;
}

int gateway_switching(void)
{
    return switching;
}

// One connection. Returns the server close code, 0 for a client-side reconnect, -1 on error.
static int run_connection(gateway *g)
{
    char url[320];
    snprintf(url, sizeof url, "%s%s", g->resume_url[0] ? g->resume_url : GATEWAY_URL, GATEWAY_QUERY);
    status_state("connecting");
    if (ws_open(&g->ws, url) != 0) {
        tls_close(&g->ws.tls);
        return -1;
    }
    if (zstream_init(&g->z) != 0) {
        ws_close(&g->ws, 1000);
        return -1;
    }
    g->next_heartbeat = 0;
    g->reconnect = 0;
    int64_t started = now_ms();

    int result = 0;
    for (;;) {
        if (switching) {
            result = GATEWAY_SWITCH;
            break;
        }
        int timeout = 1000; // short, so requests from other threads go out quickly
        if (g->next_heartbeat) {
            int64_t left = g->next_heartbeat - now_ms();
            timeout = left <= 0 ? 0 : left < timeout ? (int)left : timeout;
        }
        // Heartbeat on schedule even while events keep the socket busy.
        if (g->next_heartbeat && now_ms() >= g->next_heartbeat) {
            if (!g->acked) { // zombied connection
                LOG("gateway: heartbeat not acked, reconnecting");
                break;
            }
            if (send_heartbeat(g) != 0) {
                result = -1;
                break;
            }
            g->next_heartbeat += g->heartbeat_ms;
            continue;
        }
        voice_tick();
        if (send_pending(g) != 0) {
            result = -1;
            break;
        }
        int r = ws_recv(&g->ws, timeout);
        if (r == 0) {
            if (!g->next_heartbeat && now_ms() - started >= 30000) {
                LOG("gateway: no HELLO");
                result = -1;
                break;
            }
            continue;
        }
        if (r == WS_CLOSE) {
            result = g->ws.close_code;
            break;
        }
        if (r < 0) {
            result = -1;
            break;
        }
        const char *json = (const char *)g->ws.msg;
        if (r == WS_BINARY) {
            int z = zstream_feed(&g->z, g->ws.msg, g->ws.msg_len);
            if (z < 0) {
                LOG("gateway: inflate failed");
                result = -1;
                break;
            }
            if (z == 0)
                continue;
            json = g->z.out;
        }
        if (on_message(g, json) != 0 || g->reconnect)
            break;
    }
    zstream_free(&g->z);
    // 4000 keeps the session resumable; 1000 ends it (another user's account is next).
    ws_close(&g->ws, result == GATEWAY_SWITCH ? 1000 : 4000);
    return result;
}

int gateway_subscribe(const char *guild_id)
{
    pthread_mutex_lock(&pending_lock);
    snprintf(pending_subscribe, sizeof pending_subscribe, "%s", guild_id);
    pthread_mutex_unlock(&pending_lock);
    return 0;
}

// Op 4: join, move, leave or change mute/deaf in a voice channel.
static int send_voice_state(gateway *g, const voice_op4 *v)
{
    cJSON *d = cJSON_CreateObject();
    cJSON_AddItemToObject(d, "guild_id", v->guild[0] ? cJSON_CreateString(v->guild) : cJSON_CreateNull());
    cJSON_AddItemToObject(d, "channel_id", v->channel[0] ? cJSON_CreateString(v->channel) : cJSON_CreateNull());
    cJSON_AddBoolToObject(d, "self_mute", v->mute);
    cJSON_AddBoolToObject(d, "self_deaf", v->deaf);
    cJSON_AddFalseToObject(d, "self_video");
    cJSON_AddNumberToObject(d, "flags", 2);
    LOG("gateway: voice state %s", v->channel[0] ? "join" : "leave");
    return send_json(g, op_msg(OP_VOICE_STATE, d));
}

// Discord's "Playing <game>": a look every 15 s, op 3 only on a change (Discord allows
// about 5 presence updates a minute). A RESUME keeps the session's presence; READY starts none.
// The cover arrives a look or two after the name, and is sent then.
static int send_game(gateway *g)
{
    static char seen[128];
    static int64_t since; // when the game in front was first seen: Discord shows the time played
    int64_t now = now_ms();
    if (now < g->game_check)
        return 0;
    g->game_check = now + 15000;
    char title[16], name[128] = "", cover[512] = "";
    if (prefs_get("share_game") && !sony_shows_game)
        playing_now(title, name, sizeof name);
    if (name[0])
        playing_cover(title, g->token, cover, sizeof cover);
    if (strcmp(name, seen) != 0) {
        snprintf(seen, sizeof seen, "%s", name);
        since = (int64_t)time(NULL) * 1000;
    }
    if (strcmp(name, g->game) == 0 && strcmp(cover, g->cover) == 0)
        return 0;
    snprintf(g->game, sizeof g->game, "%s", name);
    snprintf(g->cover, sizeof g->cover, "%s", cover);
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "status", "unknown"); // as IDENTIFY: the status set elsewhere stays
    cJSON_AddNumberToObject(d, "since", 0);
    cJSON *acts = cJSON_AddArrayToObject(d, "activities");
    if (name[0]) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddNumberToObject(a, "type", 0);
        cJSON_AddStringToObject(a, "name", name);
        cJSON_AddNumberToObject(cJSON_AddObjectToObject(a, "timestamps"), "start", (double)since);
        if (PLAYING_APP_ID[0]) // its icon when there is no cover
            cJSON_AddStringToObject(a, "application_id", PLAYING_APP_ID);
        if (cover[0]) {
            cJSON *assets = cJSON_AddObjectToObject(a, "assets");
            cJSON_AddStringToObject(assets, "large_image", cover);
            cJSON_AddStringToObject(assets, "large_text", name);
        }
        cJSON_AddItemToArray(acts, a);
    }
    cJSON_AddFalseToObject(d, "afk");
    LOG("gateway: playing %s", name[0] ? name : "nothing");
    return send_json(g, op_msg(OP_PRESENCE, d));
}

// Called on the gateway thread between reads.
static int send_pending(gateway *g)
{
    // Only once Discord took this connection (READY or RESUMED): anything sent before the
    // resume is answered with 4003 and lost with the connection. It waits in its queue.
    if (!g->ready)
        return 0;
    voice_op4 v;
    if (voice_take_request(&v)) {
        if (v.leave_first) { // otherwise a join of the channel Discord has us in brings no reply
            voice_op4 leave = {.mute = v.mute, .deaf = v.deaf};
            if (send_voice_state(g, &leave) != 0)
                return -1;
        }
        if (send_voice_state(g, &v) != 0)
            return -1;
    }
    if (send_game(g) != 0)
        return -1;
    cJSON *op;
    while ((op = stream_take_gateway()))
        if (send_json(g, op) != 0) // Go Live ops 18, 22, 19
            return -1;
    char guild[32];
    pthread_mutex_lock(&pending_lock);
    snprintf(guild, sizeof guild, "%s", pending_subscribe);
    pending_subscribe[0] = 0;
    pthread_mutex_unlock(&pending_lock);
    if (!guild[0])
        return 0;
    cJSON *sub = cJSON_CreateObject(), *subs = cJSON_AddObjectToObject(sub, "subscriptions");
    cJSON *opts = cJSON_AddObjectToObject(subs, guild);
    cJSON_AddTrueToObject(opts, "typing");
    cJSON_AddTrueToObject(opts, "activities");
    cJSON_AddTrueToObject(opts, "threads");
    return send_json(g, op_msg(OP_GUILD_SUBSCRIPTIONS_BULK, sub));
}

// Why the gateway cannot be reached, for the app's connecting screen. A failed name
// lookup almost always means the console's DNS is down (e.g. nanoDNS not started).
static void note_unreachable(void)
{
    struct addrinfo hints = {.ai_socktype = SOCK_STREAM}, *ai = NULL;
    int dns_ok = getaddrinfo("gateway.discord.gg", "443", &hints, &ai) == 0;
    if (ai)
        freeaddrinfo(ai);
    // Short: the app shows it on one line. No address at all is almost always no network.
    status_note(dns_ok ? "Can't reach Discord." : "No internet connection.");
}

int gateway_run(const char *token)
{
    gateway g = {.token = token, .seq = -1};
    int backoff = 1, attempt = 0;
    for (;;) {
        int code = run_connection(&g);
        if (code == GATEWAY_SWITCH)
            return code;
        const int action = gateway_close_action(code);
        if (action == GATEWAY_STOP) {
            LOG("gateway: fatal close %d, stopping", code);
            return code;
        }
        if (action == GATEWAY_FRESH)
            drop_session(&g);
        // Only a session Discord accepted resets the wait: a close after HELLO (rate limit,
        // invalid session) must not turn into an identify every second.
        if (g.ready)
            backoff = 1, attempt = 0;
        else if (!g.heartbeat_ms && backoff >= 2) // no HELLO a second time in a row: say why in the app
            note_unreachable();
        g.heartbeat_ms = 0;
        g.ready = 0;
        LOG("gateway: connection ended (%d), retry in %d s", code, backoff);
        status_attempt(++attempt);
        retry_now = 0;
        for (int i = 0; i < backoff * 5 && !switching && !retry_now; i++) // another user, or Retry now: go now
            usleep(200000);
        if (switching)
            return GATEWAY_SWITCH;
        backoff = gateway_next_wait(backoff);
    }
}
