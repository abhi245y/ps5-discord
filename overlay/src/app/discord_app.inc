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
//  - the keyboard (our own, over the panel's left): Triangle on the call page opens it for the
//    call's text chat. D-pad or stick moves, Cross types, Square erases, Triangle is a space,
//    L1/R1 move the caret, L2 is Shift (twice: caps lock), R2 sends (send.bin, the core posts
//    it), Circle closes it and keeps the draft. Three pages (lower/upper, symbols).
// - the tabs (mockups d-chat, e-mentions): Call, Chat and Mentions on the panel's top; L1/R1 switch
//   them (not in the settings). Chat is ONE chat the core picks (the call's text chat, the channel
//   last opened in the app, or a mention's); Triangle types in it, D-pad up/down scrolls. Mentions
//   lists the recent ones (up/down, Cross opens that chat). Both come from chat.bin:
//   H<TAB>kind<TAB>channel<TAB>title<TAB>sub<TAB>icon<TAB>unseen<TAB>focus<TAB>rev  (one per feed)
//   M<TAB>id<TAB>avatar<TAB>name<TAB>time<TAB>mine<TAB>text                       (the chat, oldest first)
//   N<TAB>id<TAB>channel<TAB>place<TAB>avatar<TAB>name<TAB>age<TAB>text          (mentions, newest first)
//   kind c a channel, d a DM, - no chat; records end in a newline, at most 16 KB.
// Art: app/assets/overlay (overlay/tools/overlay_art.py, sizes shared with this file).
// Avatars: round PNGs the core keeps in the overlay's av/ folder.
//
// The core's line (src/overlay.c), at most 2047 bytes, fields split by '|':
//   3|<state>|<me>|<glance>|<live>|<viewers>|<call>|<timer>|<quality>|<settings>|<headsets>|<people>
//   state  c in a call, j joining, - none, x the core quit (nothing opens)
//   me     o mic on, m muted, d deafened
//   glance 1 show the talking card               live  Go Live's time, "" when off
//   settings  the kSettings values, dot-separated (anything after them is ignored)     headsets  on~connected addr~addr^name;...
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
                        kL1=0x400,kR1=0x800,kTriangle=0x20,kSquare=0x10,
                        kL2=0x1000,kR2=0x2000; // L2/R2 inferred from the pattern, not seen yet
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
// Low latency: hands-free in calls (less delay, phone-quality sound).
constexpr const char* kCallModes[]={"High quality","Low latency"};
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
    {"Call mode","bt_handsfree",0,1,kCallModes,0,nullptr},
};
constexpr int kSettingCount=int(sizeof kSettings/sizeof *kSettings);
constexpr int kHeadset=-1;   // the headset row: the paired headsets and Disconnect
constexpr int kBluetooth=-2; // Bluetooth on or off (the adapter)
constexpr int kCallMode=10;  // AirPods only: other headsets are always hands-free in calls
// The tabs and their rows (kSettings indexes).
struct Tab { const char* name; float w; int rows[4]; int count; };
constexpr Tab kTabs[]={
    {"Go Live",92,{0},1},
    {"Voice",76,{1,2},2},
    {"Card",66,{3,4,5},3},
    {"Alerts",76,{6,7,8,9},4},
    {"Headset",96,{kBluetooth,kHeadset,kCallMode},3}, // Call mode only with AirPods (rows_in)
};
constexpr int kTabCount=int(sizeof kTabs/sizeof *kTabs),kSettingRows=4,kOptions=6;
constexpr float kTabY=150,kItemY=214,kItemH=62,kItemStep=70,kOptionH=48;
enum class Kind{choice,toggle,slider,stepper};
Kind kind_of(int s){
    if(s==kBluetooth)return Kind::toggle;
    if(s==kHeadset)return Kind::choice;
    const Setting& set=kSettings[s];
    if(set.names==kOnOff)return Kind::toggle;
    if(set.names)return Kind::choice;
    return set.unit[0]=='s'?Kind::stepper:Kind::slider;
}

struct Person { char id[24]; char avatar[64]; char name[64]; char flag; };
// The chat feed (chat.bin).
constexpr int kMsgMax=20,kMenMax=10;
struct Msg { char id[24],avatar[64],name[40],time[12],text[140]; bool mine; };
struct Men { char id[24],chan[24],place[80],avatar[64],name[40],age[8],text[140]; };
struct Chat {
    char kind='-',chan[24]="",title[72]="",sub[72]="",icon[64]="",focus[24]="";
    int unseen=0;
    Msg msgs[kMsgMax]; int n=0;
    Men men[kMenMax]; int nmen=0;
};
struct State {
    char state='-', me='o';
    bool glance=false;
    char live[16]="", call[64]="", timer[16]="", quality[32]="";
    int viewers=0;
    int set[kSettingCount]={0,5,5,1,0,3,1,1,1,0,0};
    bool bt_on=false,airpods=false; // airpods: Call mode applies (others are hands-free in calls)
    char connected[18]="";
    char dev_addr[4][18]={}, dev_name[4][40]={};
    int devices=0;
    Person people[8];
    int count=0;
};

State st;
Chat chat;
// A change the core has not confirmed yet wins over its lines for a moment (fast slider
// presses otherwise step back to a value from a line already on its way).
struct Pending { int value=0; std::uint64_t until=0; };
Pending pending[kSettingCount+1]; // the last: Bluetooth on/off
std::uint64_t frame_now=0;
std::uint64_t alive_at=0; char seen[2048]=""; // the last line without its beat
// the frame of the core's last line: it changes every second (the beat)
constexpr std::uint64_t kStaleFrames=60*10; // 10 s without one: the core crashed or was killed
constexpr std::uint64_t kPendingFrames=90; // 1.5 s at 60 fps
bool open=false;
int focus=0;      // the focused tile
bool settings=false; // the panel shows the mini settings
int tab=0,row=0;  // the settings' open tab and focused row
bool expanded=false; // the focused row's choices are open
int option=0;     // the focused choice
int stick=0;      // the stick's last direction: -1 left, 1 right, 0 at rest
int page=0;       // the tab: 0 Call, 1 Chat, 2 Mentions
constexpr int kPages=3,kMsgRows=4;
int scroll=0;     // Chat: how many messages newer than the last row shown
int men_sel=0,men_top=0; char men_id[24]=""; // Mentions: the selected one (kept by id) and the first row shown
char last_focus[24]="";
char pend_chan[24]="",pend_title[72]="",pend_sub[72]=""; // Open chat asked for: the Chat tab waits for it
std::uint64_t pend_until=0;
constexpr std::uint64_t kPendFrames=60*4;

// The panel.
char timer_shown[16]="\x01"; // the timer's text now
// The timer's box is as wide as the widest text of its format ("8:88", "88:88", "8:88:88"),
// measured on a transparent probe once per format: right-aligned in it, the digits' right
// edge and the clock never move as the seconds change (proportional digits made them jump).
void* timer_probe=nullptr; int probe_wait=0; float timer_w=0;
void* dock=nullptr; void* dock_mid=nullptr; void* dock_end=nullptr; void* title=nullptr; void* clock=nullptr; void* timer=nullptr;
void* row_bg[kRows]={}; void* row_avatar[kRows]={}; void* row_ring[kRows]={}; void* row_name[kRows]={};
void* row_icon[kRows]={};
void* tile[kTiles]={}; void* tile_icon[kTiles]={}; void* tile_label[kTiles]={}; void* tile_sub=nullptr;
void* tile_focus=nullptr; void* rule=nullptr;
void* hint_icon[4]={}; void* hint_text[4]={};
// The tabs, and Chat/Mentions: the four rows are shared (never both at once).
void* tabs_bg=nullptr; void* tab_on=nullptr; void* page_word[kPages]={}; void* tab_badge=nullptr; void* tab_count=nullptr;
void* head_icon=nullptr; void* head_title=nullptr; void* head_sub=nullptr;
struct MsgRow { void *bg,*avatar,*name,*time,*text,*chip,*loc; } mrow[kMsgRows]={};
void* men_focus=nullptr; void* in_field=nullptr; void* in_text=nullptr; void* in_kb=nullptr;
void* empty_text=nullptr;
int rows_layout=0; bool row_mine[kMsgRows]={}; int strip_page=-1,title_mode=1;
// A tab switch: the page's content is set at once but shown a frame later (PUI trims and lays
// text out lazily: a fast switch drew the new text in the old layout, past the panel), and the
// tab highlight slides over (ease-out, 8 frames).
int reveal=0;           // frames until the new page's content shows
float pill_x=-1,pill_from=0; int slide_t=99; // the highlight's x now, where the slide began, its frame
constexpr int kSlide=8;
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
void* nitro_note[2]={}; // under Quality's choices: above 720p · 30 fps is Nitro's
int armed=-1;           // the Quality choice Select was pressed on once (it asks twice)

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
    field(&q,f,sizeof f,'~');s.bt_on=f[0]=='1'||f[0]=='2';s.airpods=f[0]=='2';
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

// Reads a chat feed into c; false (c untouched) when it is not one. Too-long fields are cut,
// records past the limits and unknown ones are skipped.
bool parse_chat(const char* feed,Chat& c){
    if(!feed||std::strncmp(feed,"H\t",2))return false;
    static Chat next; // 10 KB: off the stack
    next.n=next.nmen=0;next.kind='-';next.chan[0]=next.title[0]=next.sub[0]=next.icon[0]=next.focus[0]=0;next.unseen=0;
    bool header=false;
    const char* p=feed;
    while(*p){
        const char* nl=std::strchr(p,'\n');
        std::size_t len=nl?std::size_t(nl-p):std::strlen(p);
        char one[700];
        std::snprintf(one,sizeof one,"%.*s",int(len<sizeof one-1?len:sizeof one-1),p);
        p+=nl?len+1:len;
        if(len<2||one[1]!='\t')continue;
        const char* q=one+2;
        char f[32];
        if(one[0]=='H'&&!header){
            header=true;
            field(&q,f,sizeof f,'\t');next.kind=f[0]=='c'||f[0]=='d'?f[0]:'-';
            field(&q,next.chan,sizeof next.chan,'\t');
            field(&q,next.title,sizeof next.title,'\t');
            field(&q,next.sub,sizeof next.sub,'\t');
            field(&q,next.icon,sizeof next.icon,'\t');
            field(&q,f,sizeof f,'\t');if(std::sscanf(f,"%d",&next.unseen)!=1||next.unseen<0)next.unseen=0;
            field(&q,next.focus,sizeof next.focus,'\t');
        }else if(one[0]=='M'&&next.n<kMsgMax){
            Msg& m=next.msgs[next.n++];
            field(&q,m.id,sizeof m.id,'\t');field(&q,m.avatar,sizeof m.avatar,'\t');
            field(&q,m.name,sizeof m.name,'\t');field(&q,m.time,sizeof m.time,'\t');
            field(&q,f,sizeof f,'\t');m.mine=f[0]=='1';
            field(&q,m.text,sizeof m.text,'\t');
        }else if(one[0]=='N'&&next.nmen<kMenMax){
            Men& m=next.men[next.nmen++];
            field(&q,m.id,sizeof m.id,'\t');field(&q,m.chan,sizeof m.chan,'\t');
            field(&q,m.place,sizeof m.place,'\t');field(&q,m.avatar,sizeof m.avatar,'\t');
            field(&q,m.name,sizeof m.name,'\t');field(&q,m.age,sizeof m.age,'\t');
            field(&q,m.text,sizeof m.text,'\t');
        }
    }
    c=next;
    return true;
}

namespace {

// The panel is three slices (rounded top, a middle that stretches, rounded bottom), so it
// ends under whatever it holds: content_end is that, the rule and the hints follow it.
void place(Builder& b,float tiles_top,float content_end){
    const float ry=content_end+32,hy=ry+28,bottom=hy+70;
    b.set_rect(dock,{kDockX,kDockY,kDockW,40});
    b.set_rect(dock_mid,{kDockX,kDockY+40,kDockW,bottom-kDockY-80});
    b.set_rect(dock_end,{kDockX,bottom-40,kDockW,40});
    if(tiles_top>0){
        for(int i=0;i<kTiles;i++){
            const float x=kRowX+kTileStep*i;
            b.set_rect(tile[i],{x,tiles_top,kTileW,kTileH});
            b.set_rect(tile_icon[i],{x+38,tiles_top+(i==2?22:28),46,46});
            b.set_rect(tile_label[i],{x,tiles_top+(i==2?76:86),kTileW,30});}
        b.set_rect(tile_sub,{kRowX+kTileStep*2,tiles_top+104,kTileW,24});
        b.set_rect(tile_focus,{kRowX+kTileStep*focus-6,tiles_top-6,134,152});}
    b.set_rect(rule,{kRowX,ry,kRowW,2});
    // A label's text sits high in its box: 5 px lower lines it up with the 37 px glyphs.
    const Rect call_g[4]={{1347,hy+1,37,37},{1472,hy+1,37,37},{1600,hy+5,78,28},{0,0,1,1}};
    const Rect call_w[4]={{1392,hy+5,76,40},{1517,hy+5,66,40},{1686,hy+5,86,40},{0,0,1,1}};
    // Chat and Mentions (d-chat, e-mentions): the action, the tabs, then Close at the right.
    // With Square (back to the default chat) on Chat: Type, Call chat, Tabs, Close, closer together.
    const Rect back_g[4]={{1347,hy+1,37,37},{1745,hy+1,37,37},{1599,hy+5,82,28},{1450,hy+1,37,37}};
    const Rect back_w[4]={{1390,hy+5,50,40},{1788,hy+5,62,40},{1687,hy+5,48,40},{1493,hy+5,96,40}};
    const Rect list_g[4]={{1347,hy+1,37,37},{1722,hy+1,37,37},{1528,hy+5,82,28},{0,0,1,1}};
    const Rect list_w[4]={{1392,hy+5,90,40},{1767,hy+5,100,40},{1618,hy+5,70,40},{0,0,1,1}};
    const bool lists=!settings&&page>0,back=lists&&page==1&&chat.focus[0];
    for(int i=0;i<4;i++){
        b.set_rect(hint_icon[i],back?back_g[i]:lists?list_g[i]:call_g[i]);
        b.set_rect(hint_text[i],back?back_w[i]:lists?list_w[i]:call_w[i]);}
}

void set_avatar(Builder& b,void* w,const Person& who){
    char p[160];
    if(who.avatar[0])path(p,sizeof p,kAvatars,who.avatar);
    else path(p,sizeof p,kArt,"avatar_none"); // until the core has fetched it
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
    if(s==kBluetooth){std::snprintf(out,cap,"%s",st.bt_on?"On":"Off");return;}
    if(s==kHeadset){
        int d=-1;for(int i=0;i<st.devices;i++)if(!std::strcmp(st.dev_addr[i],st.connected))d=i;
        std::snprintf(out,cap,"%s",!st.bt_on?"Bluetooth off":!st.devices?"None paired":d<0?"Not connected":st.dev_name[d]);
        return;}
    const Setting& set=kSettings[s];const int v=st.set[s];
    if(s==0&&st.live[0])std::snprintf(out,cap,"Live now");
    else if(set.names)std::snprintf(out,cap,"%s",set.names[v<set.min?set.min:v>set.max?set.max:v]);
    else if(set.unit[0]=='s')std::snprintf(out,cap,"\xe2\x80\xb9  %d.%d s  \xe2\x80\xba",(v+3)/10,(v+3)%10);
    else std::snprintf(out,cap,"%d%s",v*set.scale,set.unit);
}

bool can_open(int s){ // a row whose choices open
    if(s==kHeadset)return st.devices&&st.bt_on;
    if(s==kBluetooth)return false;
    return kind_of(s)==Kind::choice&&!(s==0&&st.live[0]);
}

// The rows a tab shows now: Call mode only while AirPods are connected.
int rows_in(const Tab& t){return t.count-(t.rows[t.count-1]==kCallMode&&!st.airpods);}

// The highlighted Quality choice needs Nitro (or a boosted server); like the app, only
// asked when going up from 720p · 30 fps.
bool nitro_choice(){return expanded&&kTabs[tab].rows[row]==0&&option>0&&st.set[0]==0;}
constexpr float kNoteH=76;

// Where the settings' rows end (with the open row's choices).
float settings_end(){
    const Tab& t=kTabs[tab];
    float end=kItemY+kItemStep*(rows_in(t)-1)+kItemH;
    if(expanded){const char* names[kOptions];int current=0;end+=20+kOptionH*choices(t.rows[row],names,current);}
    return end+(nitro_choice()?kNoteH:0);
}

void render_settings(Builder& b){
    const Tab& t=kTabs[tab];
    // The tabs: pills tinted coral for the open one, L1 and R1 at the ends.
    float x=kRowX;
    b.set_rect(chip_l1,{x,kTabY+7,38,26});x+=38+7;
    for(int i=0;i<kTabCount;i++){
        b.set_rect(tab_pill[i],{x,kTabY,kTabs[i].w,40});
        b.set_rect(tab_word[i],{x,kTabY+8,kTabs[i].w,32}); // PUI sets text high in its box
        b.set_tint(tab_pill[i],i==tab?kCoral:Color{0.235f,0.247f,0.275f,1});
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
        const bool used=settings&&i<rows_in(t);
        const int s=used?t.rows[i]:0;
        const float y=kItemY+kItemStep*i+(expanded&&i>row?drop_h:0);
        char text[96];
        if(used){
            b.set_rect(set_bg[i],{kRowX,y,kRowW,kItemH});
            b.set_rect(set_name[i],{kRowX+22,y+15,260,36});
            b.set_text(set_name[i],s==kHeadset?"Headset":s==kBluetooth?"Bluetooth":kSettings[s].title);
            value_text(s,text,sizeof text);
            b.set_text(set_value[i],text);
            b.set_rect(set_value[i],{kRowX+200,y+15,kRowW-(can_open(s)?262:222),36});
            b.set_rect(set_chevron[i],{kRowX+kRowW-50,y+19,24,24});
            art(b,set_chevron[i],expanded&&i==row?"chevron_up":"chevron_down");
            b.set_rect(set_toggle[i],{kRowX+kRowW-82,y+14,60,34});}
        const bool toggle=used&&kind_of(s)==Kind::toggle;
        if(toggle)art(b,set_toggle[i],(s==kBluetooth?st.bt_on:st.set[s])?"toggle_on":"toggle_off");
        show(b,set_bg[i],used);show(b,set_name[i],used);show(b,set_value[i],used&&!toggle);
        show(b,set_chevron[i],used&&can_open(s));show(b,set_toggle[i],toggle);
        if(used&&s>=0&&kind_of(s)==Kind::slider&&slider<2){
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
    show(b,set_focus,settings&&rows_in(t));

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
    const bool note=settings&&nitro_choice();
    if(note){
        const float ny=dy+12+kOptionH*count+12;
        b.set_rect(nitro_note[0],{kRowX+22,ny,kRowW-44,30});
        b.set_rect(nitro_note[1],{kRowX+22,ny+32,kRowW-44,30});
        b.set_text(nitro_note[1],armed==option?"Select again to use it":"Without them it may lag or fail for viewers");
        b.set_text_style(nitro_note[1],19,0,armed==option?kCoral:kDim,0);}
    show(b,nitro_note[0],note);show(b,nitro_note[1],note);
}

// The keyboard. Geometry and tables go with overlay/tools/overlay_art.py (KB_*, LOWER, SYMBOLS).
constexpr float kKbX=60,kKbY=630,kKbW=1232,kKbH=394,kKbPad=26,kKbCol=108,kKbRow=70,kKbKey=62;
constexpr float kKbFieldY=546,kKbFieldH=72;
constexpr int kKbCols=11,kKbMax=2000; // characters: Discord's limit
enum class Fn{shift,page,space,back,left,right,send};
struct FuncKey { float x,w; Fn fn; };
constexpr FuncKey kFuncs[]={{0,1.5f,Fn::shift},{1.5f,1.5f,Fn::page},{3,3,Fn::space},{6,1.5f,Fn::back},
                            {7.5f,1,Fn::left},{8.5f,1,Fn::right},{9.5f,1.5f,Fn::send}};
constexpr int kFuncCount=int(sizeof kFuncs/sizeof *kFuncs);
constexpr const char* kLower[4]={"1234567890@","qwertyuiop#","asdfghjkl'/","zxcvbnm,.?!"};
constexpr const char* kSymbols[3][kKbCols]={
    {"!","\"","#","$","%","&","'","(",")","*","+"},
    {"-","/",":",";","<","=",">","[","]","\\","^"},
    {"_","`","{","|","}","~",",",".","?","\xe2\x82\xac","\xc2\xa3"}};
void* kb_field=nullptr; void* kb_input=nullptr; void* kb_pages=nullptr; void* kb_hl=nullptr;
void* kb_measure=nullptr; // an inkless copy of the text before the caret: its width places the caret
void* kb_bar=nullptr;     // the caret: a thin coral bar over the text (a "|" in it moved the letters)
int kb_blink=0;           // frames since the last key: the caret blinks once it rests
int kb_hold=0;            // frames the stick has stayed in one direction (it repeats)
char kb_shown[kKbMax*4+16]=""; int kb_style=-1; // what the field's label has now (skip unchanged)
bool kb=false;
char kb_channel[24]=""; // where the text goes: Chat's channel (set when it opens)
char kb_title[72]="";   // its name for the placeholder ("#raid-night", a DM's person)
char kb_text[kKbMax*4+1]="";
int kb_len=0,kb_chars=0,kb_caret=0; // bytes, characters, the caret's byte offset
int kb_shift=0;  // 0 off, 1 the next letter, 2 caps lock
int kb_page=0;   // 0 letters, 1 symbols
int kb_row=1,kb_col=0; // the highlighted key (row 4: an index into kFuncs)
float kb_x=0.5f; // the column it was last moved to along a row: up and down keep it
int kb_sx=0,kb_sy=0; // the stick's last direction

int key_count(int row){return row<4?kKbCols:kFuncCount;}
float key_left(int row,int i){return row<4?float(i):kFuncs[i].x;}
float key_width(int row,int i){return row<4?1.0f:kFuncs[i].w;}
float key_center(int row,int i){return key_left(row,i)+key_width(row,i)/2;}
// The key of a row nearest (by its middle) to x.
int key_near(int row,float x){
    int best=0;float gap=1e9f;
    for(int i=0;i<key_count(row);i++){
        const float d=key_center(row,i)>x?key_center(row,i)-x:x-key_center(row,i);
        if(d<gap-0.001f){gap=d;best=i;}}
    return best;
}
// What a character key types now (UTF-8, row 0 to 3).
const char* key_text(int row,int col,char* one){
    if(kb_page&&row>0)return kSymbols[row-1][col];
    one[0]=kLower[row][col];one[1]=0;
    if(kb_shift&&one[0]>='a'&&one[0]<='z')one[0]=char(one[0]-32);
    return one;
}

constexpr float kKbTextX=kKbX+28,kKbTextSize=30;
// The text as the field shows it: the part around the caret (an estimate of the text's width
// stands in for measuring; PUI cuts what is too long at its end, which would hide the caret).
float char_em(unsigned cp){
    if(cp>=0x2e80)return 1.0f;
    if(cp>=0x80)return 0.7f;
    if(cp=='m'||cp=='w'||cp=='M'||cp=='W'||cp=='@')return 0.95f;
    if(std::strchr("iljtf'.,:;!|I()[] `",int(cp)))return 0.34f;
    return cp>='A'&&cp<='Z'?0.72f:0.62f;
}
unsigned cp_at(const char* s,int at,int* len){
    const unsigned char c=static_cast<unsigned char>(s[at]);
    int n=c<0x80?1:c<0xe0?2:c<0xf0?3:4;
    unsigned cp=n==1?c:n==2?c&0x1f:n==3?c&0x0f:c&0x07;
    for(int i=1;i<n;i++){if((static_cast<unsigned char>(s[at+i])&0xc0)!=0x80){n=i;break;}cp=cp<<6|(static_cast<unsigned char>(s[at+i])&0x3f);}
    *len=n;return cp;
}
int cp_before(const char* s,int at){int p=at-1;while(p>0&&(static_cast<unsigned char>(s[p])&0xc0)==0x80)p--;return p;}
void field_text(char* out,std::size_t cap,int* caret_at){
    constexpr float kSize=30,kBudget=1020; // px of text the box holds (less the caret and the ellipses)
    float used=0;
    int a=kb_caret,z=kb_caret;
    auto right=[&](float room){ // grows the window rightwards by room px
        while(z<kb_len){int n;const float w=char_em(cp_at(kb_text,z,&n))*kSize;if(used+w>room)break;used+=w;z+=n;}};
    right(kBudget*0.25f);
    while(a>0){const int p=cp_before(kb_text,a);int n;const float w=char_em(cp_at(kb_text,p,&n))*kSize;
        if(used+w>kBudget)break;used+=w;a=p;}
    right(kBudget);
    int at=0;
    auto put=[&](const char* s,int n){if(at+n<int(cap)){std::memcpy(out+at,s,std::size_t(n));at+=n;}};
    if(a>0)put("\xe2\x80\xa6",3);
    put(kb_text+a,kb_caret-a);*caret_at=at;put(kb_text+kb_caret,z-kb_caret);
    if(z<kb_len)put("\xe2\x80\xa6",3);
    out[at]=0;
}

// Only the highlight (a move): the cheap path, every repeat of a held button.
void place_hl(Builder& b){
    const float w=key_width(kb_row,kb_col);
    art(b,kb_hl,w>2?"kb_hl_3":w>1?"kb_hl_15":"kb_hl_1");
    b.set_rect(kb_hl,{kKbX+kKbPad+kKbCol*key_left(kb_row,kb_col),kKbY+kKbPad+kKbRow*kb_row,kKbCol*w-8,kKbKey});
}
void render_kb(Builder& b){
    const bool on=kb&&open;
    show(b,kb_field,on);show(b,kb_input,on);show(b,kb_pages,on);show(b,kb_hl,on);show(b,kb_bar,on);
    show(b,kb_measure,on); // inkless; drawn only so PUI lays it out
    if(!on)return;
    static char text[kKbMax*4+16]; // off the stack
    int caret_at=0;
    if(kb_len)field_text(text,sizeof text,&caret_at);
    else if(kb_channel[0])std::snprintf(text,sizeof text,"Message %s",chat.title);
    else if(kb_title[0])std::snprintf(text,sizeof text,"Message %s",kb_title);
    else std::snprintf(text,sizeof text,"Message");
    if(kb_style!=(kb_len?1:0)){kb_style=kb_len?1:0;b.set_text_style(kb_input,kKbTextSize,3,kb_len?kWhite:kDim,0);}
    if(std::strcmp(text,kb_shown)){std::snprintf(kb_shown,sizeof kb_shown,"%s",text);b.set_text(kb_input,text);}
    // The caret: after the text before it, as PUI measures it (an estimate when it cannot).
    char before[kKbMax*4+16];std::snprintf(before,sizeof before,"%.*s",caret_at,text);
    float x=0;
    if(caret_at){
        b.set_text(kb_measure,before);x=b.text_width(kb_measure);
        if(x<=0){x=0;for(int i=0;i<caret_at;){int n;x+=char_em(cp_at(before,i,&n))*kKbTextSize;i+=n;}}}
    else b.set_text(kb_measure,""); // nothing stale under the placeholder
    b.set_rect(kb_bar,{kKbTextX+x,kKbFieldY+16,3,40});
    art(b,kb_pages,kb_page?"kb_symbols":kb_shift==2?"kb_caps":kb_shift?"kb_upper":"kb_lower");
    place_hl(b);
}

// The tabs, Chat and Mentions. The panel's top: the tab strip; under it Chat's header and four
// messages and the input row, or Mentions' four rows (the same row widgets).
constexpr float kStripY=46,kSegW=173,kStripH=64;
constexpr float kTitleY=118,kTitleH=44; // the call page's title line, under the tab strip (the timer shares it)
constexpr float kHeadY=124,kChatY=208,kChatH=126,kChatStep=136,kInputY=kChatY+kChatStep*kMsgRows+2,kInputH=72;
constexpr float kMenY=124,kMenH=172,kMenStep=182;
const char* const kPageNames[kPages]={"Call","Chat","Mentions"};

// "Night Shift \xc2\xb7 #raid-night" splits into the server and the channel; a DM has no
// separator.
void split_place(const char* place,char* server,std::size_t scap,char* chan,std::size_t ccap){
    const char* dot=std::strstr(place," \xc2\xb7 ");
    if(!dot){server[0]=chan[0]=0;return;}
    std::snprintf(server,scap,"%.*s",int(dot-place),place);
    std::snprintf(chan,ccap,"%s",dot+4);
}

bool loading(){return pend_chan[0]&&std::strcmp(chat.chan,pend_chan)!=0&&frame_now<pend_until;}
bool chat_ready(){return chat.kind!='-'&&!loading();}
int max_scroll(){return chat.n>kMsgRows?chat.n-kMsgRows:0;}
int men_shown(){return chat.nmen<kMsgRows?chat.nmen:kMsgRows;}

void set_key_avatar(Builder& b,void* w,const char* key){
    char p[160];
    if(key[0])path(p,sizeof p,kAvatars,key);
    else path(p,sizeof p,kArt,"avatar_none"); // until the core has fetched it
    b.set_image(w,p);
}

float pill_target(){return kRowX+3+kSegW*page;}
void render_tabs(Builder& b,bool on){
    show(b,tabs_bg,on);show(b,tab_on,on);
    for(void* w:page_word)show(b,w,on);
    const bool badge=on&&chat.unseen>0&&page!=2;
    show(b,tab_badge,badge);show(b,tab_count,badge);
    if(!on)return;
    b.set_rect(tabs_bg,{kRowX,kStripY,kRowW,kStripH});
    if(slide_t>=kSlide){pill_x=pill_target();b.set_rect(tab_on,{pill_x,kStripY+4,167,56});} // else on_tick slides it
    for(int i=0;i<kPages;i++)
        b.set_rect(page_word[i],{kRowX+kSegW*i-(i==2&&badge?22:0),kStripY+14,kSegW,36});
    if(strip_page!=page){
        strip_page=page;
        for(int i=0;i<kPages;i++)b.set_text_style(page_word[i],24,3,i==page?kWhite:kDim,1);}
    if(badge){
        const Rect r{kRowX+474,kStripY+17,30,30};
        char n[8];std::snprintf(n,sizeof n,chat.unseen>9?"9+":"%d",chat.unseen);
        b.set_rect(tab_badge,r);b.set_rect(tab_count,r);b.set_text(tab_count,n);}
}

void layout_rows(Builder& b,int layout){
    if(rows_layout==layout)return;
    rows_layout=layout;
    for(int i=0;i<kMsgRows;i++){
        MsgRow& r=mrow[i];row_mine[i]=false;
        if(layout==1){
            const float y=kChatY+kChatStep*i;
            b.set_rect(r.bg,{kRowX,y,kRowW,kChatH});
            b.set_rect(r.avatar,{kRowX+16,y+23,80,80});
            b.set_rect(r.name,{kRowX+112,y+14,240,40});
            b.set_rect(r.time,{kRowX+kRowW-20-130,y+14,130,40});
            b.set_rect(r.text,{kRowX+112,y+66,kRowW-112-20,44});
            b.set_rect(r.chip,{kRowX+112,y+73,62,30});
        }else{
            const float y=kMenY+kMenStep*i;
            b.set_rect(r.bg,{kRowX,y,kRowW,kMenH});
            b.set_rect(r.loc,{kRowX+24,y+14,kRowW-24-110-20,30});
            b.set_rect(r.time,{kRowX+kRowW-20-90,y+14,90,30});
            b.set_rect(r.avatar,{kRowX+24,y+58,56,56});
            b.set_rect(r.name,{kRowX+96,y+58,250,56});
            b.set_rect(r.text,{kRowX+24,y+122,kRowW-48,38});
        }}
}

// The rows' texts and pictures for the open tab; returns where the panel's content ends (0: neither tab).
float render_pages(Builder& b){
    const bool shown=open&&!settings;
    render_tabs(b,shown);
    const bool chat_on=shown&&page==1,men_on=shown&&page==2,wait=chat_on&&loading();
    const bool rows_chat=chat_on&&!wait&&chat.kind!='-'&&chat.n>0;
    const bool rows_men=men_on&&chat.nmen>0;
    float end=0;
    // Chat's header and input row.
    show(b,head_icon,chat_on);show(b,head_title,chat_on);show(b,head_sub,chat_on);
    const bool input=chat_on&&chat_ready();
    show(b,in_field,input);show(b,in_text,input);show(b,in_kb,input);
    if(chat_on){
        if(wait){ // the mention's own place until the chat arrives
            b.set_text(head_title,pend_title);b.set_text(head_sub,pend_sub);
            art(b,head_icon,"hash_icon");
        }else if(chat.kind=='-'){
            b.set_text(head_title,"No chat");b.set_text(head_sub,"Open one in Discord");
            art(b,head_icon,"hash_icon");
        }else{
            b.set_text(head_title,chat.title);b.set_text(head_sub,chat.sub);
            if(chat.kind=='d'&&chat.icon[0])set_key_avatar(b,head_icon,chat.icon);
            else art(b,head_icon,"hash_icon");
        }
        b.set_rect(head_icon,{kRowX+6,kHeadY,64,64});
        const float room=kRowW-88;
        b.set_rect(head_title,{kRowX+88,kHeadY-2,room,38});
        b.set_rect(head_sub,{kRowX+88,kHeadY+36,room,28});
        b.set_rect(in_field,{kRowX,kInputY,kRowW,kInputH});
        b.set_rect(in_text,{kRowX+24,kInputY+19,kRowW-24-76,48}); // PUI sets text high in its box
        b.set_rect(in_kb,{kRowX+kRowW-24-40,kInputY+21,40,30});
        char hint[100];std::snprintf(hint,sizeof hint,"Message %s",chat.title);
        b.set_text(in_text,hint);
        end=chat.kind=='-'&&!wait?kHeadY+64+40:kInputY+kInputH;
    }
    // The shared rows.
    int shown_rows=0;
    if(rows_chat||rows_men)layout_rows(b,rows_chat?1:2);
    if(rows_chat){
        const int first=chat.n-kMsgRows-scroll>0?chat.n-kMsgRows-scroll:0;
        shown_rows=chat.n-first<kMsgRows?chat.n-first:kMsgRows;
        for(int i=0;i<shown_rows;i++){
            const Msg& m=chat.msgs[first+i];MsgRow& r=mrow[i];
            art(b,r.bg,m.mine?"msg_row_me":"msg_row");
            set_key_avatar(b,r.avatar,m.avatar);
            b.set_text(r.name,m.name);b.set_text(r.time,m.time);b.set_text(r.text,m.text);
            if(row_mine[i]!=m.mine){row_mine[i]=m.mine;
                b.set_rect(r.text,{kRowX+112+(m.mine?72:0),kChatY+kChatStep*i+66,kRowW-112-20-(m.mine?72:0),44});}
        }
    }else if(rows_men){
        if(men_sel>=chat.nmen)men_sel=chat.nmen-1;
        if(men_sel<men_top)men_top=men_sel;
        if(men_sel>=men_top+kMsgRows)men_top=men_sel-kMsgRows+1;
        shown_rows=men_shown();
        for(int i=0;i<shown_rows;i++){
            const Men& m=chat.men[men_top+i];MsgRow& r=mrow[i];
            art(b,r.bg,"men_row");
            set_key_avatar(b,r.avatar,m.avatar);
            b.set_text(r.loc,m.place);b.set_text(r.time,m.age);b.set_text(r.name,m.name);b.set_text(r.text,m.text);
        }
        const int at=men_sel-men_top;
        b.set_rect(men_focus,{kRowX,kMenY+kMenStep*at,kRowW,kMenH});
        end=kMenY+kMenStep*(shown_rows-1)+kMenH;
    }
    for(int i=0;i<kMsgRows;i++){
        MsgRow& r=mrow[i];
        const bool on=i<shown_rows&&(rows_chat||rows_men);
        show(b,r.bg,on);show(b,r.avatar,on);show(b,r.name,on);show(b,r.time,on);show(b,r.text,on);
        show(b,r.chip,on&&rows_chat&&row_mine[i]);show(b,r.loc,on&&rows_men);
    }
    show(b,men_focus,rows_men);
    // The empty states.
    const char* none=wait?"Loading\xe2\x80\xa6":(chat_on&&chat.kind!='-'&&!chat.n)?"No messages yet":(men_on&&!chat.nmen)?"No mentions":nullptr;
    show(b,empty_text,none!=nullptr);
    if(none){
        b.set_text(empty_text,none);
        const float y=chat_on?kChatY+kChatStep*1:kMenY+20;
        b.set_rect(empty_text,{kRowX,y,kRowW,44});}
    if(men_on&&!chat.nmen)end=kMenY+110;
    if(reveal){ // the new page is laid out (and the panel sized for it); it shows next frame
        void* const heads[]={head_icon,head_title,head_sub,in_field,in_text,in_kb,men_focus,empty_text};
        for(void* w:heads)show(b,w,false);
        for(MsgRow& r:mrow){void* const parts[]={r.bg,r.avatar,r.name,r.time,r.text,r.chip,r.loc};for(void* w:parts)show(b,w,false);}}
    return end;
}

// The timer's box ends at the cards' edge; the clock sits before it, centred on the digits.
void place_timer(Builder& b){
    // A right-aligned label keeps ~7 px after its text (the same photo): the box ends past the cards' edge by that.
    const float w=timer_w+8,right=kRowX+kRowW+7;
    // Measured on the console (a straight-on photo, the 132 px card as the ruler): at +2 the
    // digits' baseline was 10 px above the title's, the clock's middle 12 px below the digits'.
    b.set_rect(timer,{right-w,kTitleY+12,w,kTitleH});
    b.set_rect(clock,{right-w-34,kTitleY+16,26,26});
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

    // A: the panel (call page, Chat, Mentions, or the mini settings).
    const bool call_page=open&&!settings&&page==0;
    if(title_mode!=(settings?0:1)){ // the call page's title shares its line with the timer, under the tabs
        title_mode=settings?0:1;
        b.set_rect(title,settings?Rect{kDockX+37,kDockY+40,kDockW-74,54}:Rect{kDockX+37,kTitleY,kDockW-74-170,kTitleH});
        b.set_text_style(title,settings?40:32,1,kWhite);
        b.set_text_valign(title,settings?0:1);} // bottom-aligned with the timer on the call page only
    b.set_text(title,settings?"Settings":in_call?st.call:"Not in a call");
    if(std::strcmp(st.timer,timer_shown)){
        const bool format=std::strlen(st.timer)!=std::strlen(timer_shown);
        std::snprintf(timer_shown,sizeof timer_shown,"%s",st.timer);b.set_text(timer,st.timer);
        if(format){ // a new format: its widest text on the probe, measured two frames on (layout first)
            char widest[16];int i=0;for(;st.timer[i]&&i<15;i++)widest[i]=st.timer[i]==':'?':':'8';widest[i]=0;
            b.set_text(timer_probe,widest);probe_wait=2;
            timer_w=0;for(const char* c=st.timer;*c;c++)timer_w+=*c==':'?7:14; // until measured: 25 px digits
            place_timer(b);}}
    show(b,timer_probe,call_page&&in_call);
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
    const int n=in_call?(st.count<kRows?st.count:kRows):0;
    const float tiles_top=kRowY+kRowStep*n+(n?10:0);
    const float pages_end=render_pages(b);
    if(settings)place(b,0,settings_end());
    else if(page>0)place(b,0,pages_end);
    else if(in_call)place(b,tiles_top,tiles_top+kTileH);
    else place(b,0,kDockY+130);
    const bool tiles_on=call_page&&in_call;
    for(int i=0;i<kTiles;i++){show(b,tile[i],tiles_on);show(b,tile_icon[i],tiles_on);show(b,tile_label[i],tiles_on);}
    show(b,tile_sub,tiles_on);show(b,tile_focus,tiles_on);
    show(b,dock,open);show(b,dock_mid,open);show(b,dock_end,open);show(b,title,open&&(settings||page==0));show(b,clock,call_page&&in_call&&st.timer[0]);show(b,timer,call_page&&in_call);
    show(b,rule,open);

    render_settings(b);

    // Hints: Cross, Circle and Options (Chat and Mentions: the action, L1/R1, Close); their words follow the page.
    const bool lists=!settings&&page>0;
    b.set_text(hint_text[0],page==2?"Open":page==1?"Type":"Select");
    art(b,hint_icon[0],page==1?"glyph_triangle":"glyph_cross");
    b.set_text(hint_text[1],settings&&!expanded?"Back":"Close");
    b.set_text(hint_text[2],lists?"Tabs":"Settings");
    art(b,hint_icon[2],lists?"chip_l1r1":"chip_options");
    b.set_text(hint_text[3],in_call?"Call chat":"Back");
    const bool act=settings||page==0||(page==1?chat_ready():chat.nmen>0);
    const bool hints[4]={act,true,!settings,!settings&&page==1&&chat.focus[0]&&!kb}; // another chat than the default
    for(int i=0;i<4;i++){
        const bool on=open&&hints[i];
        show(b,hint_icon[i],on);show(b,hint_text[i],on);}
    render_kb(b);
}

}  // namespace

void reset(){
    st=State{};chat.n=chat.nmen=chat.unseen=0;chat.kind='-';chat.chan[0]=chat.title[0]=chat.sub[0]=chat.icon[0]=chat.focus[0]=0;
    page=scroll=men_sel=men_top=0;men_id[0]=last_focus[0]=pend_chan[0]=0;pend_until=0;reveal=0;pill_x=-1;slide_t=99;
    tabs_bg=tab_on=tab_badge=tab_count=head_icon=head_title=head_sub=men_focus=in_field=in_text=in_kb=empty_text=nullptr;
    for(void*& w:page_word)w=nullptr;
    for(MsgRow& r:mrow)r=MsgRow{};
    rows_layout=0;strip_page=-1;title_mode=1;for(bool& m:row_mine)m=false;
    kb_channel[0]=0;
    open=false;focus=0;stick=0;settings=false;tab=row=option=0;expanded=false;frame_now=0;alive_at=0;seen[0]=0;
    for(Pending& p:pending)p=Pending{};
    chip_l1=chip_r1=set_focus=drop_bg=drop_on=drop_check=nullptr;
    for(int i=0;i<kTabCount;i++)tab_pill[i]=tab_word[i]=nullptr;
    for(int i=0;i<kSettingRows;i++)set_bg[i]=set_name[i]=set_value[i]=set_chevron[i]=set_toggle[i]=nullptr;
    for(int i=0;i<2;i++)slider_track[i]=slider_fill[i]=slider_knob[i]=nullptr;
    for(int j=0;j<kOptions;j++)drop_word[j]=nullptr;
    nitro_note[0]=nitro_note[1]=nullptr;armed=-1;
    kb=false;kb_text[0]=0;kb_len=kb_chars=kb_caret=0;kb_shift=kb_page=0;kb_row=1;kb_col=0;kb_x=0.5f;kb_sx=kb_sy=0;
    kb_field=kb_input=kb_pages=kb_hl=kb_measure=kb_bar=nullptr;kb_blink=kb_hold=0;kb_shown[0]=0;kb_style=-1;
    dock=dock_mid=dock_end=title=clock=timer=timer_probe=tile_sub=tile_focus=rule=nullptr;std::snprintf(timer_shown,sizeof timer_shown,"\x01");probe_wait=0;timer_w=0;
    card=pill=card_head=card_call=you_ring=you_mic=you_text=card_rule=card_hint=nullptr;
    for(int i=0;i<kRows;i++)row_bg[i]=row_avatar[i]=row_ring[i]=row_name[i]=row_icon[i]=nullptr;
    for(int i=0;i<kTiles;i++)tile[i]=tile_icon[i]=tile_label[i]=nullptr;
    for(int i=0;i<4;i++)hint_icon[i]=hint_text[i]=nullptr;
    for(int i=0;i<kTalkRows;i++)talk_avatar[i]=talk_ring[i]=talk_name[i]=talk_bars[i]=nullptr;
}

bool build(Builder& b){
    // A: the panel, back to front.
    dock=image(b,{kDockX,kDockY,kDockW,40},"dock_top");
    dock_mid=image(b,{kDockX,kDockY+40,kDockW,kDockH-80},"dock_mid");
    dock_end=image(b,{kDockX,kDockY+kDockH-40,kDockW,40},"dock_bottom");
    title=label(b,{kDockX+37,kTitleY,kDockW-74-170,kTitleH},32,1,kWhite); // the call page's: under the tabs, with the timer at its right
    clock=image(b,{kRowX+kRowW-130-34,kTitleY,26,26},"clock");
    if(clock)b.set_tint(clock,kDim);
    timer=label(b,{kRowX+kRowW-130,kTitleY+12,130,kTitleH},25,3,kDim,2); // right-aligned; placed by place_timer
    timer_probe=label(b,{kRowX,kTitleY,300,kTitleH},25,3,Color{1,1,1,0}); // inkless: measures the widest timer text
    b.set_text_valign(title,1);b.set_text_valign(timer,1); // accepted on 13.00, no visible effect: kept so every load matches the measured state
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
    const char* glyphs[4]={"glyph_cross","glyph_circle","chip_options","glyph_square"};
    const char* words[4]={"Select","Close","Settings","Chat"};
    for(int i=0;i<4;i++){
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
    nitro_note[0]=label(b,at,19,3,kText,0,"Needs Nitro or a boosted server");
    nitro_note[1]=label(b,at,19,0,kDim);
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

    // The tabs, Chat and Mentions (placed in render_pages).
    tabs_bg=image(b,{kRowX,kStripY,kRowW,kStripH},"tabs_bg");
    tab_on=image(b,at,"tab_on");
    for(int i=0;i<kPages;i++)page_word[i]=label(b,at,24,3,kDim,1,kPageNames[i]);
    tab_badge=image(b,at,"badge_dot");
    tab_count=label(b,at,18,1,kWhite,1);
    head_icon=image(b,at,"hash_icon");
    head_title=label(b,at,32,1,kWhite);
    head_sub=label(b,at,20,0,kDim);
    for(MsgRow& r:mrow){
        r.bg=image(b,at,"msg_row");
        r.avatar=b.add_image(at,"");
        r.name=label(b,at,25,3,kWhite);
        r.time=label(b,at,19,0,kDim,2);
        r.text=label(b,at,24,0,kText);
        r.chip=image(b,at,"chip_you");
        r.loc=label(b,at,19,0,kDim);}
    men_focus=image(b,at,"men_focus");
    in_field=image(b,at,"chat_field");
    in_text=label(b,at,24,0,kDim);
    in_kb=image(b,at,"kb_glyph");
    if(in_kb)b.set_tint(in_kb,kDim);
    empty_text=label(b,at,26,3,kDim,1);

    // The keyboard, on top of everything (placed in render_kb).
    kb_field=image(b,{kKbX,kKbFieldY,kKbW,kKbFieldH},"kb_field");
    kb_input=label(b,{kKbX+28,kKbFieldY+14,kKbW-56,46},30,3,kWhite);
    kb_pages=image(b,{kKbX,kKbY,kKbW,kKbH},"kb_lower");
    kb_hl=image(b,at,"kb_hl_1");
    kb_measure=label(b,{kKbTextX,kKbFieldY+14,kKbW-56,46},kKbTextSize,3,Color{1,1,1,0});
    kb_bar=image(b,at,"kb_caret");if(kb_bar)b.set_tint(kb_bar,kCoral);

    // Text is the overlay's job; pictures may be missing on a firmware without ImageBox.
    if(!title||!timer||!tile_sub||!card_head||!card_call||!you_text||!card_hint)return false;
    for(void* w:row_name)if(!w)return false;
    for(void* w:tile_label)if(!w)return false;
    for(void* w:talk_name)if(!w)return false;
    if(!kb_input||!head_title||!in_text||!empty_text||!tab_count)return false;
    for(const MsgRow& r:mrow)if(!r.name||!r.time||!r.text||!r.loc)return false;
    for(void* w:page_word)if(!w)return false;
    render(b);
    return true;
}

namespace {
void apply(int s,int v,Builder& b){
    const Setting& set=kSettings[s];
    v=v<set.min?set.min:v>set.max?set.max:v;
    if(v==st.set[s])return;
    st.set[s]=v;
    pending[s]={v,frame_now+kPendingFrames};
    char command[48];std::snprintf(command,sizeof command,"set %s %d",set.key,v);
    b.send(command);
    if(s==5)b.set_hold((v+3)*100);
}

// Typing. ponytail: no caret blink, no key repeat.
void kb_type(const char* s){
    const int n=int(std::strlen(s));
    if(kb_chars>=kKbMax||kb_len+n>int(sizeof kb_text)-1)return;
    std::memmove(kb_text+kb_caret+n,kb_text+kb_caret,std::size_t(kb_len-kb_caret+1));
    std::memcpy(kb_text+kb_caret,s,std::size_t(n));
    kb_len+=n;kb_caret+=n;kb_chars++;
    if(kb_shift==1&&n==1&&s[0]>='A'&&s[0]<='Z')kb_shift=0; // one-shot: used by a letter
}
void kb_erase(){
    if(!kb_caret)return;
    const int p=cp_before(kb_text,kb_caret);
    std::memmove(kb_text+p,kb_text+kb_caret,std::size_t(kb_len-kb_caret+1));
    kb_len-=kb_caret-p;kb_caret=p;kb_chars--;
}
void kb_move_caret(int dir){
    if(dir<0&&kb_caret)kb_caret=cp_before(kb_text,kb_caret);
    else if(dir>0&&kb_caret<kb_len){int n;cp_at(kb_text,kb_caret,&n);kb_caret+=n;}
}
void kb_shift_key(){if(!kb_page)kb_shift=kb_shift==0?1:kb_shift==1?2:0;} // twice in a row: caps lock

void kb_send(Builder& b){
    if(!kb_len)return;
    b.send_text(kb_text,kb_channel);
    kb_text[0]=0;kb_len=kb_chars=kb_caret=0;kb_shift=0;kb_page=0;
    kb=false;
}

void kb_press(Builder& b){
    if(kb_row<4){char one[2];kb_type(key_text(kb_row,kb_col,one));return;}
    switch(kFuncs[kb_col].fn){
    case Fn::shift:kb_shift_key();break;
    case Fn::page:kb_page^=1;break;
    case Fn::space:kb_type(" ");break;
    case Fn::back:kb_erase();break;
    case Fn::left:kb_move_caret(-1);break;
    case Fn::right:kb_move_caret(1);break;
    case Fn::send:kb_send(b);break;}
}

// The keyboard's buttons (the panel stays captured; Options taps do nothing here).
void kb_pad(std::uint32_t down,float lx,float ly,Builder& b){
    if(down&kCircle){kb=false;render(b);return;}
    int dx=((down&kRight)?1:0)-((down&kLeft)?1:0),dy=((down&kDown)?1:0)-((down&kUp)?1:0);
    // The stick: a step at once, then steps while held (after 300 ms; faster the further it
    // is pushed), along its stronger axis. Up is negative. The D-pad repeats in the engine.
    const float ax=lx<0?-lx:lx,ay=ly<0?-ly:ly,big=ax>ay?ax:ay;
    int sx=0,sy=0;
    if(big>kStickMove){if(ax>=ay)sx=lx<0?-1:1;else sy=ly<0?-1:1;}
    if(sx||sy){
        if(sx!=kb_sx||sy!=kb_sy){kb_sx=sx;kb_sy=sy;kb_hold=0;dx+=sx;dy+=sy;}
        else{const int every=big>0.95f?4:big>0.8f?6:9;if(++kb_hold>=18&&(kb_hold-18)%every==0){dx+=sx;dy+=sy;}}}
    else if(big<kStickRest){kb_sx=kb_sy=0;kb_hold=0;}
    dx=dx<0?-1:dx>0?1:0;dy=dy<0?-1:dy>0?1:0;
    if(!down&&!dx&&!dy)return;
    kb_blink=0;
    if(dx){const int n=key_count(kb_row);kb_col=((kb_col+dx)%n+n)%n;kb_x=key_center(kb_row,kb_col);}
    if(dy){const int r=kb_row+dy;if(r>=0&&r<5){kb_row=r;kb_col=key_near(r,kb_x);}}
    if(down&kSquare)kb_erase();
    if(down&kTriangle)kb_type(" ");
    if(down&kL1)kb_move_caret(-1);
    if(down&kR1)kb_move_caret(1);
    if(down&kL2)kb_shift_key();
    if(down&kCross)kb_press(b);
    if(down&kR2)kb_send(b);
    if(!kb){render(b);return;} // sent
    if(!(down&~(kLeft|kUp|kRight|kDown))){place_hl(b);show(b,kb_bar,true);return;} // a move only
    render_kb(b);
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
        const int was_armed=armed;armed=-1;
        if(down&kCircle)expanded=false;
        else if(move&&count)option=((option+move)%count+count)%count;
        else if((down&kCross)&&nitro_choice()&&was_armed!=option){armed=option;render(b);return;} // asks once more
        else if(down&kCross){
            if(s>=0)apply(s,kSettings[s].min+option,b);
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
    const int n=rows_in(t);
    if(move&&n)row=((row+move)%n+n)%n;
    if(s>=0&&change){
        const Kind k=kind_of(s);
        if(k==Kind::slider||k==Kind::stepper)apply(s,st.set[s]+change,b);}
    if(down&kCross){
        if(s==kBluetooth){
            st.bt_on=!st.bt_on;
            pending[kSettingCount]={st.bt_on,frame_now+kPendingFrames*4}; // the adapter takes a few seconds
            b.send(st.bt_on?"bt on":"bt off");}
        else if(s>=0&&kind_of(s)==Kind::toggle)apply(s,!st.set[s],b);
        else if(can_open(s)){
            const char* names[kOptions];int current=0;
            const int count=choices(s,names,current);
            expanded=true;option=current<count?current:0;}}
    if(move||change||(down&kCross))render(b);
}
}  // namespace

void on_gesture(Gesture,Builder& b){
    if(!open&&st.state=='x')return; // the core is gone: nothing to show or do
    open=!open;
    focus=0;stick=0;settings=false;expanded=false;kb=false;page=0; // the draft stays
    b.set_capture(open);
    render(b);
}

// Switches the tab. Mentions seen: the badge goes and the core is told.
void set_page(int to,Builder& b){
    if(to!=page){reveal=2;pill_from=pill_x<0?pill_target():pill_x;slide_t=0;}
    page=to;
    if(page==2&&chat.unseen>0){chat.unseen=0;b.send("seen");}
    render(b);
}

void open_kb(const char* channel,const char* name,Builder& b){
    kb=true;std::snprintf(kb_channel,sizeof kb_channel,"%s",channel);std::snprintf(kb_title,sizeof kb_title,"%s",name);
    kb_x=key_center(kb_row,kb_col);render(b);
}

// Chat: up/down scroll through the fetched messages; Triangle types in this chat.
void chat_pad(std::uint32_t down,Builder& b){
    const int move=((down&kDown)?-1:0)+((down&kUp)?1:0); // up: older
    if(move){
        const int to=scroll+move<0?0:scroll+move>max_scroll()?max_scroll():scroll+move;
        if(to!=scroll){scroll=to;render(b);}}
    if((down&kTriangle)&&chat_ready())open_kb(chat.chan,chat.title,b);
    if((down&kSquare)&&chat.focus[0]){b.send("chat back");chat.focus[0]=0;render(b);} // back to the call's chat (or the app's)
}

// Mentions: up/down select; Cross opens that chat (the core fetches it; the Chat tab waits for it).
void men_pad(std::uint32_t down,Builder& b){
    const int move=((down&kDown)?1:0)-((down&kUp)?1:0);
    if(move&&chat.nmen){
        const int to=men_sel+move<0?0:men_sel+move>=chat.nmen?chat.nmen-1:men_sel+move;
        if(to!=men_sel){men_sel=to;std::snprintf(men_id,sizeof men_id,"%s",chat.men[to].id);render(b);}}
    if((down&kCross)&&chat.nmen&&men_sel<chat.nmen){
        const Men& m=chat.men[men_sel];
        char command[64];
        if(std::snprintf(command,sizeof command,"chat %s %s",m.chan,m.id)>=48)return; // cmd.bin holds 47
        b.send(command);
        std::snprintf(pend_chan,sizeof pend_chan,"%s",m.chan);
        char server[72],chan[72];split_place(m.place,server,sizeof server,chan,sizeof chan);
        std::snprintf(pend_title,sizeof pend_title,"%s",chan[0]?chan:m.name);
        std::snprintf(pend_sub,sizeof pend_sub,"%s",server[0]?server:"Direct message");
        pend_until=frame_now+kPendFrames;
        scroll=0;last_focus[0]=0;
        set_page(1,b);}
}

void on_pad(std::uint32_t down,float lx,float ly,Builder& b){
    if(!open)return;
    const bool repeated=down&kPadRepeat;down&=~kPadRepeat; // the engine's repeat of a held button
    if(kb){kb_pad(down,lx,ly,b);return;}
    if(down&kOptionsButton){settings=!settings;tab=row=0;expanded=false;render(b);return;}
    if(settings){settings_pad(down,b);return;}
    if(down&kCircle){on_gesture(Gesture::Chord,b);return;}
    if(down&(kL1|kR1)){ // a held L1/R1 does not cycle the tabs: only new presses switch
        const int by=((down&kR1)?1:0)-((down&kL1)?1:0);
        if(by&&!repeated){set_page((page+by+kPages)%kPages,b);return;}
        if(repeated)return;}
    if(page==1){chat_pad(down,b);return;}
    if(page==2){men_pad(down,b);return;}
    const int dir=lx<-kStickMove?-1:lx>kStickMove?1:0;
    const int step=(dir&&dir!=stick?dir:0)+((down&(kRight|kDown))?1:0)-((down&(kLeft|kUp))?1:0);
    if(dir||(lx>-kStickRest&&lx<kStickRest))stick=dir;
    if(step){focus=((focus+step)%kTiles+kTiles)%kTiles;render(b);}
    if((down&kCross)&&(st.state=='c'||st.state=='j'))b.send(kCommands[focus]);
}

// The core is gone (state x): close an open panel so the game gets its pad back.
void go_inert(Builder& b){
    if(!open)return;
    open=settings=expanded=kb=false;
    b.set_capture(false);
}

void on_text(const char* line,Builder& b){
    // Only the beat changed (the digit after the state, every second): alive, nothing to draw.
    char now[2048];
    std::snprintf(now,sizeof now,"%s",line?line:"");
    if(now[0]&&now[1]&&now[2]&&now[3]>='0'&&now[3]<='9')std::memmove(now+3,now+4,std::strlen(now+4)+1);
    const bool same=!std::strcmp(now,seen);
    std::snprintf(seen,sizeof seen,"%s",now);
    if(same&&st.state!='x'){alive_at=frame_now;return;}
    const int hold=st.set[5];
    if(!parse(line,st))return;
    alive_at=frame_now;
    for(int i=0;i<kSettingCount;i++){ // confirmed, or given up on: the core's value again
        Pending& p=pending[i];
        if(!p.until)continue;
        if(frame_now>=p.until||st.set[i]==p.value)p.until=0;else st.set[i]=p.value;}
    Pending& bt=pending[kSettingCount];
    if(bt.until){if(frame_now>=bt.until||st.bt_on==(bt.value!=0))bt.until=0;else st.bt_on=bt.value!=0;}
    if(row>=rows_in(kTabs[tab])){row=rows_in(kTabs[tab])-1;expanded=false;} // AirPods left: Call mode went
    if(st.set[5]!=hold)b.set_hold((st.set[5]+3)*100);
    if(st.state=='x')go_inert(b);
    if(st.state!='c'&&st.state!='j'&&!kb_channel[0])kb=false; // the draft stays
    render(b);
}

// A new chat feed: the chat's messages and the mentions. The view stays on what it showed.
void on_chat(const char* feed,Builder& b){
    char anchor[24]="",sel[24]="";
    if(scroll>0&&chat.n>scroll)std::snprintf(anchor,sizeof anchor,"%s",chat.msgs[chat.n-1-scroll].id);
    std::snprintf(sel,sizeof sel,"%s",men_id);
    char was_chan[24];std::snprintf(was_chan,sizeof was_chan,"%s",chat.chan);
    if(!parse_chat(feed,chat))return;
    if(std::strcmp(was_chan,chat.chan))scroll=0;
    else if(anchor[0]){
        scroll=0;
        for(int i=0;i<chat.n;i++)if(!std::strcmp(chat.msgs[i].id,anchor)){scroll=chat.n-1-i;break;}}
    if(chat.focus[0]&&std::strcmp(chat.focus,last_focus)){ // Open chat: the mentioned message shows, with what follows it
        for(int i=0;i<chat.n;i++)if(!std::strcmp(chat.msgs[i].id,chat.focus)){
            scroll=chat.n-1-(i+1<chat.n?i+1:i);break;}}
    std::snprintf(last_focus,sizeof last_focus,"%s",chat.focus);
    if(scroll>max_scroll())scroll=max_scroll();
    if(pend_chan[0]&&!std::strcmp(chat.chan,pend_chan))pend_chan[0]=0; // it arrived
    men_sel=0;
    for(int i=0;i<chat.nmen;i++)if(!std::strcmp(chat.men[i].id,sel)){men_sel=i;break;}
    if(chat.nmen)std::snprintf(men_id,sizeof men_id,"%s",chat.men[men_sel].id);else men_id[0]=0;
    if(open&&page==2&&chat.unseen>0){chat.unseen=0;b.send("seen");} // read as they come in
    render(b);
}

void on_tick(std::uint64_t frame,Builder& b){
    frame_now=frame;
    if(open&&slide_t<kSlide){ // the tab highlight slides to its tab
        const float t=float(++slide_t)/kSlide,e=1-(1-t)*(1-t);
        pill_x=pill_from+(pill_target()-pill_from)*e;
        b.set_rect(tab_on,{pill_x,kStripY+4,167,56});}
    if(reveal&&open&&!--reveal)render(b); // the content, laid out by now
    if(probe_wait&&open&&!--probe_wait){ // the widest timer text, laid out by now
        const float w=b.text_width(timer_probe);
        if(w>0){timer_w=w;place_timer(b);}}
    if(pend_chan[0]&&frame_now>=pend_until){pend_chan[0]=0;if(open)render(b);} // the chat never came: what the core shows
    if(kb&&open&&kb_bar){ // the caret blinks (0.5 s on, 0.5 s off) while no key is pressed
        if(++kb_blink>=30&&kb_blink%30==0)show(b,kb_bar,(kb_blink/30)%2==0);}
    if(st.state!='x'&&frame_now-alive_at>kStaleFrames){ // no beat: as if the core quit
        st.state='x';
        go_inert(b);
        render(b);}
}

}}  // namespace oui::app
