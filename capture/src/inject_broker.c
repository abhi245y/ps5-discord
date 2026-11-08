// SPDX-License-Identifier: GPL-3.0-only
// Whole injector operations execute in the supervisor. Killing the worker cannot destroy the
// tracer or its restoration journal. The inherited arena is bulk transfer storage, not authority.
#include "inject_broker.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "shared broker sequence must be lock-free");

struct request {
    uint32_t sequence, op;
    uintptr_t address;
    size_t len;
    uint64_t args[6];
    int save_fp;
    char name[32];
};
struct view {
    pid_t target_pid, thread_pid;
    intptr_t scratch;
    size_t scratch_len;
    int attached, running, quarantined, stall_count, last_errno, last_prot, pending, dead, aborted_restored;
    uint64_t last_rip;
    char stage[64];
};
struct sl_rpc {
    _Atomic unsigned ready;
    struct request request;
    struct view view;
    int64_t result;
    int error;
    uint8_t bytes[SL_RPC_BYTES];
};
static sl_broker *client;

static int send_byte(int fd)
{
    const uint8_t byte = 1;
    ssize_t n;
    do { n = send(fd, &byte, 1, MSG_NOSIGNAL); } while (n < 0 && errno == EINTR);
    return n == 1 ? 0 : -1;
}
static int read_byte(int fd)
{
    uint8_t byte;
    ssize_t n;
    do { n = recv(fd, &byte, 1, 0); } while (n < 0 && errno == EINTR);
    return n == 1 && byte == 1 ? 0 : -1;
}
int sl_broker_open(sl_broker *b)
{
    memset(b, 0, sizeof *b);
    atomic_init(&b->context.restore_pending, 0);
    b->channel[0] = b->channel[1] = -1;
    b->shared = mmap(NULL, sizeof *b->shared, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (b->shared == MAP_FAILED) { b->shared = NULL; return -1; }
    memset(b->shared, 0, sizeof *b->shared);
    atomic_init(&b->shared->ready, 0);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, b->channel)) {
        munmap(b->shared, sizeof *b->shared); b->shared = NULL; return -1;
    }
    return 0;
}
void sl_broker_worker(sl_broker *b)
{
    close(b->channel[0]); b->channel[0] = -1;
    client = b;
}
void sl_broker_parent(sl_broker *b)
{
    close(b->channel[1]); b->channel[1] = -1;
}
int sl_broker_client(void) { return client != NULL; }

static void export_view(struct view *v, const sl_inject *c)
{
    *v = (struct view){.target_pid = c->target_pid, .thread_pid = c->thread_pid,
        .scratch = c->scratch, .scratch_len = c->scratch_len, .attached = c->attached,
        .running = c->running, .quarantined = c->quarantined, .stall_count = c->stall_count,
        .last_errno = c->last_errno, .last_prot = c->last_prot, .last_rip = c->last_rip,
        .pending = atomic_load(&c->restore_pending), .dead = c->dead, .aborted_restored = c->aborted_restored};
    snprintf(v->stage, sizeof v->stage, "%s", c->stage ? c->stage : "idle");
}
static void import_view(sl_inject *c, const struct view *v)
{
    c->target_pid = v->target_pid; c->thread_pid = v->thread_pid;
    c->scratch = v->scratch; c->scratch_len = v->scratch_len;
    c->attached = v->attached; c->running = v->running; c->quarantined = v->quarantined;
    c->stall_count = v->stall_count; c->last_errno = v->last_errno; c->last_prot = v->last_prot;
    c->last_rip = v->last_rip; c->dead = v->dead; c->aborted_restored = v->aborted_restored;
    atomic_store(&c->restore_pending, v->pending);
    memcpy(client->stage, v->stage, sizeof client->stage);
    client->stage[sizeof client->stage - 1] = 0; c->stage = client->stage;
}

int64_t sl_broker_request(sl_inject *view, enum sl_rpc_op op, intptr_t address, void *bytes,
                          size_t len, const uint64_t args[6], int save_fp, const char *name)
{
    sl_broker *b = client;
    if (!b || !b->shared || len > SL_RPC_BYTES || (len && !bytes)) return SL_CALL_ERROR;
    if (name && strlen(name) >= sizeof ((struct request *)0)->name) return SL_CALL_ERROR;
    struct sl_rpc *s = b->shared;
    struct request r = {.sequence = ++b->sequence, .op = op, .address = (uintptr_t)address,
                        .len = len, .save_fp = save_fp};
    if (args) memcpy(r.args, args, sizeof r.args);
    if (name) {
        if (strlen(name) >= sizeof r.name) return SL_CALL_ERROR;
        memcpy(r.name, name, strlen(name) + 1);
    }
    if (op == SL_RPC_WRITE && len) memcpy(s->bytes, bytes, len);
    s->request = r;
    atomic_store_explicit(&s->ready, r.sequence, memory_order_release);
    if (send_byte(b->channel[1]) || read_byte(b->channel[1])) {
        view->quarantined = 1; view->stage = "supervisor-channel-lost";
        return SL_CALL_ERROR;
    }
    if (atomic_load_explicit(&s->ready, memory_order_acquire) != 0 || s->request.sequence != r.sequence)
        return SL_CALL_ERROR;
    import_view(view, &s->view);
    errno = s->error;
    if (s->result == 0 && (op == SL_RPC_READ || op == SL_RPC_CODE) && len) memcpy(bytes, s->bytes, len);
    return s->result;
}

int sl_broker_poll(sl_broker *b, int timeout_ms)
{
    struct pollfd p = {.fd = b->channel[0], .events = POLLIN};
    int ready = poll(&p, 1, timeout_ms);
    if (ready <= 0) return ready < 0 && errno != EINTR ? -1 : 0;
    if (read_byte(b->channel[0])) return -1;
    struct sl_rpc *s = b->shared;
    unsigned published = atomic_load_explicit(&s->ready, memory_order_acquire);
    // Immutable header snapshot: the worker is never allowed to replace the operation mid-call.
    const struct request r = s->request;
    if (published != r.sequence || r.sequence != b->sequence + 1 || r.len > SL_RPC_BYTES ||
        r.op < SL_RPC_BEGIN || r.op > SL_RPC_CLEANUP_CALL || r.save_fp < 0 || r.save_fp > 1 ||
        (r.op == SL_RPC_BEGIN && (!memchr(r.name, 0, sizeof r.name) || strcmp(r.name, "SceRemotePlay"))))
        return -2;
    b->sequence = r.sequence;
    int64_t result = SL_CALL_ERROR;
    sl_inject *c = &b->context;
    errno = 0;
    switch (r.op) {
    case SL_RPC_BEGIN:
        if (!c->attached) result = sl_inject_begin(r.name, c);
        break;
    case SL_RPC_READ: result = sl_inject_read_at(c, (intptr_t)r.address, s->bytes, r.len); break;
    case SL_RPC_WRITE: result = sl_inject_write_at(c, (intptr_t)r.address, s->bytes, r.len); break;
    case SL_RPC_CODE: result = sl_inject_read_code_at(c, (intptr_t)r.address, s->bytes, r.len); break;
    case SL_RPC_CALL: result = inj_call_fast(c, (intptr_t)r.address, r.args, r.save_fp); break;
    case SL_RPC_END: result = sl_inject_end(c); break;
    case SL_RPC_STOP: result = sl_inject_ensure_stopped(c); break;
    case SL_RPC_CLEANUP_CALL: result = sl_inject_cleanup_call(c, (intptr_t)r.address, r.args); break;
    }
    int error = errno;
    export_view(&s->view, c);
    s->result = result; s->error = error;
    atomic_store_explicit(&s->ready, 0, memory_order_release);
    return send_byte(b->channel[0]); // EPIPE is normal after worker death; context stays here.
}
int sl_broker_recover(sl_broker *b)
{
    return sl_inject_end(&b->context);
}
int sl_broker_close(sl_broker *b)
{
    if (!sl_inject_safe_to_exit(&b->context)) return -1;
    if (b->channel[0] >= 0) close(b->channel[0]);
    if (b->channel[1] >= 0) close(b->channel[1]);
    if (b->shared) munmap(b->shared, sizeof *b->shared);
    b->shared = NULL; b->channel[0] = b->channel[1] = -1;
    if (client == b) client = NULL;
    return 0;
}
