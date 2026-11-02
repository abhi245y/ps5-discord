/* Copyright (C) 2026 porhe911; local PoC hardening 2026.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "remote_call.hpp"
#include <csignal>
#include <cstring>
#include <sys/mman.h>
#include <sys/wait.h>
extern "C" {
#include "pt.h"
}
namespace common_fps::ps5 {
long call_until_breakpoint_on_private_stack(pid_t pid,std::uintptr_t address,
    std::uint64_t a0,std::uint64_t a1,std::uint64_t a2,std::uint64_t a3,std::uint64_t a4,std::uint64_t a5,
    std::uintptr_t expected_trap) noexcept {
    if(pid<=0||!address||!expected_trap)return -1;
    reg saved{},remote{},stopped{};fpreg saved_fp{};
    if(pt_getregs(pid,&saved)||pt_getfpregs(pid,&saved_fp))return -1;
    constexpr std::size_t stack_size=128*1024;
    auto stack=pt_mmap(pid,0,stack_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(stack<=0)return -1;
    remote=saved;remote.r_rip=address;remote.r_rsp=(std::uintptr_t(stack)+stack_size-8);remote.r_rax=0;
    remote.r_rdi=a0;remote.r_rsi=a1;remote.r_rdx=a2;remote.r_rcx=a3;remote.r_r8=a4;remote.r_r9=a5;
    int status=0;bool continued=false,stopped_ok=true,ok=false;
    if(!pt_setregs(pid,&remote)) {
        if(!pt_continue(pid,0)) {
            continued=true;stopped_ok=pt_wait_stop(pid,&status,3000)==0;
            if(stopped_ok&&!pt_getregs(pid,&stopped)&&WSTOPSIG(status)==SIGTRAP&&
                std::uintptr_t(stopped.r_rip)==expected_trap+1)ok=true;
        }
    }
    if(continued&&!stopped_ok) {
        (void)kill(pid,SIGSTOP);stopped_ok=pt_wait_stop(pid,&status,1000)==0;
    }
    if(!stopped_ok)return -1; // Never write registers/memory while target runs.
    if(pt_setfpregs(pid,&saved_fp)||pt_setregs(pid,&saved))return -1;
    if(pt_munmap(pid,stack,stack_size))return -1;
    return ok?long(stopped.r_rax):-1;
}
}
