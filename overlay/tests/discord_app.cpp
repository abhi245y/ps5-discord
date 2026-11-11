// SPDX-License-Identifier: GPL-3.0-or-later
// The Discord overlay UI against a fake Builder: the core's line, the talking card, the
// call panel, its tiles and focus.
#include "../src/app/overlay_ui.hpp"
static char pill_test_dir[160]{};
#define LIVE_PILL_HOST
#define LIVE_PILL_DIR pill_test_dir
#include "../src/app/discord_app.inc"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
struct Widget {
    bool label=false, image=false, visible=true;
    char text[2200]="", path[160]="";
    oui::Rect r{};
    int valign=0,weight=0,align=0;float size=0;
    oui::Color tint{},ink{};
};
Widget widgets[192];
int used=0;

void* add(bool label,bool image,const oui::Rect* r,const char* text){
    assert(used<192); // the engine's limit
    Widget& w=widgets[used++];
    w=Widget{};w.label=label;w.image=image;w.r=*r;
    std::snprintf(image?w.path:w.text,image?160:2200,"%s",text?text:"");
    return &w;
}
void* add_panel(void*,const oui::Rect* r,const oui::Color*){return add(false,false,r,nullptr);}
void* add_label(void*,void*,const oui::Rect* r,const char* t){return add(true,false,r,t);}
void* add_image(void*,const oui::Rect* r,const char* p){return add(false,true,r,p);}
bool set_text(void*,void* w,const char* t){std::snprintf(static_cast<Widget*>(w)->text,2200,"%s",t);return true;}
bool set_visible(void*,void* w,bool on){if(w)static_cast<Widget*>(w)->visible=on;return true;}
bool set_color(void*,void*,const oui::Color*){return true;}
bool set_image(void*,void* w,const char* p){std::snprintf(static_cast<Widget*>(w)->path,160,"%s",p);return true;}
bool set_tint(void*,void* w,const oui::Color* c){static_cast<Widget*>(w)->tint=*c;return true;}
bool set_style(void*,void* w,float size,int weight,const oui::Color* c,int align){
    auto& item=*static_cast<Widget*>(w);item.size=size;item.weight=weight;item.ink=*c;item.align=align;return true;}
bool set_rect(void*,void* w,const oui::Rect* r){static_cast<Widget*>(w)->r=*r;return true;}
int hold_ms=600;
bool set_hold(void*,int ms){hold_ms=ms;return true;}
bool capturing=false;
char sent[64]="";
bool set_capture(void*,bool on){capturing=on;return true;}
bool send(void*,const char* t){std::snprintf(sent,sizeof sent,"%s",t);return true;}
char sent_text[9000]="",sent_channel[32]="";int sent_texts=0;
bool send_text(void*,const char* t,const char* c){std::snprintf(sent_text,sizeof sent_text,"%s",t);std::snprintf(sent_channel,sizeof sent_channel,"%s",c);++sent_texts;return true;}
float text_width(void*,void* l){return 15.f*float(std::strlen(static_cast<Widget*>(l)->text));} // 15 px a byte
bool set_valign(void*,void* l,int v){static_cast<Widget*>(l)->valign=v;return true;}
float sw(void*){return 1920;}
float sh(void*){return 1080;}
const oui::Builder::Vt vt{add_panel,add_label,set_text,set_visible,set_color,sw,sh,set_capture,send,
                          add_image,set_image,set_tint,set_style,set_rect,set_hold,send_text,text_width,set_valign};

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
    char temp_dir[]="/tmp/discord-pill-XXXXXX";assert(mkdtemp(temp_dir));
    std::snprintf(pill_test_dir,sizeof pill_test_dir,"%s",temp_dir);
    // Only strict generated names from older builds may be removed. Preserve unrelated
    // files, directories and symlinks, even when their names look like cache entries.
    auto touch=[&](const char* name){char path[240];std::snprintf(path,sizeof path,"%s/%s",temp_dir,name);
        FILE* f=std::fopen(path,"wb");assert(f);std::fputs("sentinel",f);assert(std::fclose(f)==0);};
    touch("ffffffffffffffff_1_400.png");touch("unrelated.png");
    char symlink_path[240];std::snprintf(symlink_path,sizeof symlink_path,"%s/ffffffffffffffff_0_400.png",temp_dir);
    assert(symlink("unrelated.png",symlink_path)==0);
    reset();
    assert(build(b));
    assert(used==188&&used<=192);
    for(int i=0;i<used;i++)assert(!widgets[i].visible); // nothing before the core says anything

    // In a call, only talkers appear: no call title, self row, header or hints.
    on_text("3|c|o|1||0|Raid Night|42:10|720p \xc2\xb7 30 fps|0.5.5.1.0.3.1.1.1.0.0.1.1|1~~|11~11_ab~Mira~t,12~~Kenji~t,13~13_cd~Ana~m,14~14_ef~Theo~-",b);
    assert(w(card)->visible&&!w(live_background)->visible&&!shows("Raid Night"));
    assert(shows("Mira")&&shows("Kenji")&&!shows("You \xc2\xb7 mic on")&&!shows("Hold Options for controls"));
    assert(ends(w(talk_avatar[0])->path,"/av/11_ab.png")&&ends(w(talk_avatar[1])->path,"avatar_none.png")); // not fetched yet
    assert(w(card)->r.h==148&&ends(w(card)->path,"glance_2.png")&&w(talk_ring[0])->r.y==34+16);
    assert(!w(talk_avatar[2])->visible&&!w(dock)->visible);

    // Live and muted: independent compact pill; one-talker card below it.
    on_text("3|c|m|1|18:42|3|Raid Night|1:02:03|720p|0.5.5.1.0.3.1.1.1.0.0.1.1|1~~|11~11_ab~Mira~t",b);
    assert(w(live_background)->visible&&shows("18:42")&&shows("3")&&!shows("Raid Night")&&!shows("You \xc2\xb7 muted"));
    assert(w(card)->r.h==88&&w(card)->r.y==pill::get(pill::Height)+14&&ends(w(live_mic)->path,"mic_off.png"));
    assert(w(live_background)->r.y==0&&w(live_background)->r.h==pill::get(pill::Height)&&w(live_background)->r.x+w(live_background)->r.w==1920);
    on_gesture(oui::Gesture::Chord,b);assert(!w(live_background)->visible&&!w(card)->visible);
    on_gesture(oui::Gesture::Chord,b);assert(w(live_background)->visible&&w(card)->visible);

    // The panel: four rows (talking ring on Mira), the tiles, focus on Mute, capture on.
    on_text("3|c|o|1||0|Raid Night|42:10|720p \xc2\xb7 30 fps|0.5.5.1.0.3.1.1.1.0.0.1.1|1~~|11~11_ab~Mira~t,13~13_cd~Ana~m,14~14_ef~Theo~d,15~~Sam~-,16~~Nova~-",b);
    on_gesture(oui::Gesture::Chord,b);
    assert(capturing&&w(dock)->visible&&!w(card)->visible);
    assert(shows("Raid Night")&&shows("42:10")&&shows("Mira")&&shows("Ana")&&shows("Theo")&&shows("Sam")&&!shows("Nova"));
    assert(w(row_ring[0])->visible&&!w(row_ring[1])->visible);
    assert(ends(w(row_icon[1])->path,"mic_off.png")&&ends(w(row_icon[2])->path,"headphones_off.png"));
    assert(shows("Mute")&&shows("Deafen")&&shows("Go Live")&&shows("720p \xc2\xb7 30 fps")&&shows("Leave"));
    assert(w(tile_focus)->r.x==1347-6&&w(tile[0])->r.y==184+143*4+10&&w(dock_end)->r.y==1036-40&&w(dock_mid)->r.h==1036-34-80);
    on_pad(0x80,0,0,b);assert(!std::strcmp(sent,"mute"));
    on_pad(0x4,0,0,b);on_pad(0x80,0,0,b);assert(!std::strcmp(sent,"deafen")&&w(tile_focus)->r.x==1347+132-6);
    on_pad(0,0.9f,0,b);on_pad(0,0.9f,0,b);on_pad(0x80,0.9f,0,b);assert(!std::strcmp(sent,"golive")); // held: one step
    on_pad(0,0,0,b);on_pad(0x1,0,0,b);on_pad(0x1,0,0,b);on_pad(0x1,0,0,b);on_pad(0x80,0,0,b);
    assert(!std::strcmp(sent,"leave")); // left from Mute wraps to Leave
    on_pad(0x40,0,0,b); // Circle closes
    assert(!capturing&&!w(dock)->visible&&!w(tile[0])->visible&&w(card)->visible);
    // One person: the panel ends under them.
    on_text("3|c|o|1||0|Raid Night|1:00|720p|0.5.5.1.0.3.1.1.1.0.0.1.1|1~~|11~11_ab~Mira~-",b);
    on_gesture(oui::Gesture::Chord,b);
    assert(w(tile[0])->r.y==184+143+10&&w(dock_end)->r.y==184+143+10+140+32+28+70-40&&w(rule)->r.y==184+143+10+172);
    on_gesture(oui::Gesture::Chord,b);

    // Muted and live: the tiles say so.
    on_text("3|c|m|1|0:12|1|Raid Night||720p|0.5.5.1.0.3.1.1.1.0.0.1.1|1~~|",b);
    on_gesture(oui::Gesture::Chord,b);
    assert(shows("Unmute")&&shows("Stop")&&shows("0:12")&&ends(w(tile_icon[0])->path,"mic_off.png"));
    on_gesture(oui::Gesture::Chord,b);

    // The card off (Settings, or nobody talking), and out of the call.
    on_text("3|c|o|0||0|Raid Night||720p|0.5.5.1.0.3.1.1.1.0.0.1.1|1~~|14~~Kenji~-",b);
    assert(!w(card)->visible);
    on_text("3|-|o|1||0||||0.5.5.1.0.3.1.1.1.0.0.1.1|1~~|",b);
    on_gesture(oui::Gesture::Chord,b);
    assert(shows("Not in a call")&&!w(row_bg[0])->visible&&!w(tile[0])->visible&&!w(timer)->visible);
    sent[0]=0;on_pad(0x80,0,0,b);assert(!sent[0]); // nothing to act on
    on_gesture(oui::Gesture::Chord,b);

    // The mini settings: Options opens them on the Go Live tab; L1/R1 moves along the tabs;
    // Cross opens a row's choices inline (up/down, Cross picks), flips a toggle; left/right
    // moves a slider; the headset row connects and disconnects; Circle goes back.
    on_text("3|c|o|1||0|Raid Night|1:00|720p|0.5.5.1.0.3.1.1.1.0.0.1.1|1~68:A5~68:A5^AirPods;00:1B^Buds|11~11_ab~Mira~-",b);
    on_gesture(oui::Gesture::Chord,b);
    on_pad(0x100,0,0,b);
    assert(!std::strcmp(w(title)->text,"Settings"));
    assert(shows("Go Live"));
    assert(shows("Quality"));
    assert(w(set_focus)->visible);
    assert(!w(tile[0])->visible);
    assert(shows("720p \xc2\xb7 30 fps")&&!shows("Category")&&!w(hint_icon[2])->visible&&w(set_chevron[0])->visible);
    on_pad(0x80,0,0,b); // open Quality's choices
    assert(w(drop_bg)->visible&&shows("1080p \xc2\xb7 60 fps")&&w(drop_on)->r.y==w(drop_bg)->r.y+6);
    assert(!w(nitro_note[0])->visible); // 720p · 30 fps: nothing to say
    on_pad(0x8,0,0,b);assert(shows("Needs Nitro or a boosted server")&&shows("Without them it may lag or fail for viewers"));
    sent[0]=0;on_pad(0x80,0,0,b); // one Select picks it (the note already said what it needs)
    assert(!std::strcmp(sent,"set golive_quality 1")&&!w(drop_bg)->visible&&shows("720p \xc2\xb7 60 fps"));
    assert(!w(nitro_note[0])->visible);
    on_pad(0x800,0,0,b); // R1: Voice
    assert(shows("Mic gain")&&shows("Call volume")&&w(slider_knob[0])->visible&&shows("100%"));
    const float knob=w(slider_knob[0])->r.x;
    on_pad(0x4,0,0,b);assert(!std::strcmp(sent,"set mic_gain 6")&&w(slider_knob[0])->r.x>knob&&shows("120%"));
    // Fast presses: a line from before them does not pull the slider back.
    on_pad(0x4,0,0,b);assert(shows("140%"));
    on_text("3|c|o|1||0|Raid Night|1:00|720p|0.6.5.1.0.3.1.1.1.0.0.1.1|1~68:A5~68:A5^AirPods;00:1B^Buds|11~11_ab~Mira~-",b);
    assert(shows("140%"));
    on_text("3|c|o|1||0|Raid Night|1:00|720p|0.7.5.1.0.3.1.1.1.0.0.1.1|1~68:A5~68:A5^AirPods;00:1B^Buds|11~11_ab~Mira~-",b);
    assert(shows("140%")); // confirmed
    on_pad(0x800,0,0,b); // Card: Show, Pill corner, Card corner, Hold Options
    assert(shows("Pill corner")&&shows("Card corner")&&shows("Hold Options"));
    on_pad(0x8,0,0,b);on_pad(0x80,0,0,b); // Pill corner: four choices, TR checked
    assert(shows("Bottom left")&&shows("Bottom right")&&!shows("Middle left")&&w(drop_check)->visible);
    assert(w(drop_check)->r.y==w(drop_bg)->r.y+6+48+10);
    assert(w(set_bg[2])->r.y>kItemY+kItemStep*2); // rows under the choices move down
    on_pad(0x8,0,0,b);on_pad(0x80,0,0,b);assert(!std::strcmp(sent,"set overlay_pill_corner 2"));
    on_pad(0x8,0,0,b);on_pad(0x80,0,0,b); // Card corner: choose TL
    on_pad(0x2,0,0,b);on_pad(0x80,0,0,b);assert(!std::strcmp(sent,"set overlay_card_corner 0"));
    on_pad(0x8,0,0,b);on_pad(0x1,0,0,b);assert(!std::strcmp(sent,"set overlay_hold 2")&&hold_ms==500);
    assert(shows("\xe2\x80\xb9  0.5 s  \xe2\x80\xba"));
    on_pad(0x800,0,0,b); // Alerts: a toggle flips with Cross
    assert(w(set_toggle[0])->visible&&ends(w(set_toggle[0])->path,"toggle_on.png"));
    on_pad(0x80,0,0,b);assert(!std::strcmp(sent,"set notify_dm 0")&&ends(w(set_toggle[0])->path,"toggle_off.png"));
    on_pad(0x80,0,0,b); // and back on
    on_pad(0x800,0,0,b); // Headset: Bluetooth, the paired ones and Disconnect, Call mode
    assert(shows("Bluetooth")&&shows("AirPods")&&!shows("Call mode")); // not AirPods' own mic: no Call mode
    on_text("3|c|o|1||0|Raid Night|1:00|720p|0.5.5.1.0.3.1.1.1.0.0.1.1|2~68:A5~68:A5^AirPods;00:1B^Buds|11~11_ab~Mira~-",b);
    assert(shows("Call mode")&&shows("High quality")); // AirPods connected (2)
    on_pad(0x8,0,0,b);on_pad(0x8,0,0,b);assert(w(set_focus)->r.y==kItemY+kItemStep*2); // on Call mode
    on_text("3|c|o|1||0|Raid Night|1:00|720p|0.5.5.1.0.3.1.1.1.0.0.1.1|1~68:A5~68:A5^AirPods;00:1B^Buds|11~11_ab~Mira~-",b);
    assert(!shows("Call mode")&&w(set_focus)->r.y==kItemY+kItemStep); // AirPods went: focus moves up a row
    on_pad(0x2,0,0,b); // back to Bluetooth
    on_pad(0x80,0,0,b);assert(!std::strcmp(sent,"bt off")&&ends(w(set_toggle[0])->path,"toggle_off.png"));
    on_text("3|c|o|1||0|Raid Night|1:00|720p|0.5.5.1.0.3.1.1.1.0.0.1.1|1~68:A5~68:A5^AirPods;00:1B^Buds|11~11_ab~Mira~-",b);
    assert(ends(w(set_toggle[0])->path,"toggle_off.png")); // the core's line has not caught up: still off
    on_pad(0x80,0,0,b);assert(!std::strcmp(sent,"bt on"));
    on_pad(0x8,0,0,b); // the headset row
    on_pad(0x80,0,0,b);assert(shows("Buds")&&shows("Disconnect")&&w(drop_check)->r.y==w(drop_bg)->r.y+6+10);
    on_pad(0x8,0,0,b);on_pad(0x80,0,0,b);assert(!std::strcmp(sent,"bt connect 00:1B"));
    on_pad(0x80,0,0,b);on_pad(0x2,0,0,b);on_pad(0x80,0,0,b);assert(!std::strcmp(sent,"bt disconnect")); // up wraps to Disconnect
    on_pad(0x80,0,0,b);on_pad(0x40,0,0,b);assert(!w(drop_bg)->visible&&w(set_focus)->visible); // Circle closes the choices
    on_pad(0x400,0,0,b);on_pad(0x400,0,0,b);on_pad(0x400,0,0,b);on_pad(0x400,0,0,b); // L1 back to Go Live
    assert(shows("Quality"));
    on_pad(0x40,0,0,b); // Circle: back to the call page
    assert(!w(set_focus)->visible&&w(tile[0])->visible&&!std::strcmp(w(title)->text,"Raid Night"));
    on_gesture(oui::Gesture::Chord,b);
    // Confirm the corner choices, then change the card independently. Legacy position is ignored.
    on_text("3|c|o|1||0|Raid Night|1:00|720p|0.5.5.1.5.3.1.1.1.0.0.2.0|1~~|11~11_ab~Mira~t",b);
    assert(w(card)->visible&&w(card)->r.x==31&&w(card)->r.y==34);
    on_text("3|c|o|1||0|Raid Night|1:00|720p|0.5.5.1.0.3.1.1.1.0.0.1.3|1~~|11~11_ab~Mira~t",b);
    assert(w(card)->r.x==1574&&w(card)->r.y==1080-34-88);
    // Live: the quality row is fixed.
    on_text("3|c|o|1|0:05|2|Raid Night|1:00|720p|0.5.5.1.0.3.1.1.1.0.0.1.1|1~~|",b);
    on_gesture(oui::Gesture::Chord,b);on_pad(0x100,0,0,b);
    sent[0]=0;on_pad(0x80,0,0,b);assert(!sent[0]&&shows("Live now")&&!w(drop_bg)->visible&&!w(set_chevron[0])->visible);
    on_pad(0x40,0,0,b);on_gesture(oui::Gesture::Chord,b);

    on_text("3|-|o|1||0||||0.5.5.1.0.3.1.1.1.0.0.1.1|1~~|",b);
    // The core quit: an open panel closes and gives the pad back; nothing opens until it is back.
    on_gesture(oui::Gesture::Chord,b);assert(capturing&&w(dock)->visible);
    on_text("3|x|o|0||0||||||",b);
    assert(!capturing&&!w(dock)->visible&&!w(card)->visible);
    on_gesture(oui::Gesture::Chord,b);assert(!capturing&&!w(dock)->visible);
    on_text("3|-|o|1||0||||0.5.5.1.0.3.1.1.1.0.0.1.1|1~~|",b);
    // The core crashed: no new line (no beat) for 10 s acts like the quit line.
    on_tick(1,b);on_text("3|c4|o|1||0|Raid Night|1:00|720p|0.5.5.1.0.3.1.1.1.0.0.1.1|1~~|11~11_ab~Mira~t",b);
    assert(w(card)->visible); // the beat digit after the state is ignored
    on_gesture(oui::Gesture::Chord,b);assert(capturing);
    on_tick(300,b);on_text("3|c9|o|1||0|Raid Night|1:00|720p|0.5.5.1.0.3.1.1.1.0.0.1.1|1~~|11~11_ab~Mira~t",b); // only the beat
    on_tick(300+60*10,b);assert(capturing); // not yet: 10 s from the last beat
    on_tick(301+60*10,b);assert(!capturing&&!w(dock)->visible&&!w(card)->visible);
    on_gesture(oui::Gesture::Chord,b);assert(!capturing);
    on_text("3|c5|o|1||0|Raid Night|1:01|720p|0.5.5.1.0.3.1.1.1.0.0.1.1|1~~|11~11_ab~Mira~t",b);assert(w(card)->visible);
    on_text("3|-|o|1||0||||0.5.5.1.0.3.1.1.1.0.0.1.1|1~~|",b);
    // The keyboard: Triangle on the Chat tab opens it for that chat; the call page has none.
    on_text("3|c|o|1||0|Raid Night|1:00|720p|0.5.5.1.0.3.1.1.1.0.0.1.1|1~~|11~11_ab~Mira~t",b);
    on_gesture(oui::Gesture::Chord,b);assert(capturing);
    auto pad=[&](std::uint32_t d,float lx=0,float ly=0){on_pad(d,lx,ly,b);};
    auto key=[&](int r,int c){kb_row=r;kb_col=c;kb_x=key_center(r,c);pad(0x80);}; // press that key
    pad(0x20);assert(!kb&&!w(kb_field)->visible&&!w(hint_icon[3])->visible); // the call page: nothing
    on_chat("H\tc\t555\t#raid-night\tNight Shift\t\t0\t\n",b);
    auto open_kb=[&]{page=1;render(b);pad(0x20);assert(kb&&w(kb_field)->visible&&w(kb_pages)->visible&&w(kb_hl)->visible);};
    sent[0]=0;open_kb();
    assert(w(kb_input)->visible&&!std::strcmp(w(kb_input)->text,"Message #raid-night")&&ends(w(kb_pages)->path,"kb_lower.png"));
    assert(w(kb_hl)->r.x==86&&w(kb_hl)->r.y==726&&w(kb_hl)->r.w==100&&w(kb_hl)->r.h==62&&ends(w(kb_hl)->path,"kb_hl_1.png"));
    assert(capturing&&w(dock)->visible&&!sent[0]); // the panel stays; the pad is the keyboard's now
    // Moving: D-pad right (+Cross types), left wraps, up/down change rows, stick one step per push.
    pad(0x80);assert(!std::strcmp(kb_text,"q")&&shows("q"));
    pad(0x4);assert(w(kb_hl)->r.x==86+108);pad(0x80);assert(!std::strcmp(kb_text,"qw"));
    pad(0x1);pad(0x1);assert(kb_col==10&&w(kb_hl)->r.x==86+1080); // wrapped to '#'
    pad(0x4);assert(kb_col==0);pad(0x1);assert(kb_col==10);
    pad(0,0.9f);pad(0,0.9f);assert(kb_col==0); // the stick: one step at once
    {const int c=kb_col;for(int i=0;i<23;i++)pad(0,0.9f); // held: a step at 300 ms, then every 6 frames
     assert(kb_col==(c+2)%11);pad(0,0);pad(0,0.9f);assert(kb_col==(c+3)%11);
     for(int i=0;i<19;i++)pad(0,0.97f);assert(kb_col==(c+4)%11); // pushed all the way: every 4
     for(int i=0;i<4;i++)pad(0,0.97f);assert(kb_col==(c+5)%11);pad(0,0);
     for(int i=0;i<6;i++)pad(0,-0.9f);pad(0,0);pad(0,0.9f);pad(0,0); // back where it was
     while(kb_col)pad(0x4);}
    pad(0,0);pad(0,-0.9f);assert(kb_col==10);pad(0,0);
    pad(0x2);assert(kb_row==0);pad(0x2);assert(kb_row==0); // no wrap up
    pad(0,0,0.9f);pad(0,0,0.9f);assert(kb_row==1);pad(0,0,0);pad(0,0,-0.9f);assert(kb_row==0);pad(0,0,0); // stick down is positive
    // Different key widths: from '#' (the 11th) down to the function row lands on Send (1.5 wide),
    // up again returns to the same column; from the 5th column, Space (3 wide).
    kb_row=1;kb_col=10;kb_x=key_center(1,10);
    pad(0x8);pad(0x8);pad(0x8);assert(kb_row==4&&kb_col==6&&ends(w(kb_hl)->path,"kb_hl_15.png")&&w(kb_hl)->r.x==86+108*9.5f&&w(kb_hl)->r.w==154);
    pad(0x2);assert(kb_row==3&&kb_col==10);
    kb_row=1;kb_col=4;kb_x=key_center(1,4);pad(0x8);pad(0x8);pad(0x8);
    assert(kb_row==4&&kb_col==2&&ends(w(kb_hl)->path,"kb_hl_3.png")&&w(kb_hl)->r.w==316&&w(kb_hl)->r.y==630+26+280);
    pad(0x1);assert(kb_col==1);pad(0x2);assert(kb_row==3&&kb_col==2); // from the @#: key (middle 2.25): the third column
    kb_row=4;kb_col=0;pad(0x1);assert(kb_col==6); // wraps along the function row
    kb_row=4;kb_col=0;kb_x=key_center(4,0);pad(0x2);assert(kb_row==3&&kb_col==0);
    pad(0x8);assert(kb_row==4&&kb_col==0);pad(0x8);assert(kb_row==4);
    // Typing letters and numbers; Shift is one-shot, twice is caps lock, a third time off.
    kb_text[0]=0;kb_len=kb_chars=kb_caret=0;
    key(1,0);key(0,0);key(0,10);assert(!std::strcmp(kb_text,"q1@")&&shows("q1@"));
    pad(0x1000);assert(kb_shift==1&&ends(w(kb_pages)->path,"kb_upper.png"));
    key(1,1);key(1,2);assert(!std::strcmp(kb_text,"q1@We")&&kb_shift==0&&ends(w(kb_pages)->path,"kb_lower.png"));
    pad(0x1000);pad(0x1000);assert(kb_shift==2&&ends(w(kb_pages)->path,"kb_caps.png"));
    key(1,3);key(1,4);key(0,1);assert(!std::strcmp(kb_text,"q1@WeRT2")&&kb_shift==2); // digits stay digits
    key(4,0);assert(kb_shift==0&&ends(w(kb_pages)->path,"kb_lower.png")); // the Shift key too
    key(4,0);key(1,5);assert(!std::strcmp(kb_text,"q1@WeRT2Y"));
    // The symbols page and back; Shift does nothing there.
    key(4,1);assert(kb_page==1&&ends(w(kb_pages)->path,"kb_symbols.png"));
    pad(0x1000);assert(kb_shift==0);
    key(3,9);key(3,10);key(1,0);key(2,10);key(0,3);
    assert(!std::strcmp(kb_text,"q1@WeRT2Y\xe2\x82\xac\xc2\xa3!^4"));
    key(4,1);assert(kb_page==0&&ends(w(kb_pages)->path,"kb_lower.png"));
    // Backspace (Square) and the caret (L1/R1) move by whole characters.
    pad(0x10);pad(0x10);pad(0x10);assert(!std::strcmp(kb_text,"q1@WeRT2Y\xe2\x82\xac\xc2\xa3")&&kb_chars==11);
    pad(0x400);assert(kb_caret==int(std::strlen("q1@WeRT2Y\xe2\x82\xac"))); // over the pound sign (2 bytes)
    pad(0x400);assert(kb_caret==int(std::strlen("q1@WeRT2Y")));            // over the euro (3 bytes)
    pad(0x10);assert(!std::strcmp(kb_text,"q1@WeRT2\xe2\x82\xac\xc2\xa3"));  // erased the Y before it
    pad(0x800);assert(kb_caret==int(std::strlen("q1@WeRT2\xe2\x82\xac")));
    pad(0x800);assert(kb_caret==kb_len);pad(0x800);assert(kb_caret==kb_len); // stays at the end
    for(int i=0;i<20;i++)pad(0x400);assert(kb_caret==0);pad(0x10);assert(kb_len>0&&kb_chars==10); // and at the start
    pad(0x20);assert(!std::strcmp(kb_text," q1@WeRT2\xe2\x82\xac\xc2\xa3")&&kb_caret==1); // Triangle: a space at the caret
    key(1,0);assert(!std::strncmp(kb_text," qq1@",5));
    // Select the whole thing empty again by erasing.
    for(int i=0;i<40;i++)pad(0x800);for(int i=0;i<40;i++)pad(0x10);
    assert(kb_len==0&&kb_chars==0&&!std::strcmp(w(kb_input)->text,"Message #raid-night"));
    // 2000 characters at most, multi-byte ones included.
    for(int i=0;i<2005;i++)key(2,0);
    assert(kb_chars==2000&&kb_len==2000);
    for(int i=0;i<2005;i++)pad(0x10);
    assert(kb_len==0);
    kb_page=1;for(int i=0;i<2005;i++)key(3,9);
    kb_page=0;
    assert(kb_chars==2000&&kb_len==6000);
    // A long text: the field shows the part around the caret, with an ellipsis at the cut end.
    // No caret character in the text: a bar over it, after the measured text before the caret.
    assert(!std::strstr(w(kb_input)->text,"|")&&!std::strncmp(w(kb_input)->text,"\xe2\x80\xa6",3)&&std::strlen(w(kb_input)->text)<400);
    assert(w(kb_bar)->r.x==kKbTextX+15.f*float(std::strlen(w(kb_input)->text))); // at the end
    for(int i=0;i<2000;i++)pad(0x400);
    assert(kb_caret==0&&std::strncmp(w(kb_input)->text,"\xe2\x80\xa6",3)&&ends(w(kb_input)->text,"\xe2\x80\xa6")&&w(kb_bar)->r.x==kKbTextX);
    for(int i=0;i<1000;i++)pad(0x800);
    assert(!std::strncmp(w(kb_input)->text,"\xe2\x80\xa6",3)&&ends(w(kb_input)->text,"\xe2\x80\xa6"));
    assert(w(kb_bar)->r.x>kKbTextX&&w(kb_bar)->r.x<kKbTextX+15.f*float(std::strlen(w(kb_input)->text)));
    assert(!std::strcmp(w(kb_measure)->text,"")||std::strlen(w(kb_measure)->text)<std::strlen(w(kb_input)->text));
    for(int i=0;i<2005;i++)pad(0x800);for(int i=0;i<2005;i++)pad(0x10);assert(kb_len==0);
    // Send with R2: the exact text goes to send_text; the keyboard closes and the draft is gone.
    const char* long_text="Hello w\xc3\xb6rld \xe2\x82\xac 5! This one is longer than the 48 bytes of a command, with \"quotes\" and \\ in it.";
    for(const char* c=long_text;*c;){int n;const unsigned cp=cp_at(long_text,int(c-long_text),&n);(void)cp;char one[8];std::snprintf(one,sizeof one,"%.*s",n,c);kb_type(one);c+=n;}
    render_kb(b);
    sent_texts=0;pad(0x2000);
    assert(sent_texts==1&&!std::strcmp(sent_text,long_text)&&std::strlen(sent_text)>48);
    assert(!std::strcmp(sent_channel,"555")); // the chat it was opened on
    assert(!kb&&!w(kb_field)->visible&&!w(kb_input)->visible&&!w(kb_pages)->visible&&!w(kb_hl)->visible&&kb_len==0);
    assert(capturing&&w(dock)->visible);
    open_kb();assert(!std::strcmp(w(kb_input)->text,"Message #raid-night"));
    pad(0x2000);assert(sent_texts==1&&kb); // nothing typed: nothing sent, still open
    // Send through its key (Cross on Send) also works.
    key(1,0);key(4,6);assert(sent_texts==2&&!std::strcmp(sent_text,"q")&&!kb);
    // Circle closes the keyboard and keeps the draft; reopening shows it.
    open_kb();key(1,0);key(1,1);pad(0x40);
    assert(!kb&&!w(kb_field)->visible&&capturing&&w(dock)->visible&&kb_len==2);
    open_kb();assert(shows("qw"));
    // Options taps do nothing while typing; Cross with no key moves nothing else.
    pad(0x100);assert(kb&&!settings&&w(kb_field)->visible);
    // The gesture (holding Options) closes the keyboard with the panel; the draft stays.
    on_gesture(oui::Gesture::Chord,b);
    assert(!kb&&!capturing&&!w(kb_field)->visible&&!w(dock)->visible&&kb_len==2);
    on_gesture(oui::Gesture::Chord,b);open_kb();assert(shows("qw"));
    // The core quits: the keyboard goes with the panel.
    on_text("3|x|o|0||0||||||",b);
    assert(!kb&&!capturing&&!w(kb_field)->visible&&!w(kb_hl)->visible);
    on_text("3|c|o|1||0|Raid Night|1:00|720p|0.5.5.1.0.3.1.1.1.0.0.1.1|1~~|11~11_ab~Mira~t",b);
    // No beat for 10 s: inert too.
    on_gesture(oui::Gesture::Chord,b);open_kb();
    on_tick(10000,b);on_tick(10000+60*10+1,b);
    assert(!kb&&!capturing&&!w(kb_field)->visible);
    on_text("3|c|o|1||0|Raid Night|1:00|720p|0.5.5.1.0.3.1.1.1.0.0.1.1|1~~|11~11_ab~Mira~t",b);
    // The call ends while typing in a chat: the keyboard stays (it types in that chat, not the call).
    on_gesture(oui::Gesture::Chord,b);open_kb();
    on_text("3|-|o|1||0||||0.5.5.1.0.3.1.1.1.0.0.1.1|1~~|",b);
    assert(kb&&w(kb_field)->visible&&capturing&&kb_len==2);
    pad(0x40);assert(!kb&&kb_len==2);
    on_gesture(oui::Gesture::Chord,b);
    on_text("3|c|o|1||0|Raid Night|1:00|720p|0.5.5.1.0.3.1.1.1.0.0.1.1|1~~|11~11_ab~Mira~t",b);
    // In the settings: Triangle does nothing.
    on_gesture(oui::Gesture::Chord,b);pad(0x100);assert(settings);
    pad(0x20);assert(!kb&&!w(kb_field)->visible&&!w(hint_icon[3])->visible);
    pad(0x40);open_kb(); // back to Chat: it opens (the draft is still "qw")
    assert(shows("qw"));pad(0x40);
    // Another chat than the call's (a mention's Open chat): Square goes back, its hint at the header's right.
    on_chat("H\tc\t556\t#other\tSomewhere\t\t0\t901\t1\n",b);page=1;render(b);
    assert(w(hint_icon[3])->visible&&!std::strcmp(w(hint_text[3])->text,"Call chat")&&ends(w(hint_icon[3])->path,"glyph_square.png"));
    assert(w(hint_icon[3])->r.y==w(hint_icon[0])->r.y&&w(hint_icon[3])->r.x>w(hint_text[0])->r.x); // in the hint row, after Type
    for(int i=0;i<4;i++)for(int j=0;j<4;j++)if(i!=j) // the four hints do not overlap
        assert(w(hint_text[i])->r.x+w(hint_text[i])->r.w<=w(hint_icon[j])->r.x||w(hint_icon[j])->r.x+w(hint_icon[j])->r.w<=w(hint_icon[i])->r.x||w(hint_icon[j])->r.x<w(hint_icon[i])->r.x);
    assert(w(hint_text[1])->r.x+w(hint_text[1])->r.w<=kDockX+kDockW-37+1); // Close still inside the panel
    sent[0]=0;pad(0x10);assert(!std::strcmp(sent,"chat back")&&!w(hint_icon[3])->visible);
    on_chat("H\tc\t555\t#raid-night\tNight Shift\t\t0\t\t2\n",b);assert(!w(hint_icon[3])->visible);
    on_gesture(oui::Gesture::Chord,b);assert(!kb&&!capturing);
    on_text("3|-|o|1||0||||0.5.5.1.0.3.1.1.1.0.0.1.1|1~~|",b);

    // ---- the tabs, Chat and Mentions ----
    const char* kCall="3|c|o|1||0|Raid Night|1:00|720p|0.5.5.1.0.3.1.1.1.0.0.1.1|1~~|11~11_ab~Mira~t";
    std::uint64_t frame=20000;
    on_tick(frame,b);on_text(kCall,b); // (a quiet stretch: the core's beat is stale after the long jump)
    auto step=[&](int n){frame+=std::uint64_t(n);on_tick(frame,b);on_text(kCall,b);};
    auto padx=[&](std::uint32_t d){on_pad(d,0,0,b);for(int i=0;i<kSlide;i++)on_tick(++frame,b);}; // a switch: its page shows after a frame, the highlight slides 8
    char feedbuf[9000];
    auto chat_feed=[&](const char* head,const char* body){std::snprintf(feedbuf,sizeof feedbuf,"%s%s",head,body);on_chat(feedbuf,b);};
    const char* kHead="H\tc\t555\t#raid-night\tNight Shift\t\t0\t\n";
    const char* kConvo="M\t1\t11_ab\tMira\t6:14 PM\t0\tboss at 9, bring fire res\n"
                       "M\t2\t\tTheo\t6:15 PM\t0\twho has the key?\n"
                       "M\t3\t13_cd\tAna\t6:16 PM\t0\tme, omw\n"
                       "M\t4\t14_ef\tKenji\t6:17 PM\t1\tyou coming?\n"
                       "M\t5\t14_ef\tKenji\t6:18 PM\t0\twhere are you\n"
                       "M\t6\t\tMira\t6:19 PM\t0\tlast one\n";
    const char* kMentions="N\t901\t555\tNight Shift \xc2\xb7 #raid-night\t14_ef\tKenji\t2m\tyou coming?\n"
                          "N\t902\t777\tDirect message\t\tTheo\t14m\tcan you stream it later?\n"
                          "N\t903\t557\tSpeedrun Lab \xc2\xb7 #routes\t11_ab\tSam\t1h\tcheck this skip\n"
                          "N\t904\t558\tCo-op Club \xc2\xb7 #general\t\tNova\t3h\tgg last night\n"
                          "N\t905\t559\tCo-op Club \xc2\xb7 #general\t\tMiro\t5h\tolder one\n";
    auto strip_on=[&]{return w(tabs_bg)->visible&&w(page_word[0])->visible&&w(page_word[1])->visible&&w(page_word[2])->visible;};
    chat_feed(kHead,kConvo);
    // The tab strip is on the panel (not on the settings); the card and a closed panel show none of it.
    assert(!strip_on()&&!w(head_title)->visible);
    on_gesture(oui::Gesture::Chord,b);
    assert(strip_on()&&page==0&&shows("Call")&&shows("Chat")&&shows("Mentions")&&w(tab_on)->r.x==1347+3);
    // The timer: a fixed box as wide as its format's widest text (probe measured two frames on),
    // right-aligned, ending at the cards' edge; the seconds change nothing; both on one baseline.
    assert(w(title)->valign==1&&w(timer)->valign==1&&w(timer)->r.y==kTitleY+12&&w(title)->r.y==kTitleY);
    assert(!std::strcmp(w(timer_probe)->text,"8:88")&&w(timer)->r.x+w(timer)->r.w==kRowX+kRowW+7);
    for(int i=0;i<3;i++)on_tick(++frame,b); // the probe is read two frames after the panel opened
    assert(w(timer)->r.w==15*4+8&&w(clock)->r.x==w(timer)->r.x-34); // 15 px a byte in this fake
    {const float tx=w(timer)->r.x,cx=w(clock)->r.x,cy=w(clock)->r.y;
     on_text("3|c|o|1||0|Raid Night|1:01|720p|0.5.5.1.0.3.1.1.1.0.0.1.1|1~~|11~11_ab~Mira~t",b);
     assert(!std::strcmp(w(timer)->text,"1:01")&&w(timer)->r.x==tx&&w(clock)->r.x==cx&&w(clock)->r.y==cy);
     on_text("3|c|o|1||0|Raid Night|10:00|720p|0.5.5.1.0.3.1.1.1.0.0.1.1|1~~|11~11_ab~Mira~t",b); // a new format
     assert(!std::strcmp(w(timer_probe)->text,"88:88"));
     for(int i=0;i<3;i++)on_tick(++frame,b);
     assert(w(timer)->r.w==15*5+8&&w(timer)->r.x+w(timer)->r.w==kRowX+kRowW+7&&w(clock)->r.x<cx);
     assert(cy>kTitleY&&cy+26<kTitleY+12+kTitleH); // the clock inside the title line
     on_text(kCall,b);for(int i=0;i<3;i++)on_tick(++frame,b);assert(w(timer)->r.x==tx);}
    // A tab switch: the page's rows show a frame later; the highlight slides over 8 frames.
    on_pad(0x800,0,0,b);assert(page==1&&!w(head_title)->visible&&w(tab_on)->r.x==1347+3);
    {const float ry=w(rule)->r.y,mh=w(dock_mid)->r.h; // the panel is already the new page's size
     for(int i=0;i<kSlide;i++)on_tick(++frame,b);assert(w(rule)->r.y==ry&&w(dock_mid)->r.h==mh&&w(head_title)->visible);
     on_pad(0x400,0,0,b);for(int i=0;i<kSlide;i++)on_tick(++frame,b);on_pad(0x800,0,0,b);assert(!w(head_title)->visible);}
    on_tick(++frame,b);assert(!w(head_title)->visible&&w(tab_on)->r.x>1347+3&&w(tab_on)->r.x<1347+3+173);
    on_tick(++frame,b);assert(w(head_title)->visible);
    for(int i=0;i<6;i++)on_tick(++frame,b);assert(w(tab_on)->r.x==1347+3+173);
    on_pad(0x400,0,0,b);on_pad(0x800,0,0,b);assert(page==1&&!w(head_title)->visible); // fast: the last one shows
    for(int i=0;i<kSlide;i++)on_tick(++frame,b);assert(w(head_title)->visible&&w(tab_on)->r.x==1347+3+173);
    on_pad(0x400,0,0,b);for(int i=0;i<kSlide;i++)on_tick(++frame,b);assert(page==0&&w(tile[0])->visible);
    assert(!w(head_title)->visible&&!w(mrow[0].bg)->visible&&w(tile[0])->visible&&shows("Raid Night"));
    // L1/R1 switch (wrapping); a held button's repeats do not.
    padx(0x800);assert(page==1&&w(tab_on)->r.x==1347+3+173&&w(head_title)->visible&&!w(tile[0])->visible&&!w(row_bg[0])->visible);
    step(40);padx(0x800);assert(page==2&&w(tab_on)->r.x==1347+3+346);
    step(40);padx(0x800);assert(page==0&&w(tile[0])->visible);
    step(40);padx(0x400);assert(page==2); // L1 from Call wraps to Mentions
    step(40);padx(0x400);assert(page==1);
    step(40);padx(0x400);step(0);assert(page==0);
    // Held R1: the press switches once; the engine's repeats (kPadRepeat) do not.
    step(40);padx(0x800);assert(page==1);
    for(int i=0;i<8;i++){step(4);padx(0x800|kPadRepeat);}
    assert(page==1);
    padx(0x800);assert(page==2); // quick new presses always switch (no gate)
    padx(0x800);assert(page==0);
    padx(0x8|kPadRepeat);assert(page==0&&focus==1);padx(0x2);assert(focus==0); // repeats still move the focus
    // Not while the settings are open: L1/R1 are the settings' tabs there.
    step(40);padx(0x100);assert(settings&&!strip_on());
    padx(0x800);assert(page==0&&tab==1);padx(0x40);assert(!settings&&page==0&&strip_on()&&tab==1);
    // Options on another page opens the settings too, and Circle comes back to it.
    step(40);padx(0x800);assert(page==1);padx(0x100);assert(settings&&!w(head_title)->visible&&!w(mrow[0].bg)->visible);
    padx(0x40);assert(!settings&&page==1&&w(head_title)->visible);
    step(40);padx(0x400);assert(page==0);

    // Chat: header, four messages (the newest at the bottom), @you on the one that mentions us.
    step(40);padx(0x800);assert(page==1);
    assert(!std::strcmp(w(head_title)->text,"#raid-night")&&!std::strcmp(w(head_sub)->text,"Night Shift"));
    assert(ends(w(head_icon)->path,"hash_icon.png")&&w(head_icon)->visible);
    assert(chat.n==6&&scroll==0);
    assert(!std::strcmp(w(mrow[0].name)->text,"Ana")&&!std::strcmp(w(mrow[3].name)->text,"Mira"));
    assert(!std::strcmp(w(mrow[2].name)->text,"Kenji")&&!std::strcmp(w(mrow[2].time)->text,"6:18 PM")&&!std::strcmp(w(mrow[2].text)->text,"where are you"));
    for(int i=0;i<kMsgRows;i++)assert(w(mrow[i].bg)->visible&&w(mrow[i].avatar)->visible&&w(mrow[i].name)->visible&&!w(mrow[i].loc)->visible&&w(mrow[i].chip)->visible==(i==1));
    assert(ends(w(mrow[0].avatar)->path,"/av/13_cd.png")&&ends(w(mrow[1].avatar)->path,"/av/14_ef.png")&&ends(w(mrow[3].avatar)->path,"avatar_none.png"));
    assert(!w(men_focus)->visible&&!w(empty_text)->visible);
    assert(w(in_field)->visible&&!std::strcmp(w(in_text)->text,"Message #raid-night")&&w(in_kb)->visible);
    assert(!std::strcmp(w(hint_text[0])->text,"Type")&&!std::strcmp(w(hint_text[2])->text,"Tabs")&&!std::strcmp(w(hint_text[1])->text,"Close"));
    assert(ends(w(hint_icon[0])->path,"glyph_triangle.png")&&ends(w(hint_icon[2])->path,"chip_l1r1.png")&&!w(hint_icon[3])->visible);
    assert(w(dock_end)->r.y+40==kInputY+kInputH+32+28+70&&w(rule)->r.y==kInputY+kInputH+32);
    // Scrolling: up shows older, down newer, never past the fetched ones.
    padx(0x2);assert(scroll==1&&!std::strcmp(w(mrow[3].name)->text,"Kenji")&&!std::strcmp(w(mrow[0].name)->text,"Theo"));
    padx(0x2);assert(scroll==2&&!std::strcmp(w(mrow[0].name)->text,"Mira")&&!std::strcmp(w(mrow[0].text)->text,"boss at 9, bring fire res"));
    padx(0x2);assert(scroll==2);
    assert(!w(mrow[0].chip)->visible&&w(mrow[3].chip)->visible&&ends(w(mrow[3].bg)->path,"msg_row_me.png")&&ends(w(mrow[0].bg)->path,"msg_row.png"));
    assert(w(mrow[3].text)->r.x==1347+112+72&&w(mrow[1].text)->r.x==1347+112); // the chip's room
    // A new message arriving while scrolled keeps the view where it was; at the end it follows.
    chat_feed(kHead,(std::string(kConvo)+"M\t7\t\tTheo\t6:20 PM\t0\tnew one\n").c_str());
    assert(scroll==3&&!std::strcmp(w(mrow[0].name)->text,"Mira")&&chat.n==7);
    padx(0x8);padx(0x8);padx(0x8);assert(scroll==0&&!std::strcmp(w(mrow[3].text)->text,"new one"));
    padx(0x8);assert(scroll==0);
    // Only the Chat page reacts to the D-pad that way: the call page's tiles did not move.
    assert(focus==0);

    // A DM: the header is the person (their avatar), "Direct message" under it.
    chat_feed("H\td\t777\tTheo\tDirect message\t14_ef\t0\t\n","M\t1\t\tTheo\t6:15 PM\t0\thi\n");
    assert(!std::strcmp(w(head_title)->text,"Theo")&&!std::strcmp(w(head_sub)->text,"Direct message")&&ends(w(head_icon)->path,"/av/14_ef.png"));
    assert(!std::strcmp(w(in_text)->text,"Message Theo")&&scroll==0); // another chat: the view starts at its end
    chat_feed("H\td\t777\tTheo\tDirect message\t\t0\t\n","M\t1\t\tTheo\t6:15 PM\t0\thi\n");
    assert(ends(w(head_icon)->path,"hash_icon.png")); // no picture yet

    // Empty states, short.
    chat_feed(kHead,"");
    assert(w(empty_text)->visible&&!std::strcmp(w(empty_text)->text,"No messages yet")&&!w(mrow[0].bg)->visible&&w(in_field)->visible);
    chat_feed("H\t-\t\t\t\t\t0\t\n","");
    assert(!std::strcmp(w(head_title)->text,"No chat")&&!std::strcmp(w(head_sub)->text,"Open one in Discord")&&!w(in_field)->visible&&!w(empty_text)->visible);
    assert(!w(hint_icon[0])->visible&&w(hint_icon[1])->visible&&w(hint_icon[2])->visible); // nothing to type in
    sent_texts=0;padx(0x20);assert(!kb);
    assert(w(dock_end)->r.y<400); // a short panel
    chat_feed(kHead,kConvo);

    // Triangle opens the keyboard for THIS chat; R2 sends to its channel.
    kb_text[0]=0;kb_len=kb_chars=kb_caret=0; // (the draft of the tests before)
    padx(0x20);assert(kb&&w(kb_field)->visible&&!std::strcmp(w(kb_input)->text,"Message #raid-night")&&!std::strcmp(kb_channel,"555"));
    key(1,0);key(1,1);assert(!std::strcmp(kb_text,"qw"));
    sent_texts=0;kb_text[0]=0;kb_len=kb_chars=kb_caret=0;
    key(1,2);key(1,3);
    pad(0x2000);assert(sent_texts==1&&!std::strcmp(sent_text,"er")&&!std::strcmp(sent_channel,"555")&&!kb);
    // The call page has no keyboard (the Chat tab types in the call's chat).
    step(40);padx(0x400);assert(page==0);
    step(40);padx(0x20);assert(!kb);
    // A channel's chat while not in a call: the chat is sent to its channel, the call page cannot type.
    on_text("3|-|o|1||0||||0.5.5.1.0.3.1.1.1.0.0.1.1|1~~|",b);
    assert(page==0);
    padx(0x20);assert(!kb);
    frame+=40;on_tick(frame,b);padx(0x800);assert(page==1&&w(in_field)->visible);
    padx(0x20);assert(kb&&!std::strcmp(kb_channel,"555"));
    key(1,0);pad(0x2000);assert(sent_texts==2&&!std::strcmp(sent_channel,"555")&&!kb);
    // The call ending does not close a chat's keyboard.
    on_text(kCall,b);padx(0x20);assert(kb);on_text("3|-|o|1||0||||0.5.5.1.0.3.1.1.1.0.0.1.1|1~~|",b);assert(kb);pad(0x40);
    on_text(kCall,b);
    step(40);padx(0x400);assert(page==0);
    pad(0x40); // (the panel is on the call page; Circle closes it: it opens again below)
    assert(!open);on_gesture(oui::Gesture::Chord,b);

    // Mentions: rows (place, person, text, age), the first selected; the badge counts them.
    chat_feed("H\tc\t555\t#raid-night\tNight Shift\t\t3\t\n",std::string(kMentions).c_str());
    assert(chat.unseen==3&&page==0&&w(tab_badge)->visible&&!std::strcmp(w(tab_count)->text,"3")); // on Call: the badge shows
    step(40);padx(0x800);assert(page==1&&w(tab_badge)->visible); // and on Chat
    sent[0]=0;step(40);padx(0x800);step(0);
    assert(page==2&&!std::strcmp(sent,"seen")&&chat.unseen==0&&!w(tab_badge)->visible&&!w(tab_count)->visible);
    assert(w(mrow[0].loc)->visible&&!std::strcmp(w(mrow[0].loc)->text,"Night Shift \xc2\xb7 #raid-night")&&!std::strcmp(w(mrow[0].name)->text,"Kenji"));
    assert(!std::strcmp(w(mrow[0].time)->text,"2m")&&!std::strcmp(w(mrow[0].text)->text,"you coming?"));
    assert(!std::strcmp(w(mrow[1].loc)->text,"Direct message")&&!std::strcmp(w(mrow[1].name)->text,"Theo")&&!std::strcmp(w(mrow[3].time)->text,"3h"));
    assert(!w(mrow[0].chip)->visible&&!w(head_title)->visible&&!w(in_field)->visible&&!w(empty_text)->visible);
    assert(ends(w(mrow[0].bg)->path,"men_row.png")&&w(mrow[0].bg)->r.h==172&&w(mrow[1].bg)->r.y==124+182);
    assert(w(men_focus)->visible&&w(men_focus)->r.y==124);
    assert(!std::strcmp(w(hint_text[0])->text,"Open")&&ends(w(hint_icon[0])->path,"glyph_cross.png")&&!std::strcmp(w(hint_text[2])->text,"Tabs"));
    assert(w(rule)->r.y==124+182*3+172+32);
    // Up and down select (no wrap); only four rows show, the list follows the selection.
    padx(0x2);assert(men_sel==0);
    padx(0x8);assert(men_sel==1&&w(men_focus)->r.y==124+182);
    padx(0x8);padx(0x8);assert(men_sel==3&&w(men_focus)->r.y==124+182*3);
    padx(0x8);assert(men_sel==4&&men_top==1&&!std::strcmp(w(mrow[0].name)->text,"Theo")&&!std::strcmp(w(mrow[3].name)->text,"Miro")&&w(men_focus)->r.y==124+182*3);
    padx(0x8);assert(men_sel==4);
    padx(0x2);padx(0x2);padx(0x2);padx(0x2);assert(men_sel==0&&men_top==0);
    // A new feed keeps the selection on the same mention, even when it moved.
    padx(0x8);padx(0x8);assert(men_sel==2);
    chat_feed(kHead,(std::string("")+"N\t950\t560\tNew Server \xc2\xb7 #new\t\tZed\tnow\tfresh\n"+kMentions).c_str());
    assert(men_sel==3&&!std::strcmp(men_id,"903")&&!std::strcmp(w(mrow[0].name)->text,"Zed")&&!std::strcmp(w(mrow[0].time)->text,"now"));
    // A new mention while the tab is open is read at once (the core is told).
    sent[0]=0;
    chat_feed("H\tc\t555\t#raid-night\tNight Shift\t1\t1\t\n",(std::string("")+"N\t950\t560\tNew Server \xc2\xb7 #new\t\tZed\tnow\tfresh\n"+kMentions).c_str());
    assert(!std::strcmp(sent,"seen")&&chat.unseen==0);
    // Cross: Open chat. The command names the channel and the message; the Chat tab waits for it.
    padx(0x2);padx(0x2);padx(0x2);assert(men_sel==0);
    padx(0x8);padx(0x8);assert(men_sel==2);
    sent[0]=0;padx(0x80);
    assert(!std::strcmp(sent,"chat 777 902")&&page==1);
    assert(pend_chan[0]&&!std::strcmp(w(head_title)->text,"Theo")&&!std::strcmp(w(head_sub)->text,"Direct message")); // a DM: the person
    assert(w(empty_text)->visible&&!std::strcmp(w(empty_text)->text,"Loading\xe2\x80\xa6")&&!w(in_field)->visible&&!w(mrow[0].bg)->visible);
    sent_texts=0;padx(0x20);assert(!kb); // not while it loads
    const std::string men2=std::string("N\t950\t560\tNew Server \xc2\xb7 #new\t\tZed\tnow\tfresh\n")+kMentions; // (every feed carries the mentions)
    // It arrives: the chat is that channel's, the mentioned message in view (and marked).
    chat_feed("H\td\t777\tTheo\tDirect message\t14_ef\t0\t902\n",("M\t900\t\tTheo\t5:00 PM\t0\tfirst\nM\t901\t\tYou\t5:01 PM\t0\tmine\nM\t902\t\tTheo\t5:02 PM\t1\tcan you stream it later?\nM\t903\t\tTheo\t5:03 PM\t0\tafter\n"+men2).c_str());
    assert(!pend_chan[0]&&!w(empty_text)->visible&&w(in_field)->visible&&!std::strcmp(w(head_title)->text,"Theo"));
    assert(w(mrow[2].chip)->visible&&!std::strcmp(w(mrow[2].text)->text,"can you stream it later?"));
    // The choice holds: another feed of the same chat keeps it; the same focus does not scroll again.
    padx(0x2);assert(scroll==0); // four messages: nothing to scroll
    chat_feed("H\td\t777\tTheo\tDirect message\t14_ef\t0\t902\n",("M\t900\t\tTheo\t5:00 PM\t0\tfirst\nM\t901\t\tYou\t5:01 PM\t0\tmine\nM\t902\t\tTheo\t5:02 PM\t1\tcan you stream it later?\nM\t903\t\tTheo\t5:03 PM\t0\tafter\n"+men2).c_str());
    assert(scroll==0);
    // Back to Mentions and open a channel one whose messages are many: the mentioned one is third of four.
    step(40);padx(0x400);assert(page==0);step(40);padx(0x400);assert(page==2);
    assert(men_sel==2);
    {std::string many="";for(int i=1;i<=20;i++){char one[96];std::snprintf(one,sizeof one,"M\t%d\t\tP%d\t5:%02d PM\t0\tmessage %d\n",100+i,i,i,i);many+=one;}
     sent[0]=0;padx(0x2);assert(men_sel==1);padx(0x80);assert(!std::strcmp(sent,"chat 555 901")&&pend_chan[0]);
     chat_feed("H\tc\t555\t#raid-night\tNight Shift\t\t0\t110\n",(many+men2).c_str());
     // the focus message (110, index 9) with one after it below: the last row is index 10
     assert(scroll==9&&!std::strcmp(w(mrow[3].text)->text,"message 11")&&!std::strcmp(w(mrow[2].text)->text,"message 10"));
     padx(0x2);assert(scroll==10);
     chat_feed("H\tc\t555\t#raid-night\tNight Shift\t\t0\t110\n",(many+men2).c_str());assert(scroll==10);} // the same focus does not pull it back
    // Open chat with the Chat tab not answering: after a few seconds it shows what the core has.
    step(40);padx(0x400);step(40);padx(0x400);assert(page==2);padx(0x80);assert(pend_chan[0]&&page==1);
    chat_feed("H\tc\t556\t#other\tSomewhere\t\t0\t\n",men2.c_str()); // the core still has another chat
    assert(!std::strcmp(w(empty_text)->text,"Loading\xe2\x80\xa6")&&!std::strcmp(w(head_title)->text,"#raid-night"));
    step(60*4+1);assert(!pend_chan[0]&&!std::strcmp(w(empty_text)->text,"No messages yet")&&!std::strcmp(w(head_title)->text,"#other"));
    // An oversized command is not sent (cmd.bin holds 47 bytes).
    padx(0x400);step(40);padx(0x400);
    chat_feed("H\tc\t555\t#raid-night\tNight Shift\t\t0\t\n","N\t12345678901234567890123\t12345678901234567890123\tX\t\tA\t1m\tt\n");
    sent[0]=0;padx(0x80);assert(!sent[0]&&page==2);
    // No mentions.
    chat_feed(kHead,"");
    assert(w(empty_text)->visible&&!std::strcmp(w(empty_text)->text,"No mentions")&&!w(mrow[0].bg)->visible&&!w(men_focus)->visible);
    assert(!w(hint_icon[0])->visible&&w(hint_icon[1])->visible);
    sent[0]=0;padx(0x80);padx(0x8);assert(!sent[0]);
    // The badge's top: 9+.
    chat_feed("H\tc\t555\t#raid-night\tNight Shift\t\t12\t\n","");
    assert(!w(tab_badge)->visible); // read as it comes in, on the tab
    step(40);padx(0x400);assert(page==1);chat.unseen=12;render(b);assert(w(tab_badge)->visible&&!std::strcmp(w(tab_count)->text,"9+"));

    // A feed that is not one, or is damaged, changes nothing and breaks nothing.
    step(40);padx(0x800);step(40);padx(0x400);
    chat_feed(kHead,kConvo);
    const int was=chat.n;
    on_chat(nullptr,b);on_chat("",b);on_chat("M\t1\ta\tb\tc\t0\ttext\n",b);on_chat("3|c|o|1||0|Raid Night",b);on_chat("H",b);on_chat("Hx\t1",b);
    assert(chat.n==was&&!std::strcmp(chat.title,"#raid-night"));
    on_chat("H\tz\t\n",b);assert(chat.kind=='-'&&chat.n==0); // a header alone: an unknown kind is no chat
    on_chat("H\tc\t5\tT\tS\ti\tnot a number\tf\nM\n\n\tM\t\nM\tonly id\nX\t1\t2\nN\n",b);assert(chat.kind=='c'&&chat.unseen==0&&chat.n==1&&!std::strcmp(chat.msgs[0].id,"only id"));
    {std::string many="H\tc\t5\tT\tS\ti\t0\t\n";for(int i=0;i<40;i++)many+="M\t"+std::to_string(i)+"\ta\tb\tc\t0\ttext\n";
     for(int i=0;i<30;i++)many+="N\t"+std::to_string(i)+"\t5\tp\ta\tn\t1m\ttext\n";
     std::vector<char> big(many.begin(),many.end());big.push_back(0);on_chat(big.data(),b);
     assert(chat.n==kMsgMax&&chat.nmen==kMenMax);}
    {std::string wide="H\tc\t"+std::string(100,'7')+"\t"+std::string(200,'T')+"\t"+std::string(100,'S')+"\t"+std::string(100,'I')+"\t5\t"+std::string(50,'F')+"\nM\t"+std::string(100,'1')+"\t\t"+std::string(150,'n')+"\t"+std::string(150,'t')+"\t1\t"+std::string(2000,'x')+"\n";
     on_chat(wide.c_str(),b);
     assert(std::strlen(chat.chan)<24&&std::strlen(chat.title)<72&&chat.unseen==5&&chat.n==1&&std::strlen(chat.msgs[0].text)<140);
     assert(std::strlen(chat.msgs[0].id)<24&&std::strlen(chat.msgs[0].name)<40&&std::strlen(chat.msgs[0].time)<12);}
    {std::string wide="H\tc\t"+std::string(3000,'7')+"\t5\n";on_chat(wide.c_str(),b); // a header longer than a line: cut, kept
     assert(chat.kind=='c'&&std::strlen(chat.chan)<24&&chat.unseen==0);}
    {std::string wide="H\tc\t1\tT\tS\ti\t0\t\n";for(int i=0;i<30;i++)wide+="M\t"+std::to_string(i)+"\ta\tb\tc\t0\t"+std::string(300,'y')+"\n";
     on_chat(wide.c_str(),b);assert(chat.n==kMsgMax&&std::strlen(chat.msgs[0].text)<140);}
    {std::string wide="H\tc\t1\tT\tS\ti\t0\t\n";for(int i=0;i<10;i++)wide+="M\t"+std::to_string(i)+"\ta\tb\tc\t0\t\xf0\x9f\x98\x80 \xe2\x82\xac\xc3\xa9\n";
     on_chat(wide.c_str(),b);step(40);padx(0x800);assert(page==2);step(40);padx(0x400);}
    on_chat("H\t-\t\t\t\t\t0\t\n",b);

    // The core quits: everything closes, the keyboard too, wherever it was.
    on_text(kCall,b);
    chat_feed(kHead,kConvo);
    padx(0x20);assert(page==1&&kb&&capturing);
    on_text("3|x|o|0||0||||||",b);
    assert(!kb&&!capturing&&!w(dock)->visible&&!w(kb_field)->visible&&!strip_on()&&!w(head_title)->visible&&!w(mrow[0].bg)->visible&&
           !w(men_focus)->visible&&!w(in_field)->visible&&!w(tab_badge)->visible);
    on_gesture(oui::Gesture::Chord,b);assert(!capturing&&!w(dock)->visible);
    // Not beating for ten seconds: inert too, from the Mentions tab.
    on_text(kCall,b);
    chat_feed(kHead,kMentions);
    on_gesture(oui::Gesture::Chord,b);assert(page==0);step(40);padx(0x400);assert(page==2&&w(men_focus)->visible);
    on_tick(frame+1000,b);
    assert(!capturing&&!w(dock)->visible&&!strip_on()&&!w(men_focus)->visible&&!w(mrow[0].bg)->visible);
    on_text(kCall,b);
    on_text("3|-|o|1||0||||0.5.5.1.0.3.1.1.1.0.0.1.1|1~~|",b);
    // Not ours: ignored.
    on_text("1|c|o|1||Raid Night|",b);on_text("",b);on_text(nullptr,b);
    assert(!w(card)->visible&&!w(dock)->visible);
    State s;
    assert(parse("3|c|o|1||0|Big|||||a~~A~t,b~~B~t,c~~C~-,d~~D~-,e~~E~-,f~~F~-,g~~G~-,h~~H~-,i~~I~-",s)&&s.count==8);
    // New settings append to the wire format. Older lines keep both TR defaults.
    assert(parse("3|c|o|0|12:34|12|Raid Night|||0.5.5.0.0.3.1.1.1.0.0|0~~|",s));
    assert(s.set[kPillCorner]==1&&s.set[kCardCorner]==1);
    assert(parse("3|c|o|1||||||0.5.5.1.0.3.1.1.1.0.0.2.3|0~~|",s));
    assert(s.set[kPillCorner]==2&&s.set[kCardCorner]==3);
    auto live_feed=[&](int mode,int glance,int pc,int cc,const char* people,const char* duration="12:34",int viewers=12,char me='o'){
        char line[1024];
        std::snprintf(line,sizeof line,"3|c|%c|%d|%s|%d|Raid Night|||0.5.5.%d.0.3.1.1.1.0.0.%d.%d|0~~|%s",
                      me,glance,duration,viewers,mode,pc,cc,people);
        on_text(line,b);
    };
    // Card Off never suppresses live status, even with talkers.
    live_feed(0,0,1,1,"11~~Mira~t");
    assert(w(live_background)->visible&&!w(card)->visible&&!w(talk_name[0])->visible);
    // Mode 1 + live + nobody talking: pill only. Even mode 2 cannot show an empty card.
    live_feed(1,0,1,1,"11~~Mira~-");assert(w(live_background)->visible&&!w(card)->visible);
    live_feed(2,1,1,1,"");assert(w(live_background)->visible&&!w(card)->visible);
    live_feed(1,1,1,1,"11~~Mira~-");assert(!w(card)->visible); // defensive: stale glance bit
    // All corner pairs: flush pill, independent card insets, shared 14 px gap.
    for(int pc=0;pc<4;pc++)for(int cc=0;cc<4;cc++){
        live_feed(1,1,pc,cc,"11~~Mira~t,12~~Kenji~t");
        assert(w(live_background)->visible&&w(card)->visible&&w(card)->r.h==148);
        assert(w(live_background)->r.y==(pc<2?0:1080-pill::get(pill::Height)));
        if(pc%2)assert(w(live_background)->r.x+w(live_background)->r.w==1920);
        else assert(w(live_background)->r.x==0);
        const auto layout=pill::layout(pc,0,0,live_time_w,live_count_w);
        char suffix[64];std::snprintf(suffix,sizeof suffix,"_%d_%d.png",pc,int(layout.background.w*2));
        assert(ends(w(live_background)->path,suffix));
        const auto shape=pill::shape(layout.background.w,layout.divider_x,pc);
        assert(shape.radius[pc]==6&&shape.radius[pc^3]==shape.h/2); // shared clamp, raw inner=16
        assert(w(live_icon)->r.x==layout.live.x&&w(live_time)->r.y==layout.time.y);
        assert(w(live_eye)->r.x==layout.eye.x&&w(live_count)->r.x==layout.count.x&&w(live_mic)->r.x==layout.mic.x);
        const float inset=pc==cc?pill::get(pill::Height)+14:34;
        assert(w(card)->r.x==(cc%2?1574:31));
        assert(w(card)->r.y==(cc<2?inset:1080-inset-148));
    }
    // Bottom card grows upward; live ending restores the ordinary bottom inset.
    live_feed(1,1,3,3,"11~~Mira~t");const float bottom=w(card)->r.y+w(card)->r.h;
    live_feed(1,1,3,3,"11~~Mira~t,12~~Kenji~t,13~~Ana~t");
    assert(w(card)->r.h==208&&w(card)->r.y+w(card)->r.h==bottom);
    live_feed(1,1,3,3,"11~~Mira~t","",0);
    assert(!w(live_background)->visible&&w(card)->r.y==1080-34-88);
    // Mic state: normal white mic; muted and deafened both use the red slash.
    live_feed(1,1,1,1,"11~~Mira~t");assert(ends(w(live_mic)->path,"/mic.png"));
    live_feed(1,1,1,1,"11~~Mira~t","12:34",12,'m');assert(ends(w(live_mic)->path,"mic_off.png"));
    live_feed(1,1,1,1,"11~~Mira~t","12:34",12,'d');assert(ends(w(live_mic)->path,"mic_off.png"));
    on_tick(frame_now+1,b);on_tick(frame_now+1,b);on_tick(frame_now+1,b);
    const float short_width=live_time_w,short_viewers=live_count_w;
    live_feed(1,1,1,1,"11~~Mira~t","1:02:45",128);
    assert(shows("1:02:45")&&shows("128"));
    assert(w(live_count)->r.x+w(live_count)->r.w<pill::layout(live_time_w,live_count_w).divider.x);
    on_tick(frame_now+1,b);on_tick(frame_now+1,b);on_tick(frame_now+1,b);
    assert(live_time_w==105.5f&&live_count_w==45.5f&&live_time_w>short_width&&live_count_w>short_viewers); // shared lab measurement slack
    assert(w(live_background)->r.x+w(live_background)->r.w==1920);
    live_feed(1,1,1,1,"11~~Mira~t","1:02:46",129);
    assert(live_probe_wait==0&&live_time_w==105.5f); // stable format: no width jitter
    assert(w(live_time)->size==14&&w(live_time)->weight==1&&w(live_time)->align==1&&w(live_time)->valign==2);
    assert(w(live_count)->r.y==2.5f&&w(live_eye)->r.w==21.5f&&w(live_mic)->r.w==21.5f);
    assert(w(live_mic)->tint.r==1&&w(live_mic)->tint.g==1&&w(live_mic)->tint.b==1);
    live_feed(1,1,1,1,"11~~Mira~t","1:02:46",129,'m');
    assert(w(live_mic)->tint.r==237.f/255&&w(live_mic)->tint.g==60.f/255&&w(live_mic)->tint.b==69.f/255);
    char cached[200];std::snprintf(cached,sizeof cached,"%s",live_image_path);
    struct stat before{},after{};assert(stat(cached,&before)==0);
    const float fixed_width=w(live_background)->r.w;
    for(int i=0;i<20;i++){
        char duration[16];std::snprintf(duration,sizeof duration,"1:02:%02d",i);
        live_feed(0,0,1,1,"",duration,100+i);
        on_tick(frame_now+1,b);
        assert(live_probe_wait==0&&w(live_background)->r.w==fixed_width&&!std::strcmp(cached,live_image_path));
        assert(w(live_time)->visible&&w(live_icon)->visible&&!w(card)->visible);
    }
    assert(stat(cached,&after)==0&&before.st_ino==after.st_ino&&before.st_mtime==after.st_mtime);
    // Corner switch then return must reuse the original PNG without rewriting it.
    live_feed(0,0,0,1,"","1:02:45",128);live_feed(0,0,1,1,"","1:02:45",128);
    assert(stat(cached,&after)==0&&before.st_ino==after.st_ino&&before.st_mtime==after.st_mtime);
    char stale[240];std::snprintf(stale,sizeof stale,"%s/ffffffffffffffff_1_400.png",temp_dir);
    assert(access(stale,F_OK)!=0&&access(symlink_path,F_OK)==0);
    touch("ffffffffffffffff_1_400.png"); // cleanup must not repeat on later width/corner changes
    live_feed(0,0,2,1,"","1:02:45",128);assert(access(stale,F_OK)==0);
    live_feed(0,0,1,1,"","12:34",1);
    assert(w(live_count)->visible&&!std::strcmp(w(live_count)->text,"1"));
    // The shared rasterizer also rejects invalid geometry without hiding native content.
    pill::parameters[pill::RenderScale].value=0;
    live_feed(0,0,3,1,"","1:02:45",9999);
    assert(!live_background_ready&&!w(live_background)->visible&&shows("1:02:45")&&shows("9999"));
    assert(w(live_icon)->visible&&w(live_eye)->visible&&w(live_mic)->visible);
    pill::set(pill::RenderScale,2);
    live_feed(0,0,2,1,"","1:02:45",9999);assert(live_background_ready&&w(live_background)->visible);
    on_text("3|x|o|0|1:02:46|129||||||",b);assert(!w(live_background)->visible&&!w(card)->visible);
    // An unwritable cache (a regular file in place of the directory) never breaks build
    // or native pill content. No console path is used by this test.
    touch("blocked");
    std::snprintf(pill_test_dir,sizeof pill_test_dir,"%s/blocked",temp_dir);
    reset();used=0;
    st.state='c';st.me='d';st.set[3]=0;st.viewers=1;std::snprintf(st.live,sizeof st.live,"12:34");
    assert(build(b)); // Includes an attempted raster write while constructing the live scene.
    live_feed(0,0,1,1,"","12:34",1,'d');
    on_tick(frame_now+1,b);on_tick(frame_now+1,b);on_tick(frame_now+1,b);
    assert(!live_background_ready&&!w(live_background)->visible);
    assert(shows("12:34")&&shows("1")&&w(live_icon)->visible&&w(live_eye)->visible&&w(live_mic)->visible);
    assert(ends(w(live_mic)->path,"mic_off.png")&&w(live_mic)->tint.g==60.f/255);
    assert(!live_cache_name("../ffffffffffffffff_1_400.png")&&!live_cache_name("ffffffffffffffff_4_400.png"));
    // Bounded removal of this test's own flat temporary cache (symlink unlinked, not followed).
    DIR* dir=opendir(temp_dir);assert(dir);
    for(unsigned i=0;i<256;i++){
        const auto* entry=readdir(dir);if(!entry)break;
        if(entry->d_name[0]=='.')continue;
        char path[256];const int n=std::snprintf(path,sizeof path,"%s/%s",temp_dir,entry->d_name);
        assert(n>0&&std::size_t(n)<sizeof path);assert(unlink(path)==0);
    }
    closedir(dir);assert(rmdir(temp_dir)==0);
    std::puts("discord app: ok");
    return 0;
}
