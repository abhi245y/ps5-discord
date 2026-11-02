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
//  - the mini settings: Options in the panel opens them (Options again, or Circle, goes back):
//    Go Live quality, mic gain, call volume, the talking card and where it sits, the Options
//    hold, notifications, call audio and the paired headsets. Up/down moves, left/right
//    changes, Cross connects or disconnects the shown headset. Changes go to the core.
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
constexpr std::uint32_t kLeft=0x1,kUp=0x2,kRight=0x4,kDown=0x8,kCircle=0x40,kCross=0x80,kOptions=0x100;
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
    {"Go Live quality","golive_quality",0,3,kQualities,0,nullptr},
    {"Mic gain","mic_gain",0,10,nullptr,20,"%"},
    {"Call volume","call_volume",0,10,nullptr,20,"%"},
    {"Talking card","overlay_glance",0,2,kCardModes,0,nullptr},
    {"Card position","overlay_pos",0,5,kPlaces,0,nullptr},
    {"Hold Options","overlay_hold",0,12,nullptr,0,"s"}, // 0.3 s + tenths
    {"DMs and mentions","notify_dm",0,1,kOnOff,0,nullptr},
    {"Server messages","notify_server",0,2,kServer,0,nullptr},
    {"PS5 alerts while playing","notify_system",0,1,kOnOff,0,nullptr},
    {"Quiet hours","quiet_hours",0,4,kQuiet,0,nullptr},
    {"Call audio","bt_handsfree",0,1,kAudio,0,nullptr},
};
constexpr int kSettingCount=int(sizeof kSettings/sizeof *kSettings);
constexpr int kItems=kSettingCount+1; // and the headset row
constexpr float kItemY=118,kItemStep=62;

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
int item=0;       // the focused setting
int device=0;     // the headset row's shown device
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
void* item_focus=nullptr; void* item_name[kItems]={}; void* item_value[kItems]={};

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

    // The mini settings.
    for(int i=0;i<kItems;i++){
        const float y=kDockY+kItemY+kItemStep*i;
        b.set_rect(item_name[i],{kRowX+20,y+13,300,36});
        b.set_rect(item_value[i],{kRowX+250,y+13,249,36});
        if(i<kSettingCount){
            const Setting& set=kSettings[i];const int v=st.set[i];
            if(i==0&&st.live[0])std::snprintf(text,sizeof text,"Live now");
            else if(set.names)std::snprintf(text,sizeof text,"%s",set.names[v<set.min?set.min:v>set.max?set.max:v]);
            else if(set.unit[0]=='s')std::snprintf(text,sizeof text,"%d.%d s",(v+3)/10,(v+3)%10);
            else std::snprintf(text,sizeof text,"%d%s",v*set.scale,set.unit);
        }else if(!st.devices)std::snprintf(text,sizeof text,"None paired");
        else if(!st.bt_on)std::snprintf(text,sizeof text,"Bluetooth off");
        else{
            const int d=device<st.devices?device:0;
            std::snprintf(text,sizeof text,"%s%s",st.dev_name[d],std::strcmp(st.dev_addr[d],st.connected)?"":" \xc2\xb7 on");}
        char shown[160];
        if(settings&&i==item&&!(i==0&&st.live[0]))std::snprintf(shown,sizeof shown,"\xe2\x80\xb9  %s  \xe2\x80\xba",text);
        else std::snprintf(shown,sizeof shown,"%s",text);
        b.set_text(item_value[i],shown);
        show(b,item_name[i],settings);show(b,item_value[i],settings);
    }
    b.set_rect(item_focus,{kRowX,kDockY+kItemY+kItemStep*item,kRowW,56});
    show(b,item_focus,settings);

    // Hints: Cross, Circle and Options; their words follow the page.
    const bool on_headset=settings&&item==kSettingCount&&st.devices&&st.bt_on;
    const int d=device<st.devices?device:0;
    b.set_text(hint_text[0],!settings?"Select":on_headset&&!std::strcmp(st.dev_addr[d],st.connected)?"Disconnect":"Connect");
    b.set_text(hint_text[1],settings?"Back":"Close");
    b.set_text(hint_text[2],settings?"Call":"Settings");
    show(b,hint_icon[0],open&&(!settings||on_headset));show(b,hint_text[0],open&&(!settings||on_headset));
    for(int i=1;i<3;i++){show(b,hint_icon[i],open);show(b,hint_text[i],open);}
}

}  // namespace

void reset(){
    st=State{};open=false;focus=0;stick=0;settings=false;item=0;device=0;
    item_focus=nullptr;
    for(int i=0;i<kItems;i++)item_name[i]=item_value[i]=nullptr;
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

    // The mini settings (placed in render).
    item_focus=image(b,at,"setting_focus");
    for(int i=0;i<kItems;i++){
        item_name[i]=label(b,at,21,3,kWhite,0,i<kSettingCount?kSettings[i].title:"Headset");
        item_value[i]=label(b,at,21,0,kText,2);
        if(!item_name[i]||!item_value[i])return false;}

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

void on_gesture(Gesture,Builder& b){
    open=!open;
    focus=0;stick=0;settings=false;
    b.set_capture(open);
    render(b);
}

void on_pad(std::uint32_t down,float lx,float,Builder& b){
    if(!open)return;
    if(down&kOptions){settings=!settings;item=0;render(b);return;}
    if(settings){
        // ponytail: the D-pad only here; the stick moves the call page's tiles.
        if(down&kCircle){settings=false;render(b);return;}
        const int move=((down&kDown)?1:0)-((down&kUp)?1:0);
        const int change=((down&kRight)?1:0)-((down&kLeft)?1:0);
        if(move)item=((item+move)%kItems+kItems)%kItems;
        if(change&&item<kSettingCount&&!(item==0&&st.live[0])){
            const Setting& set=kSettings[item];int& v=st.set[item];
            const int next=v+change<set.min?set.min:v+change>set.max?set.max:v+change;
            if(next!=v){
                v=next;
                char command[48];std::snprintf(command,sizeof command,"set %s %d",set.key,v);
                b.send(command);
                if(item==5)b.set_hold((v+3)*100);}}
        if(change&&item==kSettingCount&&st.devices)device=((device+change)%st.devices+st.devices)%st.devices;
        if((down&kCross)&&item==kSettingCount&&st.devices&&st.bt_on){
            const int d=device<st.devices?device:0;
            char command[48];
            if(!std::strcmp(st.dev_addr[d],st.connected))std::snprintf(command,sizeof command,"bt disconnect");
            else std::snprintf(command,sizeof command,"bt connect %s",st.dev_addr[d]);
            b.send(command);}
        if(move||change)render(b);
        return;}
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
