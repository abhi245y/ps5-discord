// SPDX-License-Identifier: GPL-3.0-or-later
// Reuse the recording Builder and the real lab app. No console paths are touched.
#define OVERLAY_APP_LAB
#define PILL_LAB_TEST
#include "preview.cpp"
#include <cassert>
#include <initializer_list>
namespace {
bool host_capture=false;int commands=0;
bool capture_test(void*,bool on){host_capture=on;return true;}
bool command_test(void*,const char*){++commands;return true;}
bool outbox_test(void*,const char*,const char*){++commands;return true;}
float width_test(void*,void* widget){
    const auto& w=*static_cast<Widget*>(widget);float n=0;
    // Deliberately make 0 wider than 8: widest-digit mode must probe every digit.
    for(const char* c=w.text;*c;c++)n+=w.size*(*c=='0'?.8f:*c==':'?.25f:.55f);
    return n;
}
}
int main(){
    using namespace oui::app;namespace lab=pill_lab;
    (void)mkdir("build/lab-app-test-out",0700);
    std::snprintf(lab_host_dir,sizeof lab_host_dir,"build/lab-app-test-out/lab");
    (void)mkdir(lab_host_dir,0700);
    char values[200];std::snprintf(values,sizeof values,"%s/pill-lab.txt",lab_host_dir);std::remove(values);
    auto table=vt;table.set_capture_fn=capture_test;table.send_fn=command_test;
    table.send_text_fn=outbox_test;table.text_width_fn=width_test;
    oui::Builder b{&table,nullptr};used=0;assert(build(b));
    assert(host_capture&&editor_visible&&lab::get(lab::Height)==32&&have_shape);
    assert(!std::strcmp(static_cast<Widget*>(time_label)->text,"12:34"));
    auto tick=[&](unsigned ms){lab_host_ms+=ms;on_tick(lab_host_ms,b);};
    tick(16);tick(16);tick(16);tick(160);
    assert(time_width>50); // maximum from 0 probes, not 8 probes
    const auto original_version=version;
    // Selection and tint edits do not change the PNG; x10 stepping uses the table step.
    selected=lab::TextR;on_pad(kL1,0,0,b);assert(lab::get(lab::TextR)==245);
    tick(151);assert(version==original_version&&!dirty);
    selected=lab::OutlineWidth;const float before=lab::get(lab::OutlineWidth);
    on_pad(kRight,0,0,b);assert(lab::get(lab::OutlineWidth)==before+.5f);
    for(int i=0;i<8;i++){tick(60);on_pad(kRight|kPadRepeat,0,0,b);assert(version==original_version);}
    tick(149);assert(version==original_version);tick(2);assert(version==original_version+1);
    // Previous version is retired only after swapping the native image.
    char old[200];std::snprintf(old,sizeof old,"%s/pill_%llu.png",lab_host_dir,(unsigned long long)original_version);
    assert(access(old,F_OK)!=0&&access(image_path,F_OK)==0);
    on_pad(kSquare,0,0,b);assert(!editor_visible&&static_cast<Widget*>(background)->visible);
    on_pad(kSquare|kPadRepeat,0,0,b);assert(!editor_visible);
    on_pad(kSquare,0,0,b);assert(editor_visible);
    selected=lab::Muted;on_pad(kCross,0,0,b);assert(lab::get(lab::Muted)==1);
    assert(std::strstr(static_cast<Widget*>(mic)->path,"mic_off.png"));
    selected=lab::Corner;on_pad(kCross,0,0,b);assert(lab::get(lab::Corner)==2);
    on_pad(kTriangle,0,0,b);assert(lab::get(lab::Preset)==1&&lab::get(lab::Height)==28);
    selected=lab::LongTime;on_pad(kCross,0,0,b);
    assert(!std::strcmp(static_cast<Widget*>(time_label)->text,"1:02:45"));
    tick(1000);assert(!std::strcmp(static_cast<Widget*>(time_label)->text,"1:02:46"));
    on_pad(kR2,0,0,b);assert(!dirty&&access(values,F_OK)==0);
    FILE* f=std::fopen(values,"r");assert(f);lab::preset(0);assert(lab::load(f));std::fclose(f);
    assert(lab::get(lab::LongTime)==1&&lab::get(lab::Preset)==1);
    on_pad(kCircle,0,0,b);assert(!host_capture&&!captured&&static_cast<Widget*>(background)->visible);
    selected=lab::Muted;const float muted=lab::get(lab::Muted);on_pad(kCross,0,0,b);assert(lab::get(lab::Muted)==muted);
    on_gesture(oui::Gesture::Chord,b);assert(host_capture&&captured);
    on_pad(kTriangle,0,0,b);assert(lab::get(lab::Preset)==2&&lab::get(lab::Height)==26.5f);
    on_pad(kTriangle,0,0,b);assert(lab::get(lab::Preset)==0&&lab::get(lab::Height)==32);
    const float height=lab::get(lab::Height);on_text("3|x|m|0||0",b);on_chat("garbage",b);assert(lab::get(lab::Height)==height);
    tick(31000);assert(host_capture&&static_cast<Widget*>(background)->visible&&commands==0);
    for(int i=0;i<used;i++)for(float v:{widgets[i].r.x,widgets[i].r.y,widgets[i].r.w,widgets[i].r.h}){
        assert(std::isfinite(v)&&v*2==std::floor(v*2));
    }
    dump("build/lab-app-test-out","editor");
    std::puts("lab app: controller, debounce, capture, timer, save/load, version retirement and feed isolation checks passed");
}
