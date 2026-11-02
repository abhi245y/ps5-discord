// SPDX-License-Identifier: GPL-3.0-or-later
// The Discord overlay UI against a fake Builder: the core's line, the glance, the panel.
#include "../src/app/overlay_ui.hpp"
#include "../src/app/discord_app.inc"
#include <cassert>
#include <cstdio>
#include <cstring>

namespace {
struct Widget { bool label=false; bool visible=true; char text[128]=""; };
Widget widgets[32];
int used=0;

void* add(bool label,const char* text){
    assert(used<32); // the engine's limit
    Widget& w=widgets[used++];
    w.label=label;
    std::snprintf(w.text,sizeof w.text,"%s",text?text:"");
    return &w;
}
void* add_panel(void*,const oui::Rect*,const oui::Color*){return add(false,nullptr);}
void* add_label(void*,void*,const oui::Rect*,const char* t){return add(true,t);}
bool set_text(void*,void* w,const char* t){
    std::snprintf(static_cast<Widget*>(w)->text,128,"%s",t);return true;}
bool set_visible(void*,void* w,bool on){if(w)static_cast<Widget*>(w)->visible=on;return true;}
bool set_color(void*,void*,const oui::Color*){return true;}
bool capturing=false;
char sent[64]="";
bool set_capture(void*,bool on){capturing=on;return true;}
bool send(void*,const char* t){std::snprintf(sent,sizeof sent,"%s",t);return true;}
float sw(void*){return 1920;}
float sh(void*){return 1080;}
const oui::Builder::Vt vt{add_panel,add_label,set_text,set_visible,set_color,sw,sh,set_capture,send};

Widget* w(void* p){return static_cast<Widget*>(p);}
bool shows(const char* text){
    for(int i=0;i<used;i++)if(widgets[i].visible&&widgets[i].label&&std::strcmp(widgets[i].text,text)==0)return true;
    return false;}
}

int main(){
    using namespace oui::app;
    oui::Builder b{&vt,nullptr};
    reset();
    assert(build(b));
    assert(used==23);
    // Nothing shows before the core says anything.
    for(int i=0;i<used;i++)assert(!widgets[i].visible);

    // In a call, two talking, glance on, not live.
    on_text("1|c|o|1||Raid Night|Mira~t,Kenji~t,Ana~m,Theo~-",b);
    assert(w(glance)->visible&&!w(glance_live)->visible);
    assert(shows("Raid Night")&&shows("Mira, Kenji talking")&&shows("You - mic on"));
    assert(!w(panel)->visible);

    // Live and muted.
    on_text("1|c|m|1|18:42 3|Raid Night|Mira~t",b);
    assert(w(glance_live)->visible&&shows("LIVE 18:42 - 3 watching")&&shows("You - muted"));

    // L3+R3: the panel replaces the glance; one row per person.
    on_text("1|c|o|1||Raid Night|Mira~t,Kenji~-,Ana~m,Theo~d",b);
    on_gesture(oui::Gesture::Chord,b);
    assert(w(panel)->visible&&!w(glance)->visible);
    assert(shows("Raid Night")&&shows("Mira - talking")&&shows("Kenji")&&shows("Ana - muted")&&shows("Theo - deafened"));
    assert(!w(row[4])->visible&&!w(row[5])->visible);
    // The actions: capture is on, the stick moves focus once per push, Cross sends.
    assert(capturing&&w(action[0])->visible&&shows("Mute")&&shows("Go Live")&&shows("Leave call"));
    on_pad(0x80,0,0,b);
    assert(std::strcmp(sent,"mute")==0);
    on_pad(0,0,0.9f,b);on_pad(0,0,0.9f,b); // held: one step
    on_pad(0x80,0,0.9f,b);
    assert(std::strcmp(sent,"deafen")==0);
    on_pad(0,0,0,b);on_pad(0,0,-0.9f,b);on_pad(0,0,0,b);on_pad(0,0,-0.9f,b); // up twice: wraps to Leave
    on_pad(0x80,0,0,b);
    assert(std::strcmp(sent,"leave")==0);
    on_pad(0x8,0,0,b);on_pad(0x80,0,0,b); // D-pad down from Leave wraps to Mute
    assert(std::strcmp(sent,"mute")==0);
    on_pad(0x2,0,0,b);on_pad(0x80,0,0,b); // D-pad up: Leave
    assert(std::strcmp(sent,"leave")==0);
    on_pad(0x40,0,0,b); // Circle closes
    assert(!capturing&&!w(panel)->visible&&w(glance)->visible&&!w(action[0])->visible);
    // Muted and live: the actions say so.
    on_text("1|c|m|1|0:12 1|Raid Night|",b);
    on_gesture(oui::Gesture::Chord,b);
    assert(shows("Unmute")&&shows("Stop Go Live"));
    on_gesture(oui::Gesture::Chord,b);

    // The core hides the glance (nobody talking, Settings: while someone talks).
    on_text("1|c|o|0||Raid Night|Kenji~-",b);
    assert(!w(glance)->visible);

    // Out of the call: no glance; the panel says so.
    on_text("1|-|o|1|||",b);
    assert(!w(glance)->visible);
    on_gesture(oui::Gesture::Chord,b);
    assert(shows("Not in a call")&&!w(row[0])->visible&&!w(action[0])->visible);
    sent[0]=0;on_pad(0x80,0,0,b);assert(!sent[0]); // nothing to act on outside a call
    on_gesture(oui::Gesture::Chord,b);

    // Not ours, or empty: ignored.
    on_text("HELLO",b);
    on_text("",b);
    on_text(nullptr,b);
    assert(!w(glance)->visible&&!w(panel)->visible);

    // More people than rows: the first six.
    State s;
    assert(parse("1|c|o|1||Big|a~t,b~t,c~-,d~-,e~-,f~-,g~-",s)&&s.count==6&&s.people[0].flag=='t');
    std::puts("discord app: ok");
    return 0;
}
