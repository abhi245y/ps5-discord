// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>
namespace overlay {
constexpr std::uint64_t magic=0x31434f504c564f44ULL;
constexpr unsigned version=11;
constexpr const char* parent_dir="/data/discord-overlay-poc";
constexpr const char* worker_dir="/user/data/discord-overlay-poc";
enum Function:unsigned {RootDomain,ThreadAttach,ThreadDetach,AssemblyOpen,AssemblyImage,ClassFromName,
    MethodFromName,MethodSignature,SignatureInstance,SignatureCount,SignatureReturn,TypeKind,
    CompileMethod,StringNew,RuntimeInvoke,GcNew,GcTarget,GcFree,QueryProtection,
    ObjectNew,ObjectInit,ClassProperty,PropertyGetter,PropertySetter,ObjectUnbox,TargetMprotect,TargetMmap,TargetMunmap,
    ObjectClass,ClassNameGet,ClassParent,
    // Reflection, log-only (worker describe_types): what a property's value type is on this firmware.
    SignatureParams,TypeName,ClassFromType,ClassFields,FieldName,FieldType,FieldOffset,ClassProperties,PropertyName,
    ClassMethods,MethodName, // picking an overload by its parameter types
    // Optional (0 when ShellUI lacks them; libSceSystemService): the system menu's own way of
    // holding a game while it is over it: the game's app id, the "system UI is over you"
    // status (games that can pause, pause), and controller focus (Request::shell_app).
    RunningBigApp,SetImposeStatus,SetControllerFocus,
    FunctionCount};
constexpr bool optional(unsigned i){return i>=RunningBigApp;}
inline constexpr const char* symbols[FunctionCount]={"mono_get_root_domain","mono_thread_attach","mono_thread_detach",
    "mono_domain_assembly_open","mono_assembly_get_image","mono_class_from_name","mono_class_get_method_from_name",
    "mono_method_signature","mono_signature_is_instance","mono_signature_get_param_count","mono_signature_get_return_type",
    "mono_type_get_type","mono_compile_method","mono_string_new","mono_runtime_invoke","mono_gchandle_new",
    "mono_gchandle_get_target","mono_gchandle_free","sceKernelQueryMemoryProtection",
    "mono_object_new","mono_runtime_object_init","mono_class_get_property_from_name",
    "mono_property_get_get_method","mono_property_get_set_method","mono_object_unbox","sceKernelMprotect","mmap","munmap","mono_object_get_class","mono_class_get_name","mono_class_get_parent",
    "mono_signature_get_params","mono_type_get_name","mono_class_from_mono_type","mono_class_get_fields",
    "mono_field_get_name","mono_field_get_type","mono_field_get_offset","mono_class_get_properties","mono_property_get_name",
    "mono_class_get_methods","mono_method_get_name",
    "sceSystemServiceGetAppIdOfRunningBigApp","sceShellCoreUtilSetImposeStatusFlag","sceLncUtilSetControllerFocus"};
// Service is Widget without the 60 s window: it runs while the loader keeps
// keepalive.bin fresh (Keepalive), and tears down within seconds when it stops.
enum class Mode:std::uint32_t {Resolve=0,Observe=1,Widget=2,Input=3,Service=5};
inline bool known(Mode m){return m==Mode::Resolve||m==Mode::Observe||m==Mode::Widget||m==Mode::Input||m==Mode::Service;}
inline bool draws(Mode m){return m==Mode::Widget||m==Mode::Service;}
// Managed Sce.PlayStation.Core.Input.GamePadData layout (value type, 36 bytes).
struct PadData {
    bool skip=false;
    std::uint32_t buttons=0,prev=0,down=0,up=0;
    float lx=0,ly=0,rx=0,ry=0;
};
enum class State:std::int32_t {Waiting=0,Resolved=1,Observing=2,Passed=3,Refused=-1,Failed=-2,RemoveFailed=-3};
struct Request {
    std::uint64_t signature=magic,build=0,token=0,epoch=0;
    std::uint32_t protocol=version,size=sizeof(Request);
    std::int32_t pid=0;
    // ShellUI's own app id (sceKernelGetAppInfo), whom controller focus goes to while the
    // panel is open, as Kabuki does; 0 when unknown. -1 is refused from ShellUI (0x80940004).
    std::int32_t shell_app=0;
    Mode mode=Mode::Resolve;
    std::uint64_t deadline_ms=0,functions[FunctionCount]{};
};
struct Status {
    std::uint64_t signature=magic,token=0,at_ms=0,updates=0;
    State state=State::Waiting;
    std::uint32_t scene_attempts=0;
    std::int32_t game_found=0,root_found=0,hook_installed=0,hook_removed=0;
    std::int32_t widgets_created=0,widget_removed=0,text_updates=0,create_attempts=0;
    std::uint64_t pad_polls=0;
    std::uint32_t share_events=0,chords=0,double_presses=0,last_buttons=0;
};
// Service: the loader's heartbeat (parent_dir/keepalive.bin, once a second).
struct Keepalive {std::uint64_t signature=magic,token=0,at_ms=0;};
constexpr std::uint64_t keepalive_stale_ms=5000;
// An app action for the core (worker_dir/cmd.bin): a short word, a fresh seq each time.
struct Command {std::uint64_t signature=magic,seq=0;char text[48]{};};
// A typed message for the core (worker_dir/send.bin): up to 2000 characters, UTF-8, len bytes, for
// the text chat channel (digits; empty: the call's own chat).
struct Outbox {std::uint64_t signature=magic,seq=0;std::uint32_t len=0,reserved=0;char channel[24]{};char text[8192]{};};
struct Attempt {std::uint64_t signature=magic,epoch=0,token=0;std::int32_t pid=0;std::uint32_t reserved=0;};
inline std::uint64_t now_ms(){timespec t{};if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;
    return std::uint64_t(t.tv_sec)*1000+std::uint64_t(t.tv_nsec)/1000000;}
inline bool address(std::uint64_t p){return p>=0x10000&&p<0x800000000000ULL;}
inline bool valid(const Request& r,int pid,std::uint64_t build,std::uint64_t epoch,std::uint64_t now){
    if(r.signature!=magic||r.protocol!=version||r.size!=sizeof r||r.pid!=pid||r.build!=build||r.epoch!=epoch||!r.epoch||!r.token||
       !known(r.mode)||r.deadline_ms<=now||r.deadline_ms-now>120000)return false;
    for(unsigned i=0;i<FunctionCount;i++)if(!address(r.functions[i])&&!(optional(i)&&!r.functions[i]))return false;
    return true;
}
inline bool path(char* out,std::size_t n,const char* dir,const char* file){int rc=std::snprintf(out,n,"%s/%s",dir,file);return rc>=0&&std::size_t(rc)<n;}
template<class T>bool read_file(const char* dir,const char* name,T& out){
    char p[256];if(!path(p,sizeof p,dir,name))return false;FILE* f=std::fopen(p,"rb");if(!f)return false;
    T value{};bool ok=std::fread(&value,1,sizeof value,f)==sizeof value&&std::fgetc(f)==EOF&&!std::ferror(f);
    std::fclose(f);if(ok)out=value;return ok;
}
template<class T>bool write_file(const char* dir,const char* name,const T& value){
    char p[256],tmp[272];if(!path(p,sizeof p,dir,name))return false;
    int n=std::snprintf(tmp,sizeof tmp,"%s.%d.tmp",p,getpid());if(n<0||std::size_t(n)>=sizeof tmp)return false;
    FILE* f=std::fopen(tmp,"wb");if(!f)return false;
    bool ok=std::fwrite(&value,1,sizeof value,f)==sizeof value;if(std::fclose(f))ok=false;
    if(ok&&std::rename(tmp,p)==0)return true;std::remove(tmp);return false;
}
}
