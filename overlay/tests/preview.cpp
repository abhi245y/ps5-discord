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
bool send_text(void*,const char*,const char*){return true;}
bool set_hold(void*,int){return true;}
float sw(void*){return 1920;}
float sh(void*){return 1080;}
const oui::Builder::Vt vt{add_panel,add_label,set_text,set_visible,set_color,sw,sh,set_capture,send,
                          add_image,set_image,set_tint,set_style,set_rect,set_hold,send_text,nullptr,nullptr};

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

const char* kSet="0.5.5.1.0.3.1.1.1.0.0.1.1";
}

int main(int argc,char** argv){
    const char* dir=argc>1?argv[1]:".";
    using namespace oui::app;
    oui::Builder b{&vt,nullptr};
    reset();used=0;
    if(!build(b))return 1;
    char line[1024];
    auto feed=[&](const char* people,const char* live="",const char* set=kSet,char me='o',int viewers=3,int glance=1){
        std::snprintf(line,sizeof line,"3|c|%c|%d|%s|%d|Raid Night|42:10|720p \xc2\xb7 30 fps|%s|2~68:A5~68:A5^AirPods Pro;00:1B^Buds|%s",
            me,glance,live,live[0]?viewers:0,set,people);
        on_text(line,b);};
    const char* four="11~11_ab~Mira~t,12~~Theo~-,13~13_cd~Ana~m,14~~Kenji~d";

    feed("11~11_ab~Mira~t,12~~Kenji~t");dump(dir,"card-two");
    feed("11~11_ab~Mira~t","18:42");dump(dir,"card-live");
    const char* two="11~11_ab~Mira~t,12~~Kenji~t";
    for(int corner=0;corner<4;corner++){
        char settings[80],name[40];
        std::snprintf(settings,sizeof settings,"0.5.5.1.0.3.1.1.1.0.0.%d.1",corner);
        std::snprintf(name,sizeof name,"pill-corner-%d-card-tr",corner);
        feed(two,"12:34",settings,'o',12);dump(dir,name);
    }
    feed(two,"12:34",kSet,'o',12);dump(dir,"shared-tr");
    feed(two,"12:34","0.5.5.1.0.3.1.1.1.0.0.3.3",'o',12);dump(dir,"shared-br");
    feed(two,"12:34",kSet,'m',12);dump(dir,"pill-muted");
    feed(two,"1:02:45",kSet,'o',128);dump(dir,"pill-long-session");
    feed("11~11_ab~Mira~-","12:34",kSet,'o',12,0);dump(dir,"pill-only");
    feed(two,"12:34","0.5.5.0.0.3.1.1.1.0.0.1.1",'o',12,0);dump(dir,"pill-card-off");
    feed("");on_gesture(oui::Gesture::Chord,b);dump(dir,"call-empty");on_gesture(oui::Gesture::Chord,b);
    feed("11~11_ab~Mira~t");on_gesture(oui::Gesture::Chord,b);dump(dir,"call-one");on_gesture(oui::Gesture::Chord,b);
    feed(four);on_gesture(oui::Gesture::Chord,b);dump(dir,"call-four");
    on_pad(0x100,0,0,b);dump(dir,"settings-golive");
    on_pad(0x80,0,0,b);dump(dir,"settings-golive-open");
    on_pad(0x8,0,0,b);dump(dir,"settings-golive-nitro");on_pad(0x40,0,0,b);
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
    // The tabs, Chat and Mentions.
    const char* mentions="N\t901\t555\tNight Shift \xc2\xb7 #raid-night\t14_ef\tKenji\t2m\tyou coming?\n"
        "N\t902\t556\tDirect message\t\tTheo\t14m\tcan you stream it later?\n"
        "N\t903\t557\tSpeedrun Lab \xc2\xb7 #routes\t11_ab\tSam\t1h\tcheck this skip, it saves a whole lot of time on the second floor\n"
        "N\t904\t558\tCo-op Club \xc2\xb7 #general\t\tNova\t3h\tgg last night\n"
        "N\t905\t559\tCo-op Club \xc2\xb7 #general\t\tMiro\t5h\tolder one\n";
    const char* convo="M\t1\t11_ab\tMira\t6:14 PM\t0\tboss at 9, bring fire res\n"
        "M\t2\t\tTheo\t6:15 PM\t0\twho has the key?\n"
        "M\t3\t13_cd\tAna\t6:16 PM\t0\tme, omw\n"
        "M\t4\t14_ef\tKenji\t6:17 PM\t1\tyou coming?\n";
    auto chat_feed=[&](const char* head,const char* body){
        static char text[8000];std::snprintf(text,sizeof text,"%s%s",head,body);on_chat(text,b);};
    feed(four);if(open)on_gesture(oui::Gesture::Chord,b);
    chat_feed("H\tc\t555\t#raid-night\tNight Shift\t\t2\t\n",convo);
    on_gesture(oui::Gesture::Chord,b);dump(dir,"call-four-badge");
    on_tick(100,b);on_pad(0x800,0,0,b);dump(dir,"chat-mention");
    on_pad(0x20,0,0,b);dump(dir,"chat-kb");on_pad(0x40,0,0,b);
    chat_feed("H\td\t777\tTheo\tDirect message\t14_ef\t2\t\n","M\t1\t\tTheo\t6:15 PM\t0\tlong message with plenty of words that goes on and on past the end of the line\n");
    dump(dir,"chat-dm");
    chat_feed("H\tc\t555\t#raid-night\tNight Shift\t\t2\t\n","");dump(dir,"chat-empty");
    chat_feed("H\t-\t\t\t\t\t0\t\n","");dump(dir,"chat-none");
    {static char many[4000];many[0]=0;
     for(int i=1;i<=9;i++){char one[200];std::snprintf(one,sizeof one,"M\t%d\t1%d_ab\tPerson %d\t6:%02d PM\t%d\tmessage number %d\n",i,i,i,10+i,i==3,i);std::strcat(many,one);}
     chat_feed("H\tc\t555\t#raid-night\tNight Shift\t\t0\t\n",many);}
    on_pad(0x2,0,0,b);on_pad(0x2,0,0,b);dump(dir,"chat-scrolled");
    chat_feed("H\tc\t555\t#raid-night\tNight Shift\t\t0\t\n","");
    chat_feed("H\tc\t555\t#raid-night\tNight Shift\t\t3\t\n",mentions);
    on_tick(200,b);on_pad(0x400,0,0,b);on_tick(300,b);on_pad(0x400,0,0,b);dump(dir,"mentions");
    on_pad(0x8,0,0,b);dump(dir,"mentions-second");
    on_pad(0x8,0,0,b);on_pad(0x8,0,0,b);on_pad(0x8,0,0,b);dump(dir,"mentions-scrolled");
    chat_feed("H\tc\t555\t#raid-night\tNight Shift\t\t0\t\n","");dump(dir,"mentions-empty");
    chat_feed("H\tc\t555\t#raid-night\tNight Shift\t\t0\t\n",mentions);
    on_pad(0x80,0,0,b);dump(dir,"chat-loading");
    pend_until=0;chat_feed("H\tc\t556\t#gta-online\tRockstar Games\t\t0\t2\t1\n",convo);dump(dir,"chat-other"); // from a mention: Square goes back
    std::printf("preview: %d widgets\n",used);
    return 0;
}
