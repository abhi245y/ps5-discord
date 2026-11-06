// SPDX-License-Identifier: GPL-3.0-or-later
// The overlay's layout on a PC: runs the Discord UI (discord_app.inc) through a recording
// Builder for each scenario and writes every widget, back to front, to <dir>/<scenario>.txt.
// overlay/tools/preview.py draws them with the real art and font:
//   build/preview <dir> && python3 tools/preview.py <dir>
#include "../src/app/overlay_ui.hpp"
#include "../src/app/discord_app.inc"
#include <cstdio>
#include <cstring>

namespace {
struct Widget {
    char kind='p';
    bool visible=true;
    oui::Rect r{};
    char path[200]="", text[700]="";
    oui::Color tint{1,1,1,1}, ink{1,1,1,1};
    float size=20;int weight=0,align=0;
};
Widget widgets[200];
int used=0;

void* add(char kind,const oui::Rect* r,const char* s){
    Widget& w=widgets[used++];
    w.kind=kind;w.r=*r;
    std::snprintf(kind=='i'?w.path:w.text,kind=='i'?200:700,"%s",s?s:"");
    return &w;
}
void* add_panel(void*,const oui::Rect* r,const oui::Color*){return add('p',r,nullptr);}
void* add_label(void*,void*,const oui::Rect* r,const char* t){return add('l',r,t);}
void* add_image(void*,const oui::Rect* r,const char* p){return add('i',r,p);}
bool set_text(void*,void* w,const char* t){std::snprintf(static_cast<Widget*>(w)->text,700,"%s",t);return true;}
bool set_visible(void*,void* w,bool on){if(w)static_cast<Widget*>(w)->visible=on;return true;}
bool set_color(void*,void*,const oui::Color*){return true;}
bool set_image(void*,void* w,const char* p){std::snprintf(static_cast<Widget*>(w)->path,200,"%s",p);return true;}
bool set_tint(void*,void* w,const oui::Color* c){static_cast<Widget*>(w)->tint=*c;return true;}
bool set_style(void*,void* w,float size,int weight,const oui::Color* c,int align){
    Widget* x=static_cast<Widget*>(w);x->size=size;x->weight=weight;x->ink=*c;x->align=align;return true;}
bool set_rect(void*,void* w,const oui::Rect* r){static_cast<Widget*>(w)->r=*r;return true;}
bool set_capture(void*,bool){return true;}
bool send(void*,const char*){return true;}
bool send_text(void*,const char*){return true;}
bool set_hold(void*,int){return true;}
float sw(void*){return 1920;}
float sh(void*){return 1080;}
const oui::Builder::Vt vt{add_panel,add_label,set_text,set_visible,set_color,sw,sh,set_capture,send,
                          add_image,set_image,set_tint,set_style,set_rect,set_hold,send_text,nullptr};

void dump(const char* dir,const char* name){
    char path[300];std::snprintf(path,sizeof path,"%s/%s.txt",dir,name);
    FILE* f=std::fopen(path,"w");if(!f)return;
    for(int i=0;i<used;i++){
        const Widget& w=widgets[i];
        if(!w.visible)continue;
        std::fprintf(f,"%c\t%.1f\t%.1f\t%.1f\t%.1f\t%s\t%.3f,%.3f,%.3f,%.3f\t%.1f\t%d\t%.3f,%.3f,%.3f,%.3f\t%d\t%s\n",
            w.kind,w.r.x,w.r.y,w.r.w,w.r.h,w.path,w.tint.r,w.tint.g,w.tint.b,w.tint.a,w.size,w.weight,
            w.ink.r,w.ink.g,w.ink.b,w.ink.a,w.align,w.text);}
    std::fclose(f);
}

const char* kSet="0.5.5.1.0.3.1.1.1.0.0";
}

int main(int argc,char** argv){
    const char* dir=argc>1?argv[1]:".";
    using namespace oui::app;
    oui::Builder b{&vt,nullptr};
    reset();used=0;
    if(!build(b))return 1;
    char line[1024];
    auto feed=[&](const char* people,const char* live="",const char* set=kSet){
        std::snprintf(line,sizeof line,"3|c|o|1|%s|%d|Raid Night|42:10|720p \xc2\xb7 30 fps|%s|2~68:A5~68:A5^AirPods Pro;00:1B^Buds|%s",
            live,live[0]?3:0,set,people);
        on_text(line,b);};
    const char* four="11~11_ab~Mira~t,12~~Theo~-,13~13_cd~Ana~m,14~~Kenji~d";

    feed("11~11_ab~Mira~t,12~~Kenji~t");dump(dir,"card-two");
    feed("11~11_ab~Mira~t","18:42");dump(dir,"card-live");
    feed("");on_gesture(oui::Gesture::Chord,b);dump(dir,"call-empty");on_gesture(oui::Gesture::Chord,b);
    feed("11~11_ab~Mira~t");on_gesture(oui::Gesture::Chord,b);dump(dir,"call-one");on_gesture(oui::Gesture::Chord,b);
    feed(four);on_gesture(oui::Gesture::Chord,b);dump(dir,"call-four");
    on_pad(0x100,0,0,b);dump(dir,"settings-golive");
    on_pad(0x80,0,0,b);dump(dir,"settings-golive-open");
    on_pad(0x8,0,0,b);dump(dir,"settings-golive-nitro");on_pad(0x80,0,0,b);dump(dir,"settings-golive-armed");on_pad(0x40,0,0,b);
    on_pad(0x800,0,0,b);dump(dir,"settings-voice");
    on_pad(0x800,0,0,b);on_pad(0x8,0,0,b);on_pad(0x80,0,0,b);dump(dir,"settings-card-open");on_pad(0x40,0,0,b);
    on_pad(0x800,0,0,b);dump(dir,"settings-alerts");
    on_pad(0x8,0,0,b);on_pad(0x8,0,0,b);on_pad(0x8,0,0,b);on_pad(0x80,0,0,b);dump(dir,"settings-alerts-open");
    on_pad(0x40,0,0,b);on_pad(0x800,0,0,b);dump(dir,"settings-headset");
    on_pad(0x8,0,0,b);on_pad(0x80,0,0,b);dump(dir,"settings-headset-open");on_pad(0x40,0,0,b);
    std::snprintf(line,sizeof line,"3|c|o|1||0|Raid Night|42:10|720p|%s|0~~|%s",kSet,four);on_text(line,b);
    dump(dir,"settings-headset-off");
    // The keyboard, from the call page.
    feed(four);on_gesture(oui::Gesture::Chord,b);on_gesture(oui::Gesture::Chord,b);
    on_pad(0x20,0,0,b);dump(dir,"kb-empty");
    for(const char* c="hey, are you coming to the raid? 8pm";*c;c++){char one[2]={*c,0};kb_type(one);}
    kb_row=2;kb_col=3;kb_x=key_center(2,3);render_kb(b);dump(dir,"kb-lower");
    on_pad(0x1000,0,0,b);dump(dir,"kb-upper");
    on_pad(0x1000,0,0,b);dump(dir,"kb-caps");on_pad(0x1000,0,0,b);
    kb_page=1;kb_row=3;kb_col=9;kb_x=key_center(3,9);render_kb(b);dump(dir,"kb-symbols");kb_page=0;
    kb_type("\xe2\x82\xac");on_pad(0x400,0,0,b);on_pad(0x400,0,0,b);
    kb_row=4;kb_col=2;kb_x=key_center(4,2);render_kb(b);dump(dir,"kb-space");
    kb_col=6;render_kb(b);dump(dir,"kb-send");
    kb_len=kb_chars=kb_caret=0;kb_text[0]=0;
    for(int i=0;i<150;i++){const char* w="wide words Mmm and narrow illi ";char one[2]={w[i%31],0};kb_type(one);}
    for(int i=0;i<60;i++)kb_move_caret(-1);
    render_kb(b);dump(dir,"kb-long");
    std::printf("preview: %d widgets\n",used);
    return 0;
}
