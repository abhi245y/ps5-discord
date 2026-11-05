// SPDX-License-Identifier: GPL-3.0-or-later
// The Discord call overlay, laid out from the approved mockups (docs/overlay-plan.md,
// .impeccable/mocks/overlay/a-dock.png and c-glance.png; their 1672 px measures x 1.148):
//  - the talking card (C): top right while the panel is closed: LIVE with time and viewers
//    (or the call's name), who is talking with their avatar, your mic, how to open the panel.
//    The core decides when it shows (Settings: off, while someone talks, always).
//  - the call panel (A): holding Options or L3+R3 opens it on the right edge: the call and
//    its timer, up to four people (avatar, talking ring and bars, muted marks), the four
//    actions as tiles (Mute, Deafen, Go Live, Leave) and the button hints. D-pad left/right
//    or the stick moves, Cross acts, Circle closes. The game does not get those presses.
//  - the mini settings (mockups i-settings-tabs + h-settings-inline): Options in the panel
//    opens them (Options again, or Circle, goes back). Tabs Go Live, Voice, Card, Alerts,
//    Headset (L1/R1); up/down picks a row; Cross flips a toggle or opens a row's choices
//    inline (up/down, Cross picks, Circle closes); left/right moves a slider or the hold
//    time. Changes go to the core as set / bt commands.
// Art: app/assets/overlay (overlay/tools/overlay_art.py, sizes shared with this file).
// Avatars: round PNGs the core keeps in the overlay's av/ folder.
//
// The core's line (src/overlay.c), at most 2047 bytes, fields split by '|':
//   3|<state>|<me>|<glance>|<live>|<viewers>|<call>|<timer>|<quality>|<settings>|<headsets>|<people>
//   state  c in a call, j joining, - none        me  o mic on, m muted, d deafened
//   glance 1 show the talking card               live  Go Live's time, "" when off
//   settings  the kSettings values, dot-separated     headsets  on~connected addr~addr^name;...
//   people id~avatar~name~flag,...  (talking first; flag t talking, m muted, d deafened, -)
// The worker is built without the C++ library: plain buffers only.

#include <cstdio>
#include <cstring>

namespace oui { namespace app {

namespace {

constexpr const char* kArt="/user/data/homebrew/PPSA99072/assets/overlay/";
constexpr const char* kAvatars="/user/data/discord-overlay-poc/av/";

const Color kWhite{1,1,1,1};
const Color kText{0.93f,0.94f,0.96f,1};
const Color kDim{0.62f,0.65f,0.70f,1};
const Color kCoral{1.0f,0.478f,0.349f,1};
const Color kLeaveInk{1.0f,0.42f,0.37f,1};

// GamePadButtons bits, read on 13.00 (worker.log "PAD down=", 2026-11-02/03).
constexpr std::uint32_t kLeft=0x1,kUp=0x2,kRight=0x4,kDown=0x8,kCircle=0x40,kCross=0x80,kOptionsButton=0x100,
                        kL1=0x400,kR1=0x800;
constexpr float kStickMove=0.6f,kStickRest=0.3f;

// A: the panel.
constexpr float kDockX=1324,kDockY=34,kDockW=568,kDockH=1002;
constexpr float kRowX=1347,kRowW=519,kRowH=132,kRowY=184,kRowStep=143,kAvatar=106;
constexpr float kTileW=122,kTileH=140,kTileStep=132;
constexpr int kRows=4,kTiles=4;
// C: the talking card.
constexpr float kCardX=1574,kCardY=34,kCardW=315;
constexpr int kTalkRows=3;

// The mini settings, in the core line's order (src/overlay.c compose). Values index names
// (or, with no names, show value*scale with a unit).
struct Setting { const char* title; const char* key; int min,max; const char* const* names; int scale; const char* unit; };
constexpr const char* kQualities[]={"720p \xc2\xb7 30 fps","720p \xc2\xb7 60 fps","1080p \xc2\xb7 30 fps","1080p \xc2\xb7 60 fps"};
constexpr const char* kCardModes[]={"Off","While someone talks","Always"};
constexpr const char* kPlaces[]={"Top right","Top left","Bottom right","Bottom left","Middle right","Middle left"};
constexpr const char* kOnOff[]={"Off","On"};
constexpr const char* kServer[]={"Off","As in Discord","Every message"};
constexpr const char* kQuiet[]={"Off","22:00 \xe2\x80\x93 07:00","23:00 \xe2\x80\x93 08:00","00:00 \xe2\x80\x93 08:00","21:00 \xe2\x80\x93 06:00"};
constexpr const char* kAudio[]={"Headset mic","Hands-free"};
constexpr Setting kSettings[]={
    {"Quality","golive_quality",0,3,kQualities,0,nullptr},
    {"Mic gain","mic_gain",0,10,nullptr,20,"%"},
    {"Call volume","call_volume",0,10,nullptr,20,"%"},
    {"Show","overlay_glance",0,2,kCardModes,0,nullptr},
    {"Position","overlay_pos",0,5,kPlaces,0,nullptr},
    {"Hold Options","overlay_hold",0,12,nullptr,0,"s"}, // 0.3 s + tenths
    {"DMs and mentions","notify_dm",0,1,kOnOff,0,nullptr},
    {"Server messages","notify_server",0,2,kServer,0,nullptr},
    {"PS5 alerts while playing","notify_system",0,1,kOnOff,0,nullptr},
    {"Quiet hours","quiet_hours",0,4,kQuiet,0,nullptr},
    {"Call audio","bt_handsfree",0,1,kAudio,0,nullptr},
};
constexpr int kSettingCount=int(sizeof kSettings/sizeof *kSettings);
constexpr int kHeadset=-1; // the headset row: the paired headsets and Disconnect
// The tabs and their rows (kSettings indexes).
struct Tab { const char* name; float w; int rows[4]; int count; };
constexpr Tab kTabs[]={
    {"Go Live",92,{0},1},
    {"Voice",76,{1,2},2},
    {"Card",66,{3,4,5},3},
    {"Alerts",76,{6,7,8,9},4},
    {"Headset",96,{kHeadset,10},2},
};
constexpr int kTabCount=int(sizeof kTabs/sizeof *kTabs),kSettingRows=4,kOptions=6;
constexpr float kTabY=124,kItemY=192,kItemH=62,kItemStep=70,kOptionH=48;
enum class Kind{choice,toggle,slider,stepper};
Kind kind_of(int s){
    const Setting& set=kSettings[s];
    if(set.names==kOnOff)return Kind::toggle;
    if(set.names)return Kind::choice;
    return set.unit[0]=='s'?Kind::stepper:Kind::slider;
}

struct Person { char id[24]; char avatar[64]; char name[64]; char flag; };
struct State {
    char state='-', me='o';
    bool glance=false;
    char live[16]="", call[64]="", timer[16]="", quality[32]="";
    int viewers=0;
    int set[kSettingCount]={0,5,5,1,0,3,1,1,1,0,0};
    bool bt_on=false;
    char connected[18]="";
    char dev_addr[4][18]={}, dev_name[4][40]={};
    int devices=0;
    Person people[8];
    int count=0;
};

State st;
bool open=false;
int focus=0;      // the focused tile
bool settings=false; // the panel shows the mini settings
int tab=0,row=0;  // the settings' open tab and focused row
bool expanded=false; // the focused row's choices are open
int option=0;     // the focused choice
int stick=0;      // the stick's last direction: -1 left, 1 right, 0 at rest

// The panel.
void* dock=nullptr; void* title=nullptr; void* clock=nullptr; void* timer=nullptr;
void* row_bg[kRows]={}; void* row_avatar[kRows]={}; void* row_ring[kRows]={}; void* row_name[kRows]={};
void* row_icon[kRows]={};
void* tile[kTiles]={}; void* tile_icon[kTiles]={}; void* tile_label[kTiles]={}; void* tile_sub=nullptr;
void* tile_focus=nullptr; void* rule=nullptr;
void* hint_icon[3]={}; void* hint_text[3]={};
// The talking card.
void* card=nullptr; void* pill=nullptr; void* card_head=nullptr; void* card_call=nullptr;
void* talk_avatar[kTalkRows]={}; void* talk_ring[kTalkRows]={}; void* talk_name[kTalkRows]={};
void* talk_bars[kTalkRows]={};
void* you_ring=nullptr; void* you_mic=nullptr; void* you_text=nullptr; void* card_rule=nullptr; void* card_hint=nullptr;
// The mini settings.
void* tab_pill[kTabCount]={}; void* tab_word[kTabCount]={}; void* chip_l1=nullptr; void* chip_r1=nullptr;
void* set_bg[kSettingRows]={}; void* set_name[kSettingRows]={}; void* set_value[kSettingRows]={};
void* set_chevron[kSettingRows]={}; void* set_toggle[kSettingRows]={};
void* slider_track[2]={}; void* slider_fill[2]={}; void* slider_knob[2]={};
void* set_focus=nullptr; void* drop_bg=nullptr; void* drop_on=nullptr; void* drop_check=nullptr;
void* drop_word[kOptions]={};

const char* const kCommands[kTiles]={"mute","deafen","golive","leave"};

// Copies the next field (up to stop) of *p into out and moves *p past it.
void field(const char** p,char* out,std::size_t cap,char stop='|'){
    const char* end=std::strchr(*p,stop);
    std::size_t n=end?std::size_t(end-*p):std::strlen(*p);
    std::snprintf(out,cap,"%.*s",int(n),*p);
    *p+=end?n+1:n;
}

void path(char* out,std::size_t cap,const char* dir,const char* name){std::snprintf(out,cap,"%s%s.png",dir,name);}

void* image(Builder& b,const Rect& r,const char* art){
    char p[160];path(p,sizeof p,kArt,art);
    return b.add_image(r,p);
}
void art(Builder& b,void* w,const char* name){char p[160];path(p,sizeof p,kArt,name);b.set_image(w,p);}

void* label(Builder& b,const Rect& r,float size,int weight,const Color& c,int align=0,const char* text=""){
    void* l=b.add_label(nullptr,r,text);
    if(l)b.set_text_style(l,size,weight,c,align);
    return l;
}

void show(Builder& b,void* w,bool on){if(w)b.set_visible(w,on);}

const char* mic_text(char me){
    return me=='d'?"You \xc2\xb7 deafened":me=='m'?"You \xc2\xb7 muted":"You \xc2\xb7 mic on";
}

}  // namespace

// Reads a core line into s; false when it is not one.
bool parse(const char* line,State& s){
    if(!line||std::strncmp(line,"3|",2))return false;
    s=State{};
    const char* p=line+2;
    char f[64];
    field(&p,f,sizeof f);s.state=f[0]?f[0]:'-';
    field(&p,f,sizeof f);s.me=f[0]?f[0]:'o';
    field(&p,f,sizeof f);s.glance=f[0]=='1';
    field(&p,s.live,sizeof s.live);
    field(&p,f,sizeof f);std::sscanf(f,"%d",&s.viewers);
    field(&p,s.call,sizeof s.call);
    field(&p,s.timer,sizeof s.timer);
    field(&p,s.quality,sizeof s.quality);
    char values[128];field(&p,values,sizeof values);
    const char* v=values;
    for(int i=0;i<kSettingCount&&*v;i++){char one[8];field(&v,one,sizeof one,'.');std::sscanf(one,"%d",&s.set[i]);}
    char bt[400];field(&p,bt,sizeof bt);
    const char* q=bt;
    field(&q,f,sizeof f,'~');s.bt_on=f[0]=='1';
    field(&q,s.connected,sizeof s.connected,'~');
    while(*q&&s.devices<4){
        char one[64];field(&q,one,sizeof one,';');
        const char* d=one;
        field(&d,s.dev_addr[s.devices],sizeof s.dev_addr[0],'^');
        field(&d,s.dev_name[s.devices],sizeof s.dev_name[0],'^');
        ++s.devices;}
    while(*p&&s.count<8){
        char one[200];field(&p,one,sizeof one,',');
        const char* q=one;Person& who=s.people[s.count++];
        field(&q,who.id,sizeof who.id,'~');
        field(&q,who.avatar,sizeof who.avatar,'~');
        field(&q,who.name,sizeof who.name,'~');
        who.flag=*q?*q:'-';
    }
    return true;
}

namespace {

// The panel follows its rows (n, 0-4): the tiles right under the last one, then the rule
// and the hints; the art has a panel for each height (overlay_art.py).
float tiles_y(int n){return kRowY+kRowStep*n+10;}
float rule_y(int n){return n?tiles_y(n)+172:kDockY+166;}
float dock_height(int n){return n?tiles_y(n)+236:264;} // its bottom: the hints + 42

void place(Builder& b,int n){
    char name[16];std::snprintf(name,sizeof name,"dock_%d",n);
    art(b,dock,name);
    b.set_rect(dock,{kDockX,kDockY,kDockW,dock_height(n)});
    const float ty=tiles_y(n);
    for(int i=0;i<kTiles;i++){
        const float x=kRowX+kTileStep*i;
        b.set_rect(tile[i],{x,ty,kTileW,kTileH});
        b.set_rect(tile_icon[i],{x+38,ty+(i==2?22:28),46,46});
        b.set_rect(tile_label[i],{x,ty+(i==2?76:86),kTileW,30});}
    b.set_rect(tile_sub,{kRowX+kTileStep*2,ty+104,kTileW,24});
    b.set_rect(tile_focus,{kRowX+kTileStep*focus-6,ty-6,134,152});
    const float ry=rule_y(n),hy=ry+28;
    b.set_rect(rule,{kRowX,ry,kRowW,2});
    // A label's text sits high in its box: 5 px lower lines it up with the 37 px glyphs.
    const Rect glyph_at[3]={{1361,hy+1,37,37},{1510,hy+1,37,37},{1650,hy+5,78,28}};
    const Rect word_at[3]={{1406,hy+5,96,40},{1555,hy+5,90,40},{1736,hy+5,132,40}};
    for(int i=0;i<3;i++){b.set_rect(hint_icon[i],glyph_at[i]);b.set_rect(hint_text[i],word_at[i]);}
}

void set_avatar(Builder& b,void* w,const Person& who){
    char p[160];
    if(who.avatar[0])path(p,sizeof p,kAvatars,who.avatar);else p[0]=0;
    b.set_image(w,p);
}

// The settings' choices for a row: names into out, how many, and the current one.
int choices(int s,const char* out[kOptions],int& current){
    int n=0;current=kOptions; // none checked
    if(s==kHeadset){
        for(int d=0;d<st.devices&&n<kOptions;d++){
            if(!std::strcmp(st.dev_addr[d],st.connected))current=n;
            out[n++]=st.dev_name[d];}
        if(st.connected[0]&&n<kOptions)out[n++]="Disconnect";
        return n;}
    const Setting& set=kSettings[s];
    for(int v=set.min;v<=set.max&&n<kOptions;v++)out[n++]=set.names[v];
    current=st.set[s]-set.min;
    return n;
}

void value_text(int s,char* out,std::size_t cap){
    if(s==kHeadset){
        int d=-1;for(int i=0;i<st.devices;i++)if(!std::strcmp(st.dev_addr[i],st.connected))d=i;
        std::snprintf(out,cap,"%s",!st.devices?"None paired":!st.bt_on?"Bluetooth off":d<0?"Not connected":st.dev_name[d]);
        return;}
    const Setting& set=kSettings[s];const int v=st.set[s];
    if(s==0&&st.live[0])std::snprintf(out,cap,"Live now");
    else if(set.names)std::snprintf(out,cap,"%s",set.names[v<set.min?set.min:v>set.max?set.max:v]);
    else if(set.unit[0]=='s')std::snprintf(out,cap,"\xe2\x80\xb9  %d.%d s  \xe2\x80\xba",(v+3)/10,(v+3)%10);
    else std::snprintf(out,cap,"%d%s",v*set.scale,set.unit);
}

bool can_open(int s){ // a row whose choices open
    if(s==kHeadset)return st.devices&&st.bt_on;
    return kind_of(s)==Kind::choice&&!(s==0&&st.live[0]);
}

void render_settings(Builder& b){
    const Tab& t=kTabs[tab];
    // The tabs: pills tinted coral for the open one, L1 and R1 at the ends.
    float x=kRowX;
    b.set_rect(chip_l1,{x,kTabY+7,38,26});x+=38+7;
    for(int i=0;i<kTabCount;i++){
        b.set_rect(tab_pill[i],{x,kTabY,kTabs[i].w,40});
        b.set_rect(tab_word[i],{x,kTabY+5,kTabs[i].w,32});
        b.set_tint(tab_pill[i],i==tab?kCoral:Color{0.17f,0.18f,0.20f,1});
        x+=kTabs[i].w+7;}
    b.set_rect(chip_r1,{x,kTabY+7,38,26});
    for(int i=0;i<kTabCount;i++){show(b,tab_pill[i],settings);show(b,tab_word[i],settings);}
    show(b,chip_l1,settings);show(b,chip_r1,settings);

    // The rows, the open one's choices under it pushing the rest down.
    const char* names[kOptions];int current=0,count=0;
    if(expanded)count=choices(t.rows[row],names,current);
    const float drop_h=expanded?12+kOptionH*count+8:0;
    int slider=0;
    for(int i=0;i<kSettingRows;i++){
        const bool used=settings&&i<t.count;
        const int s=used?t.rows[i]:0;
        const float y=kItemY+kItemStep*i+(expanded&&i>row?drop_h:0);
        char text[96];
        if(used){
            b.set_rect(set_bg[i],{kRowX,y,kRowW,kItemH});
            b.set_rect(set_name[i],{kRowX+22,y+15,260,36});
            b.set_text(set_name[i],s==kHeadset?"Headset":kSettings[s].title);
            value_text(s,text,sizeof text);
            b.set_text(set_value[i],text);
            b.set_rect(set_value[i],{kRowX+200,y+15,kRowW-(can_open(s)?262:222),36});
            b.set_rect(set_chevron[i],{kRowX+kRowW-50,y+19,24,24});
            art(b,set_chevron[i],expanded&&i==row?"chevron_up":"chevron_down");
            b.set_rect(set_toggle[i],{kRowX+kRowW-82,y+14,60,34});}
        const bool toggle=used&&s!=kHeadset&&kind_of(s)==Kind::toggle;
        if(toggle)art(b,set_toggle[i],st.set[s]?"toggle_on":"toggle_off");
        show(b,set_bg[i],used);show(b,set_name[i],used);show(b,set_value[i],used&&!toggle);
        show(b,set_chevron[i],used&&can_open(s));show(b,set_toggle[i],toggle);
        if(used&&s!=kHeadset&&kind_of(s)==Kind::slider&&slider<2){
            const Setting& set=kSettings[s];
            const float frac=float(st.set[s]-set.min)/float(set.max-set.min),tx=kRowX+250;
            b.set_rect(slider_track[slider],{tx,y+27,170,8});
            b.set_rect(slider_fill[slider],{tx,y+27,170*frac<8?8:170*frac,8});
            b.set_rect(slider_knob[slider],{tx+170*frac-13,y+18,26,26});
            b.set_rect(set_value[i],{kRowX+430,y+15,67,36});
            ++slider;}
    }
    for(int i=0;i<2;i++){show(b,slider_track[i],settings&&i<slider);show(b,slider_fill[i],settings&&i<slider);
        show(b,slider_knob[i],settings&&i<slider);}
    b.set_rect(set_focus,{kRowX,kItemY+kItemStep*row,kRowW,kItemH});
    show(b,set_focus,settings&&t.count);

    // The inline choices.
    const float dy=kItemY+kItemStep*row+kItemH+6;
    if(expanded){
        char bg[16];std::snprintf(bg,sizeof bg,"dropdown_%d",count<2?2:count);
        art(b,drop_bg,bg);
        b.set_rect(drop_bg,{kRowX+20,dy,kRowW-20,12+kOptionH*count});
        b.set_rect(drop_on,{kRowX+28,dy+6+kOptionH*option,kRowW-36,44});
        b.set_rect(drop_check,{kRowX+kRowW-48,dy+6+kOptionH*current+10,24,24});
        for(int j=0;j<count;j++){
            b.set_rect(drop_word[j],{kRowX+48,dy+6+kOptionH*j+6,kRowW-120,34});
            b.set_text(drop_word[j],names[j]);}}
    show(b,drop_bg,settings&&expanded);show(b,drop_on,settings&&expanded);
    show(b,drop_check,settings&&expanded&&current<count);
    for(int j=0;j<kOptions;j++)show(b,drop_word[j],settings&&expanded&&j<count);
}

void render(Builder& b){
    const bool in_call=st.state=='c'||st.state=='j';
    char text[128];

    // C: the talking card.
    int talking=0;
    for(int i=0;i<st.count&&talking<kTalkRows;i++)
        if(st.people[i].flag=='t'){
            set_avatar(b,talk_avatar[talking],st.people[i]);
            b.set_text(talk_name[talking],st.people[i].name);
            ++talking;}
    const bool card_on=!open&&in_call&&st.glance;
    // Where the card sits (Settings: overlay_pos), inside the TV's safe area.
    const float ch=205+60.0f*talking;
    const int pos=st.set[4];
    const float cx=pos%2?31:kCardX;
    const float cy=pos<2?kCardY:pos<4?1080-kCardY-ch:(1080-ch)/2;
    const float you_y=cy+107+60.0f*talking; // the "You" row's middle
    char bg[16];std::snprintf(bg,sizeof bg,"glance_%d",talking);
    art(b,card,bg);
    b.set_rect(card,{cx,cy,kCardW,ch});
    b.set_rect(pill,{cx+20,cy+17,79,34});
    b.set_rect(card_head,{cx+114,cy+16,190,36});
    b.set_rect(card_call,{cx+20,cy+16,275,36});
    for(int i=0;i<kTalkRows;i++){
        const float c=cy+107+60.0f*i;
        b.set_rect(talk_avatar[i],{cx+27,c-25,50,50});
        b.set_rect(talk_ring[i],{cx+24,c-28,56,56});
        b.set_rect(talk_name[i],{cx+91,c-18,160,36});
        b.set_rect(talk_bars[i],{cx+256,c-15,30,30});}
    b.set_rect(you_ring,{cx+27,you_y-25,50,50});
    b.set_rect(you_mic,{cx+38,you_y-14,28,28});
    b.set_rect(you_text,{cx+91,you_y-18,210,36});
    b.set_rect(card_rule,{cx+20,you_y+40,275,2});
    b.set_rect(card_hint,{cx+20,you_y+52,275,32});
    art(b,you_mic,st.me=='o'?"mic":st.me=='d'?"headphones_off":"mic_off");
    b.set_text(you_text,mic_text(st.me));
    if(st.live[0]){
        std::snprintf(text,sizeof text,"%s  |  %d watching",st.live,st.viewers);
        b.set_text(card_head,text);}
    b.set_text(card_call,st.state=='j'?"Joining\xe2\x80\xa6":st.call);
    show(b,card,card_on);
    show(b,pill,card_on&&st.live[0]);show(b,card_head,card_on&&st.live[0]);show(b,card_call,card_on&&!st.live[0]);
    for(int i=0;i<kTalkRows;i++){
        const bool on=card_on&&i<talking;
        show(b,talk_avatar[i],on);show(b,talk_ring[i],on);show(b,talk_name[i],on);show(b,talk_bars[i],on);}
    show(b,you_ring,card_on);show(b,you_mic,card_on);show(b,you_text,card_on);
    show(b,card_rule,card_on);show(b,card_hint,card_on);

    // A: the panel (call page, or the mini settings).
    const bool call_page=open&&!settings;
    b.set_text(title,settings?"Settings":in_call?st.call:"Not in a call");
    b.set_text(timer,st.timer);
    for(int i=0;i<kRows;i++){
        const bool used=in_call&&i<st.count;
        if(used){
            const Person& who=st.people[i];
            set_avatar(b,row_avatar[i],who);
            b.set_text(row_name[i],who.name);
            art(b,row_icon[i],who.flag=='m'?"mic_off":who.flag=='d'?"headphones_off":"bars");
            b.set_tint(row_icon[i],who.flag=='t'?kCoral:kDim);}
        const bool on=call_page&&used;
        show(b,row_bg[i],on);show(b,row_avatar[i],on);show(b,row_name[i],on);
        show(b,row_icon[i],on);show(b,row_ring[i],on&&st.people[i].flag=='t');
    }
    const bool muted=st.me=='m'||st.me=='d';
    art(b,tile_icon[0],muted?"mic_off":"mic");
    art(b,tile_icon[1],st.me=='d'?"headphones_off":"headphones");
    b.set_text(tile_label[0],muted?"Unmute":"Mute");
    b.set_text(tile_label[1],st.me=='d'?"Undeafen":"Deafen");
    b.set_text(tile_label[2],st.live[0]?"Stop":"Go Live");
    b.set_text(tile_label[3],"Leave");
    b.set_text(tile_sub,st.live[0]?st.live:st.quality);
    place(b,settings?kRows:in_call?(st.count<kRows?st.count:kRows):0);
    const bool tiles_on=call_page&&in_call;
    for(int i=0;i<kTiles;i++){show(b,tile[i],tiles_on);show(b,tile_icon[i],tiles_on);show(b,tile_label[i],tiles_on);}
    show(b,tile_sub,tiles_on);show(b,tile_focus,tiles_on);
    show(b,dock,open);show(b,title,open);show(b,clock,call_page&&in_call&&st.timer[0]);show(b,timer,call_page&&in_call);
    show(b,rule,open);

    render_settings(b);

    // Hints: Cross, Circle and Options (L1/R1 on the settings); their words follow the page.
    b.set_text(hint_text[0],"Select");
    b.set_text(hint_text[1],settings&&!expanded?"Back":"Close");
    b.set_text(hint_text[2],settings?"Category":"Settings");
    art(b,hint_icon[2],settings?"chip_l1r1":"chip_options");
    for(int i=0;i<3;i++){show(b,hint_icon[i],open);show(b,hint_text[i],open);}
}

}  // namespace

void reset(){
    st=State{};open=false;focus=0;stick=0;settings=false;tab=row=option=0;expanded=false;
    chip_l1=chip_r1=set_focus=drop_bg=drop_on=drop_check=nullptr;
    for(int i=0;i<kTabCount;i++)tab_pill[i]=tab_word[i]=nullptr;
    for(int i=0;i<kSettingRows;i++)set_bg[i]=set_name[i]=set_value[i]=set_chevron[i]=set_toggle[i]=nullptr;
    for(int i=0;i<2;i++)slider_track[i]=slider_fill[i]=slider_knob[i]=nullptr;
    for(int j=0;j<kOptions;j++)drop_word[j]=nullptr;
    dock=title=clock=timer=tile_sub=tile_focus=rule=nullptr;
    card=pill=card_head=card_call=you_ring=you_mic=you_text=card_rule=card_hint=nullptr;
    for(int i=0;i<kRows;i++)row_bg[i]=row_avatar[i]=row_ring[i]=row_name[i]=row_icon[i]=nullptr;
    for(int i=0;i<kTiles;i++)tile[i]=tile_icon[i]=tile_label[i]=nullptr;
    for(int i=0;i<3;i++)hint_icon[i]=hint_text[i]=nullptr;
    for(int i=0;i<kTalkRows;i++)talk_avatar[i]=talk_ring[i]=talk_name[i]=talk_bars[i]=nullptr;
}

bool build(Builder& b){
    // A: the panel, back to front.
    dock=image(b,{kDockX,kDockY,kDockW,kDockH},"dock_4");
    title=label(b,{kDockX+37,kDockY+40,kDockW-74,54},40,1,kWhite);
    clock=image(b,{kDockX+38,kDockY+94,26,26},"clock");
    if(clock)b.set_tint(clock,kDim);
    timer=label(b,{kDockX+74,kDockY+88,200,36},25,3,kDim);
    for(int i=0;i<kRows;i++){
        const float y=kRowY+kRowStep*i;
        row_bg[i]=image(b,{kRowX,y,kRowW,kRowH},"row");
        row_avatar[i]=b.add_image({kRowX+16,y+13,kAvatar,kAvatar},"");
        row_ring[i]=image(b,{kRowX+16,y+13,kAvatar,kAvatar},"ring");
        row_name[i]=label(b,{kRowX+150,y+40,280,52},27,3,kWhite);
        row_icon[i]=image(b,{kRowX+kRowW-66,y+48,36,36},"bars");
    }
    const char* icons[kTiles]={"mic","headphones","golive","leave"};
    const char* tiles[kTiles]={"tile","tile","tile_live","tile_leave"};
    const Rect at{0,0,1,1}; // place() puts them under the rows
    for(int i=0;i<kTiles;i++){
        tile[i]=image(b,at,tiles[i]);
        tile_icon[i]=image(b,at,icons[i]);
        if(i==3&&tile_icon[i])b.set_tint(tile_icon[i],kLeaveInk);
        tile_label[i]=label(b,at,19,3,i==3?kLeaveInk:kWhite,1);
    }
    tile_sub=label(b,at,14,0,kWhite,1);
    tile_focus=image(b,at,"tile_focus");
    rule=image(b,at,"line");
    const char* glyphs[3]={"glyph_cross","glyph_circle","chip_options"};
    const char* words[3]={"Select","Close","Settings"};
    for(int i=0;i<3;i++){
        hint_icon[i]=image(b,at,glyphs[i]);
        hint_text[i]=label(b,at,21,3,kText,0,words[i]);
    }

    // The mini settings, back to front (placed in render_settings).
    chip_l1=image(b,at,"chip_l1");chip_r1=image(b,at,"chip_r1");
    for(int i=0;i<kTabCount;i++){
        char name[16];std::snprintf(name,sizeof name,"pill_%d",int(kTabs[i].w));
        tab_pill[i]=image(b,at,name);
        tab_word[i]=label(b,at,18,3,kWhite,1,kTabs[i].name);}
    for(int i=0;i<kSettingRows;i++){
        set_bg[i]=image(b,at,"setting_row");
        set_name[i]=label(b,at,22,3,kWhite);
        set_value[i]=label(b,at,21,0,kText,2);
        set_chevron[i]=image(b,at,"chevron_down");
        if(set_chevron[i])b.set_tint(set_chevron[i],kDim);
        set_toggle[i]=image(b,at,"toggle_off");}
    for(int i=0;i<2;i++){
        slider_track[i]=image(b,at,"slider_track");slider_fill[i]=image(b,at,"slider_fill");
        slider_knob[i]=image(b,at,"slider_knob");}
    set_focus=image(b,at,"setting_focus");
    drop_bg=image(b,at,"dropdown_3");drop_on=image(b,at,"option_on");drop_check=image(b,at,"check");
    for(int j=0;j<kOptions;j++)drop_word[j]=label(b,at,21,3,kWhite);
    for(void* w:set_name)if(!w)return false;
    for(void* w:drop_word)if(!w)return false;

    // C: the talking card.
    card=image(b,{kCardX,kCardY,kCardW,325},"glance_2");
    pill=image(b,{kCardX+20,kCardY+17,79,34},"pill_live");
    card_head=label(b,{kCardX+114,kCardY+16,190,36},19,3,kText);
    card_call=label(b,{kCardX+20,kCardY+16,275,36},21,1,kWhite);
    for(int i=0;i<kTalkRows;i++){
        const float c=kCardY+107+60.0f*i;
        talk_avatar[i]=b.add_image({kCardX+27,c-25,50,50},"");
        talk_ring[i]=image(b,{kCardX+24,c-28,56,56},"ring_small");
        talk_name[i]=label(b,{kCardX+91,c-18,160,36},21,3,kWhite);
        talk_bars[i]=image(b,{kCardX+256,c-15,30,30},"bars");
        if(talk_bars[i])b.set_tint(talk_bars[i],kCoral);
    }
    you_ring=image(b,{kCardX+27,kCardY+202,50,50},"you");
    you_mic=image(b,{kCardX+38,kCardY+213,28,28},"mic");
    you_text=label(b,{kCardX+91,kCardY+209,210,36},21,3,kWhite);
    card_rule=image(b,{kCardX+20,kCardY+267,275,2},"glance_line");
    card_hint=label(b,{kCardX+20,kCardY+279,275,32},17,0,kDim,0,"Hold Options for controls");

    // Text is the overlay's job; pictures may be missing on a firmware without ImageBox.
    if(!title||!timer||!tile_sub||!card_head||!card_call||!you_text||!card_hint)return false;
    for(void* w:row_name)if(!w)return false;
    for(void* w:tile_label)if(!w)return false;
    for(void* w:talk_name)if(!w)return false;
    render(b);
    return true;
}

namespace {
void apply(int s,int v,Builder& b){
    const Setting& set=kSettings[s];
    v=v<set.min?set.min:v>set.max?set.max:v;
    if(v==st.set[s])return;
    st.set[s]=v;
    char command[48];std::snprintf(command,sizeof command,"set %s %d",set.key,v);
    b.send(command);
    if(s==5)b.set_hold((v+3)*100);
}

// The mini settings' buttons. ponytail: the D-pad only; the stick moves the call page's tiles.
void settings_pad(std::uint32_t down,Builder& b){
    const Tab& t=kTabs[tab];
    const int s=t.rows[row];
    const int move=((down&kDown)?1:0)-((down&kUp)?1:0);
    const int change=((down&kRight)?1:0)-((down&kLeft)?1:0);
    if(expanded){
        const char* names[kOptions];int current=0;
        const int count=choices(s,names,current);
        if(down&kCircle)expanded=false;
        else if(move&&count)option=((option+move)%count+count)%count;
        else if(down&kCross){
            if(s!=kHeadset)apply(s,kSettings[s].min+option,b);
            else{
                char command[48];
                if(option>=st.devices)std::snprintf(command,sizeof command,"bt disconnect");
                else std::snprintf(command,sizeof command,"bt connect %s",st.dev_addr[option]);
                if(option>=st.devices||std::strcmp(st.dev_addr[option],st.connected))b.send(command);}
            expanded=false;}
        render(b);return;}
    if(down&kCircle){settings=false;render(b);return;}
    const int switch_tab=((down&kR1)?1:0)-((down&kL1)?1:0);
    if(switch_tab){tab=((tab+switch_tab)%kTabCount+kTabCount)%kTabCount;row=0;render(b);return;}
    if(move&&t.count)row=((row+move)%t.count+t.count)%t.count;
    if(s!=kHeadset&&change){
        const Kind k=kind_of(s);
        if(k==Kind::slider||k==Kind::stepper)apply(s,st.set[s]+change,b);}
    if(down&kCross){
        if(s!=kHeadset&&kind_of(s)==Kind::toggle)apply(s,!st.set[s],b);
        else if(can_open(s)){
            const char* names[kOptions];int current=0;
            const int count=choices(s,names,current);
            expanded=true;option=current<count?current:0;}}
    if(move||change||(down&kCross))render(b);
}
}  // namespace

void on_gesture(Gesture,Builder& b){
    open=!open;
    focus=0;stick=0;settings=false;expanded=false;
    b.set_capture(open);
    render(b);
}

void on_pad(std::uint32_t down,float lx,float,Builder& b){
    if(!open)return;
    if(down&kOptionsButton){settings=!settings;tab=row=0;expanded=false;render(b);return;}
    if(settings){settings_pad(down,b);return;}
    if(down&kCircle){on_gesture(Gesture::Chord,b);return;}
    const int dir=lx<-kStickMove?-1:lx>kStickMove?1:0;
    const int step=(dir&&dir!=stick?dir:0)+((down&(kRight|kDown))?1:0)-((down&(kLeft|kUp))?1:0);
    if(dir||(lx>-kStickRest&&lx<kStickRest))stick=dir;
    if(step){focus=((focus+step)%kTiles+kTiles)%kTiles;render(b);}
    if((down&kCross)&&(st.state=='c'||st.state=='j'))b.send(kCommands[focus]);
}

void on_text(const char* line,Builder& b){
    const int hold=st.set[5];
    if(!parse(line,st))return;
    if(st.set[5]!=hold)b.set_hold((st.set[5]+3)*100);
    render(b);
}

void on_tick(std::uint64_t){}

}}  // namespace oui::app
