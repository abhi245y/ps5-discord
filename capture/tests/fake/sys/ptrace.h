// A stand-in for FreeBSD's <sys/ptrace.h> (the host's differs).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <signal.h>
enum { PT_ATTACH = 10, PT_DETACH = 11, PT_CONTINUE = 7, PT_GETREGS = 33, PT_SETREGS = 34, PT_GETFPREGS = 35,
       PT_SETFPREGS = 36, PT_IO = 12, PT_LWPINFO = 13, PT_GETXSTATE_INFO = 68, PT_GETXSTATE = 69, PT_SETXSTATE = 70 };
enum { PIOD_READ_D = 1, PIOD_WRITE_D = 2, PIOD_READ_I = 3 };
struct ptrace_io_desc {
    int piod_op;
    void *piod_offs;
    void *piod_addr;
    size_t piod_len;
};

#define PL_FLAG_SI 0x20
#define PL_FLAG_EXITED 0x200
struct ptrace_lwpinfo { int pl_lwpid,pl_event,pl_flags; siginfo_t pl_siginfo; };
struct ptrace_xstate_info { uint64_t xsave_mask; uint32_t xsave_len; };

#undef SI_USER
#undef SI_QUEUE
#undef SI_TIMER
#undef SI_ASYNCIO
#undef SI_MESGQ
#define SI_USER 0x10001
#define SI_QUEUE 0x10002
#define SI_TIMER 0x10003
#define SI_ASYNCIO 0x10004
#define SI_MESGQ 0x10005
#define SI_LWP 0x10007
#ifndef TRAP_BRKPT
#define TRAP_BRKPT 1
#endif
