// FreeBSD amd64 ABI fields needed by the host fake; matches the payload SDK layout.
#pragma once
#include <stdint.h>
struct reg {
    int64_t r_r15,r_r14,r_r13,r_r12,r_r11,r_r10,r_r9,r_r8,r_rdi,r_rsi,r_rbp,r_rbx,r_rdx,r_rcx,r_rax;
    uint32_t r_trapno; uint16_t r_fs,r_gs; uint32_t r_err; uint16_t r_es,r_ds;
    int64_t r_rip,r_cs,r_rflags,r_rsp,r_ss;
};
struct fpreg {
    uint64_t fpr_env[4]; uint8_t fpr_acc[8][16],fpr_xacc[16][16]; uint64_t fpr_spare[12];
};
