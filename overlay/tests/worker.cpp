// SPDX-License-Identifier: GPL-3.0-or-later
// Fake Mono runtime only. Exercises the production resolver, UI callback,
// widget lifecycle and pad/share hooks. No console calls.
#include "../src/core/worker.cpp"
#include <cassert>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
static unsigned attaches,detaches,invokes,prior_calls,object_inits,setter_calls,getter_calls,ctor_calls;
static int scenario;
static void* ptr(unsigned n){return reinterpret_cast<void*>(std::uintptr_t(n));}
// class and method identities used by the fake registry
enum :unsigned {c_application=3,c_layer=4,c_scene=5,c_widget=30,c_panel=31,c_label=32,c_uifont=33,c_uicolor=34,
    c_gamepad=35,c_eventmgr=36,c_root_scene_obj=31,c_scene_widget=32,c_runtime_root=70,
    m_update=6,m_find_scene=7,m_root_widget=8,m_append=40,m_find_widget=41,m_detach=42,
    m_font_ctor=43,m_color_ctor=44,m_getdata=45,m_onshare=46,m_root_scene=48,m_remove_scene=49,m_add_scene=50,
    t_void=9,t_class=10,t_value=47};
struct MethodDef{unsigned id,cls;const char* name;int params;bool stat;int ret;};
static const MethodDef table[]={
    {m_update,c_application,"Update",0,false,1},{m_find_scene,c_layer,"FindContainerSceneByPath",1,true,0x12},
    {m_root_widget,c_scene,"get_RootWidget",0,false,0x12},{m_append,c_widget,"AppendChild",1,false,1},
    {m_find_widget,c_widget,"FindWidgetByName",1,false,0x12},{m_detach,c_widget,"RemoveFromParent",0,false,1},
    {m_font_ctor,c_uifont,".ctor",3,false,1},{m_color_ctor,c_uicolor,".ctor",4,false,1},
    {m_getdata,c_gamepad,"GetData",1,true,0x11},{m_onshare,c_eventmgr,"OnShareButton",1,true,1},
    {m_root_scene,c_application,"get_RootScene",0,false,0x12},
    {m_remove_scene,c_scene,"RemoveFromSuperScene",0,false,1},
    {m_add_scene,c_runtime_root,"AddScene",1,true,1}};
static const MethodDef* def_of(unsigned id){for(auto&d:table)if(d.id==id)return &d;return nullptr;}
// widget tree, handles, strings and property bookkeeping
static std::map<void*,std::vector<void*>> children;
static std::map<void*,std::string> object_name;
static std::map<void*,unsigned> object_class;
static std::map<void*,std::string> string_of;
static std::map<std::string,void*> string_ptr;
static std::map<std::uint32_t,void*> handles;
static std::uint32_t next_handle=0,next_object=0,prop_counter=0;
static std::map<void*,std::pair<void*,void*>> prop_accessors; // property -> (getter method, setter method)
static std::map<void*,char> accessor_kind;
static std::map<void*,std::string> accessor_name;
static std::string pending_prop;
static std::vector<std::pair<std::string,void*>> property_sets;
static std::uint32_t pad_buttons_next=0;
static unsigned char fake_appended=0;
static unsigned add_scene_calls=0;
static int pad_device_seen=-1;
static unsigned share_orig_calls=0,freed_handles=0;
static void* root(){return scenario==1?nullptr:ptr(1);}
static void* attach(void*){++attaches;return scenario==2?nullptr:ptr(2);}
static void detach(void*){++detaches;}
static void* assembly(void*,const char* name){
    if(std::strstr(name,"AppSystem"))return ptr(12);
    if(std::strstr(name,"Sce.PlayStation.Core"))return ptr(13);
    if(std::strstr(name,"CaptureMenu"))return ptr(14);
    return ptr(11);}
static void* image_of(void* a){return a;}
static void* klass(void*,const char*,const char* name){
    if(scenario==3)return nullptr;

    if(scenario==13&&!std::strcmp(name,"Widget"))return nullptr;
    struct Named{const char* n;unsigned c;};
    static const Named names[]={{"Application",c_application},{"LayerManager",c_layer},{"Scene",c_scene},
        {"Widget",c_widget},{"Panel",c_panel},{"Label",c_label},{"UIFont",c_uifont},{"UIColor",c_uicolor},
        {"GamePad",c_gamepad},{"EventManager",c_eventmgr}};
    for(auto&n:names)if(!std::strcmp(n.n,name))return ptr(n.c);
    return nullptr;}
static unsigned id_of(void* m){return static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(m));}
static void* find_method(void* c,const char* name,int count){
    for(auto&d:table)if(d.cls==id_of(c)&&!std::strcmp(d.name,name)&&d.params==count)return ptr(d.id);
    return nullptr;}
static void* method_sig(void* m){return m;}
static int instance(void* m){auto d=def_of(id_of(m));return d&&!d->stat;}
static unsigned parameters(void* m){auto d=def_of(id_of(m));return d?unsigned(d->params):0;}
static void* return_type(void* m){
    auto d=def_of(id_of(m));if(!d)return nullptr;
    return d->ret==1?ptr(t_void):d->ret==0x11?ptr(t_value):ptr(t_class);}
static int type_kind(void* t){if(scenario==4)return 0;return t==ptr(t_void)?1:t==ptr(t_value)?0x11:0x12;}
static void record_set(void* self){
    ++setter_calls;
    if(!pending_prop.empty()){property_sets.push_back({pending_prop,self});pending_prop.clear();}}
// The worker calls compiled accessors with their true signatures
// (object,float)/(object,bool)/(object,string); this one stub receives them as
// (object,pointer,pointer). Tolerated on x86-64 SysV only: extra argument
// registers are ignored. Function-type sanitizer is off for this reason.
static void setter_stub(void* self,void* a,void*){
    record_set(self);
    if(property_sets.back().first=="Name"&&!object_name.count(self))
        object_name[self]=string_of.count(a)?string_of[a]:"?";}
static float fake_root_width=1920.f;
static float getter_stub(void*){++getter_calls;pending_prop.clear();return 0.f;}
// readable stand-in for compiled method memory (HOOKDIAG copies 16 bytes)
alignas(16) static std::uint8_t fake_code[16]={0x55,0x48,0x89,0xe5,0x53,0x50,0x51,0x52,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90};
static void* compile(void* m){
    if(scenario==5)return nullptr;
    if(accessor_kind.count(m))return accessor_kind[m]=='g'?reinterpret_cast<void*>(&getter_stub)
                                                          :reinterpret_cast<void*>(&setter_stub);
    return fake_code;}
static void* text(void*,const char* s){
    auto& slot=string_ptr[s];if(!slot)slot=ptr(0x30000+string_ptr.size());
    string_of[slot]=s;return slot;}
static void* runtime(void* m,void* obj,void** args,void** exception){
    assert(prior_calls>0);++invokes;*exception=nullptr;
    unsigned id=id_of(m);
    if(id==m_find_scene){assert(!obj&&args&&string_of.count(args[0])&&string_of[args[0]]=="Game");
        if(scenario==6)return nullptr;if(scenario==7){*exception=ptr(99);return nullptr;}return ptr(21);}
    if(id==m_root_widget){assert(obj==ptr(21));if(scenario==8){*exception=ptr(99);return nullptr;}return ptr(22);}
    if(id==m_append){assert(obj&&args&&args[0]);
        if(scenario==12){*exception=ptr(99);return nullptr;} // attach fails: clean refusal path
        children[obj].push_back(args[0]);return nullptr;}
    if(id==m_find_widget){assert(obj&&args&&args[0]&&string_of.count(args[0]));
        const std::string& want=string_of[args[0]];
        std::vector<void*> queue; // read via find: operator[] would insert into the tree
        auto seeded=children.find(obj);
        if(seeded!=children.end())queue=seeded->second;
        for(std::size_t i=0;i<queue.size();i++){void* c=queue[i];
            auto n=object_name.find(c);
            if(n!=object_name.end()&&n->second==want)return c;
            auto below=children.find(c);
            if(below!=children.end())for(void* g:below->second)queue.push_back(g);}
        return nullptr;}
    if(id==m_detach){assert(obj);for(auto&pair:children){auto& v=pair.second;
            for(std::size_t i=0;i<v.size();)if(v[i]==obj)v.erase(v.begin()+i);else ++i;}
        object_name.erase(obj);return nullptr;}
    if(id==m_font_ctor||id==m_color_ctor){++ctor_calls;assert(obj);return nullptr;}
    if(id==m_add_scene){assert(obj&&args&&args[0]);++add_scene_calls;fake_appended=1;return nullptr;}
    if(id==m_remove_scene){assert(obj);fake_appended=0;
        std::vector<void*> stack{ptr(c_scene_widget)}; // scene detach prunes its subtree
        while(!stack.empty()){void* node=stack.back();stack.pop_back();
            auto it=children.find(node);if(it==children.end())continue;
            for(void* c:it->second)stack.push_back(c);children.erase(it);}
        return nullptr;}
    if(accessor_kind.count(m)&&accessor_kind[m]=='g'){
        ++getter_calls;const std::string which=accessor_name.count(m)?accessor_name[m]:"";pending_prop.clear();
        if(which=="Width")return &fake_root_width;
        if(which=="RootScene")return ptr(c_root_scene_obj);
        if(which=="RootWidget")return ptr(c_scene_widget);
        if(which=="IsAppended")return &fake_appended;
        return nullptr;}
    if(accessor_kind.count(m)&&accessor_kind[m]=='s'){record_set(obj);return nullptr;}
    assert(!"unexpected runtime invoke");return nullptr;}
static std::uint32_t gc_new(void* obj,int pinned){assert(obj);(void)pinned;handles[++next_handle]=obj;return next_handle;}
static void* gc_target(std::uint32_t h){assert(handles.count(h));return handles[h];}
static void gc_free(std::uint32_t h){
    assert(handles.count(h));
    // Pins detach-before-free once the scene is published; before that the
    // real runtime collects the whole unreachable subtree with the scene.
    if(add_scene_calls)for(auto&pair:children)for(void* c:pair.second)assert(c!=handles[h]);
    handles.erase(h);++freed_handles;}
static void* object_new(void*,void* klass){
    if(!klass)return nullptr;
    if(scenario==10&&id_of(klass)==c_label)return nullptr;
    void* o=ptr(0x50000+(++next_object));
    object_class[o]=id_of(klass);return o;}
static void object_init(void* obj){assert(obj);++object_inits;}
static void* class_property(void*,const char* name){
    if(scenario==11&&!std::strcmp(name,"Text"))return nullptr;
    pending_prop=name;
    void* prop=ptr(0x21000+(++prop_counter));
    void* get=ptr(0x20000+2*prop_counter),*set=ptr(0x20001+2*prop_counter);
    accessor_kind[get]='g';accessor_kind[set]='s';accessor_name[get]=name;accessor_name[set]=name;
    prop_accessors[prop]={get,set};
    return prop;}
static void* property_getter(void* prop){auto it=prop_accessors.find(prop);return it==prop_accessors.end()?nullptr:it->second.first;}
static void* property_setter(void* prop){auto it=prop_accessors.find(prop);return it==prop_accessors.end()?nullptr:it->second.second;}
static void* unbox(void* obj){assert(obj);return obj;}
// Reflection (describe_types): the fake has no types, so the log says "?".
static void* iter_none(void*,void**){return nullptr;}
static const char* name_none(void*){return nullptr;}
static void* one_none(void*){return nullptr;}
static unsigned offset_none(void*){return 0;}
// The system menu's hold (hold_game/release_game): who has focus, and the impose flag.
static int focus_now=0x60000001,impose_now=0;
static int big_app_fake(){return 0x60000001;}
static int impose_fake(int on){impose_now=on;return 0;}
static int focus_fake(int app){focus_now=app;return 0;}
static int query_fake(void*,void**,void**,int*){return -1;}
static int mprotect_fake(void*,std::size_t,int){return 0;}
static void* mmap_fake(void*,std::size_t,int,int,int,long){return nullptr;}
static int munmap_fake(void*,std::size_t){return 0;}
static void* object_class_fake(void* o){return o==ptr(c_root_scene_obj)?ptr(c_runtime_root):o?ptr(60):nullptr;}
static const char* class_name_fake(void*){return "FakeException";}
static void* class_parent_fake(void* c){return c==ptr(70)?nullptr:nullptr;}
static void prior(void*){++prior_calls;}
static PadData fake_pad(int device){pad_device_seen=device;
    PadData d;d.skip=true;d.buttons=pad_buttons_next;d.prev=0x11;d.down=0x22;d.up=0x33;
    d.lx=0.25f;d.ly=0.75f;d.rx=-0.5f;d.ry=0.5f;return d;}
static void fake_share(void*){++share_orig_calls;}
static void bind(){
    std::uint64_t values[]={reinterpret_cast<std::uintptr_t>(&root),reinterpret_cast<std::uintptr_t>(&attach),
        reinterpret_cast<std::uintptr_t>(&detach),reinterpret_cast<std::uintptr_t>(&assembly),
        reinterpret_cast<std::uintptr_t>(&image_of),reinterpret_cast<std::uintptr_t>(&klass),
        reinterpret_cast<std::uintptr_t>(&find_method),reinterpret_cast<std::uintptr_t>(&method_sig),
        reinterpret_cast<std::uintptr_t>(&instance),reinterpret_cast<std::uintptr_t>(&parameters),
        reinterpret_cast<std::uintptr_t>(&return_type),reinterpret_cast<std::uintptr_t>(&type_kind),
        reinterpret_cast<std::uintptr_t>(&compile),reinterpret_cast<std::uintptr_t>(&text),
        reinterpret_cast<std::uintptr_t>(&runtime),reinterpret_cast<std::uintptr_t>(&gc_new),
        reinterpret_cast<std::uintptr_t>(&gc_target),reinterpret_cast<std::uintptr_t>(&gc_free),
        reinterpret_cast<std::uintptr_t>(&query_fake),reinterpret_cast<std::uintptr_t>(&object_new),
        reinterpret_cast<std::uintptr_t>(&object_init),reinterpret_cast<std::uintptr_t>(&class_property),
        reinterpret_cast<std::uintptr_t>(&property_getter),reinterpret_cast<std::uintptr_t>(&property_setter),
        reinterpret_cast<std::uintptr_t>(&unbox),reinterpret_cast<std::uintptr_t>(&mprotect_fake),reinterpret_cast<std::uintptr_t>(&mmap_fake),
        reinterpret_cast<std::uintptr_t>(&munmap_fake),reinterpret_cast<std::uintptr_t>(&object_class_fake),
        reinterpret_cast<std::uintptr_t>(&class_name_fake),reinterpret_cast<std::uintptr_t>(&class_parent_fake),
        reinterpret_cast<std::uintptr_t>(&iter_none),reinterpret_cast<std::uintptr_t>(&name_none),
        reinterpret_cast<std::uintptr_t>(&one_none),reinterpret_cast<std::uintptr_t>(&iter_none),
        reinterpret_cast<std::uintptr_t>(&name_none),reinterpret_cast<std::uintptr_t>(&one_none),
        reinterpret_cast<std::uintptr_t>(&offset_none),reinterpret_cast<std::uintptr_t>(&iter_none),
        reinterpret_cast<std::uintptr_t>(&name_none),
        reinterpret_cast<std::uintptr_t>(&iter_none),reinterpret_cast<std::uintptr_t>(&name_none),
        reinterpret_cast<std::uintptr_t>(&big_app_fake),reinterpret_cast<std::uintptr_t>(&impose_fake),
        reinterpret_cast<std::uintptr_t>(&focus_fake)};
    static_assert(sizeof values==sizeof request.functions);std::memcpy(request.functions,values,sizeof values);
    request.mode=Mode::Resolve;request.deadline_ms=now_ms()+18000;
}
static void reset_runtime(){
    attaches=detaches=invokes=prior_calls=object_inits=setter_calls=getter_calls=ctor_calls=0;
    children.clear();object_name.clear();object_class.clear();handles.clear();freed_handles=0;
    prop_accessors.clear();accessor_kind.clear();accessor_name.clear();pending_prop.clear();property_sets.clear();
    pad_buttons_next=0;share_orig_calls=0;fake_appended=0;add_scene_calls=0;
    request=Request{};update_hook_store=Hook{};pad_hook_store=Hook{};share_hook_store=Hook{};
    domain=nullptr;find_scene=nullptr;root_getter=nullptr;resolved_update_entry=nullptr;
    original.store(nullptr);pad_original.store(nullptr);share_original.store(nullptr);
    enabled.store(false);input_enabled.store(false);teardown.store(false);
    updates.store(0);pad_polls.store(0);scene_attempts.store(0);create_attempts.store(0);text_updates.store(0);
    share_events.store(0);chords.store(0);double_presses.store(0);last_buttons.store(0);last_share_at.store(0);
    game_found.store(0);root_found.store(0);widgets_created.store(0);widget_removed.store(0);chord_held.store(false);
    gesture_requested.store(false);oui::app::reset();last_text[0]=0;
    capture.store(false);pending_down.store(0);pad_logged.store(0);holding.store(false);
    for(auto&slot:widget_slots)slot=WidgetSlot{};
    app_handle=0;root_scene_handle=0;scene_handle=0;
    scene_root_handle=0;removal_requested=false;cleanup_retry_used=false;
}
static bool attached(void* obj){for(auto&pair:children)for(void* c:pair.second)if(c==obj)return true;return false;}
static bool tree_detached(){for(auto&pair:children)if(!pair.second.empty())return false;return true;}
static bool set_recorded(const char* name,void* obj){
    for(auto&s:property_sets)if(s.first==name&&s.second==obj)return true;return false;}
// Direct update_hook tests bypass run(), so bind what resolve_update_methods would.
static void bind_widget_runtime(){
    find_scene=ptr(m_find_scene);root_getter=ptr(m_root_widget);
    class_application=ptr(c_application);class_scene=ptr(c_scene);
    class_widget=ptr(c_widget);class_panel=ptr(c_panel);class_label=ptr(c_label);
    class_font=ptr(c_uifont);
    method_append=ptr(m_append);method_find=ptr(m_find_widget);method_font_ctor=ptr(m_font_ctor);}
int main(){
    // Resolver refusal paths (mode Resolve).
    for(scenario=0;scenario<=5;++scenario){reset_runtime();bind();
        State result=run();assert(result==(scenario==0?State::Resolved:State::Refused));assert(!invokes);
        assert(attaches==(scenario==1?0u:1u));assert(detaches==((scenario==1||scenario==2)?0u:1u));}
    // Update callback pass-through, bounded scene probes, GC roots, managed exceptions.
    for(scenario=0;scenario<=8;scenario=scenario==0?6:scenario+1){
        reset_runtime();bind();domain=ptr(1);find_scene=ptr(m_find_scene);root_getter=ptr(m_root_widget);
        original=&prior;enabled=true;
        for(unsigned i=0;i<130;i++)update_hook(nullptr);
        enabled=false;update_hook(nullptr);assert(prior_calls==131&&updates.load()==130);
        assert(handles.empty());
        if(!scenario){assert(scene_attempts==1&&game_found&&root_found&&invokes==2);}
        else{assert(scene_attempts==3&&!root_found);assert(game_found==(scenario==8?1:0));}
    }
    // Widget lifecycle: creation at frame 30, attach tree, teardown removal, handle release.
    {scenario=0;reset_runtime();bind();request.mode=Mode::Widget;domain=ptr(1);
        bind_widget_runtime();original=&prior;enabled=true;
        for(unsigned i=0;i<200;i++)update_hook(ptr(88)); // instance IS the Application
        assert(prior_calls==200&&updates.load()==200);
        assert(create_attempts==1&&widgets_created==1&&widget_removed==0);
        assert(object_inits==3&&ctor_calls==1&&add_scene_calls==1); // scene+panel+label; the label's UIFont
        assert(set_recorded("ScreenRenderOverlay",nullptr)||!property_sets.empty());
        bool seen_overlay=false;for(auto&e:property_sets)if(e.first=="ScreenRenderOverlay")seen_overlay=true;
        assert(seen_overlay);
        void* panel=nullptr,*label=nullptr;
        for(auto&pair:object_class)if(pair.second==c_panel)panel=pair.first;else if(pair.second==c_label)label=pair.first;
        assert(panel&&label);
        assert(attached(panel)&&attached(label)&&set_recorded("Text",label)&&set_recorded("Visible",panel));
        assert(handles.size()==6&&last_text[0]==0&&text_updates.load()==0);
        teardown.store(true);update_hook(ptr(88)); // queues RemoveFromSuperScene: still pending
        assert(widget_removed==0&&!handles.empty()&&fake_appended==0);
        update_hook(ptr(88)); // next callback confirms not appended, frees handles
        assert(widget_removed==1&&handles.empty()&&!attached(panel)&&!attached(label));
        assert(prior_calls==202&&create_attempts==1);}
    // Widget refusal: Label class missing, three bounded attempts, nothing attached.
    {scenario=10;reset_runtime();bind();request.mode=Mode::Widget;domain=ptr(1);
        bind_widget_runtime();original=&prior;enabled=true;
        for(unsigned i=0;i<200;i++)update_hook(ptr(88));
        assert(create_attempts==3&&widgets_created==-1&&handles.size()==1);} // app pin only
    // Widget refusal: Text property unavailable, no attach, no handles.
    {scenario=11;reset_runtime();bind();request.mode=Mode::Widget;domain=ptr(1);
        bind_widget_runtime();original=&prior;enabled=true;
        for(unsigned i=0;i<200;i++)update_hook(ptr(88));
        assert(create_attempts==3&&widgets_created==-1&&handles.size()==1);} // app pin only
    // Widget attach failure: three clean refusals, nothing attached, no handles.
    {scenario=12;reset_runtime();bind();request.mode=Mode::Widget;domain=ptr(1);
        bind_widget_runtime();original=&prior;enabled=true;
        for(unsigned i=0;i<200;i++)update_hook(ptr(88));
        assert(create_attempts==3&&widgets_created==-1&&tree_detached()&&handles.size()==1);} // app pin only
    // Stop before create: no widget is ever built, removal still completes.
    {scenario=0;reset_runtime();bind();request.mode=Mode::Widget;domain=ptr(1);
        bind_widget_runtime();original=&prior;enabled=true;teardown=true;
        for(unsigned i=0;i<200;i++)update_hook(ptr(88));
        assert(create_attempts==0&&widgets_created==0&&widget_removed==1&&handles.empty()&&children.empty());}
    // Widget refusal end to end: missing Widget class refuses before any
    // method lookup on a null class, attach never happens, thread detaches.
    {scenario=13;reset_runtime();bind();request.mode=Mode::Widget;
        State result=run();assert(result==State::Refused);assert(attaches==1&&detaches==1);
        assert(children.empty()&&handles.empty());}
    // Pad hook: pass-through, chord edges, share double press.
    {scenario=0;reset_runtime();bind();request.mode=Mode::Input;
        input_enabled.store(true);pad_original.store(&fake_pad);share_original.store(&fake_share);
        pad_buttons_next=bit_l3|128u;PadData out=pad_hook(3); // Cross, device 3
        assert(pad_device_seen==3&&out.skip&&out.buttons==(bit_l3|128u)&&out.prev==0x11&&out.down==0x22&&
               out.up==0x33&&out.lx==0.25f&&out.ly==0.75f&&out.rx==-0.5f&&out.ry==0.5f);
        assert(pad_polls==1&&chords==0);
        pad_buttons_next=bit_l3|bit_r3;pad_hook(0);assert(chords==1&&gesture_requested.load());
        pad_hook(0);assert(chords==1); // held, no new edge
        pad_buttons_next=0;pad_hook(0);pad_buttons_next=bit_l3|bit_r3;pad_hook(0);assert(chords==2);
        // Hold Options 0.6 s: one gesture per hold, none for a press.
        gesture_requested.store(false);pad_buttons_next=0;pad_hook(0);
        pad_buttons_next=bit_options;pad_hook(0);pad_hook(0);assert(chords==2&&!gesture_requested.load());
        usleep(650000);pad_hook(0);assert(chords==3&&gesture_requested.load());
        pad_hook(0);assert(chords==3); // still held: no repeat
        pad_buttons_next=0;pad_hook(0);gesture_requested.store(false);
        note_share(1000);note_share(1150);assert(double_presses==1);
        note_share(3000);assert(double_presses==1);note_share(3100);assert(double_presses==2);
        assert(share_events==4);
        share_hook(ptr(77));assert(share_orig_calls==1&&share_events==5);
        // Capture: newly pressed buttons (down) collect for the app, the stick is kept;
        // nothing collects while the app's panel is closed.
        pad_buttons_next=0;pad_hook(0);assert(pending_down.load()==0);
        oui::Builder b=ui_builder();b.set_capture(true);
        pad_hook(0);assert(pending_down.load()==0x22&&pad_logged.load()==1);
        float y=0;std::uint32_t by=stick_y.load();std::memcpy(&y,&by,4);assert(y==0.75f);
        b.set_capture(false);pending_down.store(0);pad_hook(0);assert(pending_down.load()==0);
        // Capture holds the game like the system menu: impose on, focus to the system (-1);
        // closing gives both back.
        assert(focus_now==0x60000001&&impose_now==0);
        request.shell_app=0x60000002;
        b.set_capture(true);assert(holding.load()&&focus_now==0x60000002&&impose_now==1);
        {PadData quiet=pad_hook(0);assert(pending_down.load()==0x22&&quiet.buttons==0&&quiet.down==0&&quiet.ly==0);}
        b.set_capture(false);assert(!holding.load()&&focus_now==0x60000001&&impose_now==0);
        assert(pad_hook(0).down==0x22); // released: ShellUI sees the pad again
        // Teardown never leaves it held.
        b.set_capture(true);release_game("test");assert(focus_now==0x60000001&&impose_now==0);
        input_enabled.store(false);std::uint64_t before=pad_polls.load();pad_hook(0);assert(pad_polls==before);}
    // Input mode run(): hooks cannot prepare against the fake query, refusal stays clean.
    {scenario=0;reset_runtime();bind();request.mode=Mode::Input;
        State result=run();assert(result==State::Refused);assert(attaches==1&&detaches==1);}
    std::puts("PASS: fake Mono resolution/signature failures, thread detach, Update pass-through, bounded probes,"
              " GC roots, widget create/rollback/remove lifecycle, handle release, stop-before-create,"
              " pad/share pass-through and gestures; no console calls");
}
