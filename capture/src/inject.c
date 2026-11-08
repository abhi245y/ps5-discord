// GPL-3.0: MiladRTX/Glint e555d237f40901814949dd6132891ea870b5460a.
// Local changes: bounded calls, strict restoration/detach checks, PID parsing,
// complete scratch-window validation, and no forwarding unexpected signals.
#include "inject.h"
#include "inject_broker.h"

static int broker_status(int64_t rc) { return rc == SL_CALL_ERROR ? -1 : (int)rc; }

#ifdef SL_PS5

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <sys/thr.h>
#include <time.h>
#include <unistd.h>
#include <x86/reg.h>

#include <ps5/kernel.h>

#define SYS_PTRACE 26

/*
 * Authid temporarily installed on the payload process while it attaches to
 * a system process as a debugger. The payload keeps its original authid in
 * `saved_authid` across the ptrace call and restores it before returning.
 */
#define SL_PTRACE_AUTHID 0x4800000000010003ULL

/*
 * struct kinfo_proc offsets used by sl_find_pid.
 *
 * We walk the KERN_PROC_PROC sysctl output (mib = { CTL_KERN, KERN_PROC,
 * KERN_PROC_PROC }) rather than linking the FreeBSD libutil helpers. Each
 * entry starts with its total size as an int, so we read the pid at +0x48
 * and the comm string at +0x1bf relative to that entry.
 */
#define KINFO_PID_OFF   0x48
#define KINFO_COMM_OFF  0x1bf

/* Attach retry: SIGSTOP is delivered asynchronously, so the stopped state
 * takes a few ms to settle after PT_ATTACH. We poll waitpid() in 5 ms
 * increments for up to this many ms before giving up. */
#define SL_WAIT_STOP_MS 1995
#define SL_WAIT_STEP_MS 5

/*
 * ptrace with the payload temporarily given the debugger authid and full caps.
 * Restores authid and caps afterwards and preserves errno.
 */
struct credential_journal { int pending; uint64_t authid; uint8_t caps[16]; };
static struct credential_journal credentials;
static int ptrace_applied;

static int restore_credentials(void)
{
    if (!credentials.pending) return 0;
    pid_t self = getpid();
    int caps_ok = kernel_set_ucred_caps(self, credentials.caps);
    int auth_ok = kernel_set_ucred_authid(self, credentials.authid);
    uint8_t check[16];
    if (caps_ok || auth_ok || kernel_get_ucred_authid(self) != credentials.authid ||
        kernel_get_ucred_caps(self, check) || memcmp(check, credentials.caps, sizeof check)) return -1;
    credentials.pending = 0;
    return 0;
}

static long inj_ptrace(int req, pid_t pid, void *addr, int data)
{
    ptrace_applied = 0;
    if (restore_credentials()) { errno = EPERM; return -1; }
    pid_t self = getpid();
    uint64_t auth = kernel_get_ucred_authid(self);
    uint8_t caps[16], full[16];
    if (!auth || kernel_get_ucred_caps(self, caps)) { errno = EPERM; return -1; }
    credentials.authid = auth;
    memcpy(credentials.caps, caps, sizeof caps);
    credentials.pending = 1; // even a failed credential update may have partially applied
    memset(full, 0xff, sizeof full);
    if (kernel_set_ucred_authid(self, SL_PTRACE_AUTHID) || kernel_set_ucred_caps(self, full)) {
        (void)restore_credentials(); errno = EPERM; return -1;
    }
    errno = 0;
    long rc = syscall(SYS_PTRACE, req, pid, addr, data);
    int err = errno;
    ptrace_applied = rc == 0; // attach/detach ownership survives a credential-rollback error
    if (restore_credentials()) { errno = EPERM; return -1; }
    errno = err;
    return rc;
}

int sl_inject_safe_to_exit(const sl_inject *ctx)
{
    return !credentials.pending && (!ctx || !ctx->attached);
}

int sl_find_pid(const char *name)
{
    int mib[4] = { 1, 14, 8, 0 };
    size_t len = 0;
    if (sysctl(mib, 4, NULL, &len, NULL, 0) != 0) return -1;

    char *buf = malloc(len);
    if (!buf) return -1;

    int pid = -1;
    if (sysctl(mib, 4, buf, &len, NULL, 0) == 0) {
        char *end = buf + len;
        for (char *p = buf; end - p >= (ptrdiff_t)sizeof(int);) {
            int entry_size;
            memcpy(&entry_size, p, sizeof entry_size);
            if (entry_size <= KINFO_COMM_OFF || entry_size > end - p) break;
            const char *comm = p + KINFO_COMM_OFF;
            if (memchr(comm, 0, (size_t)entry_size - KINFO_COMM_OFF) && strcmp(comm, name) == 0) {
                memcpy(&pid, p + KINFO_PID_OFF, sizeof pid);
                break;
            }
            p += entry_size;
        }
    }
    free(buf);
    return pid;
}

intptr_t sl_resolve_in(pid_t pid, const char *basename, const char *sym)
{
    uint32_t handle = 0;
    if (kernel_dynlib_handle(pid, basename, &handle) != 0) return 0;
    return kernel_dynlib_dlsym(pid, handle, sym);
}

intptr_t sl_resolve_nid_in(pid_t pid, const char *basename, const char *nid)
{
    uint32_t handle = 0;
    if (kernel_dynlib_handle(pid, basename, &handle) != 0) return 0;
    return kernel_dynlib_resolve(pid, handle, nid);
}

intptr_t sl_module_base(pid_t pid, const char *basename)
{
    uint32_t handle = 0;
    if (kernel_dynlib_handle(pid, basename, &handle) != 0) return 0;
    return kernel_dynlib_mapbase_addr(pid, handle);
}

// Console diagnosis: which check refused a stop, with its values, for the capture log.
static char stop_diag[64];
#define STOP_FAIL(...) do { snprintf(stop_diag, sizeof stop_diag, __VA_ARGS__); \
                            ctx->stage = stop_diag; return -1; } while (0)

static pid_t tracee(const sl_inject *ctx)
{
    return ctx->thread_pid > 0 ? ctx->thread_pid : ctx->target_pid;
}

static int stopped_lwp(sl_inject *ctx, struct ptrace_lwpinfo *info)
{
    memset(info, 0, sizeof *info);
    if (inj_ptrace(PT_LWPINFO, ctx->target_pid, info, sizeof *info) || info->pl_lwpid <= 0 ||
        info->pl_lwpid == ctx->target_pid || (info->pl_flags & PL_FLAG_EXITED)) return -1;
    return 0;
}

static int injected_registers(sl_inject *ctx);

static int preserve_signal(sl_inject *ctx, int sig, const struct ptrace_lwpinfo *info)
{
    if (!sig || sig == SIGSTOP || sig == SIGCONT) {
        ctx->unclassified_signal = 0;
        return 1; // our stop/resume control signals
    }
    if (!(info->pl_flags & PL_FLAG_SI)) {
        ctx->unclassified_signal = sig; return -1;
    }
    // A failed/missing lookup is retryable while this original stop remains collected.
    ctx->unclassified_signal = 0;
    int code = info->pl_siginfo.si_code;
    int user = code == SI_USER || code == SI_QUEUE || code == SI_TIMER || code == SI_ASYNCIO ||
               code == SI_MESGQ || code == SI_LWP;
    int fault = sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE || sig == SIGTRAP;
    int payload = code == SI_QUEUE || code == SI_TIMER || code == SI_ASYNCIO || code == SI_MESGQ ||
                  (!user && !fault);
    if (payload) {
        // Preserve the original kernel siginfo by detaching with this same stop signal.
        // No further remote calls may consume that stop before final restoration/detach.
        ctx->detach_signal = sig; ctx->detach_tid = info->pl_lwpid; return 1;
    }
    if (fault && !user) {
        if (injected_registers(ctx) && info->pl_lwpid == ctx->thread_pid) return 0;
        ctx->detach_signal = sig; ctx->detach_tid = info->pl_lwpid; return 1;
    }
    if (ctx->deferred_count == 32) { ctx->unclassified_signal = sig; return -1; }
    ctx->deferred[ctx->deferred_count].tid = info->pl_lwpid;
    ctx->deferred[ctx->deferred_count++].signal = sig;
    return 1;
}

static int acquire_stop(sl_inject *ctx, int need_boundary);
static int acquire_boundary(sl_inject *ctx) { return acquire_stop(ctx, 1); }

static int continue_target(sl_inject *ctx, int signal)
{
    ctx->running = 1;
    int rc = inj_ptrace(PT_CONTINUE, ctx->target_pid, (void *)1, signal);
    if (rc && !ptrace_applied) ctx->running = 0;
    else ctx->boundary_ready = ctx->stop_classified = 0;
    return rc;
}

/* Poll waitpid until the target is stopped by a signal, or SL_WAIT_STOP_MS
 * elapses. Returns 0 if a stop was observed, -1 on timeout or wait error. */
static int wait_for_stop(sl_inject *ctx)
{
    pid_t pid = ctx->target_pid;
    int status;
    for (int waited = 0; waited < SL_WAIT_STOP_MS; waited += SL_WAIT_STEP_MS) {
        pid_t r = waitpid(pid, &status, WNOHANG | WUNTRACED);
        /* FreeBSD WSTOPPED(status): low 8 bits are 0x7f. */
        if (r == pid) {
            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                ctx->dead = 1; ctx->attached = ctx->running = 0; return -1;
            }
            if (WIFSTOPPED(status)) {
                int sig = WSTOPSIG(status);
                ctx->last_stop_signal = sig;
                ctx->boundary_ready = ctx->stop_classified = 0;
                ctx->running = 0; return 0;
            }
            return -1;
        }
        if (r < 0 && errno != EINTR) return -1;
        usleep(SL_WAIT_STEP_MS * 1000);
    }
    return -1;
}

/*
 * Attach to `name` and stop it. Retries EPERM/EBUSY for about 2 s, then
 * records a scratch area just below the stopped RIP.
 */
static FILE *g_map_dump;

void sl_inject_set_map_dump(FILE *out)
{
    g_map_dump = out;
}

#define SL_MAP_DUMP_SPAN 0x200000
#define SL_MAP_DUMP_STEP 0x4000

static void dump_map(sl_inject *ctx, uint64_t center, uint64_t rsp)
{
    if (!g_map_dump) return;
    FILE *out = g_map_dump;
    g_map_dump = NULL;

    fprintf(out, "map dump rip=0x%llx rsp=0x%llx pid=%d\n",
            (unsigned long long)center, (unsigned long long)rsp, ctx->target_pid);
    uint64_t lo = center > SL_MAP_DUMP_SPAN ? center - SL_MAP_DUMP_SPAN : 0x1000;
    uint64_t hi = center + SL_MAP_DUMP_SPAN;
    for (uint64_t a = lo; a < hi; a += SL_MAP_DUMP_STEP) {
        int p = kernel_get_vmem_protection(ctx->target_pid, (intptr_t)a, SL_MAP_DUMP_STEP);
        if (p >= 0) fprintf(out, "  0x%llx prot=0x%x\n", (unsigned long long)a, (unsigned)p);
    }
    fflush(out);
}

#define SL_SCRATCH_PROT_RW 0x80000002u
#define SL_SCRATCH_WINDOW  0x4000

static int prot_is_rw(int prot)
{
    return prot >= 0 && ((uint32_t)prot & SL_SCRATCH_PROT_RW) == 2;
}

/*
 * Pick a 16 KB writable window for the trap and the call: rip - 0x4000 first.
 * If that page isn't mapped writable, scan outward from RIP in 16 KB steps for
 * the nearest writable window.
 */
static int pick_scratch(sl_inject *ctx, uint64_t rip_page, intptr_t *out)
{
    int prot = -1;
    if (rip_page > SL_SCRATCH_WINDOW) {
        prot = kernel_get_vmem_protection(ctx->target_pid,
                                          (intptr_t)((rip_page - SL_SCRATCH_WINDOW) & ~(uint64_t)(SL_SCRATCH_WINDOW - 1)), SL_SCRATCH_WINDOW);
        if (prot_is_rw(prot)) {
            *out = (intptr_t)((rip_page - SL_SCRATCH_WINDOW) & ~(uint64_t)(SL_SCRATCH_WINDOW - 1));
            ctx->last_prot = prot;
            return 0;
        }
    }

    for (uint64_t d = SL_SCRATCH_WINDOW; d <= SL_MAP_DUMP_SPAN; d += SL_SCRATCH_WINDOW) {
        for (int dir = -1; dir <= 1; dir += 2) {
            if (dir < 0 && rip_page < d) continue;
            uint64_t a = dir < 0 ? rip_page - d : rip_page + d;
            a &= ~(uint64_t)(SL_SCRATCH_WINDOW - 1);
            prot = kernel_get_vmem_protection(ctx->target_pid, (intptr_t)a, SL_SCRATCH_WINDOW);
            if (prot_is_rw(prot)) {
                *out = (intptr_t)a;
                ctx->last_prot = prot;
                return 0;
            }
        }
    }
    ctx->last_prot = prot;
    return -1;
}

/* Attach errors worth retrying. On FreeBSD/PS5, PT_ATTACH can fail with
 * EBUSY (another tracer is attaching) or EEXIST (the target is already
 * being traced) during normal rapid-restart windows; both clear in a few
 * tens of ms. */
#define SL_ATTACH_EEXIST 17
#define SL_ATTACH_EBUSY  16

int sl_inject_begin(const char *name, sl_inject *ctx)
{
    if (sl_broker_client()) return broker_status(sl_broker_request(ctx, SL_RPC_BEGIN, 0, NULL, 0, NULL, 0, name));
    int did_attach = 0;
    memset(ctx, 0, sizeof(*ctx));
    atomic_init(&ctx->restore_pending, 0);
    ctx->stage = "find-pid";
    ctx->target_pid = sl_find_pid(name);
    if (ctx->target_pid <= 0) return -1;

    ctx->stage = "attach";
    ctx->stop_pending = 1; // PT_ATTACH itself posts SIGSTOP
    long rc = inj_ptrace(PT_ATTACH, ctx->target_pid, NULL, 0);
    ctx->last_errno = errno;
    if (rc != 0 && ptrace_applied) {
        ctx->attached = ctx->running = did_attach = 1;
        goto fail;
    }
    if (rc != 0) {
        for (int tries = 199; ; tries--) {
            if (ctx->last_errno != SL_ATTACH_EEXIST && ctx->last_errno != SL_ATTACH_EBUSY) goto fail;
            if (tries == 0) goto fail;
            usleep(10000);
            rc = inj_ptrace(PT_ATTACH, ctx->target_pid, NULL, 0);
            ctx->last_errno = errno;
            if (rc != 0 && ptrace_applied) { ctx->attached = ctx->running = did_attach = 1; goto fail; }
            if (rc == 0) break;
        }
    }

    did_attach = 1;
    ctx->attached = 1;
    ctx->stage = "wait-stop";
    ctx->running = 1;
    if (wait_for_stop(ctx) != 0) goto fail;

    ctx->stage = "pin-lwp";
    struct ptrace_lwpinfo info;
    if (stopped_lwp(ctx, &info)) {
        snprintf(stop_diag, sizeof stop_diag, "pin-lwpinfo e%d lwp%d pid%d f%x sig%d", errno, info.pl_lwpid,
                 ctx->target_pid, info.pl_flags, ctx->last_stop_signal);
        ctx->stage = stop_diag; goto fail;
    }
    ctx->thread_pid = info.pl_lwpid;
    if (acquire_boundary(ctx)) goto fail;
    if (ctx->deferred_count || ctx->detach_signal) {
        snprintf(stop_diag, sizeof stop_diag, "pin-signal deferred%d detach%d", ctx->deferred_count,
                 ctx->detach_signal);
        ctx->stage = stop_diag; goto fail;
    }
    ctx->stage = "get-regs";
    struct reg regs;
    if (inj_ptrace(PT_GETREGS, ctx->thread_pid, &regs, 0) != 0) goto fail;

    ctx->stage = "scratch-prot";
    uint64_t rip_page = regs.r_rip & ~0xfffULL;
    ctx->last_rip = regs.r_rip;
    ctx->scratch_len = 0x4000;
    if (pick_scratch(ctx, rip_page, &ctx->scratch) != 0) {
        ctx->last_errno = errno;
        dump_map(ctx, rip_page, regs.r_rsp);
        goto fail;
    }

    ctx->attached = 1;
    ctx->running = 0;
    ctx->stage = "save-scratch";
    ctx->scratch_saved = 1;
    ctx->stage = "attached";
    return 0;

fail:
    ctx->last_errno = errno;
    if (did_attach) {
        const char *failed = ctx->stage; // the capture log names the step that failed, not cleanup's last
        if (sl_inject_end(ctx) != 0) {
            ctx->stage = "attach-cleanup-failed";
            return -1;
        }
        ctx->stage = failed;
    }
    ctx->attached = 0;
    ctx->running = 0;
    return -1;
}

int sl_inject_ensure_stopped(sl_inject *ctx)
{
    if (sl_broker_client()) return broker_status(sl_broker_request(ctx, SL_RPC_STOP, 0, NULL, 0, NULL, 0, NULL));
    if (!ctx || !ctx->attached) return -1;
    return acquire_boundary(ctx);
}

int sl_inject_read_at(sl_inject *ctx, intptr_t addr, void *buf, size_t len)
{
    if (sl_broker_client()) return broker_status(sl_broker_request(ctx, SL_RPC_READ, addr, buf, len, NULL, 0, NULL));
    if (!ctx || !ctx->attached) return -1;
    if (sl_inject_ensure_stopped(ctx) != 0) return -1;

    struct ptrace_io_desc io = {
        .piod_op = PIOD_READ_D,
        .piod_offs = (void *)addr,
        .piod_addr = buf,
        .piod_len = len,
    };
    if (inj_ptrace(PT_IO, tracee(ctx), &io, 0) != 0) return -1;
    return io.piod_len == len ? 0 : -1;
}

int sl_inject_read_code_at(sl_inject *ctx, intptr_t addr, void *buf, size_t len)
{
    if (sl_broker_client()) return broker_status(sl_broker_request(ctx, SL_RPC_CODE, addr, buf, len, NULL, 0, NULL));
    if (!ctx || !ctx->attached || sl_inject_ensure_stopped(ctx) != 0) return -1;
    struct ptrace_io_desc io = {
        .piod_op = PIOD_READ_I, .piod_offs = (void *)addr,
        .piod_addr = buf, .piod_len = len,
    };
    if (inj_ptrace(PT_IO, tracee(ctx), &io, 0) != 0) return -1;
    return io.piod_len == len ? 0 : -1;
}

int sl_inject_write_at(sl_inject *ctx, intptr_t addr, const void *buf, size_t len)
{
    if (sl_broker_client()) return broker_status(sl_broker_request(ctx, SL_RPC_WRITE, addr, (void *)buf, len, NULL, 0, NULL));
    if (!ctx || !ctx->attached) return -1;
    if (sl_inject_ensure_stopped(ctx) != 0) return -1;

    if (addr < ctx->scratch || (uintptr_t)(addr - ctx->scratch) > 0x4000 ||
        len > 0x4000 - (uintptr_t)(addr - ctx->scratch) || !ctx->scratch_saved) return -1;
    size_t begin = (size_t)(addr - ctx->scratch), end = begin + len;
    for (size_t i = begin; i < end;) {
        if (ctx->scratch_owned[i / 8] & (1u << (i % 8))) { i++; continue; }
        size_t first = i++;
        while (i < end && !(ctx->scratch_owned[i / 8] & (1u << (i % 8)))) i++;
        if (sl_inject_read_at(ctx, ctx->scratch + (intptr_t)first, ctx->scratch_backup + first, i - first)) return -1;
        for (size_t j = first; j < i; j++) ctx->scratch_owned[j / 8] |= (uint8_t)(1u << (j % 8));
    }
    if (len) {
        ctx->scratch_dirty = 1;
        atomic_store_explicit(&ctx->restore_pending, 1, memory_order_release);
    }
    struct ptrace_io_desc io = {
        .piod_op = PIOD_WRITE_D,
        .piod_offs = (void *)addr,
        .piod_addr = (void *)buf,
        .piod_len = len,
    };
    if (inj_ptrace(PT_IO, tracee(ctx), &io, 0) != 0) return -1;
    return io.piod_len == len ? 0 : -1;
}

static int io_xfer(int op, pid_t pid, intptr_t addr, void *buf, size_t len)
{
    struct ptrace_io_desc io = {
        .piod_op = op,
        .piod_offs = (void *)addr,
        .piod_addr = buf,
        .piod_len = len,
    };
    if (inj_ptrace(PT_IO, pid, &io, 0) != 0) return -1;
    return io.piod_len == len ? 0 : -1;
}

/*
 * Wait up to timeout_ms for a stop. Returns 0 and sets *sig on stop,
 * 1 if the target is still running at the deadline, -1 on wait error.
 */
static int wait_stop_sig(sl_inject *ctx, int timeout_ms, int *sig)
{
    pid_t pid = ctx->target_pid;
    /* Poll finely at first: most calls (ReadAudio, or ReadVideo with a frame
     * ready) finish within microseconds, and a flat 5 ms sleep added up to
     * 5 ms to each of the several calls made per video frame. Calls that
     * block (ReadVideo waiting for the encoder) are still seen within 0.1 ms
     * for a frame time; after that, back off. */
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG | WUNTRACED);
        if (r == pid) {
            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                ctx->dead = 1; ctx->attached = ctx->running = 0; return -1;
            }
            if (!WIFSTOPPED(status)) return -1;
            *sig = WSTOPSIG(status);
            ctx->last_stop_signal = *sig; ctx->running = 0;
            ctx->boundary_ready = ctx->stop_classified = 0;
            return 0;
        }
        if (r < 0 && errno != EINTR) return -1;
        clock_gettime(CLOCK_MONOTONIC, &t);
        int64_t us = (int64_t)(t.tv_sec - t0.tv_sec) * 1000000 + (t.tv_nsec - t0.tv_nsec) / 1000;
        if (us >= (int64_t)timeout_ms * 1000) return 1;
        usleep(us < 20000 ? 100 : us < 100000 ? 1000 : 5000);
    }
}

// A process stop is insufficient: the pinned LWP may still be inside an injected syscall.
// Its own stop may still be inside sleepq/cursig during a syscall. Do not borrow/restore
// registers while RIP is immediately after SYSCALL (or legacy INT 0x80); let the syscall
// finish, then interrupt again. A bounded failure retains the tracer and journal.
static int acquire_stop(sl_inject *ctx, int need_boundary)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    uint64_t deadline = (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000 + 2000;
    int requested = 0;
    for (;;) {
        if (ctx->dead) return -1;
        clock_gettime(CLOCK_MONOTONIC, &t);
        if ((uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000 >= deadline) return -1;
        if (!ctx->running) {
            if (ctx->boundary_ready) return 0;
            struct ptrace_lwpinfo info;
            if (stopped_lwp(ctx, &info))
                STOP_FAIL("stop-lwpinfo e%d lwp%d pid%d f%x", errno, info.pl_lwpid, ctx->target_pid, info.pl_flags);
            if (!ctx->thread_pid) ctx->thread_pid = info.pl_lwpid;
            int forward = 0;
            if (!need_boundary || info.pl_lwpid == ctx->thread_pid) {
                if (!ctx->stop_classified) {
                    if (preserve_signal(ctx, ctx->last_stop_signal, &info) < 0)
                        STOP_FAIL("stop-signal %d f%x code%d", ctx->last_stop_signal, info.pl_flags,
                                  info.pl_siginfo.si_code);
                    ctx->stop_classified = 1;
                }
                // With no owned mutations, detach never writes registers or memory. Any
                // collected stop is sufficient; inspecting/resuming A would add risk and
                // could consume the very signal metadata that detach must preserve.
                if (!need_boundary) return 0;
                struct reg regs;
                uint8_t opcode[2];
                if (inj_ptrace(PT_GETREGS, ctx->thread_pid, &regs, 0) || regs.r_rip < 2)
                    STOP_FAIL("stop-regs e%d lwp%d", errno, ctx->thread_pid);
                ctx->last_rip = regs.r_rip;
                struct ptrace_io_desc io = {.piod_op = PIOD_READ_I, .piod_offs = (void *)(regs.r_rip - 2),
                    .piod_addr = opcode, .piod_len = sizeof opcode};
                if (inj_ptrace(PT_IO, ctx->target_pid, &io, 0) || io.piod_len != sizeof opcode) {
                    int e = errno;
                    // Console 2026-11-09 (g77555b0): this read fails at SceRemotePlay's stopped
                    // RIP (system code is execute-only), so the opcode check can never pass there.
                    // With none of our registers installed, borrow the stop unchecked, as the
                    // hardware-tested PoC (Glint) always did. A hung injected call still holds.
                    if (!injected_registers(ctx)) { ctx->boundary_ready = 1; return 0; }
                    STOP_FAIL("stop-code-read e%d len%zu", e, (size_t)io.piod_len);
                }
                if (!((opcode[0] == 0x0f && opcode[1] == 0x05) ||
                      (opcode[0] == 0xcd && opcode[1] == 0x80))) {
                    ctx->boundary_ready = 1; return 0;
                }
                // Consuming a payload stop would lose the original kernel siginfo. Hold
                // instead if a syscall boundary and a payload-bearing signal coincide.
                if (ctx->detach_signal) { ctx->stage = "signal-syscall-held"; return -1; }
                snprintf(stop_diag, sizeof stop_diag, "stop-syscall-boundary rip%llx", (unsigned long long)regs.r_rip);
                ctx->stage = stop_diag; // overwritten by success; left only if the deadline ends it
            } else {
                // B is untouched. Forward its original signal in-kernel, preserving siginfo.
                forward = ctx->last_stop_signal;
                if (forward == SIGSTOP || forward == SIGCONT) forward = 0;
            }
            if (continue_target(ctx, forward)) return -1;
            // Post only AFTER resume: while P_TRACED is stopped a signal merely queues
            // and does not interrupt A's sleep. Recheck RIP even if A itself reports.
            requested = 0;
        }
        if (!requested) {
            // An unconsumed stop may already exist; observe it before posting another stop.
            int sig = 0;
            int observed = wait_stop_sig(ctx, 0, &sig);
            if (observed < 0) return -1;
            if (!observed) continue;
            ctx->stop_pending = 1;
            if (thr_kill2(ctx->target_pid, ctx->thread_pid, SIGSTOP)) return -1;
            requested = 1;
        }
        clock_gettime(CLOCK_MONOTONIC, &t);
        uint64_t now = (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
        if (now >= deadline) return -1;
        int sig = 0;
        if (wait_stop_sig(ctx, (int)(deadline - now), &sig) < 0) return -1;
        // A timed-out wait leaves running set. Keep ownership and retry at the outer recovery.
        if (ctx->running) return -1;
    }
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

#define SL_CALL_TIMEOUT_MS 1000
#define SL_STOP_WAIT_MS 2000
#define SL_CALL_MAX_RESTARTS 4
#define SL_SIGTRAP 5
#define SL_SCRATCH_TRAP_OFF 0xf00
#define SL_SCRATCH_REGION 0x4000
#define SL_CALL_STACK_RESERVE 0x208

/*
 * The restoration journal: everything of ours that is installed in the target during a call
 * (registers, FP state, return slot, trap byte, page protection). It lives in the context, not
 * on inj_call_fast's stack, so that a call that could not finish cleanly is still restored later
 * (sl_inject_end), and so that nothing detaches the target while it is installed. Each part has
 * its own "still installed" flag, cleared only when its restoration is confirmed.
 */
struct journal {
    int valid;
    int regs_dirty, trap_dirty, stack_dirty, prot_dirty;
    pid_t pid, tid;
    struct reg saved;
    struct fpreg saved_fp;
    int have_fp;
    uint32_t xstate_len;
    uint64_t xstate_mask;
    _Alignas(64) uint8_t xstate[4096];
    uint64_t old_word;
    intptr_t call_sp, trap, region;
    int prot;
    uint8_t old_trap_byte;
};
_Static_assert(sizeof(struct journal) <= sizeof(((sl_inject *)0)->journal_mem), "journal does not fit the context");

static void journal_load(sl_inject *ctx, struct journal *j)
{
    (void)atomic_load_explicit(&ctx->restore_pending, memory_order_acquire);
    memcpy(j, ctx->journal_mem, sizeof *j);
}

static int injected_registers(sl_inject *ctx)
{
    struct journal j; journal_load(ctx, &j); return j.regs_dirty;
}

static void journal_publish(sl_inject *ctx, struct journal *j)
{
    j->valid = j->regs_dirty || j->trap_dirty || j->stack_dirty || j->prot_dirty;
    memcpy(ctx->journal_mem, j, sizeof *j);
    atomic_store_explicit(&ctx->restore_pending, j->valid || ctx->scratch_dirty, memory_order_release);
}

static int restore_bytes(pid_t pid, intptr_t at, const void *original, size_t n)
{
    unsigned char check[0x4000];
    if (n > sizeof check || io_xfer(PIOD_WRITE_D, pid, at, (void *)original, n) ||
        io_xfer(PIOD_READ_D, pid, at, check, n)) return -1;
    return memcmp(check, original, n) ? -1 : 0;
}

/* Trap diagnostics and FXSAVE reserved bytes are kernel bookkeeping, not writable CPU
 * state. Compare named architectural registers and x87/SSE payloads, never struct padding. */
static int regs_equal(const struct reg *a, const struct reg *b)
{
#define SAME(field) if (a->field != b->field) return 0
    SAME(r_r15); SAME(r_r14); SAME(r_r13); SAME(r_r12); SAME(r_r11); SAME(r_r10);
    SAME(r_r9); SAME(r_r8); SAME(r_rdi); SAME(r_rsi); SAME(r_rbp); SAME(r_rbx);
    SAME(r_rdx); SAME(r_rcx); SAME(r_rax); SAME(r_fs); SAME(r_gs); SAME(r_es); SAME(r_ds);
    SAME(r_rip); SAME(r_cs); SAME(r_rflags); SAME(r_rsp); SAME(r_ss);
#undef SAME
    return 1;
}
static int fp_equal(const struct fpreg *a, const struct fpreg *b)
{
    const uint8_t *x = (const void *)a->fpr_env, *y = (const void *)b->fpr_env;
    for (unsigned i = 0; i < 28; i++) if (i != 5 && x[i] != y[i]) return 0;
    for (unsigned i = 0; i < 8; i++) if (memcmp(a->fpr_acc[i], b->fpr_acc[i], 10)) return 0;
    return !memcmp(a->fpr_xacc, b->fpr_xacc, sizeof a->fpr_xacc);
}

/* The target is stopped. Every flag survives failed or silently ineffective restoration. */
static int journal_restore(sl_inject *ctx)
{
    struct journal copy; journal_load(ctx, &copy);
    struct journal *j = &copy;
    if (j->trap_dirty) {
        ctx->stage = "restore-trap";
        if (!restore_bytes(j->pid, j->trap, &j->old_trap_byte, 1)) j->trap_dirty = 0;
        journal_publish(ctx, j);
    }
    if (j->stack_dirty) {
        ctx->stage = "restore-stack";
        if (!restore_bytes(j->pid, j->call_sp, &j->old_word, sizeof j->old_word)) j->stack_dirty = 0;
        journal_publish(ctx, j);
    }
    if (j->regs_dirty) {
        ctx->stage = "restore-regs";
        struct reg check;
        int ok = inj_ptrace(PT_SETREGS, j->tid, &j->saved, 0) == 0 &&
                 inj_ptrace(PT_GETREGS, j->tid, &check, 0) == 0 && regs_equal(&check, &j->saved);
        if (ok && j->have_fp) {
            struct fpreg fp;
            _Alignas(64) uint8_t state[4096];
            uint64_t expected, actual;
            ok = inj_ptrace(PT_SETXSTATE, j->tid, j->xstate, (int)j->xstate_len) == 0 &&
                 inj_ptrace(PT_GETFPREGS, j->tid, &fp, 0) == 0 && fp_equal(&fp, &j->saved_fp) &&
                 inj_ptrace(PT_GETXSTATE, j->tid, state, 0) == 0;
            if (ok) {
                memcpy(&expected, j->xstate + 512, 8); memcpy(&actual, state + 512, 8);
                uint64_t format; memcpy(&format, state + 520, 8);
                ok = !format && !(actual & ~j->xstate_mask);
                // An inactive XSAVE component means architectural zero, even if its backing
                // bytes contain stale XSAVEOPT data. Compare AVX state, not unused storage.
                if (j->xstate_mask & 4) for (unsigned i = 0; ok && i < 256; i++) {
                    uint8_t a = actual & 4 ? state[576 + i] : 0;
                    uint8_t e = expected & 4 ? j->xstate[576 + i] : 0;
                    if (a != e) ok = 0;
                }
            }
        }
        if (ok) j->regs_dirty = 0;
        journal_publish(ctx, j);
    }
    if (j->prot_dirty) {
        ctx->stage = "restore-protection";
        if (kernel_set_vmem_protection(j->pid, j->region, SL_SCRATCH_REGION, j->prot) == 0 &&
            kernel_get_vmem_protection(j->pid, j->region, SL_SCRATCH_REGION) == j->prot) j->prot_dirty = 0;
        journal_publish(ctx, j);
    }
    return j->valid ? -1 : 0;
}

/* A stopped target again (SIGSTOP and wait). 0 when it is. */
static int stop_target(sl_inject *ctx)
{
    return sl_inject_ensure_stopped(ctx); // first detect an already-stopped target, then signal/wait
}

/*
 * Remote call. Writes an int3 at scratch+0xf00, points the target's return address at it, runs
 * fn, and waits for the trap. Afterwards restores, from the journal, the trap byte, return slot,
 * registers, optional FP state and page protection. If the target cannot be stopped again the
 * call is quarantined and the journal stays pending: sl_inject_end finishes the restoration
 * before it ever detaches.
 */
int64_t inj_call_fast(sl_inject *ctx, intptr_t fn, const uint64_t args[6], int save_fp)
{
    if (sl_broker_client()) return sl_broker_request(ctx, SL_RPC_CALL, fn, NULL, 0, args, save_fp, NULL);
    if (!ctx || ctx->quarantined || ctx->detach_signal || ctx->unclassified_signal || !fn) return SL_CALL_ERROR;
    ctx->aborted_restored = 0;

    ctx->stage = "ensure-stopped";
    int deferred_before = ctx->deferred_count;
    if (sl_inject_ensure_stopped(ctx) != 0) return SL_CALL_ERROR;
    if (ctx->detach_signal || ctx->unclassified_signal || ctx->deferred_count != deferred_before) return SL_CALL_ERROR;

    pid_t pid = ctx->target_pid;
    pid_t tid = tracee(ctx);
    struct journal previous; journal_load(ctx, &previous);
    if (previous.valid) return SL_CALL_ERROR;
    struct journal state = {0};
    struct journal *j = &state;
    j->pid = pid, j->tid = tid;
    j->trap = ctx->scratch + SL_SCRATCH_TRAP_OFF;
    j->region = j->trap & ~(intptr_t)(SL_SCRATCH_REGION - 1);

    j->prot = kernel_get_vmem_protection(pid, j->region, SL_SCRATCH_REGION);
    if (j->prot < 0) return SL_CALL_ERROR;

    ctx->stage = "get-regs";
    if (inj_ptrace(PT_GETREGS, tid, &j->saved, 0) != 0) return SL_CALL_ERROR;

    if (save_fp) {
        ctx->stage = "get-xstate-info";
        struct ptrace_xstate_info info;
        if (inj_ptrace(PT_GETXSTATE_INFO, tid, &info, sizeof info) || info.xsave_len < 576 ||
            info.xsave_len > sizeof j->xstate || (info.xsave_mask & 3) != 3 || (info.xsave_mask & ~7ull) ||
            ((info.xsave_mask & 4) && info.xsave_len < 832)) return SL_CALL_ERROR;
        j->xstate_len = info.xsave_len; j->xstate_mask = info.xsave_mask;
        if (inj_ptrace(PT_GETXSTATE, tid, j->xstate, 0)) return SL_CALL_ERROR;
        uint64_t format; memcpy(&format, j->xstate + 520, 8);
        if (format) return SL_CALL_ERROR; // compacted/future layouts are not interpreted here
        ctx->stage = "get-fpregs";
        if (inj_ptrace(PT_GETFPREGS, tid, &j->saved_fp, 0) != 0) return SL_CALL_ERROR;
        j->have_fp = 1;
    }

    j->call_sp = (intptr_t)((j->saved.r_rsp & ~0xfULL) - SL_CALL_STACK_RESERVE);

    ctx->stage = "read-stack";
    if (io_xfer(PIOD_READ_D, pid, j->call_sp, &j->old_word, sizeof(j->old_word)) != 0) return SL_CALL_ERROR;
    ctx->stage = "read-trap";
    if (io_xfer(PIOD_READ_D, pid, j->trap, &j->old_trap_byte, 1) != 0) return SL_CALL_ERROR;

    /* From here on something of ours may be installed: every step is journalled before it is tried. */
    int64_t result = SL_CALL_ERROR;
    int ok = 0;
    uint8_t int3 = 0xcc;
    ctx->stage = "protect";
    j->prot_dirty = 1;
    journal_publish(ctx, j);
    if (kernel_set_vmem_protection(pid, j->region, SL_SCRATCH_REGION, j->prot | PROT_EXEC) != 0) goto finish;

    ctx->stage = "write-trap";
    j->trap_dirty = 1;
    journal_publish(ctx, j);
    if (io_xfer(PIOD_WRITE_D, pid, j->trap, &int3, 1) != 0) goto finish;
    uint64_t ret_addr = (uint64_t)j->trap;
    j->stack_dirty = 1;
    journal_publish(ctx, j);
    if (io_xfer(PIOD_WRITE_D, pid, j->call_sp, &ret_addr, sizeof(ret_addr)) != 0) goto finish;

    struct reg call = j->saved;
    call.r_rip = (int64_t)fn;
    call.r_rsp = (int64_t)j->call_sp;
    call.r_rdi = (int64_t)args[0];
    call.r_rsi = (int64_t)args[1];
    call.r_rdx = (int64_t)args[2];
    call.r_rcx = (int64_t)args[3];
    call.r_r8  = (int64_t)args[4];
    call.r_r9  = (int64_t)args[5];
    call.r_rax = 0;

    ctx->stage = "set-regs";
    j->regs_dirty = 1;
    journal_publish(ctx, j);
    if (inj_ptrace(PT_SETREGS, tid, &call, 0) != 0) goto finish;

    ctx->stage = "continue";
    if (continue_target(ctx, 0) != 0) goto finish;

    ctx->stage = "wait-trap";
    int64_t deadline = now_ms() + SL_CALL_TIMEOUT_MS;
    int kicked = 0;
    int restarts = 0;
    struct reg cur;

    while (now_ms() < deadline) {
        int sig = 0;
        int remaining = (int)(deadline - now_ms());
        if (remaining > 250) remaining = 250;

        int w = wait_stop_sig(ctx, remaining, &sig);
        if (w < 0) break;
        if (w > 0) {
            if (!kicked) {
                kill(pid, SIGCONT);
                kicked = 1;
                ctx->stall_count++;
            }
            continue;
        }

        ctx->running = 0;
        if (inj_ptrace(PT_GETREGS, tid, &cur, 0) != 0) break;

        struct ptrace_lwpinfo stopped;
        if (stopped_lwp(ctx, &stopped)) break;
        if (sig == SL_SIGTRAP && stopped.pl_lwpid == j->tid && cur.r_rip == (int64_t)j->trap + 1) {
            result = cur.r_rax;
            ok = 1;
            ctx->boundary_ready = 1; // A sits on our own trap, not in a syscall: no code read needed
            break;
        }
        if (restarts >= SL_CALL_MAX_RESTARTS) break;

        // An unexpected stop may be a bad media call. Restore the borrowed
        // state instead of delivering a fault to this system process.
        if (sig != SIGSTOP && sig != SIGCONT) {
            // An asynchronous signal belongs to the original thread context. Restore and
            // end this capture attempt, then requeue it before detach instead of running its
            // handler in the borrowed call or delaying it for an open-ended capture session.
            break;
        }
        restarts++;
        if (continue_target(ctx, 0) != 0) break;
    }

finish:
    ctx->stage = "timeout-stop";
    if (ctx->dead || stop_target(ctx) != 0) {
        /* Still running inside our call, or unreachable: nothing can be restored now. The journal
         * stays pending, the call is quarantined, and sl_inject_end keeps trying before it detaches. */
        ctx->aborted_restored = 0;
        ctx->quarantined = 1;
        return SL_CALL_ERROR;
    }
    const int restored = journal_restore(ctx) == 0;
    ctx->stage = "complete";
    if (!ok || !restored) {
        ctx->aborted_restored = restored && !ctx->unclassified_signal && !ctx->detach_signal;
        ctx->quarantined = 1;
        return SL_CALL_ERROR;
    }
    return result;
}

/* Every owned mutation must be restored and read back before detach. Any failure retains
 * the tracer and the stopped target; the supervisor retries, including after worker death. */
int sl_inject_end(sl_inject *ctx)
{
    if (sl_broker_client()) return broker_status(sl_broker_request(ctx, SL_RPC_END, 0, NULL, 0, NULL, 0, NULL));
    if (restore_credentials()) { if (ctx) ctx->stage = "restore-credentials"; return SL_END_HELD; }
    if (ctx && ctx->dead) return SL_END_GONE;
    if (!ctx || !ctx->attached) return 0;
    int status = 0;
    pid_t observed = waitpid(ctx->target_pid, &status, WNOHANG | WUNTRACED);
    if (observed == ctx->target_pid && (WIFEXITED(status) || WIFSIGNALED(status))) {
        ctx->dead = 1; ctx->attached = ctx->running = 0; return SL_END_GONE;
    }
    if (observed == ctx->target_pid && WIFSTOPPED(status)) {
        ctx->running = 0; ctx->last_stop_signal = WSTOPSIG(status);
        ctx->boundary_ready = ctx->stop_classified = 0;
    }
    struct journal pending; journal_load(ctx, &pending);
    int need_boundary = pending.valid || ctx->scratch_dirty || atomic_load(&ctx->restore_pending);
    if (acquire_stop(ctx, need_boundary)) { ctx->stage = "end-stop-failed"; return ctx->dead ? SL_END_GONE : SL_END_HELD; }
    int restored = journal_restore(ctx) == 0;
    if (restored && ctx->scratch_dirty) {
        ctx->stage = "restore-scratch";
        if (!ctx->scratch_saved) return SL_END_HELD;
        for (size_t i = 0; i < 0x4000;) {
            if (!(ctx->scratch_owned[i / 8] & (1u << (i % 8)))) { i++; continue; }
            size_t first = i++;
            while (i < 0x4000 && (ctx->scratch_owned[i / 8] & (1u << (i % 8)))) i++;
            if (restore_bytes(ctx->target_pid, ctx->scratch + (intptr_t)first, ctx->scratch_backup + first, i - first))
                return SL_END_HELD;
            for (size_t j = first; j < i; j++) ctx->scratch_owned[j / 8] &= (uint8_t)~(1u << (j % 8));
        }
        ctx->scratch_dirty = 0;
        struct journal j; journal_load(ctx, &j); journal_publish(ctx, &j);
    }
    if (ctx->unclassified_signal) { ctx->stage = "unclassified-stop"; return SL_END_HELD; }
    if (!restored || atomic_load_explicit(&ctx->restore_pending, memory_order_acquire))
        return SL_END_HELD;
    ctx->stage = "detach";
    // FreeBSD tdsendsignal(SIGCONT) deletes pending stop signals while P_TRACED keeps
    // the trace stop intact. Clear our raced SIGSTOP BEFORE releasing any thread; merely
    // passing SIGCONT to PT_DETACH would generate it only after threads are unsuspended.
    if (ctx->stop_pending) {
        struct reg check;
        ctx->stage = "clear-owned-stop";
        if (kill(ctx->target_pid, SIGCONT) || inj_ptrace(PT_GETREGS, tracee(ctx), &check, 0))
            return SL_END_HELD;
        ctx->stop_pending = 0;
    }
    while (ctx->deferred_count) {
        int at = 0;
        // Requeue on the original LWP while all threads are still trace-stopped. FreeBSD's
        // thr_kill2 preserves per-thread delivery instead of turning it into a process signal.
        if (thr_kill2(ctx->target_pid, ctx->deferred[at].tid, ctx->deferred[at].signal)) return SL_END_HELD;
        ctx->deferred_count--;
        memmove(ctx->deferred, ctx->deferred + 1, (size_t)ctx->deferred_count * sizeof ctx->deferred[0]);
    }
    if (ctx->detach_signal) {
        struct ptrace_lwpinfo info;
        if (stopped_lwp(ctx, &info) || info.pl_lwpid != ctx->detach_tid ||
            ctx->last_stop_signal != ctx->detach_signal) { ctx->stage = "signal-metadata-held"; return SL_END_HELD; }
    }
    int detach_ok = inj_ptrace(PT_DETACH, ctx->target_pid, (void *)1, ctx->detach_signal) == 0;
    if (!detach_ok && !ptrace_applied) return SL_END_HELD;
    ctx->stop_pending = ctx->detach_signal = 0;
    ctx->attached = ctx->running = 0;
    ctx->thread_pid = 0;
    return detach_ok ? 0 : SL_END_HELD;
}

int64_t sl_inject_cleanup_call(sl_inject *ctx, intptr_t fn, const uint64_t args[6])
{
    if (sl_broker_client()) return sl_broker_request(ctx, SL_RPC_CLEANUP_CALL, fn, NULL, 0, args, 1, NULL);
    if (!ctx || (ctx->quarantined && !ctx->aborted_restored)) return SL_CALL_ERROR;
    int was_aborted = ctx->aborted_restored, quarantine = ctx->quarantined;
    ctx->quarantined = 0;
    int64_t rc = inj_call_fast(ctx, fn, args, 1);
    if (was_aborted && (rc != SL_CALL_ERROR || !ctx->quarantined)) {
        ctx->aborted_restored = 1; ctx->quarantined = quarantine;
    }
    return rc;
}

int64_t sl_inject_call_fast(sl_inject *ctx, intptr_t fn, const uint64_t args[6])
{
    if (!ctx || ctx->quarantined) return SL_CALL_ERROR;
    return inj_call_fast(ctx, fn, args, 0);
}

#endif /* SL_PS5 */
