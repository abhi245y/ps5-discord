// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "../../vendor/shellui-loader/prologue.hpp"
#include <cstdint>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
namespace overlay {
using ProtectionQuery=int(*)(void*,void**,void**,int*);
// mprotect is injected by pointer: the payload-side libc names it
// kernel_mprotect, which does not exist inside SceShellUI.
using MprotectCall=int(*)(void*,std::size_t,int);
// Same rule as mprotect: plain mmap/munmap do not resolve inside SceShellUI.
using MmapCall=void*(*)(void*,std::size_t,int,int,int,std::int64_t);
using MunmapCall=int(*)(void*,std::size_t);
struct Hook {
    void* target=nullptr;
    void* trampoline=nullptr;
    void* page=nullptr;
    std::size_t page_size=0;
    MprotectCall mprotect=nullptr;
    int original_prot=0;
    alignas(16) std::uint8_t before[16]{},after[16]{};
    bool installed=false;
    bool was_installed=false; // sticky: distinguishes "prepared" from "really patched"
};
inline bool replace16(void* at,const std::uint8_t expected[16],const std::uint8_t desired[16]){
    if(reinterpret_cast<std::uintptr_t>(at)&15)return false;
    std::uint64_t a,d,b,c;std::memcpy(&a,expected,8);std::memcpy(&d,expected+8,8);
    std::memcpy(&b,desired,8);std::memcpy(&c,desired+8,8);unsigned char ok;
    __asm__ volatile("lock cmpxchg16b %1; sete %0":"=q"(ok),"+m"(*static_cast<volatile __int128*>(at)),"+a"(a),"+d"(d):"b"(b),"c"(c):"cc","memory");
    return ok;
}
inline void absolute_jump(std::uint8_t* out,const void* to){
    out[0]=0x48;out[1]=0xb8;
    auto address=reinterpret_cast<std::uintptr_t>(to);std::memcpy(out+2,&address,8);
    out[10]=0xff;out[11]=0xe0;
}
inline std::size_t native_patch_length(const std::uint8_t* code){
    if(!code)return 0;std::size_t length=0;
    while(length<12){
        auto decoded=home_bridge::decode_relocatable_instruction(code+length,16-length);
        if(!decoded)return 0;length+=decoded;
    }
    return length<=16?length:0;
}
inline bool code_protection(int prot){
    return prot==(PROT_READ|PROT_EXEC)||prot==PROT_EXEC||prot==(PROT_READ|PROT_WRITE|PROT_EXEC);}
inline bool page_bounds(void* target,std::size_t page,ProtectionQuery query,void** page_out,
                        void** lo_out,void** hi_out,int* prot_out){
    if(!target||!query||page<16||(page&(page-1))||(reinterpret_cast<std::uintptr_t>(target)&15))return false;
    void* page_at=reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(target)&~(page-1));
    void* lo=nullptr;void* hi=nullptr;int prot=0;
    if(query(target,&lo,&hi,&prot)||!code_protection(prot))return false;
    if(reinterpret_cast<std::uintptr_t>(lo)>reinterpret_cast<std::uintptr_t>(page_at)||
       reinterpret_cast<std::uintptr_t>(hi)<reinterpret_cast<std::uintptr_t>(page_at)+page)return false;
    *page_out=page_at;*lo_out=lo;*hi_out=hi;*prot_out=prot;return true;
}
inline bool prepare_hook(Hook& h,void* target,void* replacement,ProtectionQuery query,MprotectCall mprotect,
                         MmapCall do_mmap,MunmapCall do_munmap,void(*step)(const char*)=nullptr){
    if(h.target||!replacement||!mprotect||!do_mmap||!do_munmap)return false;long size=sysconf(_SC_PAGESIZE);if(size<=0)return false;
    void* page=nullptr,*lo=nullptr,*hi=nullptr;int prot=0;
    if(!page_bounds(target,std::size_t(size),query,&page,&lo,&hi,&prot))return false;
    if(step)step("mprotect+r begin");
    // Exec-only JIT pages need read permission only while copying the prologue.
    if(prot==PROT_EXEC&&mprotect(page,std::size_t(size),PROT_READ|PROT_EXEC))return false;
    if(step)step("copy begin");
    std::memcpy(h.before,target,16);
    if(step)step("restore begin");
    if(prot==PROT_EXEC&&mprotect(page,std::size_t(size),prot))return false;
    if(step)step("decode begin");
    auto length=native_patch_length(h.before);if(length<12||length>16)return false;
    if(step)step("decode done");
    void* trampoline=do_mmap(nullptr,std::size_t(size),PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(!trampoline||trampoline==reinterpret_cast<void*>(-1))return false;
    if(step)step("mmap done");
    std::memcpy(trampoline,h.before,length);absolute_jump(static_cast<std::uint8_t*>(trampoline)+length,static_cast<std::uint8_t*>(target)+length);
    if(step)step("trampoline protect begin");
    if(mprotect(trampoline,std::size_t(size),PROT_READ|PROT_EXEC)){do_munmap(trampoline,std::size_t(size));return false;}
    std::memcpy(h.after,h.before,16);absolute_jump(h.after,replacement);for(std::size_t i=12;i<length;i++)h.after[i]=0x90;
    h.target=target;h.trampoline=trampoline;h.page=page;h.page_size=std::size_t(size);
    h.mprotect=mprotect;h.original_prot=prot;return true;
}
inline bool install_hook(Hook& h,ProtectionQuery query){
    if(!h.trampoline||h.installed||!h.mprotect)return false;
    void* page=nullptr,*lo=nullptr,*hi=nullptr;int prot=0;
    if(!page_bounds(h.target,h.page_size,query,&page,&lo,&hi,&prot))return false;
    if(prot!=(PROT_READ|PROT_WRITE|PROT_EXEC)&&h.mprotect(page,h.page_size,PROT_READ|PROT_WRITE|PROT_EXEC))return false;
    h.installed=replace16(h.target,h.before,h.after);
    // A protection restoration failure is visible to the caller even if CAS worked.
    bool protected_ok=h.mprotect(page,h.page_size,h.original_prot)==0;
    if(h.installed)h.was_installed=true;
    return h.installed&&protected_ok;
}
inline bool remove_hook(Hook& h,ProtectionQuery query){
    if(!h.installed)return true;
    void* page=nullptr,*lo=nullptr,*hi=nullptr;int prot=0;
    // Permit our page left RWX by a failed restore, but never change foreign code.
    if(!page_bounds(h.target,h.page_size,query,&page,&lo,&hi,&prot))return false;
    if(prot!=(PROT_READ|PROT_WRITE|PROT_EXEC)&&h.mprotect(page,h.page_size,PROT_READ|PROT_WRITE|PROT_EXEC))return false;
    bool removed=replace16(h.target,h.after,h.before);
    bool protected_ok=h.mprotect(page,h.page_size,h.original_prot)==0;
    if(removed)h.installed=false;
    return removed&&protected_ok;
}
// Keep a published trampoline mapped after removal: a CPU can already be
// entering the hook before its in-flight counter becomes visible.
}
