// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/core/protocol.hpp"
#include "../src/core/hook.hpp"
#include <cassert>
#include <cstdlib>
using namespace overlay;
static void* mapped=nullptr;static std::size_t mapped_size=0;static int reported_prot=PROT_READ|PROT_EXEC;
static int mapped_prot=0;static bool deny_write=false,deny_trampoline=false;
static int query(void* at,void** lo,void** hi,int* prot){
    if(at!=mapped)return -1;*lo=mapped;*hi=static_cast<char*>(mapped)+mapped_size;*prot=reported_prot;return 0;
}
static Hook active;static int observed=0;
static void replacement(int* n){reinterpret_cast<void(*)(int*)>(active.trampoline)(n);++observed;}
static void* host_mmap(void* at,std::size_t n,int p,int f,int fd,long off){return mmap(at,n,p,f,fd,off);}
static int host_munmap(void* at,std::size_t n){return munmap(at,n);}
static int host_mprotect(void* at,std::size_t n,int prot){
    if((deny_write&&at==mapped&&(prot&PROT_WRITE))||
       (deny_trampoline&&at!=mapped&&(prot&PROT_EXEC)))return -1;
    int rc=mprotect(at,n,prot);if(!rc&&at==mapped)mapped_prot=prot;return rc;
}
static int refused_query(void*,void**,void**,int*){return -1;}
static void check_jump(const std::uint8_t* code,const void* to){
    assert(code[0]==0x48&&code[1]==0xb8&&code[10]==0xff&&code[11]==0xe0);
    std::uintptr_t address=0;std::memcpy(&address,code+2,8);assert(address==reinterpret_cast<std::uintptr_t>(to));
    for(unsigned i=0;i<15;i++)assert(code[i]!=0xff||code[i+1]!=0x25);
}
int main(){
    Request r;r.pid=123;r.epoch=456;r.build=789;r.token=1;r.deadline_ms=15000;
    for(auto& f:r.functions)f=0x100000;
    assert(valid(r,123,789,456,10000));
    auto bad=r;bad.pid=124;assert(!valid(bad,123,789,456,10000));
    bad=r;bad.build++;assert(!valid(bad,123,789,456,10000));
    bad=r;bad.epoch++;assert(!valid(bad,123,789,456,10000));
    bad=r;bad.token=0;assert(!valid(bad,123,789,456,10000));
    bad=r;bad.size--;assert(!valid(bad,123,789,456,10000));
    bad=r;bad.signature++;assert(!valid(bad,123,789,456,10000));
    bad=r;bad.protocol++;assert(!valid(bad,123,789,456,10000));
    bad=r;bad.mode=static_cast<Mode>(4);assert(!valid(bad,123,789,456,10000));
    bad=r;bad.deadline_ms=9999;assert(!valid(bad,123,789,456,10000));
    bad=r;bad.deadline_ms=130001;assert(!valid(bad,123,789,456,10000));
    bad=r;bad.functions[0]=1;assert(!valid(bad,123,789,456,10000));
    bad=r;bad.functions[0]=0x800000000000ULL;assert(!valid(bad,123,789,456,10000));
    bad=r;bad.functions[RunningBigApp]=0;bad.functions[SetControllerFocus]=0;assert(valid(bad,123,789,456,10000)); // optional: may be missing
    bad=r;bad.functions[0]=0;assert(!valid(bad,123,789,456,10000));
    char directory[]="/tmp/overlay-poc-test-XXXXXX";assert(mkdtemp(directory));
    assert(write_file(directory,"request.bin",r));Request copy;assert(read_file(directory,"request.bin",copy));assert(copy.token==r.token);
    char file[256];path(file,sizeof file,directory,"request.bin");FILE* extra=std::fopen(file,"ab");assert(extra);std::fputc(1,extra);std::fclose(extra);
    assert(!read_file(directory,"request.bin",copy));std::remove(file);assert(!rmdir(directory));
    alignas(16) std::uint8_t expected[16]{},desired[16];std::memset(desired,0x5a,16);
    alignas(16) std::uint8_t block[16]{};assert(replace16(block,expected,desired));assert(!replace16(block,expected,desired));
    assert(!replace16(block+1,expected,desired));assert(replace16(block,desired,expected));
    const std::uint8_t rip_relative[]={0x48,0x8b,0x05,0,0,0,0},relative_call[]={0xe8,0,0,0,0};
    assert(!home_bridge::decode_relocatable_instruction(rip_relative,sizeof rip_relative));
    assert(!home_bridge::decode_relocatable_instruction(relative_call,sizeof relative_call));
    const int protections[]={PROT_READ|PROT_EXEC,PROT_EXEC};
    for(int original_prot:protections){
        active=Hook{};observed=0;
        mapped_size=std::size_t(sysconf(_SC_PAGESIZE));mapped=mmap(nullptr,mapped_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(mapped!=MAP_FAILED);
        auto* code=static_cast<std::uint8_t*>(mapped);
        reported_prot=original_prot;
        for(std::size_t length=12;length<=16;length++){
            assert(!host_mprotect(mapped,mapped_size,PROT_READ|PROT_WRITE));std::memset(code,0xcc,16);
            std::memset(code,0x90,length-10);code[length-10]=0x49;code[length-9]=0xbb;
            std::memset(code+length-8,0,8);
            assert(native_patch_length(code)==length);assert(!host_mprotect(mapped,mapped_size,original_prot));
            Hook prepared;assert(prepare_hook(prepared,mapped,reinterpret_cast<void*>(&replacement),query,host_mprotect,host_mmap,host_munmap));
            assert(mapped_prot==original_prot);check_jump(prepared.after,reinterpret_cast<void*>(&replacement));
            for(std::size_t i=12;i<length;i++)assert(prepared.after[i]==0x90);
            for(std::size_t i=length;i<16;i++)assert(prepared.after[i]==prepared.before[i]);
            check_jump(static_cast<std::uint8_t*>(prepared.trampoline)+length,code+length);
            assert(!munmap(prepared.trampoline,prepared.page_size));
        }
        assert(!host_mprotect(mapped,mapped_size,PROT_READ|PROT_WRITE));std::memset(code,0x90,16);code[11]=0xc3;
        assert(!host_mprotect(mapped,mapped_size,original_prot));
        Hook invalid;assert(!prepare_hook(invalid,mapped,reinterpret_cast<void*>(&replacement),query,host_mprotect,host_mmap,host_munmap));
        assert(mapped_prot==original_prot);
        assert(!host_mprotect(mapped,mapped_size,PROT_READ|PROT_WRITE));
        std::memset(code,0x90,16);
        // Isolated executable fixture, never a system process: push rbp; mov rbp,rsp;
        // NOPs; increment caller's integer; pop rbp; ret.
        code[0]=0x55;code[1]=0x48;code[2]=0x89;code[3]=0xe5;code[16]=0xff;code[17]=0x07;code[18]=0x5d;code[19]=0xc3;
        assert(!host_mprotect(mapped,mapped_size,original_prot));
        Hook refused;reported_prot=PROT_READ|PROT_WRITE;assert(!prepare_hook(refused,mapped,reinterpret_cast<void*>(&replacement),query,host_mprotect,host_mmap,host_munmap));
        reported_prot=PROT_READ|PROT_EXEC;assert(!prepare_hook(refused,code+1,reinterpret_cast<void*>(&replacement),query,host_mprotect,host_mmap,host_munmap));
        reported_prot=original_prot;deny_trampoline=true;
        assert(!prepare_hook(refused,mapped,reinterpret_cast<void*>(&replacement),query,host_mprotect,host_mmap,host_munmap));
        assert(mapped_prot==original_prot);deny_trampoline=false;
        assert(prepare_hook(active,mapped,reinterpret_cast<void*>(&replacement),query,host_mprotect,host_mmap,host_munmap));
        assert(mapped_prot==original_prot);check_jump(active.after,reinterpret_cast<void*>(&replacement));
        assert(!active.was_installed); // preparation alone must not count as patched
        assert(!install_hook(active,refused_query));assert(mapped_prot==original_prot);
        deny_write=true;assert(!install_hook(active,query));assert(mapped_prot==original_prot);deny_write=false;
        int n=0;reinterpret_cast<void(*)(int*)>(mapped)(&n);assert(n==1&&observed==0);
        assert(install_hook(active,query));assert(active.was_installed&&mapped_prot==original_prot);
        reinterpret_cast<void(*)(int*)>(mapped)(&n);assert(n==2&&observed==1);
        assert(!remove_hook(active,refused_query));assert(mapped_prot==original_prot);
        deny_write=true;assert(!remove_hook(active,query));assert(mapped_prot==original_prot);deny_write=false;
        assert(remove_hook(active,query));assert(active.was_installed&&!active.installed&&mapped_prot==original_prot);
        assert(!host_mprotect(mapped,mapped_size,PROT_READ|PROT_EXEC));
        assert(!std::memcmp(mapped,active.before,16));
        assert(!host_mprotect(mapped,mapped_size,original_prot));
        reinterpret_cast<void(*)(int*)>(mapped)(&n);assert(n==3&&observed==1);
        assert(install_hook(active,query));assert(!host_mprotect(mapped,mapped_size,PROT_READ|PROT_WRITE));
        std::uint8_t foreign[16];std::memcpy(foreign,active.after,16);foreign[15]^=1;std::memcpy(mapped,foreign,16);
        assert(!host_mprotect(mapped,mapped_size,original_prot));assert(!remove_hook(active,query));assert(mapped_prot==original_prot);
        assert(!host_mprotect(mapped,mapped_size,PROT_READ|PROT_EXEC));assert(!std::memcmp(mapped,foreign,16));
        assert(!host_mprotect(mapped,mapped_size,PROT_READ|PROT_WRITE));std::memcpy(mapped,active.after,16);
        assert(!host_mprotect(mapped,mapped_size,original_prot));assert(remove_hook(active,query));assert(mapped_prot==original_prot);
        assert(!host_mprotect(mapped,mapped_size,PROT_READ|PROT_WRITE));
        code[15]^=1;assert(!host_mprotect(mapped,mapped_size,original_prot));
        assert(!install_hook(active,query));assert(mapped_prot==original_prot&&!active.installed);
        munmap(active.trampoline,active.page_size);munmap(mapped,mapped_size);
    }
    std::puts("PASS: protocol identity/deadline guards, exact files, atomic ownership, instruction rejection, 12..16-byte register jumps, RX/exec-only hook/trampoline/restore and refusal cleanup; no console calls");
}
