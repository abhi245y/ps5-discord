// SPDX-License-Identifier: GPL-3.0-or-later
// The demo UI (a magenta bar L3+R3 toggles). The engine host tests build with it
// (-DOVERLAY_APP_DEMO); the Discord UI is discord_app.inc.

namespace oui { namespace app {

namespace {
const Color kBarColor{0.95f,0.05f,0.75f,0.92f};   // loud magenta demo skin
constexpr float kBarW=700,kBarH=64,kBarY=27,kInset=12;
const char* kDefaultText="HOLD L3+R3 TO TOGGLE THIS OVERLAY";

void* bar=nullptr;
void* text_label=nullptr;
bool shown=true;
char last_line[128]="";
}

void reset(){
    bar=nullptr;text_label=nullptr;shown=true;last_line[0]=0;
}

bool build(Builder& b){
    const float x=b.screen_width()-kBarW-kInset*3;
    bar=b.add_panel({x,kBarY,kBarW,kBarH},kBarColor);
    if(!bar)return false;
    text_label=b.add_label(bar,{x+kInset,kBarY+9,kBarW-kInset*2,kBarH-18},kDefaultText);
    return text_label!=nullptr;
}

void on_gesture(Gesture,Builder& b){
    shown=!shown;
    b.set_visible(nullptr,shown);
}

void on_text(const char* line,Builder& b){
    if(text_label&&line&&line[0]&&line!=last_line){ // engine already deduped
        b.set_text(text_label,line);
    }
}

void on_tick(std::uint64_t){}

void on_pad(std::uint32_t,float,float,Builder&){}

}}  // namespace oui::app
