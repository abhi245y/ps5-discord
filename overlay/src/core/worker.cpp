// SPDX-License-Identifier: GPL-3.0-or-later
#include "protocol.hpp"
#include "hook.hpp"
#include "../app/overlay_ui.hpp"
#include "../app/overlay_app.cpp"
#include <atomic>
#include <cstdarg>
#include <fcntl.h>
#include <sys/file.h>
using namespace overlay;
#ifndef OVERLAY_BUILD
#define OVERLAY_BUILD 0ULL
#endif
namespace {
using Object=void;using Method=void;using Update=void(*)(Object*);
using PadCall=PadData(*)(int);
using ShareCall=void(*)(void*);
constexpr int kind_void=1,kind_class=0x12,kind_valuetype=0x11;
constexpr std::uint32_t bit_l3=0x4000,bit_r3=0x8000;
constexpr unsigned create_attempt_limit=3,create_delay_ticks=30,text_period_ticks=12;
Request request;
Hook update_hook_store,pad_hook_store,share_hook_store;
void* domain=nullptr;
void* resolved_update_entry=nullptr;
Method* find_scene=nullptr;
Method* root_getter=nullptr;
void* class_widget=nullptr,*class_panel=nullptr,*class_label=nullptr,*class_font=nullptr;
void* class_application=nullptr,*class_scene=nullptr;
Method* method_append=nullptr,*method_find=nullptr;
std::atomic<Update> original{nullptr};
std::atomic<PadCall> pad_original{nullptr};
std::atomic<ShareCall> share_original{nullptr};
std::atomic<bool> enabled{false},input_enabled{false},teardown{false};
std::atomic<std::uint64_t> updates{0},pad_polls{0};
std::atomic<unsigned> scene_attempts{0},create_attempts{0},text_updates{0},share_events{0},chords{0},double_presses{0};
std::atomic<std::uint32_t> last_buttons{0};
std::atomic<std::uint64_t> last_share_at{0};
std::atomic<int> game_found{0},root_found{0},widgets_created{0},widget_removed{0};
std::atomic<bool> chord_held{false},gesture_requested{false};
// Capture (the app's panel is open): pad_hook ORs newly pressed buttons in and keeps the
// left stick; the UI thread hands them to app::on_pad.
std::atomic<bool> capture{false};
std::atomic<std::uint32_t> pending_down{0},stick_x{0},stick_y{0};
std::atomic<unsigned> pad_logged{0};
std::uint64_t command_seq=0;                 // UI thread only
// UI thread only. Pinned (1) handles like the reference: scene graph objects
// and the captured Application instance survive GC moves.
std::uint32_t app_handle=0,root_scene_handle=0,scene_handle=0,scene_root_handle=0;
bool removal_requested=false,cleanup_retry_used=false;
char last_text[128]{};                       // UI thread only
float ui_screen_w=1920,ui_screen_h=1080;     // UI thread only
struct WidgetSlot{std::uint32_t handle=0;bool label=false;};
constexpr std::uint32_t widget_limit=32;
WidgetSlot widget_slots[widget_limit];       // UI thread only, app-facing ids
static_assert(std::atomic<Update>::is_always_lock_free&&std::atomic<PadCall>::is_always_lock_free&&
    std::atomic<ShareCall>::is_always_lock_free&&std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(sizeof(PadData)==36&&offsetof(PadData,buttons)==4&&offsetof(PadData,down)==12&&
    offsetof(PadData,lx)==20&&offsetof(PadData,ry)==32);
template<class T>T fn(Function which){return reinterpret_cast<T>(request.functions[which]);}
void note(const char* format,...){
    char p[256];if(!path(p,sizeof p,worker_dir,"worker.log"))return;
    FILE* f=std::fopen(p,"a");if(!f)return;
    std::fprintf(f,"[overlay-worker pid=%d token=%llu ms=%llu] ",getpid(),(unsigned long long)request.token,(unsigned long long)now_ms());
    va_list ap;va_start(ap,format);std::vfprintf(f,format,ap);va_end(ap);std::fputc('\n',f);std::fclose(f);
}
void* invoke(Method* method,void* instance,void** args,bool& ok){
    void* exception=nullptr;void* result=fn<void*(*)(void*,void*,void**,void**)>(RuntimeInvoke)(method,instance,args,&exception);
    ok=exception==nullptr;return result;
}
bool signature(Method* m,bool instance,unsigned count,int return_kind){
    void* sig=fn<void*(*)(void*)>(MethodSignature)(m);if(!sig)return false;
    if(bool(fn<int(*)(void*)>(SignatureInstance)(sig))!=instance||fn<unsigned(*)(void*)>(SignatureCount)(sig)!=count)return false;
    void* type=fn<void*(*)(void*)>(SignatureReturn)(sig);return type&&fn<int(*)(void*)>(TypeKind)(type)==return_kind;
}
void* image(const char* path){
    void* assembly=fn<void*(*)(void*,const char*)>(AssemblyOpen)(domain,path);
    return assembly?fn<void*(*)(void*)>(AssemblyImage)(assembly):nullptr;
}
Method* method(void* img,const char* ns,const char* klass,const char* name,int count){
    if(!img)return nullptr;void* type=fn<void*(*)(void*,const char*,const char*)>(ClassFromName)(img,ns,klass);
    return type?fn<void*(*)(void*,const char*,int)>(MethodFromName)(type,name,count):nullptr;
}
// Property access: the compiled accessor is called directly for ABI-simple
// parameters and through mono_runtime_invoke for value-type parameters, which
// boxes from the pointed-to bytes.
// kabuki-style: every accessor through mono_runtime_invoke (13.x-proven);
// managed exceptions are logged with their type name.
void* invoke_ex(Method* method,void* instance,void** args,bool& ok,void** exception){
    void* result=fn<void*(*)(void*,void*,void**,void**)>(RuntimeInvoke)(method,instance,args,exception);
    ok=*exception==nullptr;return result;
}
bool describe_exception(const char* what,void* exception){
    if(!exception)return true;
    void* type=fn<void*(*)(void*)>(ObjectClass)(exception);
    const char* name=type?fn<const char*(*)(void*)>(ClassNameGet)(type):nullptr;
    note("EXC %s type=%s",what,name?name:"?");
    return false;
}
// value: for value-type parameters a pointer to the bytes; for reference
// parameters (string, font) the object pointer itself.
Method* method_inherited(void* klass,const char* name,int count){
    for(void* walk=klass;walk;){
        if(Method* m=fn<Method*(*)(void*,const char*,int)>(MethodFromName)(walk,name,count))return m;
        walk=fn<void*(*)(void*)>(ClassParent)(walk);}
    return nullptr;
}
bool prop_get_object(void* klass,void* obj,const char* name,void** out){
    if(!klass||!obj||!out)return false;
    void* prop=fn<void*(*)(void*,const char*)>(ClassProperty)(klass,name);
    if(!prop){note("PROP miss get %s",name);return false;}
    Method* getter=fn<Method*(*)(void*)>(PropertyGetter)(prop);
    if(!getter){note("PROP write-only %s",name);return false;}
    void* exception=nullptr;bool ok=false;
    void* result=invoke_ex(getter,obj,nullptr,ok,&exception);
    if(!ok){describe_exception(name,exception);return false;}
        *out=result;return result!=nullptr;
}
bool prop_get_bool(void* klass,void* obj,const char* name,bool* out){
    void* result=nullptr;
    if(!prop_get_object(klass,obj,name,&result))return false;
    void* unboxed=fn<void*(*)(void*)>(ObjectUnbox)(result);
    if(!unboxed)return false;
    *out=*static_cast<unsigned char*>(unboxed)!=0;return true;
}
bool prop_set(void* klass,void* obj,const char* name,void* value){
    if(!klass||!obj)return false;
    void* prop=fn<void*(*)(void*,const char*)>(ClassProperty)(klass,name);
    if(!prop){note("PROP miss %s",name);return false;}
    Method* setter=fn<Method*(*)(void*)>(PropertySetter)(prop);
    if(!setter){note("PROP read-only %s",name);return false;}
    void* args[1];args[0]=value;void* exception=nullptr;bool ok=false;
    invoke_ex(setter,obj,args,ok,&exception);
    if(!ok){describe_exception(name,exception);return false;}
    return true;
}
bool prop_set_float(void* k,void* o,const char* n,float v){return prop_set(k,o,n,&v);}
bool prop_set_int(void* k,void* o,const char* n,int v){return prop_set(k,o,n,&v);}
bool prop_get_float(void* klass,void* obj,const char* name,float& out){
    if(!klass||!obj)return false;
    void* prop=fn<void*(*)(void*,const char*)>(ClassProperty)(klass,name);
    if(!prop){note("PROP miss get %s",name);return false;}
    Method* getter=fn<Method*(*)(void*)>(PropertyGetter)(prop);
    if(!getter){note("PROP write-only %s",name);return false;}
    void* exception=nullptr;bool ok=false;
    void* result=invoke_ex(getter,obj,nullptr,ok,&exception);
    if(!ok||!result){describe_exception(name,exception);return false;}
    void* unboxed=fn<void*(*)(void*)>(ObjectUnbox)(result);
    if(!unboxed)return false;
    out=*static_cast<float*>(unboxed);return true;
}
void observe_scene(){
    scene_attempts.fetch_add(1,std::memory_order_relaxed);
    auto text=fn<void*(*)(void*,const char*)>(StringNew)(domain,"Game");if(!text)return;
    void* args[]={text};bool ok=false;void* scene=invoke(find_scene,nullptr,args,ok);if(!ok||!scene)return;
    auto handle=fn<std::uint32_t(*)(void*,int)>(GcNew)(scene,0);if(!handle)return;
    scene=fn<void*(*)(std::uint32_t)>(GcTarget)(handle);
    if(scene){game_found.store(1,std::memory_order_release);
        void* root=invoke(root_getter,scene,nullptr,ok);if(ok&&root)root_found.store(1,std::memory_order_release);}
    fn<void(*)(std::uint32_t)>(GcFree)(handle);
}
bool append_child(void* parent,void* child){
    void* args[1];args[0]=child;bool ok=false;invoke(method_append,parent,args,ok);return ok;
}
bool load_text(char* out,std::size_t n){
    char p[256];if(!path(p,sizeof p,worker_dir,"text.bin"))return false;
    FILE* f=std::fopen(p,"rb");if(!f)return false;
    char raw[128];std::size_t got=std::fread(raw,1,sizeof raw,f);
    // A full buffer means the file is too long: never display a silent prefix.
    bool ok=!std::ferror(f)&&got<sizeof raw&&!std::memchr(raw,0,got);
    std::fclose(f);if(!ok||!got)return false;
    while(got&&(raw[got-1]=='\n'||raw[got-1]=='\r'))--got;
    if(got&&(raw[got-1]&0xc0)==0x80){ // may end inside a multi-byte sequence
        std::size_t run=1;
        while(run<got&&run<4&&(raw[got-run]&0xc0)==0x80)++run;
        std::uint8_t lead=raw[got-run];
        std::size_t need=(lead&0xe0)==0xc0?2:(lead&0xf0)==0xe0?3:(lead&0xf8)==0xf0?4:0;
        if(need!=run)got-=(need==0?run-1:run); // orphan continuations vs truncated/malformed sequence
    }
    if(!got||got+1>n)return false;
    std::memcpy(out,raw,got);out[got]=0;return true;
}
// ---- oui::Builder engine implementation (all UI-thread) --------------------
void* slot_object(std::uint32_t index){
    return index&&index<=widget_limit&&widget_slots[index-1].handle?
        fn<void*(*)(std::uint32_t)>(GcTarget)(widget_slots[index-1].handle):nullptr;}
std::uint32_t slot_store(void* object,bool label){
    for(auto& slot:widget_slots){
        if(slot.handle)continue;
        slot.handle=fn<std::uint32_t(*)(void*,int)>(GcNew)(object,1);
        slot.label=label;
        return slot.handle?std::uint32_t(&slot-widget_slots)+1:0;}
    return 0;}
void slots_release(){
    for(auto& slot:widget_slots){
        if(slot.handle){fn<void(*)(std::uint32_t)>(GcFree)(slot.handle);slot.handle=0;}}
}
void* engine_add_panel(void*,const oui::Rect* r,const oui::Color* c){
    void* root=scene_root_handle?fn<void*(*)(std::uint32_t)>(GcTarget)(scene_root_handle):nullptr;
    if(!root||!r)return nullptr;
    void* panel=fn<void*(*)(void*,void*)>(ObjectNew)(domain,class_panel);
    if(!panel)return nullptr;
    fn<void(*)(void*)>(ObjectInit)(panel);
    int passive=0,shown=1;
    if(!prop_set_int(class_panel,panel,"Focusable",passive)||
       !prop_set_float(class_panel,panel,"X",r->x)||
       !prop_set_float(class_panel,panel,"Y",r->y)||
       !prop_set_float(class_panel,panel,"Width",r->w)||
       !prop_set_float(class_panel,panel,"Height",r->h)||
       !prop_set_int(class_panel,panel,"Visible",shown)){note("UI panel props failed");return nullptr;}
    (void)prop_set_float(class_panel,panel,"Opacity",c->a);
    float back[4]={c->r,c->g,c->b,1.f};
    (void)prop_set(class_panel,panel,"BackgroundColor",back);
    if(!append_child(root,panel)){note("UI panel attach failed");return nullptr;}
    std::uint32_t id=slot_store(panel,false);
    return id?reinterpret_cast<void*>(std::uintptr_t(id)):nullptr;}
void* engine_add_label(void*,void* parent,const oui::Rect* r,const char* text){
    void* owner=parent?slot_object(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(parent)))
                      :(scene_root_handle?fn<void*(*)(std::uint32_t)>(GcTarget)(scene_root_handle):nullptr);
    if(!owner||!r||!text)return nullptr;
    void* label=fn<void*(*)(void*,void*)>(ObjectNew)(domain,class_label);
    if(!label)return nullptr;
    fn<void(*)(void*)>(ObjectInit)(label);
    int passive=0,shown=1;
    if(!prop_set_int(class_label,label,"Focusable",passive)||
       !prop_set_float(class_label,label,"X",r->x)||
       !prop_set_float(class_label,label,"Y",r->y)||
       !prop_set_float(class_label,label,"Width",r->w)||
       !prop_set_float(class_label,label,"Height",r->h)||
       !prop_set_int(class_label,label,"Visible",shown)||
       !prop_set(class_label,label,"Text",fn<void*(*)(void*,const char*)>(StringNew)(domain,text))){
        note("UI label props failed");return nullptr;}
    if(!append_child(owner,label)){note("UI label attach failed");return nullptr;}
    std::uint32_t id=slot_store(label,true);
    return id?reinterpret_cast<void*>(std::uintptr_t(id)):nullptr;}
bool engine_set_text(void*,void* widget,const char* text){
    void* label=slot_object(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(widget)));
    if(!label||!text)return false;
    return prop_set(class_label,label,"Text",fn<void*(*)(void*,const char*)>(StringNew)(domain,text));}
bool engine_set_visible(void*,void* widget,bool show){
    int state=show?1:0;
    if(!widget){
        void* root=scene_root_handle?fn<void*(*)(std::uint32_t)>(GcTarget)(scene_root_handle):nullptr;
        return root&&prop_set_int(class_widget,root,"Visible",state);}
    void* object=slot_object(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(widget)));
    if(!object)return false;
    void* type=fn<void*(*)(void*)>(ObjectClass)(object);
    return type&&prop_set_int(type,object,"Visible",state);}
bool engine_set_color(void*,void* panel,const oui::Color* c){
    void* object=slot_object(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(panel)));
    if(!object||!c)return false;
    float back[4]={c->r,c->g,c->b,c->a};
    return prop_set(class_panel,object,"BackgroundColor",back);}
float engine_screen_w(void*){return ui_screen_w;}
float engine_screen_h(void*){return ui_screen_h;}
bool engine_set_capture(void*,bool on){
    if(on&&!capture.load(std::memory_order_relaxed))pending_down.store(0,std::memory_order_relaxed);
    capture.store(on,std::memory_order_release);return true;}
bool engine_send(void*,const char* text){
    if(!text)return false;
    Command c;c.seq=now_ms()*64+(++command_seq%64); // grows across runs too
    std::snprintf(c.text,sizeof c.text,"%s",text);
    note("COMMAND %s",c.text);
    return write_file(worker_dir,"cmd.bin",c);}
constexpr oui::Builder::Vt engine_vt{
    engine_add_panel,engine_add_label,engine_set_text,engine_set_visible,engine_set_color,
    engine_screen_w,engine_screen_h,engine_set_capture,engine_send};
oui::Builder ui_builder(){return oui::Builder{&engine_vt,nullptr};}
void refresh_text(){
    char text[128];
    if(load_text(text,sizeof text)&&std::strcmp(text,last_text)){
        std::snprintf(last_text,sizeof last_text,"%s",text);
        oui::Builder b=ui_builder();
        oui::app::on_text(text,b);
        text_updates.fetch_add(1,std::memory_order_relaxed);}
}
bool try_create(){
    if(create_attempts.fetch_add(1,std::memory_order_relaxed)>=create_attempt_limit)return false;
    // Kabuki-proven layer: our OWN scene added to Application.RootScene with
    // ScreenRenderOverlay - the Game container scene never renders our widgets.
    void* app=app_handle?fn<void*(*)(std::uint32_t)>(GcTarget)(app_handle):nullptr;
    if(!app){note("WIDGET no application instance yet");return false;}
    void* root_scene=nullptr;
    if(!prop_get_object(class_application,app,"RootScene",&root_scene)){note("WIDGET RootScene missing");return false;}
    std::uint32_t root_pin=fn<std::uint32_t(*)(void*,int)>(GcNew)(root_scene,1);
    void* scene=fn<void*(*)(void*,void*)>(ObjectNew)(domain,class_scene);
    if(!scene||!root_pin){note("WIDGET scene allocation failed");return false;}
    fn<void(*)(void*)>(ObjectInit)(scene);
    std::uint32_t scene_pin=fn<std::uint32_t(*)(void*,int)>(GcNew)(scene,1);
    if(!scene_pin){note("WIDGET scene pin failed");return false;}
    int yes=1,no=0;
    if(!prop_set_int(class_scene,scene,"Focusable",no)||
       !prop_set_int(class_scene,scene,"ScreenFixedTransform",yes)||
       !prop_set_int(class_scene,scene,"ScreenRenderOverlay",yes)){
        note("WIDGET scene flags failed");fn<void(*)(std::uint32_t)>(GcFree)(scene_pin);
        fn<void(*)(std::uint32_t)>(GcFree)(root_pin);return false;}
    void* root=nullptr;
    if(!prop_get_object(class_scene,scene,"RootWidget",&root)){note("WIDGET scene RootWidget missing");        fn<void(*)(std::uint32_t)>(GcFree)(scene_pin);fn<void(*)(std::uint32_t)>(GcFree)(root_pin);return false;}
    std::uint32_t root_widget_pin=fn<std::uint32_t(*)(void*,int)>(GcNew)(root,1);
    if(!root_widget_pin){note("WIDGET root widget pin failed");        fn<void(*)(std::uint32_t)>(GcFree)(scene_pin);fn<void(*)(std::uint32_t)>(GcFree)(root_pin);return false;}
    float screen=0;if(!prop_get_float(class_widget,root,"Width",screen)||screen<=1)screen=1920;
    ui_screen_w=screen;ui_screen_h=1080;
    // Handles publish before the app builds: engine_add_* resolves the scene
    // root through scene_root_handle.
    root_scene_handle=root_pin;scene_handle=scene_pin;scene_root_handle=root_widget_pin;

    // The application builds its UI through the oui API; the engine only
    // provides the scene and the Builder.
    oui::Builder builder=ui_builder();
    if(!oui::app::build(builder)){
        note("WIDGET app build refused");
        slots_release(); // anything the half-built UI created is never published
        fn<void(*)(std::uint32_t)>(GcFree)(root_widget_pin);fn<void(*)(std::uint32_t)>(GcFree)(scene_pin);
        fn<void(*)(std::uint32_t)>(GcFree)(root_pin);
        widgets_created.store(-1,std::memory_order_release);
        return false;}
    // Publish the scene. AddScene lives on the runtime class of the root scene
    // object (inherited), exactly like the reference.
    void* root_class=fn<void*(*)(void*)>(ObjectClass)(root_scene);
    Method* add_scene=method_inherited(root_class,"AddScene",1);
    if(!add_scene){note("WIDGET AddScene method missing");
        slots_release();
        fn<void(*)(std::uint32_t)>(GcFree)(root_widget_pin);fn<void(*)(std::uint32_t)>(GcFree)(scene_pin);
        fn<void(*)(std::uint32_t)>(GcFree)(root_pin);
        widgets_created.store(-1,std::memory_order_release);
        return false;}
    void* scene_args[1];scene_args[0]=scene;void* exception=nullptr;bool ok_invoke=false;
    invoke_ex(add_scene,root_scene,scene_args,ok_invoke,&exception);
    if(!ok_invoke){describe_exception("AddScene",exception);note("WIDGET AddScene failed");
        slots_release();
        fn<void(*)(std::uint32_t)>(GcFree)(root_widget_pin);fn<void(*)(std::uint32_t)>(GcFree)(scene_pin);
        fn<void(*)(std::uint32_t)>(GcFree)(root_pin);
        widgets_created.store(-1,std::memory_order_release);
        return false;}
    removal_requested=false;
    bool appended=false;(void)prop_get_bool(class_scene,scene,"IsAppended",&appended);
    widgets_created.store(1,std::memory_order_release);
    note("WIDGET scene added screen=%.0f appended=%d",screen,appended);
    return true;}
// 0 pending (scene removal queued / still appended), 1 done, -1 error.
// The reference tears the whole scene down via RemoveFromSuperScene and polls
// IsAppended across UI callbacks; per-widget RemoveFromParent cannot finish it.
int remove_widgets(){
    void* scene=scene_handle?fn<void*(*)(std::uint32_t)>(GcTarget)(scene_handle):nullptr;
    if(!scene){note("WIDGET removed (no scene)");return scene_handle?-1:1;}
    bool appended=false;
    if(!prop_get_bool(class_scene,scene,"IsAppended",&appended)){note("WIDGET IsAppended unreadable");return 0;}
    if(appended){
        if(!removal_requested){
            Method* remove=method_inherited(class_scene,"RemoveFromSuperScene",0);
            if(!remove){note("WIDGET RemoveFromSuperScene missing");return -1;}
            void* exception=nullptr;bool ok_invoke=false;
            invoke_ex(remove,scene,nullptr,ok_invoke,&exception);
            if(!ok_invoke){describe_exception("RemoveFromSuperScene",exception);return -1;}
            removal_requested=true;note("WIDGET scene removal requested");}
        return 0;} // still appended: next UI callback re-checks
    slots_release();
    fn<void(*)(std::uint32_t)>(GcFree)(scene_root_handle);scene_root_handle=0;
    fn<void(*)(std::uint32_t)>(GcFree)(scene_handle);scene_handle=0;
    fn<void(*)(std::uint32_t)>(GcFree)(root_scene_handle);root_scene_handle=0;
    if(app_handle){fn<void(*)(std::uint32_t)>(GcFree)(app_handle);app_handle=0;}
    note("WIDGET scene removed and handles freed");
    return 1;
}
void update_hook(Object* instance){
    Update prior=original.load(std::memory_order_acquire);if(prior)prior(instance);
    if(!enabled.load(std::memory_order_acquire))return;
    auto tick=updates.fetch_add(1,std::memory_order_relaxed)+1;
    if(!root_found.load(std::memory_order_acquire)&&scene_attempts.load()<3&&(tick==1||tick==61||tick==121))observe_scene();
    if(!draws(request.mode))return;
    // The Update callback's instance IS the Application; pin it once so scene
    // creation can ask it for RootScene (captured before the stop check only
    // while we may still build).
    if(!teardown.load(std::memory_order_acquire)&&!app_handle&&instance)
        app_handle=fn<std::uint32_t(*)(void*,int)>(GcNew)(instance,1);
    // Stop is checked first so no new widgets appear after a stop request, even
    // when creation never succeeded.
    if(teardown.load(std::memory_order_acquire)){
        int state=widget_removed.load(std::memory_order_relaxed);
        // 0: poll every callback (queued removal may take frames). -1 with live
        // scene handles: one bounded retry. 1: done.
        if(state==0){
            int progress=remove_widgets();
            if(progress!=0)widget_removed.store(progress,std::memory_order_release);}
        else if(state==-1&&!cleanup_retry_used&&scene_handle){
            cleanup_retry_used=true;
            widget_removed.store(remove_widgets(),std::memory_order_release);}
        return;}
    if(widgets_created.load(std::memory_order_acquire)<=0){
        if(tick>=create_delay_ticks&&create_attempts.load(std::memory_order_relaxed)<create_attempt_limit){
            if(try_create()&&widgets_created.load()>0)refresh_text();}
        return;}
    if((tick%text_period_ticks)==0)refresh_text();
    oui::Builder builder=ui_builder();
    oui::app::on_tick(tick);
    if(gesture_requested.exchange(false,std::memory_order_acq_rel)){
        oui::app::on_gesture(oui::Gesture::Chord,builder);
        note("GESTURE chords=%u",chords.load());}
    if(capture.load(std::memory_order_acquire)){
        float x=0,y=0;std::uint32_t bx=stick_x.load(std::memory_order_relaxed),by=stick_y.load(std::memory_order_relaxed);
        std::memcpy(&x,&bx,4);std::memcpy(&y,&by,4);
        oui::app::on_pad(pending_down.exchange(0,std::memory_order_acq_rel),x,y,builder);}
}
void note_share(std::uint64_t at){
    std::uint64_t prior=last_share_at.load(std::memory_order_relaxed);
    unsigned n=share_events.fetch_add(1,std::memory_order_relaxed)+1;
    if(prior&&at>=prior&&at-prior<=350)double_presses.fetch_add(1,std::memory_order_relaxed);
    last_share_at.store(at,std::memory_order_relaxed);
    if(n<=5||n%50==0)note("SHARE n=%u delta=%lldms buttons=%x",n,(long long)(prior?at-prior:0),
        last_buttons.load(std::memory_order_relaxed));
}
PadData pad_hook(int device){
    PadCall prior=pad_original.load(std::memory_order_acquire);
    PadData data{};
    if(prior)data=prior(device);
    if(!input_enabled.load(std::memory_order_acquire))return data;
    pad_polls.fetch_add(1,std::memory_order_relaxed);
    std::uint32_t buttons=data.buttons;
    last_buttons.store(buttons,std::memory_order_relaxed);
    if((buttons&(bit_l3|bit_r3))==(bit_l3|bit_r3)){
        if(!chord_held.exchange(true,std::memory_order_relaxed)){
            chords.fetch_add(1,std::memory_order_relaxed);
            gesture_requested.store(true,std::memory_order_release);}}
    else chord_held.store(false,std::memory_order_relaxed);
    if(capture.load(std::memory_order_acquire)){
        std::uint32_t down=data.down&~(bit_l3|bit_r3);
        if(down){
            pending_down.fetch_or(down,std::memory_order_acq_rel);
            // Calibration evidence: which bit each button is (only Cross, L3, R3 are known).
            if(pad_logged.fetch_add(1,std::memory_order_relaxed)<200)note("PAD down=%x",down);}
        std::uint32_t x,y;std::memcpy(&x,&data.lx,4);std::memcpy(&y,&data.ly,4);
        stick_x.store(x,std::memory_order_relaxed);stick_y.store(y,std::memory_order_relaxed);}
    return data;
}
void share_hook(void* event){
    if(ShareCall prior=share_original.load(std::memory_order_acquire))prior(event);
    if(!input_enabled.load(std::memory_order_acquire))return;
    note_share(now_ms());
}
bool resolve_update_methods(){
    void* pui=image("/system_ex/common_ex/lib/Sce.PlayStation.PUI.dll");
    void* app=image("/system_ex/common_ex/lib/Sce.Vsh.ShellUI.AppSystem.dll");
    Method* update=method(pui,"Sce.PlayStation.PUI","Application","Update",0);
    find_scene=method(app,"Sce.Vsh.ShellUI.AppSystem","LayerManager","FindContainerSceneByPath",1);
    root_getter=method(pui,"Sce.PlayStation.PUI.UI2","Scene","get_RootWidget",0);
    if(!update||!find_scene||!root_getter||!signature(update,true,0,kind_void)||
       !signature(find_scene,false,1,kind_class)||!signature(root_getter,true,0,kind_class)){
        note("REFUSED required methods/signature missing update=%p find=%p root=%p",update,find_scene,root_getter);return false;}
    if(draws(request.mode)){
        class_application=fn<void*(*)(void*,const char*,const char*)>(ClassFromName)(pui,"Sce.PlayStation.PUI","Application");
        class_scene=fn<void*(*)(void*,const char*,const char*)>(ClassFromName)(pui,"Sce.PlayStation.PUI.UI2","Scene");
        Method* root_scene_getter=class_application?fn<Method*(*)(void*,const char*,int)>(MethodFromName)(class_application,"get_RootScene",0):nullptr;
        class_widget=fn<void*(*)(void*,const char*,const char*)>(ClassFromName)(pui,"Sce.PlayStation.PUI.UI2","Widget");
        class_panel=fn<void*(*)(void*,const char*,const char*)>(ClassFromName)(pui,"Sce.PlayStation.PUI.UI2","Panel");
        class_label=fn<void*(*)(void*,const char*,const char*)>(ClassFromName)(pui,"Sce.PlayStation.PUI.UI2","Label");
        class_font=fn<void*(*)(void*,const char*,const char*)>(ClassFromName)(pui,"Sce.PlayStation.PUI.UI2","UIFont");
        // Classes are validated before every MethodFromName: a null class is not a
        // safe lookup input in the real runtime, only in our fake.
        if(!class_widget||!class_panel||!class_label||!class_font||!class_application||!class_scene){
            note("REFUSED widget classes missing widget=%p panel=%p label=%p font=%p app=%p scene=%p",
                class_widget,class_panel,class_label,class_font,class_application,class_scene);return false;}
        if(!root_scene_getter||!signature(root_scene_getter,true,0,kind_class)){
            note("REFUSED Application.RootScene signature missing getter=%p",root_scene_getter);return false;}
        method_append=fn<Method*(*)(void*,const char*,int)>(MethodFromName)(class_widget,"AppendChild",1);
        method_find=fn<Method*(*)(void*,const char*,int)>(MethodFromName)(class_widget,"FindWidgetByName",1);
        Method* font_ctor=fn<Method*(*)(void*,const char*,int)>(MethodFromName)(class_font,".ctor",3);
        Method* remove_scene=fn<Method*(*)(void*,const char*,int)>(MethodFromName)(class_scene,"RemoveFromSuperScene",0);
        if(!method_append||!font_ctor||!remove_scene||
           !signature(method_append,true,1,kind_void)||!signature(font_ctor,true,3,kind_void)||
           !signature(remove_scene,true,0,kind_void)){
            note("REFUSED widget methods/signature missing append=%p find=%p detach=%p font_ctor=%p",
                method_append,method_find,font_ctor);return false;}}
    void* entry=fn<void*(*)(void*)>(CompileMethod)(update);
    note("RESOLVED Update=%p FindContainerSceneByPath=%p RootWidget=%p",entry,find_scene,root_getter);
    if(entry){ // log-only evidence for the hook stage. Never read code bytes:
        // Mono JIT pages here are not data-readable (a memcpy at the entry
        // killed SceShellUI on console, 2026-11-02).
        auto query=fn<ProtectionQuery>(QueryProtection);
        void* lo=nullptr,*hi=nullptr;int prot=0;
        int qrc=query?query(entry,&lo,&hi,&prot):-999;
        note("HOOKDIAG entry=%p qrc=%d prot=%d lo=%p hi=%p",entry,qrc,prot,lo,hi);}
    resolved_update_entry=entry;
    return entry&&now_ms()<request.deadline_ms;
}
void hook_step(const char* text){note("HOOKSTEP %s",text);}
bool prepare_update_hook(void* entry){
    auto query=fn<ProtectionQuery>(QueryProtection);
    note("HOOKSTEP prepare begin");
    if(!prepare_hook(update_hook_store,entry,reinterpret_cast<void*>(&update_hook),query,fn<MprotectCall>(TargetMprotect),fn<MmapCall>(TargetMmap),fn<MunmapCall>(TargetMunmap),hook_step)){
        note("REFUSED Update alignment, permissions or relocatable prologue");return false;}
    auto length=native_patch_length(update_hook_store.before);
    note("HOOKSTEP prepared len=%zu bytes=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x tramp=%p",
        (std::size_t)length,
        update_hook_store.before[0],update_hook_store.before[1],update_hook_store.before[2],update_hook_store.before[3],
        update_hook_store.before[4],update_hook_store.before[5],update_hook_store.before[6],update_hook_store.before[7],
        update_hook_store.before[8],update_hook_store.before[9],update_hook_store.before[10],update_hook_store.before[11],
        update_hook_store.before[12],update_hook_store.before[13],update_hook_store.before[14],update_hook_store.before[15],
        update_hook_store.trampoline);
    original.store(reinterpret_cast<Update>(update_hook_store.trampoline),std::memory_order_release);
    note("HOOKSTEP install begin");
    if(!install_hook(update_hook_store,query)){note("REFUSED hook installation/protection restore");return false;}
    note("HOOKSTEP installed");
    return true;
}
bool install_pad_hook(){
    void* core=image("/system_ex/common_ex/lib/Sce.PlayStation.Core.dll");
    Method* get_data=method(core,"Sce.PlayStation.Core.Input","GamePad","GetData",1);
    if(!get_data||!signature(get_data,false,1,kind_valuetype)){
        note("REFUSED GamePad.GetData missing for toggle");return false;}
    void* pad_entry=fn<void*(*)(void*)>(CompileMethod)(get_data);
    if(!pad_entry||now_ms()>=request.deadline_ms)return false;
    auto query=fn<ProtectionQuery>(QueryProtection);
    if(!prepare_hook(pad_hook_store,pad_entry,reinterpret_cast<void*>(&pad_hook),query,
                     fn<MprotectCall>(TargetMprotect),fn<MmapCall>(TargetMmap),fn<MunmapCall>(TargetMunmap))){
        note("REFUSED pad hook preparation");return false;}
    pad_original.store(reinterpret_cast<PadCall>(pad_hook_store.trampoline),std::memory_order_release);
    if(!install_hook(pad_hook_store,query)){note("REFUSED pad hook installation");return false;}
    return true;
}
bool resolve_input_methods(){
    void* core=image("/system_ex/common_ex/lib/Sce.PlayStation.Core.dll");
    void* capture=image("/system_ex/common_ex/lib/Sce.Vsh.ShellUI.CaptureMenu.dll");
    Method* get_data=method(core,"Sce.PlayStation.Core.Input","GamePad","GetData",1);
    Method* on_share=method(capture,"Sce.Vsh.ShellUI.CaptureMenu","EventManager","OnShareButton",1);
    if(!get_data||!on_share||!signature(get_data,false,1,kind_valuetype)||!signature(on_share,false,1,kind_void)){
        note("REFUSED input methods/signature missing get=%p share=%p",get_data,on_share);return false;}
    void* pad_entry=fn<void*(*)(void*)>(CompileMethod)(get_data);
    void* share_entry=fn<void*(*)(void*)>(CompileMethod)(on_share);
    note("RESOLVED GamePad.GetData=%p EventManager.OnShareButton=%p",pad_entry,share_entry);
    if(!pad_entry||!share_entry||now_ms()>=request.deadline_ms)return false;
    auto query=fn<ProtectionQuery>(QueryProtection);
    if(!prepare_hook(pad_hook_store,pad_entry,reinterpret_cast<void*>(&pad_hook),query,fn<MprotectCall>(TargetMprotect),fn<MmapCall>(TargetMmap),fn<MunmapCall>(TargetMunmap),hook_step)||
       !prepare_hook(share_hook_store,share_entry,reinterpret_cast<void*>(&share_hook),query,fn<MprotectCall>(TargetMprotect),fn<MmapCall>(TargetMmap),fn<MunmapCall>(TargetMunmap),hook_step)){
        note("REFUSED input hook preparation");return false;}
    pad_original.store(reinterpret_cast<PadCall>(pad_hook_store.trampoline),std::memory_order_release);
    share_original.store(reinterpret_cast<ShareCall>(share_hook_store.trampoline),std::memory_order_release);
    if(!install_hook(pad_hook_store,query)||!install_hook(share_hook_store,query)){
        note("REFUSED input hook installation");return false;}
    input_enabled.store(true,std::memory_order_release);
    return true;
}
void publish(State state){
    Status s;s.token=request.token;s.at_ms=now_ms();s.state=state;s.updates=updates.load();
    s.scene_attempts=scene_attempts.load();s.game_found=game_found.load();s.root_found=root_found.load();
    s.hook_installed=(update_hook_store.was_installed?1:0)+(pad_hook_store.was_installed?1:0)+(share_hook_store.was_installed?1:0);
    s.hook_removed=(update_hook_store.was_installed&&!update_hook_store.installed?1:0)+
        (pad_hook_store.was_installed&&!pad_hook_store.installed?1:0)+
        (share_hook_store.was_installed&&!share_hook_store.installed?1:0);
    s.widgets_created=widgets_created.load();s.widget_removed=widget_removed.load();
    s.create_attempts=create_attempts.load();s.text_updates=text_updates.load();
    s.pad_polls=pad_polls.load();s.share_events=share_events.load();s.chords=chords.load();
    s.double_presses=double_presses.load();s.last_buttons=last_buttons.load();
    (void)write_file(worker_dir,"status.bin",s);
}
State run(){
    domain=fn<void*(*)()>(RootDomain)();if(!domain){note("REFUSED root domain missing");return State::Refused;}
    void* thread=fn<void*(*)(void*)>(ThreadAttach)(domain);if(!thread){note("REFUSED Mono thread attach");return State::Refused;}
    State result=State::Refused;
    do {
        if(request.mode==Mode::Input){
            if(!resolve_input_methods())break;
            note("OBSERVING pad and share for at most60seconds; pure pass-through, no button changes");
        } else {
            void* entry=nullptr;
            if(!resolve_update_methods()||!(entry=resolved_update_entry))break;
            if(request.mode==Mode::Resolve){result=State::Resolved;break;}
            if(!prepare_update_hook(entry))break;
            enabled.store(true,std::memory_order_release);
            if(draws(request.mode)&&install_pad_hook())
                input_enabled.store(true,std::memory_order_release);
            note("OBSERVING Update%s for %s; %s",
                draws(request.mode)?" plus the app's widgets":"",
                request.mode==Mode::Service?"as long as the loader's keepalive is fresh":
                request.mode==Mode::Widget?"at most 60 seconds":"at most 8 seconds",
                draws(request.mode)?"widgets removed on stop or deadline":"no widgets, pad hooks or focus calls");
        }
        auto deadline=now_ms()+(request.mode==Mode::Widget||request.mode==Mode::Input?60000:8000);
        if(deadline>request.deadline_ms)deadline=request.deadline_ms;
        const std::uint64_t began=now_ms();
        for(;;){
            std::uint64_t stop=0;if(read_file(worker_dir,"stop.bin",stop)&&stop==request.token)break;
            if(request.mode==Mode::Service){
                // No fixed end: the loader's heartbeat keeps it. A stale or foreign one
                // (the loader is gone) ends it, after a grace for the first beat.
                Keepalive k;const std::uint64_t now=now_ms();
                const bool fresh=read_file(worker_dir,"keepalive.bin",k)&&k.signature==magic&&
                    k.token==request.token&&k.at_ms<=now+1000&&(k.at_ms>now||now-k.at_ms<keepalive_stale_ms);
                if(!fresh&&now-began>2*keepalive_stale_ms){note("SERVICE keepalive stale; tearing down");break;}
            } else if(now_ms()>=deadline)break;
            publish(State::Observing);usleep(200000);
        }
        if(draws(request.mode)){
            teardown.store(true,std::memory_order_release);
            for(unsigned wait=0;wait<100&&widget_removed.load(std::memory_order_acquire)==0;wait++){
                publish(State::Observing);usleep(20000);}
            // CAS so a removal completing on the UI thread in this window is
            // never overwritten by the timeout verdict.
            int stalled=0;
            if(widget_removed.compare_exchange_strong(stalled,-1))
                note("WIDGET teardown timed out; UI thread stalled, cleanup left to ShellUI restart");
            note("WIDGET teardown result=%d",widget_removed.load());
            result=updates.load()>0&&widgets_created.load()==1&&widget_removed.load()==1
                ?State::Passed:State::Failed;
        } else if(request.mode==Mode::Input){
            result=pad_polls.load()>0?State::Passed:State::Failed;
        } else {
            result=updates.load()>0&&root_found.load()?State::Passed:State::Failed;
        }
    }while(false);
    enabled.store(false,std::memory_order_release);
    input_enabled.store(false,std::memory_order_release);
    auto query=fn<ProtectionQuery>(QueryProtection);
    // Every installed hook gets its removal attempted; a failed one must not
    // short-circuit the rest.
    bool removed=true;
    if(share_hook_store.installed)removed&=remove_hook(share_hook_store,query);
    if(pad_hook_store.installed)removed&=remove_hook(pad_hook_store,query);
    if(update_hook_store.installed)removed&=remove_hook(update_hook_store,query);
    if(!removed)result=State::RemoveFailed;
    // A pending callback may still use Mono and our trampolines. The module and
    // trampolines stay mapped; widget handles were freed after tree removal.
    fn<void(*)(void*)>(ThreadDetach)(thread);
    note("RESULT state=%d updates=%llu pad_polls=%llu share=%u chords=%u doubles=%u game=%d root=%d widgets=%d removed=%d",
        int(result),(unsigned long long)updates.load(),(unsigned long long)pad_polls.load(),share_events.load(),chords.load(),
        double_presses.load(),game_found.load(),root_found.load(),widgets_created.load(),removed);
    publish(result);
    return result;
}
}
extern "C" __attribute__((visibility("default"))) void* elf_main(void*){
    char p[256];if(!path(p,sizeof p,worker_dir,"worker.lock"))return nullptr;
    int lock=open(p,O_CREAT|O_RDWR,0600);if(lock<0)return nullptr;
    if(flock(lock,LOCK_EX|LOCK_NB)){close(lock);return nullptr;}
    if(!read_file(worker_dir,"request.bin",request)||!valid(request,getpid(),OVERLAY_BUILD,request.epoch,now_ms())){
        close(lock);return nullptr;}
    note("START build=%016llx mode=%u",(unsigned long long)request.build,unsigned(request.mode));
    State result=run();
    publish(result);close(lock);return nullptr;
}
