// SPDX-License-Identifier: GPL-3.0-or-later
// Shared by the freestanding worker and host tests. No allocation or C++ runtime.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include "overlay_ui.hpp"

namespace pill_lab {
struct Parameter {
    const char* name;
    float value,min,max,step,original,notch,tuned;
};
// The single source of truth: editor order, persistence, bounds and all three presets.
#define LAB_PARAMETERS(P) \
 P(Preset,"preset",0,0,2,1,1,2.0f) \
 P(Height,"height",32,16,96,.5f,28,26.5f) \
 P(Corner,"corner",1,0,3,1,1,1.0f) \
 P(InsetX,"inset_x",16,0,240,.5f,0,0.0f) \
 P(InsetY,"inset_y",16,0,200,.5f,0,0.0f) \
 P(RadiusOuter,"radius_outer",16,0,48,.5f,0,6.0f) \
 P(RadiusEdge,"radius_edge",16,0,48,.5f,0,5.0f) \
 P(RadiusInner,"radius_inner",16,0,48,.5f,7.5f,16.0f) \
 P(Outline,"outline",1,0,1,1,0,0.0f) \
 P(OutlineWidth,"outline_width",1,0,8,.5f,0,0.5f) \
 P(OutlineR,"outline_r",255,0,255,1,255,255.0f) \
 P(OutlineG,"outline_g",255,0,255,1,255,255.0f) \
 P(OutlineB,"outline_b",255,0,255,1,255,255.0f) \
 P(OutlineAlpha,"outline_alpha",22,0,100,1,22,22.0f) \
 P(FillR,"fill_r",32,0,255,1,32,34.0f) \
 P(FillG,"fill_g",34,0,255,1,34,34.0f) \
 P(FillB,"fill_b",39,0,255,1,39,34.0f) \
 P(FillAlpha,"fill_alpha",62,0,100,1,92,30.0f) \
 P(SegmentWidth,"segment_width",35,0,160,.5f,31,32.5f) \
 P(SegmentR,"segment_r",255,0,255,1,255,255.0f) \
 P(SegmentG,"segment_g",122,0,255,1,122,49.0f) \
 P(SegmentB,"segment_b",89,0,255,1,89,67.0f) \
 P(SegmentAlpha,"segment_alpha",100,0,100,1,100,85.0f) \
 P(LiveSize,"live_icon_size",22,4,64,.5f,19.5f,21.5f) \
 P(LiveR,"live_icon_r",255,0,255,1,255,255.0f) \
 P(LiveG,"live_icon_g",255,0,255,1,255,255.0f) \
 P(LiveB,"live_icon_b",255,0,255,1,255,255.0f) \
 P(LiveAlpha,"live_icon_alpha",100,0,100,1,100,100.0f) \
 P(FontSize,"font_size",16,8,48,1,13,14.0f) \
 P(FontWeight,"font_weight",1,0,3,1,0,1.0f) \
 P(TextR,"text_r",255,0,255,1,255,255.0f) \
 P(TextG,"text_g",255,0,255,1,255,255.0f) \
 P(TextB,"text_b",255,0,255,1,255,255.0f) \
 P(TextAlpha,"text_alpha",100,0,100,1,100,100.0f) \
 P(TimeAlign,"time_align",1,0,2,1,1,1.0f) \
 P(TimeMode,"time_width_mode",0,0,1,1,0,0.0f) \
 P(TimeY,"time_y",0,-24,24,.5f,0,2.5f) \
 P(CountY,"count_y",0,-24,24,.5f,0,2.5f) \
 P(EyeSize,"eye_size",18,4,64,.5f,15.5f,21.5f) \
 P(EyeR,"eye_r",158,0,255,1,158,158.0f) \
 P(EyeG,"eye_g",166,0,255,1,166,166.0f) \
 P(EyeB,"eye_b",179,0,255,1,179,179.0f) \
 P(EyeAlpha,"eye_alpha",100,0,100,1,100,100.0f) \
 P(EyeGap,"eye_gap",5,0,48,.5f,3.5f,6.5f) \
 P(Gap,"item_gap",8,0,48,.5f,5.5f,8.0f) \
 P(PaddingLeft,"padding_left",8,0,64,.5f,5.5f,10.0f) \
 P(PaddingRight,"padding_right",10,0,64,.5f,6.5f,11.0f) \
 P(Divider,"divider",1,0,1,1,1,1.0f) \
 P(DividerWidth,"divider_width",1,0,8,.5f,.5f,1.0f) \
 P(DividerHeight,"divider_height",13,0,96,.5f,11,13.5f) \
 P(DividerR,"divider_r",255,0,255,1,255,255.0f) \
 P(DividerG,"divider_g",255,0,255,1,255,255.0f) \
 P(DividerB,"divider_b",255,0,255,1,255,255.0f) \
 P(DividerAlpha,"divider_alpha",12,0,100,1,12,12.0f) \
 P(MicSize,"mic_size",22,4,64,.5f,19.5f,21.5f) \
 P(MicR,"mic_r",255,0,255,1,255,255.0f) \
 P(MicG,"mic_g",255,0,255,1,255,255.0f) \
 P(MicB,"mic_b",255,0,255,1,255,255.0f) \
 P(MicAlpha,"mic_alpha",100,0,100,1,100,100.0f) \
 P(MutedR,"muted_r",237,0,255,1,237,237.0f) \
 P(MutedG,"muted_g",66,0,255,1,66,60.0f) \
 P(MutedB,"muted_b",69,0,255,1,69,69.0f) \
 P(RenderScale,"render_scale",2,1,3,1,2,2.0f) \
 P(LongTime,"long_time",0,0,1,1,0,0.0f) \
 P(Viewers,"viewers",12,0,9999,1,12,12.0f) \
 P(Muted,"muted",0,0,1,1,0,0.0f)
#define LAB_ENUM(id,key,v,lo,hi,step,notch,tuned) id,
enum Id { LAB_PARAMETERS(LAB_ENUM) Count };
#undef LAB_ENUM
#define LAB_ENTRY(id,key,v,lo,hi,step,notch,tuned) {key,v,lo,hi,step,v,notch,tuned},
static Parameter parameters[]={ LAB_PARAMETERS(LAB_ENTRY) };
#undef LAB_ENTRY
#undef LAB_PARAMETERS
static_assert(sizeof parameters/sizeof parameters[0]==Count);
inline float get(Id id){return parameters[id].value;}
inline float clamp(float v,float lo,float hi){return v!=v?lo:v<lo?lo:v>hi?hi:v;}
inline float snap(float v){return std::floor(v*2.f+.5f)*.5f;}
inline float bounded(const Parameter& p,float v){
    v=clamp(v,p.min,p.max);
    return clamp(p.min+std::floor((v-p.min)/p.step+.5f)*p.step,p.min,p.max);
}
inline void set(Id id,float v){parameters[id].value=bounded(parameters[id],v);}
inline void preset(int which){
    which=int(clamp(float(which),0,2));
    for(int i=0;i<Count;i++)set(Id(i),which==2?parameters[i].tuned:which==1?parameters[i].notch:parameters[i].original);
}
inline bool choice(Id id){
    return id==Preset||id==Corner||id==Outline||id==FontWeight||id==TimeAlign||id==TimeMode||
           id==Divider||id==RenderScale||id==LongTime||id==Muted;
}
inline const char* choice_name(Id id,int value){
    static const char* corners[]={"TL","TR","BL","BR"};
    static const char* weights[]={"Normal","Bold","Light","Medium"};
    static const char* aligns[]={"Left","Centre","Right"};
    if(id==Preset)return value==2?"Tuned":value==1?"Notch":"Original";
    if(id==Corner)return corners[int(clamp(float(value),0,3))];
    if(id==FontWeight)return weights[int(clamp(float(value),0,3))];
    if(id==TimeAlign)return aligns[int(clamp(float(value),0,2))];
    if(id==TimeMode)return value?"Measured":"Widest digits";
    if(id==RenderScale)return value==1?"1x":value==2?"2x":"3x";
    return value?"On":"Off";
}
// Bounded line/file scanning: reject oversized lines, junk suffixes and non-finite numbers.
// Unknown keys and malformed records leave defaults intact. Later valid duplicates win.
inline bool load_line(char* line){
    char* eq=std::strchr(line,'=');if(!eq)return false;*eq++=0;
    char* end=nullptr;const float v=std::strtof(eq,&end);
    if(end==eq||!std::isfinite(v))return false;
    while(*end==' '||*end=='\t'||*end=='\r')++end;
    if(*end)return false;
    for(int i=0;i<Count;i++)if(!std::strcmp(line,parameters[i].name)){
        set(Id(i),v);return true;
    }
    return false;
}
inline bool load(FILE* f){
    if(!f)return false;
    char line[160];unsigned used=0,bytes=0;bool discard=false,any=false;
    // Count every byte, including NUL. Binary junk cannot bypass the file-size bound.
    while(bytes<32768){
        const int c=std::fgetc(f);
        if(c==EOF){
            if(used&&!discard){line[used]=0;any=load_line(line)||any;}
            break;
        }
        ++bytes;
        if(c=='\n'){
            if(!discard){line[used]=0;any=load_line(line)||any;}
            used=0;discard=false;
        }else if(c==0)discard=true;
        else if(!discard){
            if(used<sizeof line-1)line[used++]=char(c);
            else discard=true;
        }
    }
    return any;
}
inline bool write_values(FILE* f){
    if(!f)return false;
    for(int i=0;i<Count;i++)if(std::fprintf(f,"%s=%.1f\n",parameters[i].name,double(parameters[i].value))<0)return false;
    return !std::ferror(f);
}

struct Ink {float r,g,b,a;};
inline Ink ink(Id r,Id g,Id b,Id a){return {get(r)/255,get(g)/255,get(b)/255,get(a)/100};}
struct Shape {
    float w=0,h=0,radius[4]{},segment=0,outline=0,divider_x=0,divider_w=0,divider_h=0;
    Ink fill{},live{},edge{},rule{};
    int scale=2;
};
// Corner order TL, TR, BL, BR. XOR maps outer, adjacent edges, opposite inner.
inline Shape shape(float width,float divider_x,int corner){
    Shape s;s.w=snap(clamp(width,16,1536));s.h=get(Height);s.segment=get(SegmentWidth);
    const int c=int(clamp(float(corner),0,3));
    for(int i=0;i<4;i++)s.radius[i]=clamp(get(i==c?RadiusOuter:i==(c^3)?RadiusInner:RadiusEdge),0,s.h/2);
    s.outline=get(Outline)?get(OutlineWidth):0;
    s.divider_x=snap(divider_x);s.divider_w=get(Divider)?get(DividerWidth):0;
    s.divider_h=clamp(get(DividerHeight),0,s.h);
    s.fill=ink(FillR,FillG,FillB,FillAlpha);s.live=ink(SegmentR,SegmentG,SegmentB,SegmentAlpha);
    s.edge=ink(OutlineR,OutlineG,OutlineB,OutlineAlpha);s.rule=ink(DividerR,DividerG,DividerB,DividerAlpha);
    s.scale=int(get(RenderScale));return s;
}
inline Shape shape(float width,float divider_x){return shape(width,divider_x,int(get(Corner)));}

inline oui::Rect rect(float x,float y,float w,float h){return {snap(x),snap(y),snap(w),snap(h)};}
struct Layout {
    oui::Rect background{},segment{},live{},time{},eye{},count{},divider{},mic{};
    float divider_x=0;
    int corner=0;
};
// Shared lab geometry, including half-pixel snapping and text baseline nudges.
// Measured slots are bounded before arithmetic; the maximum layout fits the raster bound.
inline Layout layout(int corner,float inset_x,float inset_y,float time_width,float count_width){
    Layout l;l.corner=int(clamp(float(corner),0,3));
    time_width=clamp(time_width,0,400);count_width=clamp(count_width,0,200);
    inset_x=clamp(inset_x,0,240);inset_y=clamp(inset_y,0,200);
    const float h=get(Height),gap=get(Gap),segment=get(SegmentWidth);
    const float tx=segment+get(PaddingLeft),ex=tx+time_width+gap;
    const float cx=ex+get(EyeSize)+get(EyeGap),dx=cx+count_width+gap;
    const float rule=get(Divider)?get(DividerWidth):0;
    const float mx=dx+(rule>0?rule+gap:0),w=snap(mx+get(MicSize)+get(PaddingRight));
    const float x=l.corner%2?1920-w-inset_x:inset_x;
    const float y=l.corner<2?inset_y:1080-h-inset_y;
    l.background=rect(x,y,w,h);l.segment=rect(x,y,segment,h);
    const float live=get(LiveSize),es=get(EyeSize),ms=get(MicSize);
    l.live=rect(x+(segment-live)/2,y+(h-live)/2,live,live);
    l.time=rect(x+tx,y+get(TimeY),time_width,h);
    l.eye=rect(x+ex,y+(h-es)/2,es,es);
    l.count=rect(x+cx,y+get(CountY),count_width,h);
    l.divider=rect(x+dx,y+snap((h-clamp(get(DividerHeight),0,h))/2),rule,clamp(get(DividerHeight),0,h));
    l.mic=rect(x+mx,y+(h-ms)/2,ms,ms);l.divider_x=dx;
    return l;
}
inline Layout layout(float time_width,float count_width){
    return layout(int(get(Corner)),get(InsetX),get(InsetY),time_width,count_width);
}
inline bool same_ink(const Ink& a,const Ink& b){return a.r==b.r&&a.g==b.g&&a.b==b.b&&a.a==b.a;}
inline bool same(const Shape& a,const Shape& b){
    if(a.w!=b.w||a.h!=b.h||a.segment!=b.segment||a.outline!=b.outline||a.divider_x!=b.divider_x||
       a.divider_w!=b.divider_w||a.divider_h!=b.divider_h||a.scale!=b.scale)return false;
    for(int i=0;i<4;i++)if(a.radius[i]!=b.radius[i])return false;
    return same_ink(a.fill,b.fill)&&same_ink(a.live,b.live)&&same_ink(a.edge,b.edge)&&same_ink(a.rule,b.rule);
}
inline Ink over(Ink top,Ink bottom){
    const float a=top.a+bottom.a*(1-top.a);
    if(a<=0)return {0,0,0,0};
    const float t=bottom.a*(1-top.a);
    return {(top.r*top.a+bottom.r*t)/a,(top.g*top.a+bottom.g*t)/a,(top.b*top.a+bottom.b*t)/a,a};
}
inline bool inside(const Shape& s,float x,float y,float inset){
    const float w=s.w-2*inset,h=s.h-2*inset;x-=inset;y-=inset;
    if(w<=0||h<=0||x<0||y<0||x>=w||y>=h)return false;
    const int corner=(x>=w/2?1:0)+(y>=h/2?2:0);
    const float r=clamp(s.radius[corner]-inset,0,h/2);
    const float dx=x<w/2?r-x:x-(w-r),dy=y<h/2?r-y:y-(h-r);
    return dx<=0||dy<=0||dx*dx+dy*dy<=r*r;
}
// Max 1536 x 96 layout px at 3x. One static scanline-filtered RGBA image (~5.1 MB).
constexpr int MaxW=4608,MaxH=288;
constexpr std::size_t MaxRaw=(MaxW*4+1)*MaxH;
static unsigned char raw[MaxRaw];
inline bool raster(const Shape& s,int& width,int& height,std::size_t& bytes){
    width=height=0;bytes=0;
    if(!std::isfinite(s.w)||!std::isfinite(s.h)||s.w<16||s.w>1536||s.h<16||s.h>96||s.scale<1||s.scale>3)return false;
    width=int(std::ceil(s.w*s.scale));height=int(std::ceil(s.h*s.scale));
    const std::size_t stride=std::size_t(width)*4+1;bytes=stride*height;
    if(bytes>sizeof raw)return false;
    // 4x4 subpixels per output pixel. Accumulate premultiplied, export straight RGBA.
    for(int y=0;y<height;y++){
        unsigned char* row=raw+std::size_t(y)*stride;row[0]=0;
        for(int x=0;x<width;x++){
            float sum[4]{};
            for(int sy=0;sy<4;sy++)for(int sx=0;sx<4;sx++){
                const float px=(x+(sx+.5f)/4)/s.scale,py=(y+(sy+.5f)/4)/s.scale;
                if(!inside(s,px,py,0))continue;
                Ink c=px<s.segment?s.live:s.fill;
                if(s.divider_w>0&&px>=s.divider_x&&px<s.divider_x+s.divider_w&&
                   py>=snap((s.h-s.divider_h)/2)&&py<snap((s.h-s.divider_h)/2)+s.divider_h)c=over(s.rule,c);
                if(s.outline>0&&!inside(s,px,py,s.outline))c=over(s.edge,c);
                sum[0]+=c.r*c.a;sum[1]+=c.g*c.a;sum[2]+=c.b*c.a;sum[3]+=c.a;
            }
            unsigned char* out=row+1+x*4;
            for(int i=0;i<3;i++)out[i]=sum[3]>0?static_cast<unsigned char>(clamp(sum[i]/sum[3]*255+.5f,0,255)):0;
            out[3]=static_cast<unsigned char>(clamp(sum[3]/16*255+.5f,0,255));
        }
    }
    return true;
}
inline void be32(unsigned char* p,std::uint32_t v){p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
inline std::uint32_t crc_update(std::uint32_t c,const unsigned char* p,std::size_t n){
    for(std::size_t i=0;i<n;i++){c^=p[i];for(int j=0;j<8;j++)c=(c>>1)^((c&1)?0xedb88320u:0);}
    return c;
}
inline bool chunk(FILE* f,const char* type,const unsigned char* p,std::size_t n){
    unsigned char head[8],tail[4];be32(head,std::uint32_t(n));std::memcpy(head+4,type,4);
    auto crc=crc_update(0xffffffffu,head+4,4);if(n)crc=crc_update(crc,p,n);be32(tail,crc^0xffffffffu);
    return std::fwrite(head,1,8,f)==8&&(!n||std::fwrite(p,1,n,f)==n)&&std::fwrite(tail,1,4,f)==4;
}
// Separate IDAT chunks form one zlib stream. Stored blocks need only 64 KB scratch.
inline bool png(FILE* f,const Shape& s){
    int w=0,h=0;std::size_t bytes=0;if(!f||!raster(s,w,h,bytes))return false;
    static const unsigned char signature[]={137,80,78,71,13,10,26,10};
    unsigned char ihdr[13]{};be32(ihdr,w);be32(ihdr+4,h);ihdr[8]=8;ihdr[9]=6;
    if(std::fwrite(signature,1,8,f)!=8||!chunk(f,"IHDR",ihdr,13))return false;
    const unsigned char zhead[]={0x78,0x01};if(!chunk(f,"IDAT",zhead,2))return false;
    static unsigned char block[65540];std::uint32_t a=1,b=0;
    for(std::size_t at=0;at<bytes;){
        const std::size_t n=bytes-at>65535?65535:bytes-at;
        block[0]=(at+n==bytes)?1:0;block[1]=n;block[2]=n>>8;block[3]=~n;block[4]=(~n)>>8;
        std::memcpy(block+5,raw+at,n);
        for(std::size_t i=0;i<n;i++){a=(a+raw[at+i])%65521;b=(b+a)%65521;}
        if(!chunk(f,"IDAT",block,n+5))return false;
        at+=n;
    }
    unsigned char adler[4];be32(adler,(b<<16)|a);
    return chunk(f,"IDAT",adler,4)&&chunk(f,"IEND",nullptr,0);
}
} // namespace pill_lab
