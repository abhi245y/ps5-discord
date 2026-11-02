// SPDX-License-Identifier: GPL-3.0-or-later
// The Discord overlay UI against a fake Builder: the core's line, the talking card, the
// call panel, its tiles and focus.
#include "../src/app/overlay_ui.hpp"
#include "../src/app/discord_app.inc"
#include <cassert>
#include <cstdio>
#include <cstring>

namespace {
struct Widget {
    bool label=false, image=false, visible=true;
    char text[128]="", path[160]="";
    oui::Rect r{};
};
Widget widgets[128];
int used=0;

void* add(bool label,bool image,const oui::Rect* r,const char* text){
    assert(used<96); // the engine's limit
    Widget& w=widgets[used++];
    w.label=label;w.image=image;w.r=*r;
    std::snprintf(image?w.path:w.text,128,"%s",text?text:"");
    return &w;
}
void* add_panel(void*,const oui::Rect* r,const oui::Color*){return add(false,false,r,nullptr);}
void* add_label(void*,void*,const oui::Rect* r,const char* t){return add(true,false,r,t);}
void* add_image(void*,const oui::Rect* r,const char* p){return add(false,true,r,p);}
bool set_text(void*,void* w,const char* t){std::snprintf(static_cast<Widget*>(w)->text,128,"%s",t);return true;}
bool set_visible(void*,void* w,bool on){if(w)static_cast<Widget*>(w)->visible=on;return true;}
bool set_color(void*,void*,const oui::Color*){return true;}
bool set_image(void*,void* w,const char* p){std::snprintf(static_cast<Widget*>(w)->path,160,"%s",p);return true;}
bool set_tint(void*,void*,const oui::Color*){return true;}
bool set_style(void*,void*,float,int,const oui::Color*,int){return true;}
bool set_rect(void*,void* w,const oui::Rect* r){static_cast<Widget*>(w)->r=*r;return true;}
bool capturing=false;
char sent[64]="";
bool set_capture(void*,bool on){capturing=on;return true;}
bool send(void*,const char* t){std::snprintf(sent,sizeof sent,"%s",t);return true;}
float sw(void*){return 1920;}
float sh(void*){return 1080;}
const oui::Builder::Vt vt{add_panel,add_label,set_text,set_visible,set_color,sw,sh,set_capture,send,
                          add_image,set_image,set_tint,set_style,set_rect};

Widget* w(void* p){return static_cast<Widget*>(p);}
bool shows(const char* text){
    for(int i=0;i<used;i++)if(widgets[i].visible&&widgets[i].label&&std::strcmp(widgets[i].text,text)==0)return true;
    return false;}
bool ends(const char* s,const char* tail){
    const std::size_t a=std::strlen(s),b=std::strlen(tail);return a>=b&&!std::strcmp(s+a-b,tail);}
}

int main(){
    using namespace oui::app;
    oui::Builder b{&vt,nullptr};
    reset();
    assert(build(b));
    assert(used==66);
    for(int i=0;i<used;i++)assert(!widgets[i].visible); // nothing before the core says anything

    // In a call, two talking, the card on: their avatars and names, "You" under them.
    on_text("2|c|o|1||0|Raid Night|42:10|720p \xc2\xb7 30 fps|11~11_ab~Mira~t,12~~Kenji~t,13~13_cd~Ana~m,14~14_ef~Theo~-",b);
    assert(w(card)->visible&&!w(pill)->visible&&shows("Raid Night"));
    assert(shows("Mira")&&shows("Kenji")&&shows("You \xc2\xb7 mic on"));
    assert(ends(w(talk_avatar[0])->path,"/av/11_ab.png")&&w(talk_avatar[1])->path[0]==0); // not fetched yet
    assert(w(card)->r.h==325&&ends(w(card)->path,"glance_2.png")&&w(you_ring)->r.y==34+107+120-25);
    assert(!w(talk_avatar[2])->visible&&!w(dock)->visible);

    // Live and muted: the LIVE pill and "time | viewers"; the card shrinks to one talker.
    on_text("2|c|m|1|18:42|3|Raid Night|1:02:03|720p|11~11_ab~Mira~t",b);
    assert(w(pill)->visible&&shows("18:42  |  3 watching")&&!shows("Raid Night")&&shows("You \xc2\xb7 muted"));
    assert(w(card)->r.h==265&&ends(w(you_mic)->path,"mic_off.png"));

    // The panel: four rows (talking ring on Mira), the tiles, focus on Mute, capture on.
    on_text("2|c|o|1||0|Raid Night|42:10|720p \xc2\xb7 30 fps|11~11_ab~Mira~t,13~13_cd~Ana~m,14~14_ef~Theo~d,15~~Sam~-,16~~Nova~-",b);
    on_gesture(oui::Gesture::Chord,b);
    assert(capturing&&w(dock)->visible&&!w(card)->visible);
    assert(shows("Raid Night")&&shows("42:10")&&shows("Mira")&&shows("Ana")&&shows("Theo")&&shows("Sam")&&!shows("Nova"));
    assert(w(row_ring[0])->visible&&!w(row_ring[1])->visible);
    assert(ends(w(row_icon[1])->path,"mic_off.png")&&ends(w(row_icon[2])->path,"headphones_off.png"));
    assert(shows("Mute")&&shows("Deafen")&&shows("Go Live")&&shows("720p \xc2\xb7 30 fps")&&shows("Leave"));
    assert(w(tile_focus)->r.x==1347-6);
    on_pad(0x80,0,0,b);assert(!std::strcmp(sent,"mute"));
    on_pad(0x4,0,0,b);on_pad(0x80,0,0,b);assert(!std::strcmp(sent,"deafen")&&w(tile_focus)->r.x==1347+132-6);
    on_pad(0,0.9f,0,b);on_pad(0,0.9f,0,b);on_pad(0x80,0.9f,0,b);assert(!std::strcmp(sent,"golive")); // held: one step
    on_pad(0,0,0,b);on_pad(0x1,0,0,b);on_pad(0x1,0,0,b);on_pad(0x1,0,0,b);on_pad(0x80,0,0,b);
    assert(!std::strcmp(sent,"leave")); // left from Mute wraps to Leave
    on_pad(0x40,0,0,b); // Circle closes
    assert(!capturing&&!w(dock)->visible&&!w(tile[0])->visible&&w(card)->visible);

    // Muted and live: the tiles say so.
    on_text("2|c|m|1|0:12|1|Raid Night||720p|",b);
    on_gesture(oui::Gesture::Chord,b);
    assert(shows("Unmute")&&shows("Stop")&&shows("0:12")&&ends(w(tile_icon[0])->path,"mic_off.png"));
    on_gesture(oui::Gesture::Chord,b);

    // The card off (Settings, or nobody talking), and out of the call.
    on_text("2|c|o|0||0|Raid Night||720p|14~~Kenji~-",b);
    assert(!w(card)->visible);
    on_text("2|-|o|1||0||||",b);
    on_gesture(oui::Gesture::Chord,b);
    assert(shows("Not in a call")&&!w(row_bg[0])->visible&&!w(tile[0])->visible&&!w(timer)->visible);
    sent[0]=0;on_pad(0x80,0,0,b);assert(!sent[0]); // nothing to act on
    on_gesture(oui::Gesture::Chord,b);

    // Not ours: ignored.
    on_text("1|c|o|1||Raid Night|",b);on_text("",b);on_text(nullptr,b);
    assert(!w(card)->visible&&!w(dock)->visible);
    State s;
    assert(parse("2|c|o|1||0|Big|||a~~A~t,b~~B~t,c~~C~-,d~~D~-,e~~E~-,f~~F~-,g~~G~-,h~~H~-,i~~I~-",s)&&s.count==8);
    std::puts("discord app: ok");
    return 0;
}
