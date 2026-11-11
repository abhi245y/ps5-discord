// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone synthetic Go Live pill. No commands, feeds, core, or session state.
#include "pill_lab_model.hpp"
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>
#ifndef PILL_LAB_DIR
#define PILL_LAB_DIR "/user/data/discord-overlay-poc/lab"
#endif
#ifndef PILL_LAB_LOG
#define PILL_LAB_LOG "/user/data/discord-overlay-poc/worker.log"
#endif

namespace oui { namespace app {
namespace {
namespace lab=pill_lab;
constexpr std::uint32_t kLeft=1,kUp=2,kRight=4,kDown=8,kSquare=0x10,kTriangle=0x20,
    kCircle=0x40,kCross=0x80,kL1=0x400,kR1=0x800,kR2=0x2000;
constexpr const char* kArt="/user/data/homebrew/PPSA99072/assets/overlay/";
constexpr const char* kDir=PILL_LAB_DIR;
constexpr Color kWhite{1,1,1,1},kDim{.70f,.74f,.80f,1},kCoral{1,.478f,.349f,1};
void* background=nullptr,*live_icon=nullptr,*time_label=nullptr,*eye=nullptr,*count_label=nullptr,*mic=nullptr;
void* editor=nullptr,*selection=nullptr,*title=nullptr,*range_label=nullptr,*hint=nullptr,*status_label=nullptr;
void* rows[5]{};void* time_probes[10]{};void* count_probes[10]{};
bool editor_visible=true,captured=true,dirty=false,have_shape=false,shape_pending=false;
int selected=0,probe_wait=0;
std::uint64_t started=0,last_change=0,last_raster_attempt=0,shape_since=0,save_retry_at=0,version=0;
unsigned last_second=~0u;
char image_path[160]{},time_text[16]{},count_text[16]{},probe_key[64]{},status[100]="Ready";
float time_width=0,count_width=0;
lab::Shape rendered_shape{},wanted_shape{};

std::uint64_t lab_now(){
#ifdef PILL_LAB_NOW_MS
    return PILL_LAB_NOW_MS();
#else
    timespec t{};if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;
    return std::uint64_t(t.tv_sec)*1000+std::uint64_t(t.tv_nsec)/1000000;
#endif
}
Color color(lab::Id r,lab::Id g,lab::Id b,lab::Id a){const auto c=lab::ink(r,g,b,a);return {c.r,c.g,c.b,c.a};}
Rect rect(float x,float y,float w,float h){return {lab::snap(x),lab::snap(y),lab::snap(w),lab::snap(h)};}
bool directory(){
    // Parent normally exists (worker log/request directory). Also support a fresh install.
#ifndef PILL_LAB_HOST
    (void)mkdir("/user/data/discord-overlay-poc",0700);
#endif
    (void)mkdir(kDir,0700);struct stat st{};return stat(kDir,&st)==0&&S_ISDIR(st.st_mode);
}
bool path(char* out,std::size_t n,const char* leaf){
    const int written=std::snprintf(out,n,"%s/%s",kDir,leaf);
    return written>=0&&std::size_t(written)<n;
}
void load(){
    char p[200];if(!path(p,sizeof p,"pill-lab.txt"))return;
    FILE* f=std::fopen(p,"r");if(f){lab::load(f);std::fclose(f);}
}
bool save(){
    char temp[200],dest[200];
    if(!directory()||!path(temp,sizeof temp,"pill-lab.txt.tmp")||!path(dest,sizeof dest,"pill-lab.txt"))return false;
    FILE* f=std::fopen(temp,"w");if(!f)return false;
    bool ok=lab::write_values(f);if(std::fflush(f)!=0)ok=false;
    if(std::fclose(f)!=0)ok=false;
    if(ok)ok=std::rename(temp,dest)==0;
    if(!ok){std::remove(temp);return false;}
    // Exactly one copyable line per successful save; fixed buffer, checked append.
    static char line[8192];std::size_t at=0;
    for(int i=0;i<lab::Count;i++){
        const auto& p=lab::parameters[i];
        int n=std::snprintf(line+at,sizeof line-at,"%s%s=%.1f",i?" ":"",p.name,double(p.value));
        if(n<0||std::size_t(n)>=sizeof line-at)return false;
        at+=std::size_t(n);
    }
    FILE* log=std::fopen(PILL_LAB_LOG,"a");
    if(log){std::fprintf(log,"PILL_LAB %s\n",line);std::fclose(log);}
    return true;
}
void save_now(){
    const bool ok=save();dirty=!ok;
    save_retry_at=ok?0:lab_now()+1000; // Save failures never block a pending raster update.
    std::snprintf(status,sizeof status,"%s",ok?"Saved pill-lab.txt":"Save failed: check lab directory; R2 retries");
}
void visible_editor(Builder& b){
    void* parts[]={editor,selection,title,range_label,hint,status_label,rows[0],rows[1],rows[2],rows[3],rows[4]};
    for(void* part:parts)if(part)b.set_visible(part,editor_visible);
}
void render_editor(Builder& b){
    char text[240];
    std::snprintf(text,sizeof text,"Pill lab / %s%s",lab::choice_name(lab::Preset,int(lab::get(lab::Preset))),dirty?" / unsaved":"");
    b.set_text(title,text);
    for(int row=0;row<5;row++){
        const int index=selected+row-2;
        if(index<0||index>=lab::Count){b.set_text(rows[row],"");continue;}
        const auto& p=lab::parameters[index];
        if(lab::choice(lab::Id(index)))std::snprintf(text,sizeof text,"%s    %s (%.0f)",p.name,lab::choice_name(lab::Id(index),int(p.value)),double(p.value));
        else std::snprintf(text,sizeof text,"%s    %.1f",p.name,double(p.value));
        b.set_text(rows[row],text);
    }
    const auto& p=lab::parameters[selected];
    std::snprintf(text,sizeof text,"%d / %d    Range %.1f - %.1f    Step %.1f    L1/R1: %.1f",selected+1,lab::Count,
        double(p.min),double(p.max),double(p.step),double(p.step*10));
    b.set_text(range_label,text);
    std::snprintf(text,sizeof text,"%s | %s",captured?"Controller captured":"Released: hold Options or L3+R3",status);
    b.set_text(status_label,text);visible_editor(b);
}
void style(Builder& b,void* label,float size,int weight,const Color& c,int align=0){
    b.set_text_style(label,size,weight,c,align);b.set_text_valign(label,2);
}
void format_time(){
    const std::uint64_t elapsed=(lab_now()>=started?lab_now()-started:0)/1000;
    const unsigned base=lab::get(lab::LongTime)?3765u:754u;
    const unsigned total=unsigned((elapsed+base)%(100*3600));
    if(lab::get(lab::LongTime)||total>=3600)std::snprintf(time_text,sizeof time_text,"%u:%02u:%02u",total/3600,(total/60)%60,total%60);
    else std::snprintf(time_text,sizeof time_text,"%u:%02u",total/60,total%60);
    std::snprintf(count_text,sizeof count_text,"%d",int(lab::get(lab::Viewers)));
}
void prepare_probes(Builder& b){
    char key[64];const bool measured=lab::get(lab::TimeMode)!=0;
    std::snprintf(key,sizeof key,"%.0f/%.0f/%.0f/%s/%zu",double(lab::get(lab::FontSize)),double(lab::get(lab::FontWeight)),
        double(lab::get(lab::TimeMode)),measured?time_text:"",std::strlen(count_text));
    // Fixed slots change only at digit-format boundaries, not every second.
    if(!measured){const auto n=std::strlen(key);std::snprintf(key+n,sizeof key-n,"/%zu",std::strlen(time_text));}
    if(!std::strcmp(key,probe_key))return;
    std::snprintf(probe_key,sizeof probe_key,"%s",key);
    const float size=lab::get(lab::FontSize);const int weight=int(lab::get(lab::FontWeight));
    for(int digit=0;digit<10;digit++){
        char t[16]{},c[16]{};
        for(std::size_t i=0;time_text[i]&&i<sizeof t-1;i++)t[i]=measured?time_text[i]:time_text[i]==':'?':':char('0'+digit);
        for(std::size_t i=0;count_text[i]&&i<sizeof c-1;i++)c[i]=char('0'+digit);
        b.set_text(time_probes[digit],t);b.set_text(count_probes[digit],c);
        style(b,time_probes[digit],size,weight,{1,1,1,0});style(b,count_probes[digit],size,weight,{1,1,1,0});
    }
    time_width=lab::snap(size*.70f*float(std::strlen(time_text)));
    count_width=lab::snap(size*.70f*float(std::strlen(count_text)));
    probe_wait=3;
}
void measure(Builder& b){
    if(probe_wait<=0||--probe_wait)return;
    float tw=0,cw=0;
    for(int i=0;i<10;i++){
        const float t=b.text_width(time_probes[i]),c=b.text_width(count_probes[i]);
        if(std::isfinite(t)&&t>tw&&t<400)tw=t;
        if(std::isfinite(c)&&c>cw&&c<200)cw=c;
    }
    if(tw>0)time_width=lab::snap(tw+.5f);
    if(cw>0)count_width=lab::snap(cw+.5f);
}
bool raster_image(Builder& b,const lab::Shape& shape){
    if(!directory())return false;
    char leaf[64],next[160];
    // Time seed avoids reusing an old path after a new payload instance starts.
    if(!version)version=lab_now()*1000;
    std::snprintf(leaf,sizeof leaf,"pill_%llu.png",(unsigned long long)++version);
    if(!path(next,sizeof next,leaf))return false;
    FILE* f=std::fopen(next,"wb");if(!f)return false;
    bool ok=lab::png(f,shape);if(std::fclose(f)!=0)ok=false;
    if(ok)ok=b.set_image(background,next);
    if(!ok){std::remove(next);return false;}
    if(image_path[0])std::remove(image_path);
    std::snprintf(image_path,sizeof image_path,"%s",next);
    rendered_shape=shape;have_shape=true;return true;
}
void render(Builder& b,bool force=false){
    format_time();prepare_probes(b);
    const float h=lab::get(lab::Height),gap=lab::get(lab::Gap),segment=lab::get(lab::SegmentWidth);
    const float tx=segment+lab::get(lab::PaddingLeft),ex=tx+time_width+gap;
    const float cx=ex+lab::get(lab::EyeSize)+lab::get(lab::EyeGap),dx=cx+count_width+gap;
    const float rule=lab::get(lab::Divider)?lab::get(lab::DividerWidth):0;
    const float mx=dx+(rule>0?rule+gap:0),w=lab::snap(mx+lab::get(lab::MicSize)+lab::get(lab::PaddingRight));
    const int corner=int(lab::get(lab::Corner));
    const float x=corner%2?1920-w-lab::get(lab::InsetX):lab::get(lab::InsetX);
    const float y=corner<2?lab::get(lab::InsetY):1080-h-lab::get(lab::InsetY);
    const auto shape=lab::shape(w,dx);const auto now=lab_now();
    if(!lab::same(shape,wanted_shape)){wanted_shape=shape;shape_since=now;}
    if((!have_shape||!lab::same(shape,rendered_shape))&&(force||(now-last_change>=150&&now-shape_since>=150))&&
       (force||!last_raster_attempt||now-last_raster_attempt>=150)){
        last_raster_attempt=now;
        if(!raster_image(b,shape))std::snprintf(status,sizeof status,"PNG failed: check lab directory / image support");
    }
    // Keep the last valid image while repeats settle; native content updates immediately.
    b.set_rect(background,rect(x,y,w,h));
    const float live=lab::get(lab::LiveSize),es=lab::get(lab::EyeSize),ms=lab::get(lab::MicSize);
    b.set_rect(live_icon,rect(x+(segment-live)/2,y+(h-live)/2,live,live));
    b.set_rect(time_label,rect(x+tx,y+lab::get(lab::TimeY),time_width,h));
    b.set_rect(eye,rect(x+ex,y+(h-es)/2,es,es));
    b.set_rect(count_label,rect(x+cx,y+lab::get(lab::CountY),count_width,h));
    b.set_rect(mic,rect(x+mx,y+(h-ms)/2,ms,ms));
    b.set_text(time_label,time_text);b.set_text(count_label,count_text);
    const Color text=color(lab::TextR,lab::TextG,lab::TextB,lab::TextAlpha);
    style(b,time_label,lab::get(lab::FontSize),int(lab::get(lab::FontWeight)),text,int(lab::get(lab::TimeAlign)));
    style(b,count_label,lab::get(lab::FontSize),int(lab::get(lab::FontWeight)),text,1);
    b.set_tint(live_icon,color(lab::LiveR,lab::LiveG,lab::LiveB,lab::LiveAlpha));
    b.set_tint(eye,color(lab::EyeR,lab::EyeG,lab::EyeB,lab::EyeAlpha));
    char p[160];std::snprintf(p,sizeof p,"%s%s.png",kArt,lab::get(lab::Muted)?"mic_off":"mic");b.set_image(mic,p);
    b.set_tint(mic,lab::get(lab::Muted)?color(lab::MutedR,lab::MutedG,lab::MutedB,lab::MicAlpha):color(lab::MicR,lab::MicG,lab::MicB,lab::MicAlpha));
    shape_pending=!have_shape||!lab::same(shape,rendered_shape);
    render_editor(b);
}
void change(int direction,int multiplier,bool cycle=false){
    auto id=lab::Id(selected);const auto& p=lab::parameters[selected];
    float next=p.value+direction*p.step*multiplier;
    if(cycle&&next>p.max)next=p.min;
    if(id==lab::Preset){const int which=int(lab::bounded(p,next));if(which!=int(p.value))lab::preset(which);}
    else lab::set(id,next);
    if(id==lab::LongTime)started=lab_now();
    dirty=true;last_change=lab_now();save_retry_at=0;std::snprintf(status,sizeof status,"Editing; autosave after input settles");
}
} // namespace

void reset(){
    lab::preset(0);background=live_icon=time_label=eye=count_label=mic=nullptr;
    editor=selection=title=range_label=hint=status_label=nullptr;
    for(void*& p:rows)p=nullptr;
    for(void*& p:time_probes)p=nullptr;
    for(void*& p:count_probes)p=nullptr;
    editor_visible=captured=true;dirty=have_shape=shape_pending=false;selected=probe_wait=0;
    started=last_change=last_raster_attempt=shape_since=save_retry_at=version=0;last_second=~0u;
    image_path[0]=time_text[0]=count_text[0]=probe_key[0]=0;time_width=count_width=0;
    std::snprintf(status,sizeof status,"Ready");rendered_shape={};wanted_shape={};
}
bool build(Builder& b){
    reset();load();started=last_change=lab_now();dirty=true;
    background=b.add_image({0,0,1,1},"");
    char p[160];std::snprintf(p,sizeof p,"%sgolive.png",kArt);live_icon=b.add_image({0,0,1,1},p);
    time_label=b.add_label(nullptr,{0,0,400,96},"");
    std::snprintf(p,sizeof p,"%seye.png",kArt);eye=b.add_image({0,0,1,1},p);
    count_label=b.add_label(nullptr,{0,0,200,96},"");
    std::snprintf(p,sizeof p,"%smic.png",kArt);mic=b.add_image({0,0,1,1},p);
    for(int i=0;i<10;i++){
        time_probes[i]=b.add_label(nullptr,{0,0,512,96},"");count_probes[i]=b.add_label(nullptr,{0,0,256,96},"");
        if(!time_probes[i]||!count_probes[i])return false;
    }
    // Centre-screen editor remains clear of every allowed pill inset/height.
    editor=b.add_panel({300,330,1320,420},{.055f,.065f,.085f,.97f});
    selection=b.add_panel({324,498,1272,48},{.20f,.12f,.11f,1});
    title=b.add_label(nullptr,{336,350,1248,42},"");style(b,title,28,1,kWhite);
    for(int i=0;i<5;i++){
        rows[i]=b.add_label(nullptr,{348,float(404+i*48),1224,44},"");
        style(b,rows[i],i==2?24:20,i==2?1:0,i==2?kCoral:kDim);
        if(!rows[i])return false;
    }
    range_label=b.add_label(nullptr,{336,648,1248,30},"");style(b,range_label,18,0,kDim);
    hint=b.add_label(nullptr,{336,684,1248,26},"D-pad: select / adjust | L1/R1: x10 | X: cycle | Triangle: preset | Square: editor | R2: save | Circle: release");
    style(b,hint,17,0,kWhite);
    status_label=b.add_label(nullptr,{336,716,1248,24},"");style(b,status_label,16,0,kDim);
    if(!background||!live_icon||!time_label||!eye||!count_label||!mic||!editor||!selection||!title||!range_label||!hint||!status_label)return false;
    b.set_hold(600);render(b,true);b.set_capture(true);return true;
}
void on_gesture(Gesture,Builder& b){captured=true;b.set_capture(true);render_editor(b);}
void on_text(const char*,Builder&){}
void on_chat(const char*,Builder&){}
void on_pad(std::uint32_t down,float,float,Builder& b){
    if(!captured)return;
    const bool repeat=(down&kPadRepeat)!=0;
    if(down&kCircle){captured=false;b.set_capture(false);render_editor(b);return;}
    if((down&kSquare)&&!repeat){editor_visible=!editor_visible;visible_editor(b);}
    if((down&kTriangle)&&!repeat){lab::preset(1-int(lab::get(lab::Preset)));started=lab_now();dirty=true;last_change=lab_now();save_retry_at=0;}
    if(down&kUp)selected=(selected+lab::Count-1)%lab::Count;
    if(down&kDown)selected=(selected+1)%lab::Count;
    if(down&kLeft)change(-1,1);
    if(down&kRight)change(1,1);
    if(down&kL1)change(-1,10);
    if(down&kR1)change(1,10);
    if((down&kCross)&&!repeat&&lab::choice(lab::Id(selected)))change(1,1,true);
    if((down&kR2)&&!repeat)save_now();
    if(down&~kPadRepeat)render(b);
}
void on_tick(std::uint64_t,Builder& b){
    const auto now=lab_now();
    const unsigned second=unsigned((now>=started?now-started:0)/1000);bool refresh=second!=last_second;
    last_second=second;
    if(probe_wait>0){measure(b);if(!probe_wait)refresh=true;}
    if(dirty&&now-last_change>=150&&now>=save_retry_at){save_now();refresh=true;}
    // Shape changes caused by font measurement or measured timer digits also settle here.
    if(refresh||(shape_pending&&now-last_change>=150&&now-shape_since>=150&&now-last_raster_attempt>=150))render(b);
}
}} // namespace oui::app
