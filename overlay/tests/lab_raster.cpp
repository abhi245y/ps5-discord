// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/app/pill_lab_model.hpp"
#include <cassert>
#include <limits>
#include <sys/stat.h>
#include <vector>
#include <zlib.h>
namespace lab=pill_lab;
namespace {
unsigned read32(const unsigned char* p){return unsigned(p[0])<<24|unsigned(p[1])<<16|unsigned(p[2])<<8|p[3];}
std::vector<unsigned char> decode(const char* path,int w,int h){
    FILE* f=std::fopen(path,"rb");assert(f);
    unsigned char signature[8];assert(std::fread(signature,1,8,f)==8);
    const unsigned char expected[]={137,80,78,71,13,10,26,10};assert(!std::memcmp(signature,expected,8));
    std::vector<unsigned char> z;bool ihdr=false,iend=false;
    while(!iend){
        unsigned char head[8],tail[4];assert(std::fread(head,1,8,f)==8);
        const unsigned n=read32(head);assert(n<lab::MaxRaw+65536);
        std::vector<unsigned char> data(n);assert(!n||std::fread(data.data(),1,n,f)==n);
        assert(std::fread(tail,1,4,f)==4);
        uLong crc=crc32(0,head+4,4);if(n)crc=crc32(crc,data.data(),n);
        assert(read32(tail)==crc);
        if(!std::memcmp(head+4,"IHDR",4)){
            assert(!ihdr&&n==13&&read32(data.data())==unsigned(w)&&read32(data.data()+4)==unsigned(h));
            assert(data[8]==8&&data[9]==6);ihdr=true;
        }else if(!std::memcmp(head+4,"IDAT",4)){assert(ihdr);z.insert(z.end(),data.begin(),data.end());}
        else {assert(!std::memcmp(head+4,"IEND",4)&&n==0);iend=true;}
    }
    assert(std::fgetc(f)==EOF);std::fclose(f);
    std::vector<unsigned char> out((w*4+1)*h);uLongf size=out.size();
    assert(uncompress(out.data(),&size,z.data(),z.size())==Z_OK&&size==out.size());
    for(int y=0;y<h;y++)assert(out[y*(w*4+1)]==0);
    return out;
}
const unsigned char* pixel(const std::vector<unsigned char>& p,int w,int x,int y){return p.data()+y*(w*4+1)+1+x*4;}
void parameters(){
    for(int preset=0;preset<2;preset++){
        lab::preset(preset);
        for(int i=0;i<lab::Count;i++){
            auto& p=lab::parameters[i];assert(p.value>=p.min&&p.value<=p.max&&p.step>0);
            assert(p.original>=p.min&&p.original<=p.max&&p.notch>=p.min&&p.notch<=p.max);
            for(int j=0;j<i;j++)assert(std::strcmp(p.name,lab::parameters[j].name));
            for(float v:{-1e20f,1e20f,std::numeric_limits<float>::quiet_NaN(),
                         std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()}){
                lab::set(lab::Id(i),v);assert(std::isfinite(p.value)&&p.value>=p.min&&p.value<=p.max);
            }
        }
    }
    lab::preset(0);FILE* f=std::tmpfile();assert(f);
    std::fputs("height=nan\nheight=inf\nfont_size=18junk\ncorner=-99\nviewers=999999999\nunknown=4\noutline_alpha=-100\n=2\nfont_weight=2.5\n",f);
    for(int i=0;i<500;i++)std::fputc('a',f);
    std::fputs("height=44\nheight=33.5\nfill_r=1e99\n",f);std::rewind(f);assert(lab::load(f));std::fclose(f);
    assert(lab::get(lab::Height)==33.5f&&lab::get(lab::Corner)==0&&lab::get(lab::Viewers)==9999);
    assert(lab::get(lab::FontSize)==16&&lab::get(lab::FontWeight)==3&&lab::get(lab::FillR)==32);
    assert(lab::get(lab::OutlineAlpha)==0);
    f=std::tmpfile();assert(f);assert(lab::write_values(f));std::rewind(f);
    const float height=lab::get(lab::Height);lab::preset(1);assert(lab::load(f));assert(lab::get(lab::Height)==height);std::fclose(f);
    f=std::tmpfile();assert(f);for(int i=0;i<40000;i++)std::fputc('x',f);
    std::fputs("\nheight=77\n",f);std::rewind(f);assert(!lab::load(f));std::fclose(f);
    lab::preset(0);f=std::tmpfile();assert(f);std::fputs("height=40",f);std::rewind(f);
    assert(lab::load(f)&&lab::get(lab::Height)==40);std::fclose(f);
    f=std::tmpfile();assert(f);const char binary[]="height=77\0hidden\nheight=41\n";
    assert(std::fwrite(binary,1,sizeof binary-1,f)==sizeof binary-1);std::rewind(f);
    assert(lab::load(f)&&lab::get(lab::Height)==41);std::fclose(f);
}
}
int main(){
    parameters();(void)mkdir("build/lab-raster",0700);
    // Original at 1x/2x; Notch in every corner at 1x/2x/3x. Fractional outlines included.
    for(int test=0;test<6;test++){
        lab::preset(test<2?0:1);
        lab::set(lab::Corner,float(test<2?test:test-2));lab::set(lab::RenderScale,float(test%3+1));
        if(test<2)lab::set(lab::OutlineWidth,test?.5f:2.5f);
        const auto shape=lab::shape(240,198);int w=0,h=0;std::size_t bytes=0;
        assert(lab::raster(shape,w,h,bytes));assert(w==240*shape.scale&&h==int(shape.h*shape.scale));
        char path[100];std::snprintf(path,sizeof path,"build/lab-raster/case-%d.png",test);
        FILE* f=std::fopen(path,"wb");assert(f&&lab::png(f,shape));assert(std::fclose(f)==0);
        const auto rgba=decode(path,w,h);
        for(int corner=0;corner<4;corner++){
            const auto* p=pixel(rgba,w,corner%2?w-1:0,corner/2?h-1:0);
            if(shape.radius[corner]>=7)assert(p[3]==0);
            else {
                assert(shape.radius[corner]==0);
                const float alpha=corner%2?shape.fill.a:shape.live.a;
                assert(p[3]==static_cast<unsigned char>(alpha*255+.5f));
            }
        }
        if(test<2){
            const int x=int((shape.segment+10)*shape.scale),y=0;
            const auto* edge=pixel(rgba,w,x,y);
            // White outline raises RGB/alpha above the underlying dark glass.
            assert(edge[0]>shape.fill.r*255+10&&edge[3]>shape.fill.a*255);
        }
        bool antialiased=false;
        for(int y=0;y<h;y++)for(int x=0;x<w;x++){
            const unsigned char a=pixel(rgba,w,x,y)[3];
            const int full=int((x<float(shape.segment*shape.scale)?shape.live.a:shape.fill.a)*255+.5f);
            if(a>0&&a<full)antialiased=true;
        }
        assert(antialiased);
        assert(pixel(rgba,w,10*shape.scale,h/2)[0]>240); // coral segment
        assert(pixel(rgba,w,100*shape.scale,h/2)[3]>0); // body
        assert(bytes<=sizeof lab::raw);
    }
    lab::preset(0);auto invalid=lab::shape(240,198);invalid.w=1e20f;
    int w,h;std::size_t bytes;assert(!lab::raster(invalid,w,h,bytes));
    // Full allowed image bound, fractional geometry and a multi-block zlib stream.
    auto large=lab::shape(1535.5f,1400);large.h=96;large.scale=3;
    FILE* f=std::fopen("build/lab-raster/max.png","wb");assert(f&&lab::png(f,large));assert(std::fclose(f)==0);
    decode("build/lab-raster/max.png",4607,288);
    std::puts("lab raster: bounds, malformed load, presets, corners, CRC/zlib and outline checks passed");
}
