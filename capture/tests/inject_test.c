// SPDX-License-Identifier: GPL-3.0-only
// The injector's restoration journal (src/inject.c) against a fake tracee: no ptrace, no console.
// The question under test: is the target ever detached with our registers, return slot, trap byte
// or protection still installed? Time is simulated, so the 1 s call and 2 s stop timeouts are free.
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int fake_waitpid(pid_t pid, int *status, int options);
int fake_kill(pid_t pid, int sig);
int fake_usleep(unsigned us);
int fake_clock_gettime(clockid_t id, struct timespec *t);
long fake_syscall(long number, ...);
static int host_kill(pid_t pid, int sig) { return kill(pid, sig); }
#define SL_PS5 1
#define waitpid fake_waitpid
#define kill fake_kill
#define usleep fake_usleep
#define clock_gettime fake_clock_gettime
#define syscall fake_syscall
#include "../src/inject.c"
#undef waitpid
#undef kill
#undef usleep
#undef clock_gettime
#undef syscall
#include "../src/inject_broker.c"

// ---- the fake time ------------------------------------------------------------------------
static int64_t fake_now;
int fake_clock_gettime(clockid_t id, struct timespec *t)
{
    (void)id;
    fake_now += 10;
    t->tv_sec = fake_now / 1000000;
    t->tv_nsec = (fake_now % 1000000) * 1000;
    return 0;
}
int fake_usleep(unsigned us) { fake_now += us; return 0; }

// ---- the fake kernel ----------------------------------------------------------------------
static uint64_t fake_authid = 1;
static uint8_t fake_caps[16];
static int fail_caps_restore, lie_caps_restore, fail_auth_install, credential_failure_after;
uint64_t kernel_get_ucred_authid(pid_t pid) { (void)pid; return fake_authid; }
int kernel_get_ucred_caps(pid_t pid, uint8_t caps[16]) { (void)pid; memcpy(caps, fake_caps, 16); return 0; }
int kernel_set_ucred_authid(pid_t pid, uint64_t a)
{
    (void)pid; fake_authid = a;
    return fail_auth_install && a == SL_PTRACE_AUTHID ? -1 : 0;
}
int kernel_set_ucred_caps(pid_t pid, const uint8_t c[16])
{
    (void)pid;
    if (!c[0] && fail_caps_restore) return -1;
    if (!c[0] && lie_caps_restore) return 0;
    memcpy(fake_caps, c, 16); return 0;
}
int kernel_dynlib_handle(pid_t pid, const char *n, uint32_t *h) { (void)pid, (void)n, (void)h; return -1; }
intptr_t kernel_dynlib_dlsym(pid_t pid, uint32_t h, const char *s) { (void)pid, (void)h, (void)s; return 0; }
intptr_t kernel_dynlib_resolve(pid_t pid, uint32_t h, const char *s) { (void)pid, (void)h, (void)s; return 0; }
intptr_t kernel_dynlib_mapbase_addr(pid_t pid, uint32_t h) { (void)pid, (void)h; return 0; }
int sysctl(const int *a, unsigned b, void *c, size_t *d, const void *e, size_t f)
{
    (void)a, (void)b, (void)e, (void)f;
    if (!c) { *d = 512; return 0; }
    assert(*d >= 512); memset(c, 0, 512);
    int size = 512, pid = 7;
    memcpy(c, &size, 4); memcpy((char *)c + 0x48, &pid, 4);
    memcpy((char *)c + 0x1bf, "SceRemotePlay", 13); *d = 512; return 0;
}

static sl_inject *active_ctx;
static pid_t crash_worker;
static int page_prot = PROT_READ | PROT_WRITE;
int kernel_get_vmem_protection(pid_t pid, intptr_t addr, size_t len) { (void)pid, (void)addr, (void)len; return page_prot; }
static int fail_prot_restore, lie_prot_restore, fail_prot_install;
int kernel_set_vmem_protection(pid_t pid, intptr_t addr, size_t len, int prot)
{
    (void)pid, (void)addr, (void)len;
    if (active_ctx) { struct journal j; journal_load(active_ctx, &j); assert(j.prot_dirty); }
    if (fail_prot_restore && !(prot & PROT_EXEC)) return -1;
    if (lie_prot_restore && !(prot & PROT_EXEC)) return 0;
    if (fail_prot_install && (prot & PROT_EXEC)) { page_prot = prot; return -1; }
    page_prot = prot;
    return 0;
}

// ---- the fake tracee ------------------------------------------------------------------------
enum behavior { RETURNS, HANGS };
static struct {
    int running, detached, attached, waited, stop_status_ready;
    struct reg regs;
    struct fpreg fp;
    enum behavior behavior;
    int stop_works;             // SIGSTOP stops it
    int setregs_calls, fail_setregs_from, fail_setregs_until; // 1-based call numbers, inclusive range
    int fail_trap_write, fail_stack_restore, ignore_regs_restore, fail_fp_restore, ignore_fp_restore;
    int short_restore_read, short_after_continue, fail_scratch_write;
    int fail_detach, altered_install_failure, exits, race_stop_trap, attach_calls;
    int reporting_tid, switch_stop_tid, b_reported, a_in_kernel, targeted_stop;
    int kernel_stop_reports, kernel_reported, syscall_boundary, continue_calls, fail_continue_once, attach_signal;
    int async_tid, async_signal, async_code, async_sent, replayed, forwarded, no_xstate, replay_fail;
    int lwp_lookups, fail_lwp_at, missing_siginfo, code_reads, fail_code_read, immediate_stops;
    int delivered_code, delivered_value;
    struct reg other_regs;
    uint8_t ymm[256];
    int ymm_active;
    int stop_pending, stopped_by_kill;
    struct reg regs_at_detach;
    struct fpreg fp_at_detach;
} t;

static alignas(0x4000) uint8_t scratch_mem[0x8000];
static alignas(16) uint8_t stack_mem[0x1000];
#define original_rip ((int64_t)(scratch_mem + 0x8000 + 0x200))
static const int64_t result_value = 0x1234;

int fake_kill(pid_t pid, int sig)
{
    (void)pid;
    if (sig == SIGSTOP && t.stop_works) t.stop_pending = 1;
    if (sig == SIGCONT) t.stop_pending = 0;
    return 0;
}

int fake_waitpid(pid_t pid, int *status, int options)
{
    (void)pid, (void)options;
    if (t.exits) { t.attached = t.running = 0; *status = 7 << 8; return pid; }
    if (t.stop_status_ready) {
        t.stop_status_ready = 0; t.waited = 1; *status = 0x7f | (SIGSTOP << 8); return pid;
    }
    if (!t.running) return 0;
    if (t.immediate_stops) {
        fake_now += 100000;
        t.running = 0; t.waited = 1; t.reporting_tid = 100007;
        *status = 0x7f | (SIGSTOP << 8); return pid;
    }
    if (t.stop_pending && t.race_stop_trap) {
        t.running = 0; t.waited = 1; t.regs.r_rip = (int64_t)(scratch_mem + 0x4000 + 0xf00) + 1;
        *status = 0x7f | (SIGTRAP << 8); return pid;
    }
    if (t.stop_pending) {
        if (t.switch_stop_tid && !t.b_reported) { t.reporting_tid = 100008; t.b_reported = 1; }
        else {
            t.reporting_tid = t.targeted_stop ? 100007 : t.reporting_tid;
            if (t.a_in_kernel && t.reporting_tid == 100007) {
                if (t.kernel_stop_reports && !t.kernel_reported) {
                    t.kernel_reported = t.syscall_boundary = 1;
                } else {
                    t.regs.r_rax = 0x777; t.regs.r_rflags |= 1; t.a_in_kernel = t.syscall_boundary = 0;
                }
            }
        }
        t.targeted_stop = 0;
        t.stop_pending = 0;
        t.running = 0; t.waited = 1;
        *status = 0x7f | (SIGSTOP << 8);
        return pid;
    }
    if (t.async_signal && !t.async_sent) {
        t.async_sent = 1; t.running = 0; t.waited = 1; t.reporting_tid = t.async_tid;
        *status = 0x7f | (t.async_signal << 8); return pid;
    }
    // The trap reports only while our int3 byte is actually armed: a target with no call in
    // flight cannot stop at the trap slot, so its next stop is our posted SIGSTOP instead.
    if (t.behavior == RETURNS && scratch_mem[0x4000 + 0xf00] == 0xcc) {
        t.reporting_tid = 100007;
        t.running = 0; t.waited = 1;
        t.regs.r_rip = (int64_t)(scratch_mem + 0x4000 + 0xf00) + 1; // the trap, hit
        t.regs.r_rax = result_value;
        *status = 0x7f | (SIGTRAP << 8);
        return pid;
    }
    return 0;
}

long fake_syscall(long number, ...)
{
    assert(number == 26);
    va_list ap;
    va_start(ap, number);
    int req = va_arg(ap, int);
    pid_t pid = va_arg(ap, pid_t);
    void *addr = va_arg(ap, void *);
    int data = va_arg(ap, int);
    va_end(ap);
    (void)pid, (void)data;
    if (req == credential_failure_after) { fail_caps_restore = 1; credential_failure_after = 0; }
    switch (req) {
    case PT_LWPINFO: {
        assert(pid == 7);
        if (++t.lwp_lookups == t.fail_lwp_at) { errno = EIO; return -1; }
        struct ptrace_lwpinfo *i = addr; memset(i, 0, sizeof *i);
        i->pl_lwpid = t.reporting_tid; i->pl_flags = t.missing_siginfo ? 0 : PL_FLAG_SI;
        i->pl_siginfo.si_code = t.async_sent && t.reporting_tid == t.async_tid ? t.async_code : TRAP_BRKPT;
        i->pl_siginfo.si_value.sival_int = 0x334455; return 0;
    }
    case PT_GETXSTATE_INFO: {
        if (t.no_xstate) return -1;
        assert(pid == 100007);
        struct ptrace_xstate_info *i = addr; i->xsave_mask = 7; i->xsave_len = 832; return 0;
    }
    case PT_GETXSTATE:
        assert(pid == 100007);
        memset(addr, 0, 832); memcpy(addr, &t.fp, 512);
        { uint64_t bits = t.ymm_active ? 7 : 3; memcpy((uint8_t *)addr + 512, &bits, 8); }
        memcpy((uint8_t *)addr + 576, t.ymm, sizeof t.ymm); return 0;
    case PT_SETXSTATE:
        assert(pid == 100007 && data == 832);
        if (t.fail_fp_restore) return -1;
        if (!t.ignore_fp_restore) memcpy(&t.fp, addr, sizeof t.fp);
        { uint64_t bits; memcpy(&bits, (uint8_t *)addr + 512, 8); t.ymm_active = !!(bits & 4); }
        if (t.ymm_active) memcpy(t.ymm, (uint8_t *)addr + 576, sizeof t.ymm);
        else memset(t.ymm, 0, sizeof t.ymm);
        return 0;
    case PT_ATTACH:
        t.attach_calls++; t.attached = t.running = 1; t.stop_pending = !t.attach_signal; return 0;
    case PT_GETREGS:
        if (t.running || !t.waited) { errno = EBUSY; return -1; }
        *(struct reg *)addr = pid == 100008 || (pid == 7 && t.reporting_tid == 100008) ? t.other_regs : t.regs;
        return 0;
    case PT_SETREGS:
        assert(pid != 100007 || !t.a_in_kernel); // syscall return must not overwrite restored registers
        if (((struct reg *)addr)->r_rip == 0x600000) {
            struct journal j; journal_load(active_ctx, &j); assert(j.regs_dirty);
        }
        if (t.running || !t.waited) { errno = EBUSY; return -1; }
        t.setregs_calls++;
        if (t.setregs_calls >= t.fail_setregs_from && t.setregs_calls <= t.fail_setregs_until) {
            if (t.altered_install_failure) t.regs = *(struct reg *)addr;
            errno = EIO; return -1;
        }
        if (t.ignore_regs_restore && t.setregs_calls >= 2) return 0;
        if (pid == 100008 || (pid == 7 && t.reporting_tid == 100008)) t.other_regs = *(struct reg *)addr;
        else t.regs = *(struct reg *)addr;
        return 0;
    case PT_GETFPREGS:
        if (t.running || !t.waited) { errno = EBUSY; return -1; }
        *(struct fpreg *)addr = t.fp;
        return 0;
    case PT_SETFPREGS:
        if (t.fail_fp_restore) return -1;
        if (t.ignore_fp_restore) return 0;
        if (t.running || !t.waited) { errno = EBUSY; return -1; }
        t.fp = *(struct fpreg *)addr;
        return 0;
    case PT_CONTINUE:
        t.continue_calls++;
        if (t.fail_continue_once) { t.fail_continue_once = 0; errno = EIO; return -1; }
        if (data && t.reporting_tid == 100008) { assert(data == t.async_signal); t.forwarded++; }
        if (t.switch_stop_tid) t.a_in_kernel = 1;
        if (t.short_after_continue) t.short_restore_read = 1;
        t.ymm_active = 1;
        memset(t.ymm, 0xa5, sizeof t.ymm);
        memset(&t.fp, 0xa5, sizeof t.fp);
        t.running = 1; t.waited = 0;
        if (crash_worker > 0) { assert(!host_kill(crash_worker, SIGKILL)); crash_worker = 0; }
        return 0;
    case PT_DETACH:
        if (t.fail_detach) return -1;
        assert(data == 0 || data == t.async_signal);
        if (data) {
            t.replayed++; // original siginfo retained by PT_DETACH, not synthetic thr_kill2
            t.delivered_code = t.async_code; t.delivered_value = 0x334455;
        }
        // Pending stops must already be cleared before detach.
        assert(!t.stop_pending);
        t.detached = 1;
        t.attached = 0;
        t.regs_at_detach = t.regs;
        t.fp_at_detach = t.fp;
        return 0;
    case PT_IO: {
        struct ptrace_io_desc *io = addr;
        if (t.running || !t.waited) { errno = EBUSY; return -1; }
        if (io->piod_op == PIOD_READ_I) {
            t.code_reads++;
            if (t.fail_code_read) { errno = EIO; return -1; }
            memset(io->piod_addr, 0x90, io->piod_len);
            if (t.syscall_boundary && io->piod_len == 2 && (intptr_t)io->piod_offs == t.regs.r_rip - 2) {
                ((uint8_t *)io->piod_addr)[0] = 0x0f; ((uint8_t *)io->piod_addr)[1] = 0x05;
            }
            return 0;
        }
        if (io->piod_op == PIOD_WRITE_D && io->piod_len == 1 && *(uint8_t *)io->piod_addr == 0xcc) {
            struct journal j; journal_load(active_ctx, &j); assert(j.trap_dirty);
        }
        if (io->piod_op == PIOD_WRITE_D && io->piod_len == 8 &&
            *(uint64_t *)io->piod_addr == (uint64_t)(scratch_mem + 0x4000 + 0xf00)) {
            struct journal j; journal_load(active_ctx, &j); assert(j.stack_dirty);
        }
        if (io->piod_op == PIOD_WRITE_D && t.fail_trap_write && io->piod_len == 1 &&
            io->piod_offs == (void *)(scratch_mem + 0x4000 + 0xf00) && *(uint8_t *)io->piod_addr != 0xcc) {
            errno = EIO;
            return -1;
        }
        if (io->piod_op == PIOD_WRITE_D && io->piod_len == 64 && t.fail_scratch_write) {
            assert(active_ctx->scratch_dirty && active_ctx->restore_pending);
            memcpy(io->piod_offs, io->piod_addr, 32); return -1;
        }
        if (io->piod_op == PIOD_WRITE_D && t.fail_stack_restore && io->piod_len == 8 &&
            *(uint64_t *)io->piod_addr == 0xdeadbeefcafef00dULL) return -1;
        if (io->piod_op == PIOD_READ_D && t.short_restore_read && io->piod_len == 8) {
            memcpy(io->piod_addr, io->piod_offs, io->piod_len - 1); io->piod_len--; return 0;
        }
        if (io->piod_op == PIOD_READ_D) memcpy(io->piod_addr, io->piod_offs, io->piod_len);
        else memcpy(io->piod_offs, io->piod_addr, io->piod_len);
        return 0;
    }
    default: errno = EINVAL; return -1;
    }
}

int thr_kill2(pid_t pid, long tid, int sig)
{
    assert(pid == 7);
    if (sig == SIGSTOP) {
        assert(tid == 100007); if (t.stop_works) t.targeted_stop = t.stop_pending = 1; return 0;
    }
    assert(tid == t.async_tid && sig == t.async_signal && !t.running);
    if (t.replay_fail) return -1;
    t.replayed++; t.delivered_code = SI_LWP; t.delivered_value = 0; return 0;
}

// ---- the scenarios ----------------------------------------------------------------------------

static const uint8_t trap_before = 0x90;
static const uint64_t stack_before = 0xdeadbeefcafef00dULL;

static void setup(sl_inject *ctx)
{
    active_ctx = ctx; crash_worker = 0;
    assert(!credentials.pending);
    fake_authid = 1; memset(fake_caps, 0, sizeof fake_caps);
    fail_caps_restore = lie_caps_restore = fail_auth_install = credential_failure_after = 0;
    memset(&t, 0, sizeof t);
    memset(ctx, 0, sizeof *ctx);
    atomic_init(&ctx->restore_pending, 0);
    page_prot = PROT_READ | PROT_WRITE;
    fail_prot_restore = lie_prot_restore = fail_prot_install = 0;
    fake_now = 1000000;
    memset(scratch_mem, 0, sizeof scratch_mem);
    memset(stack_mem, 0, sizeof stack_mem);
    t.async_code = SI_USER; t.async_tid = 100007;
    t.stop_works = t.waited = 1; t.reporting_tid = 100007;
    t.other_regs.r_rip = 0x123456; t.other_regs.r_rsp = 0x987654;
    t.ymm_active = 1;
    memset(t.ymm, 0x6b, sizeof t.ymm);
    t.attached = 1;
    t.regs.r_rip = original_rip;
    t.regs.r_rsp = (int64_t)(stack_mem + 0x800);
    t.regs.r_rax = 77, t.regs.r_rdi = 5, t.regs.r_rbx = 99;
    memset(&t.fp, 0x5a, sizeof t.fp);
    ctx->attached = 1;
    ctx->target_pid = 7; ctx->thread_pid = 100007;
    ctx->scratch = (intptr_t)(scratch_mem + 0x4000);
    ctx->scratch_len = 0x4000;
    scratch_mem[0x4000 + 0xf00] = trap_before;
    int64_t call_sp = (int64_t)(((uint64_t)t.regs.r_rsp & ~0xfULL) - 0x208);
    memcpy((void *)call_sp, &stack_before, 8);
    memcpy(ctx->scratch_backup, scratch_mem + 0x4000, 0x4000);
    ctx->scratch_saved = 1;
}

static struct reg original_regs(void)
{
    struct reg r;
    memset(&r, 0, sizeof r);
    r.r_rip = original_rip, r.r_rsp = (int64_t)(stack_mem + 0x800), r.r_rax = 77, r.r_rdi = 5, r.r_rbx = 99;
    return r;
}

static void assert_original_state(void)
{
    struct reg o = original_regs();
    assert(regs_equal(&t.regs, &o));
    struct fpreg original; memset(&original, 0x5a, sizeof original);
    assert(fp_equal(&t.fp, &original));
    if (t.ymm_active) for (unsigned i = 0; i < sizeof t.ymm; i++) assert(t.ymm[i] == 0x6b);
    assert(scratch_mem[0x4000 + 0xf00] == trap_before);
    int64_t call_sp = (int64_t)(((uint64_t)o.r_rsp & ~0xfULL) - 0x208);
    uint64_t word;
    memcpy(&word, (void *)call_sp, 8);
    assert(word == stack_before);
    assert(page_prot == (PROT_READ | PROT_WRITE));
}

static const uint64_t no_args[6] = {0};
#define CALL(ctx) inj_call_fast(&(ctx), (intptr_t)0x600000, no_args, 1)

static int broker_cases(void)
{
    int cases = 0;
    for (int phase = 0; phase < 7; phase++) {
        static sl_broker b;
        assert(!sl_broker_open(&b));
        setup(&b.context); memset(&b.context, 0, sizeof b.context); t.attached = 0;
        pid_t child = fork(); assert(child >= 0);
        if (!child) {
            sl_broker_worker(&b);
            sl_inject view = {0};
            if (phase == 0) _exit(1);
            assert(!sl_inject_begin("SceRemotePlay", &view));
            if (phase == 1) _exit(1);
            uint8_t bytes[32]; memset(bytes, 0x42, sizeof bytes);
            assert(!sl_inject_write_at(&view, view.scratch + 0x100, bytes, sizeof bytes));
            if (phase == 2) _exit(1);
            int64_t rc = inj_call_fast(&view, 0x600000, no_args, 1);
            if (phase == 6) assert(rc == SL_CALL_ERROR);
            else assert(rc == result_value);
            if (phase == 4 || phase == 6) _exit(1);
            assert(!sl_inject_end(&view));
            _exit(0);
        }
        sl_broker_parent(&b);
        if (phase == 3) crash_worker = child;
        if (phase == 6) { t.fail_setregs_from = 2; t.fail_setregs_until = 1000; }
        int status = 0;
        for (;;) {
            pid_t ended = waitpid(child, &status, WNOHANG);
            if (ended == child) break;
            assert(ended == 0 || errno == EINTR);
            int r = sl_broker_poll(&b, 10);
            if (r < 0) { assert(waitpid(child, &status, 0) == child); break; }
        }
        if (phase == 6) {
            assert(sl_broker_recover(&b) == SL_END_HELD && !t.detached);
            assert(sl_broker_close(&b) == -1);
            t.fail_setregs_until = 0;
        }
        assert(!sl_broker_recover(&b));
        assert(!b.context.attached && !b.context.restore_pending);
        assert(t.attach_calls == (phase == 0 ? 0 : 1));
        assert(t.detached == (phase == 0 ? 0 : 1));
        assert_original_state();
        assert(!sl_broker_close(&b)); cases++;
    }
    // Invalid operation/size/sequence never reach the target and cannot dispose its context.
    for (int invalid = 0; invalid < 3; invalid++) {
        static sl_broker b; assert(!sl_broker_open(&b)); setup(&b.context);
        b.shared->request = (struct request){.sequence = 1, .op = SL_RPC_READ};
        if (invalid == 0) b.shared->request.op = 999;
        if (invalid == 1) b.shared->request.len = SL_RPC_BYTES + 1;
        if (invalid == 2) b.shared->request.sequence = 99;
        atomic_store(&b.shared->ready, b.shared->request.sequence);
        assert(!send_byte(b.channel[1]));
        assert(sl_broker_poll(&b, 10) == -2 && !t.detached && !t.setregs_calls);
        assert(!sl_broker_recover(&b) && !sl_broker_close(&b)); cases++;
    }
    return cases;
}

int main(void)
{
    sl_inject c;
    int cases = 0;

    // 1. A call that finishes: result back, everything put back, nothing pending, clean detach.
    setup(&c);
    assert(CALL(c) == result_value && !c.quarantined && !atomic_load(&c.restore_pending));
    assert_original_state();
    assert(sl_inject_end(&c) == 0 && t.detached);
    assert_original_state();
    cases++;

    // 2. Timeout, the stop fails (the target is unreachable for 2 s): the call is quarantined and
    // the journal stays pending, mirrored for the supervisor. The second stop (at the end) works:
    // the state is restored BEFORE the detach. (Before the fix the detach came with our registers.)
    setup(&c);
    t.behavior = HANGS;
    t.stop_works = 0;
    assert(CALL(c) == SL_CALL_ERROR && c.quarantined && c.running && c.restore_pending);
    assert(t.regs.r_rip == 0x600000 && !t.detached);
    assert(CALL(c) == SL_CALL_ERROR); // quarantined: nothing more is called
    t.stop_works = 1; // it answers now
    assert(sl_inject_end(&c) == 0);
    assert(t.detached && !atomic_load(&c.restore_pending));
    t.regs = t.regs_at_detach;
    assert_original_state();
    cases++;

    // 3. The call returned but putting the registers back failed once: pending; the end retries and
    // detaches only with the original state.
    setup(&c);
    t.fail_setregs_from = 2, t.fail_setregs_until = 2; // call 1 installs the call, call 2 is the restore
    assert(CALL(c) == SL_CALL_ERROR && c.quarantined && c.restore_pending);
    assert(!t.detached && t.regs.r_rip != original_rip);
    assert(sl_inject_end(&c) == 0 && t.detached);
    assert(t.regs_at_detach.r_rip == original_rip);
    t.regs = t.regs_at_detach;
    assert_original_state();
    cases++;

    // 4. Restoration keeps failing: NOT detached. The target stays attached and stopped, and the
    // caller is told to keep the process alive.
    setup(&c);
    t.fail_setregs_from = 2, t.fail_setregs_until = 1000;
    assert(CALL(c) == SL_CALL_ERROR && c.restore_pending);
    assert(sl_inject_end(&c) == SL_END_HELD);
    assert(!t.detached && t.attached && c.attached && !t.running);
    cases++;

    // 5. The target cannot be stopped at the end either: held, not detached.
    setup(&c);
    t.behavior = HANGS;
    t.stop_works = 0;
    assert(CALL(c) == SL_CALL_ERROR && c.running);
    assert(sl_inject_end(&c) == SL_END_HELD && !t.detached && c.attached);
    cases++;

    // 6. Unrestored trap memory blocks detach even when the registers are original.
    setup(&c);
    t.fail_trap_write = 1;
    assert(CALL(c) == SL_CALL_ERROR && c.restore_pending && c.quarantined);
    assert(t.regs.r_rip == original_rip); // the registers did go back
    assert(sl_inject_end(&c) == SL_END_HELD && !t.detached);
    t.fail_trap_write = 0;
    assert(sl_inject_end(&c) == 0 && t.detached);
    assert_original_state();
    cases++;

    // 7. Unrestored protection also blocks detach until a later successful recovery.
    setup(&c);
    fail_prot_restore = 1;
    assert(CALL(c) == SL_CALL_ERROR && c.restore_pending);
    assert(sl_inject_end(&c) == SL_END_HELD && !t.detached);
    fail_prot_restore = 0;
    assert(sl_inject_end(&c) == 0 && t.detached);
    assert_original_state();
    assert(t.regs_at_detach.r_rip == original_rip);
    cases++;

    // 8. An unexpected stop (a fault, not the trap) restores before reporting; nothing is left.
    setup(&c);
    t.behavior = HANGS;
    t.stop_works = 1;
    assert(CALL(c) == SL_CALL_ERROR && c.quarantined && !atomic_load(&c.restore_pending));
    assert_original_state();
    assert(sl_inject_end(&c) == 0 && t.detached);
    cases++;

    // Claimed success without restored registers or FP state is not proof of restoration.
    setup(&c); t.ignore_regs_restore = 1;
    assert(CALL(c) == SL_CALL_ERROR && c.restore_pending);
    assert(sl_inject_end(&c) == SL_END_HELD && !t.detached);
    t.ignore_regs_restore = 0; assert(!sl_inject_end(&c)); assert_original_state(); cases++;

    setup(&c); t.fail_fp_restore = 1;
    assert(CALL(c) == SL_CALL_ERROR && c.restore_pending);
    assert(sl_inject_end(&c) == SL_END_HELD && !t.detached);
    t.fail_fp_restore = 0; assert(!sl_inject_end(&c)); assert_original_state(); cases++;

    setup(&c); t.ignore_fp_restore = 1;
    assert(CALL(c) == SL_CALL_ERROR && c.restore_pending);
    assert(sl_inject_end(&c) == SL_END_HELD && !t.detached);
    t.ignore_fp_restore = 0; assert(!sl_inject_end(&c)); assert_original_state(); cases++;

    setup(&c); t.fail_stack_restore = 1;
    assert(CALL(c) == SL_CALL_ERROR && c.restore_pending);
    assert(sl_inject_end(&c) == SL_END_HELD && !t.detached);
    t.fail_stack_restore = 0; assert(!sl_inject_end(&c)); assert_original_state(); cases++;

    setup(&c); lie_prot_restore = 1;
    assert(CALL(c) == SL_CALL_ERROR && c.restore_pending);
    assert(sl_inject_end(&c) == SL_END_HELD && !t.detached);
    lie_prot_restore = 0; assert(!sl_inject_end(&c)); assert_original_state(); cases++;

    // Even an install that changes state and then returns failure must have a journal first.
    setup(&c); fail_prot_install = 1;
    assert(CALL(c) == SL_CALL_ERROR && !c.restore_pending);
    assert(!sl_inject_end(&c)); assert_original_state(); cases++;

    setup(&c); t.fail_setregs_from = t.fail_setregs_until = 1; t.altered_install_failure = 1;
    assert(CALL(c) == SL_CALL_ERROR && !c.restore_pending);
    assert(!sl_inject_end(&c)); assert_original_state(); cases++;

    setup(&c); assert(CALL(c) == result_value); t.fail_detach = 1;
    assert(sl_inject_end(&c) == SL_END_HELD && c.attached && !t.detached);
    t.fail_detach = 0; assert(!sl_inject_end(&c)); assert_original_state(); cases++;

    // Session scratch survives call cleanup and must be verified before detach too.
    setup(&c); uint8_t overwrite[64]; memset(overwrite, 0x33, sizeof overwrite);
    assert(!sl_inject_write_at(&c, c.scratch + 0x100, overwrite, sizeof overwrite));
    assert(c.scratch_dirty && c.restore_pending);
    assert(CALL(c) == result_value && c.restore_pending);
    assert(!sl_inject_end(&c) && !c.restore_pending);
    assert(!memcmp((void *)(c.scratch + 0x100), c.scratch_backup + 0x100, sizeof overwrite));
    assert_original_state(); cases++;

    setup(&c); assert(!sl_inject_write_at(&c, c.scratch + 0x100, overwrite, sizeof overwrite));
    scratch_mem[0x4000 + 0x900] = 0x99; // another thread's unrelated data must survive
    assert(!sl_inject_end(&c)); assert(scratch_mem[0x4000 + 0x900] == 0x99); cases++;

    setup(&c); t.behavior = HANGS; t.race_stop_trap = 1;
    assert(CALL(c) == SL_CALL_ERROR && c.stop_pending && t.stop_pending);
    assert(!sl_inject_end(&c) && !t.stop_pending); assert_original_state(); cases++;

    setup(&c); t.exits = 1;
    assert(CALL(c) == SL_CALL_ERROR && c.dead && !c.attached);
    assert(sl_inject_end(&c) == SL_END_GONE && !t.detached); cases++;

    // Privilege rollback errors must not erase actual attach/detach ownership.
    setup(&c); memset(&c, 0, sizeof c); t.attached = 0;
    credential_failure_after = PT_ATTACH;
    assert(sl_inject_begin("SceRemotePlay", &c) == -1 && c.attached && t.attached);
    assert(!sl_inject_safe_to_exit(&c)); fail_caps_restore = 0;
    assert(!sl_inject_end(&c) && sl_inject_safe_to_exit(&c)); cases++;

    setup(&c); credential_failure_after = PT_DETACH;
    assert(sl_inject_end(&c) == SL_END_HELD && !c.attached && t.detached);
    assert(!sl_inject_safe_to_exit(&c)); fail_caps_restore = 0;
    assert(!sl_inject_end(&c) && sl_inject_safe_to_exit(&c)); cases++;

    setup(&c); fail_auth_install = 1;
    assert(CALL(c) == SL_CALL_ERROR && !credentials.pending && fake_authid == 1);
    fail_auth_install = 0; assert(!sl_inject_end(&c)); cases++;

    setup(&c); lie_caps_restore = 1;
    assert(CALL(c) == SL_CALL_ERROR && credentials.pending);
    assert(sl_inject_end(&c) == SL_END_HELD && !t.detached);
    lie_caps_restore = 0; assert(!sl_inject_end(&c) && sl_inject_safe_to_exit(&c)); cases++;

    setup(&c); t.short_after_continue = 1;
    assert(CALL(c) == SL_CALL_ERROR && c.restore_pending);
    assert(sl_inject_end(&c) == SL_END_HELD && !t.detached);
    t.short_restore_read = 0; assert(!sl_inject_end(&c)); assert_original_state(); cases++;

    setup(&c); t.fail_scratch_write = 1;
    assert(sl_inject_write_at(&c, c.scratch + 0x100, overwrite, sizeof overwrite) == -1);
    assert(c.scratch_dirty && c.restore_pending); t.fail_scratch_write = 0;
    assert(!sl_inject_end(&c) && !memcmp((void *)(c.scratch + 0x100), c.scratch_backup + 0x100, sizeof overwrite));
    assert_original_state(); cases++;

    setup(&c); t.behavior = HANGS; t.switch_stop_tid = 1;
    struct reg other = t.other_regs;
    assert(CALL(c) == SL_CALL_ERROR && !c.restore_pending);
    assert(regs_equal(&t.other_regs, &other));
    assert(!sl_inject_end(&c)); assert_original_state(); cases++;

    setup(&c); t.async_signal = SIGUSR1; other = t.other_regs;
    assert(CALL(c) == SL_CALL_ERROR && c.deferred_count == 1);
    assert(regs_equal(&t.other_regs, &other));
    assert(!sl_inject_end(&c) && t.replayed == 1); assert_original_state(); cases++;

    setup(&c); t.no_xstate = 1;
    assert(CALL(c) == SL_CALL_ERROR && !t.setregs_calls && !c.restore_pending);
    assert(!sl_inject_end(&c)); assert_original_state(); cases++;

    setup(&c); c.running = 1; t.running = t.waited = 0; t.stop_status_ready = 1;
    assert(!sl_inject_ensure_stopped(&c) && t.waited && c.thread_pid == 100007);
    assert(!sl_inject_end(&c) && !t.stop_pending); assert_original_state(); cases++;

    const int async_codes[] = {SI_LWP, SI_TIMER, 1};
    const int async_signals[] = {SIGUSR2, SIGALRM, SIGCHLD};
    for (unsigned i = 0; i < 3; i++) {
        setup(&c); t.async_signal = async_signals[i]; t.async_code = async_codes[i];
        assert(CALL(c) == SL_CALL_ERROR && (c.deferred_count == 1 || c.detach_signal));
        assert(!sl_inject_end(&c) && t.replayed == 1); assert_original_state(); cases++;
    }
    setup(&c); t.async_signal = SIGUSR1; t.replay_fail = 1;
    assert(CALL(c) == SL_CALL_ERROR && c.deferred_count == 1);
    assert(sl_inject_end(&c) == SL_END_HELD && c.deferred_count == 1 && !t.detached);
    t.replay_fail = 0; assert(!sl_inject_end(&c) && t.replayed == 1); assert_original_state(); cases++;

    setup(&c); t.async_signal = SIGUSR1; t.async_tid = 100008;
    assert(CALL(c) == SL_CALL_ERROR && t.forwarded == 1 && !c.deferred_count && c.aborted_restored);
    t.async_signal = 0; t.behavior = RETURNS;
    assert(sl_inject_cleanup_call(&c, 0x600000, no_args) == result_value);
    assert(!sl_inject_end(&c)); assert_original_state(); cases++;

    setup(&c); t.ymm_active = 0; // inactive AVX backing bytes may be stale
    assert(CALL(c) == result_value && !t.ymm_active);
    assert(!sl_inject_end(&c)); assert_original_state(); cases++;

    // A can report SIGSTOP from sleepq/cursig before syscall return. That return still
    // overwrites rax/rflags: do not restore until a later stop outside the syscall stub.
    setup(&c); t.behavior = HANGS; t.switch_stop_tid = t.kernel_stop_reports = 1;
    assert(CALL(c) == SL_CALL_ERROR && t.kernel_reported && !t.a_in_kernel && !c.restore_pending);
    assert(!sl_inject_end(&c)); assert_original_state(); cases++;

    // Same syscall-boundary guard applies before borrowing registers at attach.
    setup(&c); t.behavior = HANGS; t.a_in_kernel = t.syscall_boundary = t.kernel_stop_reports = 1;
    assert(!sl_inject_begin("SceRemotePlay", &c) && t.continue_calls && !t.a_in_kernel);
    assert(!sl_inject_end(&c)); cases++;

    // A failed attach names its own check and values, not cleanup's last step.
    setup(&c); t.fail_lwp_at = 1;
    assert(sl_inject_begin("SceRemotePlay", &c) == -1 && !strncmp(c.stage, "pin-lwpinfo e5 ", 15) && !t.attached);
    t.fail_lwp_at = 0; cases++;

    // Clean end needs no A boundary: deliver B's signal once without resuming the process.
    const int b_codes[] = {SI_USER, SI_QUEUE};
    for (unsigned i = 0; i < 2; i++) {
        setup(&c); c.running = t.running = 1; t.waited = 0;
        t.async_signal = SIGUSR1; t.async_tid = 100008; t.async_code = b_codes[i];
        assert(!sl_inject_end(&c) && !t.forwarded && t.replayed == 1 && !c.detach_signal && !c.deferred_count);
        assert(!t.continue_calls && !t.code_reads);
        cases++;
    }

    // A payload-bearing attach stop must never be consumed by a remote call.
    setup(&c); t.attach_signal = t.async_signal = SIGALRM; t.async_code = SI_TIMER;
    assert(sl_inject_begin("SceRemotePlay", &c) == -1 && t.detached && t.replayed == 1 && !t.continue_calls);
    assert(CALL(c) == SL_CALL_ERROR); cases++;

    // Stops collected inside acquire_boundary itself must retain A's signal exactly once.
    for (unsigned i = 0; i < 2; i++) {
        setup(&c); c.running = t.running = 1; t.waited = 0;
        t.async_signal = SIGUSR1; t.async_code = b_codes[i];
        assert(!sl_inject_ensure_stopped(&c));
        assert(!sl_inject_ensure_stopped(&c)); // repeat must not duplicate classification
        assert(!sl_inject_end(&c) && t.replayed == 1 && !t.forwarded); cases++;
    }

    // Failed continue with no kernel effect leaves the collected stop usable for restore.
    setup(&c); t.fail_continue_once = 1;
    assert(CALL(c) == SL_CALL_ERROR && !c.running && !c.restore_pending && c.aborted_restored);
    assert(!sl_inject_end(&c)); assert_original_state(); cases++;

    // Early cleanup failure must retain verified-abort quarantine and permit another cleanup.
    setup(&c); t.behavior = HANGS;
    assert(CALL(c) == SL_CALL_ERROR && c.aborted_restored && c.quarantined);
    t.no_xstate = 1;
    assert(sl_inject_cleanup_call(&c, 0x600000, no_args) == SL_CALL_ERROR && c.aborted_restored && c.quarantined);
    assert(CALL(c) == SL_CALL_ERROR);
    t.no_xstate = 0; t.behavior = RETURNS;
    assert(sl_inject_cleanup_call(&c, 0x600000, no_args) == result_value && c.quarantined);
    assert(!sl_inject_end(&c)); assert_original_state(); cases++;

    // A clean target can detach at an ambiguous syscall stop, retaining payload metadata.
    setup(&c); c.running = t.running = 1; t.waited = 0;
    t.async_signal = SIGALRM; t.async_code = SI_TIMER; t.syscall_boundary = 1;
    assert(!sl_inject_end(&c) && t.detached && t.replayed == 1 && !t.continue_calls && !t.code_reads);
    assert(t.delivered_code == SI_TIMER && t.delivered_value == 0x334455);
    assert_original_state(); cases++;

    // Execute-only system code (the console): an opcode read that fails with nothing of ours
    // installed borrows the stop unchecked, as the PoC did; calls work, since a stop on our own
    // trap needs no code read, and everything is restored.
    setup(&c); t.fail_code_read = 1;
    assert(!sl_inject_begin("SceRemotePlay", &c) && c.attached && t.code_reads == 1 && !t.continue_calls);
    assert(CALL(c) == result_value && CALL(c) == result_value);
    assert(!sl_inject_end(&c)); assert_original_state(); t.fail_code_read = 0; cases++;

    // Dirty state still requires the boundary and readback; clean-detach bypass cannot release it.
    setup(&c); t.behavior = HANGS; t.stop_works = 0;
    assert(CALL(c) == SL_CALL_ERROR && c.restore_pending);
    t.stop_works = 1; t.async_signal = SIGALRM; t.async_code = SI_TIMER; t.syscall_boundary = 1;
    assert(sl_inject_end(&c) == SL_END_HELD && c.restore_pending && !t.detached);
    // This combination cannot advance without consuming the payload stop. A retry must
    // retain ownership too; the real console may need a restart to clear this condition.
    assert(sl_inject_end(&c) == SL_END_HELD && c.restore_pending && !t.detached); cases++;

    setup(&c); t.behavior = HANGS; t.stop_works = 0;
    assert(CALL(c) == SL_CALL_ERROR && c.restore_pending);
    t.stop_works = 1; t.fail_code_read = 1;
    assert(sl_inject_end(&c) == SL_END_HELD && !t.detached && c.restore_pending);
    t.fail_code_read = 0; assert(!sl_inject_end(&c)); assert_original_state(); cases++;

    // Back-to-back already-ready stops must not skip the acquisition deadline.
    setup(&c); t.syscall_boundary = t.immediate_stops = 1; c.last_stop_signal = SIGSTOP;
    int64_t before = fake_now;
    assert(sl_inject_ensure_stopped(&c) == -1 && fake_now - before >= 2000000);
    assert(fake_now - before < 2200000 && t.continue_calls < 25 && c.attached && !t.detached);
    t.immediate_stops = t.syscall_boundary = 0; assert(!sl_inject_end(&c)); cases++;

    // A transient lookup failure leaves the original stop available for retry.
    setup(&c); c.running = t.running = 1; t.waited = 0;
    t.async_signal = SIGUSR1; t.fail_lwp_at = 1;
    assert(sl_inject_end(&c) == SL_END_HELD && !t.detached && !t.replayed);
    assert(!sl_inject_end(&c) && t.replayed == 1 && !c.unclassified_signal); cases++;

    // Missing siginfo can recover too; classify once after it becomes available.
    setup(&c); c.running = t.running = 1; t.waited = 0;
    t.async_signal = SIGUSR1; t.missing_siginfo = 1;
    assert(sl_inject_end(&c) == SL_END_HELD && c.unclassified_signal && !t.detached);
    t.missing_siginfo = 0;
    assert(!sl_inject_end(&c) && !c.unclassified_signal && t.replayed == 1); cases++;

    // AIO and other kernel notifications carry metadata, not just a signal number.
    const int payload_codes[] = {SI_ASYNCIO, 2};
    for (unsigned i = 0; i < 2; i++) {
        setup(&c); t.async_signal = SIGUSR1; t.async_code = payload_codes[i];
        assert(CALL(c) == SL_CALL_ERROR && c.detach_signal && !c.deferred_count);
        assert(!sl_inject_end(&c) && t.replayed == 1 && t.delivered_code == payload_codes[i]);
        assert(t.delivered_value == 0x334455); assert_original_state(); cases++;
    }

    cases += broker_cases();
    printf("PASS: injector restoration journal + supervisor broker: %d cases (hang, stop fails then works, restore fails once and always, strict held state)\n", cases);
    return 0;
}
