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
    char text[2200]="", path[160]="";
    oui::Rect r{};
};
Widget widgets[160];
int used=0;

void* add(bool label,bool image,const oui::Rect* r,const char* text){
    assert(used<160); // the engine's limit
    Widget& w=widgets[used++];
    w.label=label;w.image=image;w.r=*r;
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
bool set_tint(void*,void*,const oui::Color*){return true;}
bool set_style(void*,void*,float,int,const oui::Color*,int){return true;}
bool set_rect(void*,void* w,const oui::Rect* r){static_cast<Widget*>(w)->r=*r;return true;}
int hold_ms=600;
bool set_hold(void*,int ms){hold_ms=ms;return true;}
bool capturing=false;
char sent[64]="";
bool set_capture(void*,bool on){capturing=on;return true;}
bool send(void*,const char* t){std::snprintf(sent,sizeof sent,"%s",t);return true;}
char sent_text[9000]="";int sent_texts=0;
bool send_text(void*,const char* t){std::snprintf(sent_text,sizeof sent_text,"%s",t);++sent_texts;return true;}
float sw(void*){return 1920;}
float sh(void*){return 1080;}
const oui::Builder::Vt vt{add_panel,add_label,set_text,set_visible,set_color,sw,sh,set_capture,send,
                          add_image,set_image,set_tint,set_style,set_rect,set_hold,send_text};

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
    assert(used==124);
    for(int i=0;i<used;i++)assert(!widgets[i].visible); // nothing before the core says anything

    // In a call, two talking, the card on: their avatars and names, "You" under them.
    on_text("3|c|o|1||0|Raid Night|42:10|720p \xc2\xb7 30 fps|0.5.5.1.0.3.1.1.1.0.0|1~~|11~11_ab~Mira~t,12~~Kenji~t,13~13_cd~Ana~m,14~14_ef~Theo~-",b);
    assert(w(card)->visible&&!w(pill)->visible&&shows("Raid Night"));
    assert(shows("Mira")&&shows("Kenji")&&shows("You \xc2\xb7 mic on"));
    assert(ends(w(talk_avatar[0])->path,"/av/11_ab.png")&&ends(w(talk_avatar[1])->path,"avatar_none.png")); // not fetched yet
    assert(w(card)->r.h==325&&ends(w(card)->path,"glance_2.png")&&w(you_ring)->r.y==34+107+120-25);
    assert(!w(talk_avatar[2])->visible&&!w(dock)->visible);

    // Live and muted: the LIVE pill and "time | viewers"; the card shrinks to one talker.
    on_text("3|c|m|1|18:42|3|Raid Night|1:02:03|720p|0.5.5.1.0.3.1.1.1.0.0|1~~|11~11_ab~Mira~t",b);
    assert(w(pill)->visible&&shows("18:42  |  3 watching")&&!shows("Raid Night")&&shows("You \xc2\xb7 muted"));
    assert(w(card)->r.h==265&&ends(w(you_mic)->path,"mic_off.png"));

    // The panel: four rows (talking ring on Mira), the tiles, focus on Mute, capture on.
    on_text("3|c|o|1||0|Raid Night|42:10|720p \xc2\xb7 30 fps|0.5.5.1.0.3.1.1.1.0.0|1~~|11~11_ab~Mira~t,13~13_cd~Ana~m,14~14_ef~Theo~d,15~~Sam~-,16~~Nova~-",b);
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
    on_text("3|c|o|1||0|Raid Night|1:00|720p|0.5.5.1.0.3.1.1.1.0.0|1~~|11~11_ab~Mira~-",b);
    on_gesture(oui::Gesture::Chord,b);
    assert(w(tile[0])->r.y==184+143+10&&w(dock_end)->r.y==184+143+10+140+32+28+70-40&&w(rule)->r.y==184+143+10+172);
    on_gesture(oui::Gesture::Chord,b);

    // Muted and live: the tiles say so.
    on_text("3|c|m|1|0:12|1|Raid Night||720p|0.5.5.1.0.3.1.1.1.0.0|1~~|",b);
    on_gesture(oui::Gesture::Chord,b);
    assert(shows("Unmute")&&shows("Stop")&&shows("0:12")&&ends(w(tile_icon[0])->path,"mic_off.png"));
    on_gesture(oui::Gesture::Chord,b);

    // The card off (Settings, or nobody talking), and out of the call.
    on_text("3|c|o|0||0|Raid Night||720p|0.5.5.1.0.3.1.1.1.0.0|1~~|14~~Kenji~-",b);
    assert(!w(card)->visible);
    on_text("3|-|o|1||0||||0.5.5.1.0.3.1.1.1.0.0|1~~|",b);
    on_gesture(oui::Gesture::Chord,b);
    assert(shows("Not in a call")&&!w(row_bg[0])->visible&&!w(tile[0])->visible&&!w(timer)->visible);
    sent[0]=0;on_pad(0x80,0,0,b);assert(!sent[0]); // nothing to act on
    on_gesture(oui::Gesture::Chord,b);

    // The mini settings: Options opens them on the Go Live tab; L1/R1 moves along the tabs;
    // Cross opens a row's choices inline (up/down, Cross picks), flips a toggle; left/right
    // moves a slider; the headset row connects and disconnects; Circle goes back.
    on_text("3|c|o|1||0|Raid Night|1:00|720p|0.5.5.1.0.3.1.1.1.0.0|1~68:A5~68:A5^AirPods;00:1B^Buds|11~11_ab~Mira~-",b);
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
    sent[0]=0;on_pad(0x80,0,0,b); // the first Select only asks
    assert(!sent[0]&&w(drop_bg)->visible&&shows("Select again to use it"));
    on_pad(0x80,0,0,b);
    assert(!std::strcmp(sent,"set golive_quality 1")&&!w(drop_bg)->visible&&shows("720p \xc2\xb7 60 fps"));
    assert(!w(nitro_note[0])->visible);
    on_pad(0x800,0,0,b); // R1: Voice
    assert(shows("Mic gain")&&shows("Call volume")&&w(slider_knob[0])->visible&&shows("100%"));
    const float knob=w(slider_knob[0])->r.x;
    on_pad(0x4,0,0,b);assert(!std::strcmp(sent,"set mic_gain 6")&&w(slider_knob[0])->r.x>knob&&shows("120%"));
    // Fast presses: a line from before them does not pull the slider back.
    on_pad(0x4,0,0,b);assert(shows("140%"));
    on_text("3|c|o|1||0|Raid Night|1:00|720p|0.6.5.1.0.3.1.1.1.0.0|1~68:A5~68:A5^AirPods;00:1B^Buds|11~11_ab~Mira~-",b);
    assert(shows("140%"));
    on_text("3|c|o|1||0|Raid Night|1:00|720p|0.7.5.1.0.3.1.1.1.0.0|1~68:A5~68:A5^AirPods;00:1B^Buds|11~11_ab~Mira~-",b);
    assert(shows("140%")); // confirmed
    on_pad(0x800,0,0,b); // Card: Show, Position, Hold Options
    on_pad(0x8,0,0,b);on_pad(0x80,0,0,b); // Position's choices: six, Top right checked
    assert(shows("Middle left")&&w(drop_check)->visible);
    assert(w(set_bg[2])->r.y>kItemY+kItemStep*2); // the row under it moved down
    on_pad(0x8,0,0,b);on_pad(0x80,0,0,b);assert(!std::strcmp(sent,"set overlay_pos 1"));
    on_pad(0x8,0,0,b);on_pad(0x1,0,0,b);assert(!std::strcmp(sent,"set overlay_hold 2")&&hold_ms==500);
    assert(shows("\xe2\x80\xb9  0.5 s  \xe2\x80\xba"));
    on_pad(0x800,0,0,b); // Alerts: a toggle flips with Cross
    assert(w(set_toggle[0])->visible&&ends(w(set_toggle[0])->path,"toggle_on.png"));
    on_pad(0x80,0,0,b);assert(!std::strcmp(sent,"set notify_dm 0")&&ends(w(set_toggle[0])->path,"toggle_off.png"));
    on_pad(0x80,0,0,b); // and back on
    on_pad(0x800,0,0,b); // Headset: Bluetooth, the paired ones and Disconnect, Call mode
    assert(shows("Bluetooth")&&shows("AirPods")&&!shows("Call mode")); // not AirPods' own mic: no Call mode
    on_text("3|c|o|1||0|Raid Night|1:00|720p|0.5.5.1.0.3.1.1.1.0.0|2~68:A5~68:A5^AirPods;00:1B^Buds|11~11_ab~Mira~-",b);
    assert(shows("Call mode")&&shows("High quality")); // AirPods connected (2)
    on_pad(0x8,0,0,b);on_pad(0x8,0,0,b);assert(w(set_focus)->r.y==kItemY+kItemStep*2); // on Call mode
    on_text("3|c|o|1||0|Raid Night|1:00|720p|0.5.5.1.0.3.1.1.1.0.0|1~68:A5~68:A5^AirPods;00:1B^Buds|11~11_ab~Mira~-",b);
    assert(!shows("Call mode")&&w(set_focus)->r.y==kItemY+kItemStep); // AirPods went: focus moves up a row
    on_pad(0x2,0,0,b); // back to Bluetooth
    on_pad(0x80,0,0,b);assert(!std::strcmp(sent,"bt off")&&ends(w(set_toggle[0])->path,"toggle_off.png"));
    on_text("3|c|o|1||0|Raid Night|1:00|720p|0.5.5.1.0.3.1.1.1.0.0|1~68:A5~68:A5^AirPods;00:1B^Buds|11~11_ab~Mira~-",b);
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
    // The card where Settings put it: top left after "set overlay_pos 1".
    on_text("3|c|o|1||0|Raid Night|1:00|720p|0.5.5.1.1.3.1.1.1.0.0|1~~|11~11_ab~Mira~t",b);
    assert(w(card)->visible&&w(card)->r.x==31&&w(card)->r.y==34);
    on_text("3|c|o|1||0|Raid Night|1:00|720p|0.5.5.1.2.3.1.1.1.0.0|1~~|11~11_ab~Mira~t",b);
    assert(w(card)->r.x==1574&&w(card)->r.y==1080-34-265);
    // Live: the quality row is fixed.
    on_text("3|c|o|1|0:05|2|Raid Night|1:00|720p|0.5.5.1.0.3.1.1.1.0.0|1~~|",b);
    on_gesture(oui::Gesture::Chord,b);on_pad(0x100,0,0,b);
    sent[0]=0;on_pad(0x80,0,0,b);assert(!sent[0]&&shows("Live now")&&!w(drop_bg)->visible&&!w(set_chevron[0])->visible);
    on_pad(0x40,0,0,b);on_gesture(oui::Gesture::Chord,b);

    on_text("3|-|o|1||0||||0.5.5.1.0.3.1.1.1.0.0|1~~|",b);
    // The core quit: an open panel closes and gives the pad back; nothing opens until it is back.
    on_gesture(oui::Gesture::Chord,b);assert(capturing&&w(dock)->visible);
    on_text("3|x|o|0||0||||||",b);
    assert(!capturing&&!w(dock)->visible&&!w(card)->visible);
    on_gesture(oui::Gesture::Chord,b);assert(!capturing&&!w(dock)->visible);
    on_text("3|-|o|1||0||||0.5.5.1.0.3.1.1.1.0.0|1~~|",b);
    // The core crashed: no new line (no beat) for 10 s acts like the quit line.
    on_tick(1,b);on_text("3|c4|o|1||0|Raid Night|1:00|720p|0.5.5.1.0.3.1.1.1.0.0|1~~|11~11_ab~Mira~t",b);
    assert(w(card)->visible); // the beat digit after the state is ignored
    on_gesture(oui::Gesture::Chord,b);assert(capturing);
    on_tick(300,b);on_text("3|c9|o|1||0|Raid Night|1:00|720p|0.5.5.1.0.3.1.1.1.0.0|1~~|11~11_ab~Mira~t",b); // only the beat
    on_tick(300+60*10,b);assert(capturing); // not yet: 10 s from the last beat
    on_tick(301+60*10,b);assert(!capturing&&!w(dock)->visible&&!w(card)->visible);
    on_gesture(oui::Gesture::Chord,b);assert(!capturing);
    on_text("3|c5|o|1||0|Raid Night|1:01|720p|0.5.5.1.0.3.1.1.1.0.0|1~~|11~11_ab~Mira~t",b);assert(w(card)->visible);
    on_text("3|-|o|1||0||||0.5.5.1.0.3.1.1.1.0.0|1~~|",b);
    // The keyboard: Triangle on the call page opens it for the call's chat.
    on_text("3|c|o|1||0|Raid Night|1:00|720p|0.5.5.1.0.3.1.1.1.0.0|1~~|11~11_ab~Mira~t",b);
    on_gesture(oui::Gesture::Chord,b);assert(capturing);
    assert(!w(kb_field)->visible&&w(hint_icon[3])->visible&&shows("Chat")&&ends(w(hint_icon[3])->path,"glyph_triangle.png"));
    auto pad=[&](std::uint32_t d,float lx=0,float ly=0){on_pad(d,lx,ly,b);};
    auto key=[&](int r,int c){kb_row=r;kb_col=c;kb_x=key_center(r,c);pad(0x80);}; // press that key
    auto open_kb=[&]{pad(0x20);assert(kb&&w(kb_field)->visible&&w(kb_pages)->visible&&w(kb_hl)->visible);};
    sent[0]=0;open_kb();
    assert(w(kb_input)->visible&&!std::strcmp(w(kb_input)->text,"Message #Raid Night")&&ends(w(kb_pages)->path,"kb_lower.png"));
    assert(w(kb_hl)->r.x==86&&w(kb_hl)->r.y==726&&w(kb_hl)->r.w==100&&w(kb_hl)->r.h==62&&ends(w(kb_hl)->path,"kb_hl_1.png"));
    assert(capturing&&w(dock)->visible&&!sent[0]); // the panel stays; the pad is the keyboard's now
    // Moving: D-pad right (+Cross types), left wraps, up/down change rows, stick one step per push.
    pad(0x80);assert(!std::strcmp(kb_text,"q")&&shows("q|"));
    pad(0x4);assert(w(kb_hl)->r.x==86+108);pad(0x80);assert(!std::strcmp(kb_text,"qw"));
    pad(0x1);pad(0x1);assert(kb_col==10&&w(kb_hl)->r.x==86+1080); // wrapped to '#'
    pad(0x4);assert(kb_col==0);pad(0x1);assert(kb_col==10);
    pad(0,0.9f);pad(0,0.9f);assert(kb_col==0); // the stick: one step while held
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
    key(1,0);key(0,0);key(0,10);assert(!std::strcmp(kb_text,"q1@")&&shows("q1@|"));
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
    assert(kb_len==0&&kb_chars==0&&!std::strcmp(w(kb_input)->text,"Message #Raid Night"));
    // 2000 characters at most, multi-byte ones included.
    for(int i=0;i<2005;i++)key(2,0);
    assert(kb_chars==2000&&kb_len==2000);
    for(int i=0;i<2005;i++)pad(0x10);
    assert(kb_len==0);
    kb_page=1;for(int i=0;i<2005;i++)key(3,9);
    kb_page=0;
    assert(kb_chars==2000&&kb_len==6000);
    // A long text: the field shows the part around the caret, with an ellipsis at the cut end.
    assert(ends(w(kb_input)->text,"|")&&!std::strncmp(w(kb_input)->text,"\xe2\x80\xa6",3)&&std::strlen(w(kb_input)->text)<400);
    for(int i=0;i<2000;i++)pad(0x400);
    assert(kb_caret==0&&!std::strncmp(w(kb_input)->text,"|",1)&&ends(w(kb_input)->text,"\xe2\x80\xa6"));
    for(int i=0;i<1000;i++)pad(0x800);
    assert(!std::strncmp(w(kb_input)->text,"\xe2\x80\xa6",3)&&ends(w(kb_input)->text,"\xe2\x80\xa6")&&std::strstr(w(kb_input)->text,"|"));
    for(int i=0;i<2005;i++)pad(0x800);for(int i=0;i<2005;i++)pad(0x10);assert(kb_len==0);
    // Send with R2: the exact text goes to send_text; the keyboard closes and the draft is gone.
    const char* long_text="Hello w\xc3\xb6rld \xe2\x82\xac 5! This one is longer than the 48 bytes of a command, with \"quotes\" and \\ in it.";
    for(const char* c=long_text;*c;){int n;const unsigned cp=cp_at(long_text,int(c-long_text),&n);(void)cp;char one[8];std::snprintf(one,sizeof one,"%.*s",n,c);kb_type(one);c+=n;}
    render_kb(b);
    sent_texts=0;pad(0x2000);
    assert(sent_texts==1&&!std::strcmp(sent_text,long_text)&&std::strlen(sent_text)>48);
    assert(!kb&&!w(kb_field)->visible&&!w(kb_input)->visible&&!w(kb_pages)->visible&&!w(kb_hl)->visible&&kb_len==0);
    assert(capturing&&w(dock)->visible);
    open_kb();assert(!std::strcmp(w(kb_input)->text,"Message #Raid Night"));
    pad(0x2000);assert(sent_texts==1&&kb); // nothing typed: nothing sent, still open
    // Send through its key (Cross on Send) also works.
    key(1,0);key(4,6);assert(sent_texts==2&&!std::strcmp(sent_text,"q")&&!kb);
    // Circle closes the keyboard and keeps the draft; reopening shows it.
    open_kb();key(1,0);key(1,1);pad(0x40);
    assert(!kb&&!w(kb_field)->visible&&capturing&&w(dock)->visible&&kb_len==2);
    open_kb();assert(shows("qw|"));
    // Options taps do nothing while typing; Cross with no key moves nothing else.
    pad(0x100);assert(kb&&!settings&&w(kb_field)->visible);
    // The gesture (holding Options) closes the keyboard with the panel; the draft stays.
    on_gesture(oui::Gesture::Chord,b);
    assert(!kb&&!capturing&&!w(kb_field)->visible&&!w(dock)->visible&&kb_len==2);
    on_gesture(oui::Gesture::Chord,b);open_kb();assert(shows("qw|"));
    // The core quits: the keyboard goes with the panel.
    on_text("3|x|o|0||0||||||",b);
    assert(!kb&&!capturing&&!w(kb_field)->visible&&!w(kb_hl)->visible);
    on_text("3|c|o|1||0|Raid Night|1:00|720p|0.5.5.1.0.3.1.1.1.0.0|1~~|11~11_ab~Mira~t",b);
    // No beat for 10 s: inert too.
    on_gesture(oui::Gesture::Chord,b);open_kb();
    on_tick(10000,b);on_tick(10000+60*10+1,b);
    assert(!kb&&!capturing&&!w(kb_field)->visible);
    on_text("3|c|o|1||0|Raid Night|1:00|720p|0.5.5.1.0.3.1.1.1.0.0|1~~|11~11_ab~Mira~t",b);
    // The call ends while typing: it closes (the draft stays).
    on_gesture(oui::Gesture::Chord,b);open_kb();
    on_text("3|-|o|1||0||||0.5.5.1.0.3.1.1.1.0.0|1~~|",b);
    assert(!kb&&!w(kb_field)->visible&&capturing&&kb_len==2);
    // Not in a call: Triangle does nothing, and the hint is not shown.
    pad(0x20);assert(!kb&&!w(kb_field)->visible&&!w(hint_icon[3])->visible&&!shows("Chat"));
    on_gesture(oui::Gesture::Chord,b);
    on_text("3|c|o|1||0|Raid Night|1:00|720p|0.5.5.1.0.3.1.1.1.0.0|1~~|11~11_ab~Mira~t",b);
    // In the settings: Triangle does nothing.
    on_gesture(oui::Gesture::Chord,b);pad(0x100);assert(settings);
    pad(0x20);assert(!kb&&!w(kb_field)->visible&&!w(hint_icon[3])->visible);
    pad(0x40);pad(0x20);assert(kb); // back on the call page: it opens (the draft is still "qw")
    assert(shows("qw|"));
    on_gesture(oui::Gesture::Chord,b);assert(!kb&&!capturing);
    on_text("3|-|o|1||0||||0.5.5.1.0.3.1.1.1.0.0|1~~|",b);
    // Not ours: ignored.
    on_text("1|c|o|1||Raid Night|",b);on_text("",b);on_text(nullptr,b);
    assert(!w(card)->visible&&!w(dock)->visible);
    State s;
    assert(parse("3|c|o|1||0|Big|||||a~~A~t,b~~B~t,c~~C~-,d~~D~-,e~~E~-,f~~F~-,g~~G~-,h~~H~-,i~~I~-",s)&&s.count==8);
    std::puts("discord app: ok");
    return 0;
}
