// SPDX-License-Identifier: GPL-3.0-or-later
// The Discord call overlay (docs/overlay-plan.md, layouts A + C):
//  - the glance (C): a small see-through card at the top right while the panel is closed:
//    LIVE and time, who is talking, your mic. The core decides when it shows (Settings:
//    always, while someone talks, off).
//  - the call panel (A): L3+R3 (or holding Options) opens it on the right edge: the call, everyone in it with
//    talking and muted marks, your mic and Go Live, and four actions (Mute, Deafen, Go
//    Live, Leave). D-pad or the left stick moves between them, Cross does one, Circle or
//    L3+R3 closes. The game still gets those presses (no controller focus yet).
//
// The core writes one line to the text channel (src/overlay.c in the core), at most 127
// bytes, fields split by '|':
//   1|<state>|<me>|<glance>|<live>|<call>|<people>
//   state  c in a call, j joining, - none
//   me     o mic on, m muted, d deafened
//   glance 1 show the glance card, 0 not
//   live   "" or "<m:ss> <viewers>" while Go Live is on
//   call   the room, or the other person in a DM call
//   people Name~f,... (talking first; f: t talking, m muted, d deafened, - none)
// The worker is built without the C++ library: plain buffers only.

#include <cstdio>
#include <cstring>

namespace oui { namespace app {

namespace {

// The app's colours (screens/style.hpp): charcoal ground, raised cards, one coral accent.
const Color kGlance{0.086f,0.094f,0.114f,0.6f};
const Color kPanel{0.133f,0.145f,0.173f,0.94f};
const Color kCoral{1.0f,0.478f,0.349f,1.0f};
const Color kButton{0.165f,0.18f,0.212f,1.0f};

// GamePadButtons bits, read on 13.00 (worker.log "PAD down=", 2026-11-02).
constexpr std::uint32_t kUp=0x2,kDown=0x8,kCircle=0x40,kCross=0x80;
constexpr float kStickMove=0.6f,kStickRest=0.3f;

// 1920x1080 logical; the TV safe area is 48 px at the sides, 27 px top and bottom.
constexpr float kSafeX=48,kSafeY=27;
constexpr float kGlanceW=440,kGlanceH=156;
constexpr float kPanelW=560;
constexpr int kRows=6;
constexpr int kActions=4;
// What each action sends to the core (src/overlay.c).
const char* const kCommands[kActions]={"mute","deafen","golive","leave"};

struct Person { char name[40]; char flag; };
struct State {
    char state='-', me='o';
    bool glance=false;
    char live[24]="", call[48]="";
    Person people[kRows];
    int count=0;
};

State st;
bool open=false;
int focus=0;      // the focused action
int stick=0;      // the stick's last direction: -1 up, 1 down, 0 at rest
void* glance=nullptr; void* glance_live=nullptr;
void* glance_line[3]={};
void* panel=nullptr; void* title=nullptr; void* status=nullptr; void* hint=nullptr;
void* row[kRows]={};
void* action[kActions]={}; void* action_label[kActions]={};

// Copies the next '|' field of *p into out and moves *p past it.
void field(const char** p,char* out,std::size_t cap){
    const char* end=std::strchr(*p,'|');
    std::size_t n=end?std::size_t(end-*p):std::strlen(*p);
    std::snprintf(out,cap,"%.*s",int(n),*p);
    *p+=end?n+1:n;
}

}  // namespace

// Reads a core line into s; false when it is not one.
bool parse(const char* line,State& s){
    if(!line||std::strncmp(line,"1|",2))return false;
    s=State{};
    const char* p=line+2;
    char f[128];
    field(&p,f,sizeof f);s.state=f[0]?f[0]:'-';
    field(&p,f,sizeof f);s.me=f[0]?f[0]:'o';
    field(&p,f,sizeof f);s.glance=f[0]=='1';
    field(&p,s.live,sizeof s.live);
    field(&p,s.call,sizeof s.call);
    field(&p,f,sizeof f);
    for(const char* q=f;*q&&s.count<kRows;){
        const char* comma=std::strchr(q,',');
        std::size_t n=comma?std::size_t(comma-q):std::strlen(q);
        const char* tilde=static_cast<const char*>(std::memchr(q,'~',n));
        Person& who=s.people[s.count++];
        std::size_t name_n=tilde?std::size_t(tilde-q):n;
        std::snprintf(who.name,sizeof who.name,"%.*s",int(name_n),q);
        who.flag=tilde&&tilde+1<q+n?tilde[1]:'-';
        q+=comma?n+1:n;
    }
    return true;
}

namespace {

const char* mic_text(char me){
    return me=='d'?"You - deafened":me=='m'?"You - muted":"You - mic on";
}

void show(Builder& b,void* w,bool on){if(w)b.set_visible(w,on);}

// Puts the state on screen.
void render(Builder& b){
    const bool in_call=st.state=='c'||st.state=='j';
    char text[96];

    // The glance.
    if(st.live[0]){
        char time[16]="";int viewers=0;
        std::sscanf(st.live,"%15s %d",time,&viewers);
        std::snprintf(text,sizeof text,"LIVE %s - %d watching",time,viewers);
    }else
        std::snprintf(text,sizeof text,"%s",st.state=='j'?"Joining...":st.call);
    b.set_text(glance_line[0],text);
    text[0]=0;
    for(int i=0;i<st.count;i++)
        if(st.people[i].flag=='t'){
            std::size_t n=std::strlen(text);
            std::snprintf(text+n,sizeof text-n,"%s%s",n?", ":"",st.people[i].name);}
    if(text[0]){std::size_t n=std::strlen(text);std::snprintf(text+n,sizeof text-n," talking");}
    else std::snprintf(text,sizeof text,"Nobody talking");
    b.set_text(glance_line[1],text);
    b.set_text(glance_line[2],mic_text(st.me));
    const bool glance_on=!open&&in_call&&st.glance;
    show(b,glance,glance_on);
    show(b,glance_live,glance_on&&st.live[0]);
    for(void* w:glance_line)show(b,w,glance_on);

    // The call panel.
    b.set_text(title,in_call?st.call:"Not in a call");
    if(st.live[0])std::snprintf(text,sizeof text,"%s - Go Live on (%s)",mic_text(st.me),st.live);
    else std::snprintf(text,sizeof text,"%s",in_call?mic_text(st.me):"Join a call in the app");
    b.set_text(status,text);
    for(int i=0;i<kRows;i++){
        const bool used=in_call&&i<st.count;
        if(used){
            const Person& who=st.people[i];
            const char* mark=who.flag=='t'?" - talking":who.flag=='m'?" - muted":who.flag=='d'?" - deafened":"";
            std::snprintf(text,sizeof text,"%s%s",who.name,mark);
            b.set_text(row[i],text);}
        show(b,row[i],open&&used);
    }
    const char* names[kActions]={st.me=='m'||st.me=='d'?"Unmute":"Mute",st.me=='d'?"Undeafen":"Deafen",
                                 st.live[0]?"Stop Go Live":"Go Live","Leave call"};
    for(int i=0;i<kActions;i++){
        b.set_text(action_label[i],names[i]);
        b.set_color(action[i],i==focus?kCoral:kButton);
        show(b,action[i],open&&in_call);show(b,action_label[i],open&&in_call);
    }
    show(b,panel,open);show(b,title,open);show(b,status,open);show(b,hint,open);
}

}  // namespace

void reset(){
    st=State{};open=false;focus=0;stick=0;
    for(void*& w:action)w=nullptr;
    for(void*& w:action_label)w=nullptr;
    glance=glance_live=panel=title=status=hint=nullptr;
    for(void*& w:glance_line)w=nullptr;
    for(void*& w:row)w=nullptr;
}

// Labels go on the overlay's root: a child's X/Y are relative to its parent, and these are
// screen coordinates. render() shows and hides each one with its card.
bool build(Builder& b){
    const float w=b.screen_width(),h=b.screen_height();
    // The glance, top right.
    const float gx=w-kSafeX-kGlanceW,gy=kSafeY;
    glance=b.add_panel({gx,gy,kGlanceW,kGlanceH},kGlance);
    glance_live=b.add_panel({gx,gy,8,kGlanceH},kCoral); // the live mark: a coral edge
    for(int i=0;i<3;i++)
        glance_line[i]=b.add_label(nullptr,{gx+24,gy+12+i*46.0f,kGlanceW-40,40},"");
    // The call panel, right edge, top to bottom of the safe area.
    const float px=w-kSafeX-kPanelW,py=kSafeY,ph=h-2*kSafeY;
    panel=b.add_panel({px,py,kPanelW,ph},kPanel);
    title=b.add_label(nullptr,{px+32,py+28,kPanelW-64,52},"");
    status=b.add_label(nullptr,{px+32,py+90,kPanelW-64,40},"");
    for(int i=0;i<kRows;i++)
        row[i]=b.add_label(nullptr,{px+32,py+160+i*64.0f,kPanelW-64,48},"");
    for(int i=0;i<kActions;i++){
        const float y=py+580+i*84.0f;
        action[i]=b.add_panel({px+24,y,kPanelW-48,72},kButton);
        action_label[i]=b.add_label(nullptr,{px+48,y+16,kPanelW-96,40},"");
        if(!action[i]||!action_label[i])return false;}
    hint=b.add_label(nullptr,{px+32,py+ph-56,kPanelW-64,40},"D-pad: move   Cross: select   Circle: close");
    if(!glance||!glance_live||!panel||!title||!status||!hint)return false;
    for(void* x:glance_line)if(!x)return false;
    for(void* x:row)if(!x)return false;
    render(b);
    return true;
}

void on_gesture(Gesture,Builder& b){
    open=!open;
    focus=0;stick=0;
    b.set_capture(open);
    render(b);
}

void on_pad(std::uint32_t down,float,float ly,Builder& b){
    if(!open)return;
    const int dir=ly<-kStickMove?-1:ly>kStickMove?1:0;
    bool moved=false;
    if(down&kCircle){on_gesture(Gesture::Chord,b);return;}
    const int step=(dir&&dir!=stick?dir:0)+(down&kDown?1:0)-(down&kUp?1:0);
    if(step){focus=((focus+step)%kActions+kActions)%kActions;moved=true;}
    if(dir||(ly>-kStickRest&&ly<kStickRest))stick=dir;
    if((down&kCross)&&(st.state=='c'||st.state=='j'))b.send(kCommands[focus]);
    if(moved)render(b);
}

void on_text(const char* line,Builder& b){
    if(parse(line,st))render(b);
}

void on_tick(std::uint64_t){}

}}  // namespace oui::app
