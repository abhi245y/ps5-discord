// SPDX-License-Identifier: GPL-3.0-or-later
// The overlay UI API. This header and src/app/overlay_app.cpp are the only
// files UI work should touch: everything here is plain UI code (rectangles,
// colours, labels, gestures). All console, threading and runtime plumbing
// lives behind it in src/core (do not edit that from UI tasks).
#pragma once
#include <cstdint>

namespace oui {

struct Color { float r=1,g=1,b=1,a=1; };
struct Rect { float x=0,y=0,w=0,h=0; };

// Gesture edges the engine reports. L3+R3 is the open/close chord today.
enum class Gesture : std::uint8_t { Chord };

// Handle to a widget created through the Builder. Opaque; only valid inside
// app callbacks. nullptr where documented means "the whole overlay".
class Builder {
public:
    // Adds a filled panel as a child of the overlay scene.
    void* add_panel(const Rect& r,const Color& c){
        return vt&&vt->add_panel_fn?vt->add_panel_fn(ctx,&r,&c):nullptr;}
    // Adds a text label. Parent may be a panel or nullptr for the scene root.
    void* add_label(void* parent,const Rect& r,const char* text){
        return vt&&vt->add_label_fn?vt->add_label_fn(ctx,parent,&r,text):nullptr;}
    // Replaces a label's text.
    bool set_text(void* label,const char* text){
        return vt&&vt->set_text_fn&&vt->set_text_fn(ctx,label,text);}
    // Shows or hides one widget, or the whole overlay when widget is nullptr.
    bool set_visible(void* widget,bool show){
        return vt&&vt->set_visible_fn&&vt->set_visible_fn(ctx,widget,show);}
    // Refills a panel's background colour.
    bool set_color(void* panel,const Color& c){
        return vt&&vt->set_color_fn&&vt->set_color_fn(ctx,panel,&c);}
    // While on, the engine passes the pad to app::on_pad every frame (the panel is
    // open). The game still gets the presses too.
    bool set_capture(bool on){return vt&&vt->set_capture_fn&&vt->set_capture_fn(ctx,on);}
    // Hands a short action word to the console side (cmd.bin; the newest one wins).
    bool send(const char* command){return vt&&vt->send_fn&&vt->send_fn(ctx,command);}
    // A picture from a PNG file (a path as ShellUI sees it, /user/data/...), stretched to r,
    // on the overlay's root. nullptr when pictures are unavailable.
    void* add_image(const Rect& r,const char* path){
        return vt&&vt->add_image_fn?vt->add_image_fn(ctx,&r,path):nullptr;}
    // Another file for a picture ("" clears it); the same path again does nothing.
    bool set_image(void* image,const char* path){
        return vt&&vt->set_image_fn&&vt->set_image_fn(ctx,image,path);}
    // Multiplies a picture by c: white art takes any colour.
    bool set_tint(void* image,const Color& c){return vt&&vt->set_tint_fn&&vt->set_tint_fn(ctx,image,&c);}
    // A label's font size (px at 1080p), weight (0 normal, 1 bold, 2 light, 3 medium), colour and
    // alignment (0 left, 1 centre, 2 right). Long text ends in an ellipsis.
    bool set_text_style(void* label,float size,int weight,const Color& c,int align=0){
        return vt&&vt->set_text_style_fn&&vt->set_text_style_fn(ctx,label,size,weight,&c,align);}
    // Moves and resizes a widget.
    bool set_rect(void* widget,const Rect& r){return vt&&vt->set_rect_fn&&vt->set_rect_fn(ctx,widget,&r);}
    // How long Options is held to open the panel (ms; the engine keeps 300-1500).
    bool set_hold(int ms){return vt&&vt->set_hold_fn&&vt->set_hold_fn(ctx,ms);}
    // A text field the system keyboard types into, hidden until begin_edit. nullptr when
    // this ShellUI has no keyboard for us (the class or its methods are missing).
    void* add_field(const Rect& r,const char* placeholder){
        return vt&&vt->add_field_fn?vt->add_field_fn(ctx,&r,placeholder):nullptr;}
    // Shows the field empty and opens the system keyboard on it, for this console user (the
    // keyboard is per user; 0 leaves ShellUI's). While it is open ShellUI gets the real pad
    // and on_pad gets nothing.
    bool begin_edit(void* field,int user){return vt&&vt->begin_edit_fn&&vt->begin_edit_fn(ctx,field,user);}
    // 1 while the keyboard is open, 0 once it closed, -1 on error; out gets the text so far
    // (UTF-8), the final text once it closed.
    int edit_poll(void* field,char* out,int cap){return vt&&vt->edit_poll_fn?vt->edit_poll_fn(ctx,field,out,cap):-1;}
    float screen_width() const{return vt&&vt->screen_w_fn?vt->screen_w_fn(ctx):1920.f;}
    float screen_height() const{return vt&&vt->screen_h_fn?vt->screen_h_fn(ctx):1080.f;}

    // Engine wiring. Application code never reads these.
    struct Vt {
        void*(*add_panel_fn)(void*,const Rect*,const Color*);
        void*(*add_label_fn)(void*,void*,const Rect*,const char*);
        bool(*set_text_fn)(void*,void*,const char*);
        bool(*set_visible_fn)(void*,void*,bool);
        bool(*set_color_fn)(void*,void*,const Color*);
        float(*screen_w_fn)(void*);
        float(*screen_h_fn)(void*);
        bool(*set_capture_fn)(void*,bool);
        bool(*send_fn)(void*,const char*);
        void*(*add_image_fn)(void*,const Rect*,const char*);
        bool(*set_image_fn)(void*,void*,const char*);
        bool(*set_tint_fn)(void*,void*,const Color*);
        bool(*set_text_style_fn)(void*,void*,float,int,const Color*,int);
        bool(*set_rect_fn)(void*,void*,const Rect*);
        bool(*set_hold_fn)(void*,int);
        void*(*add_field_fn)(void*,const Rect*,const char*);
        bool(*begin_edit_fn)(void*,void*,int);
        int(*edit_poll_fn)(void*,void*,char*,int);
    };
    const Vt* vt=nullptr;
    void* ctx=nullptr;
};

namespace app {

// Build the initial UI. Called once on the UI thread when the overlay scene
// is ready. Return false to abort (the overlay is not shown).
bool build(Builder& b);
// A gesture edge was recognised (UI thread). Show/hide, switch layouts...
void on_gesture(Gesture g,Builder& b);
// A new content line arrived (text.bin today; the core API later).
void on_text(const char* line,Builder& b);
// While capture is on: the buttons newly pressed since the last call (GamePadButtons
// bits, L3/R3 excluded: they are the chord) and the left stick (-1..1), every frame.
void on_pad(std::uint32_t down,float lx,float ly,Builder& b);
// Every rendered frame after build (UI thread). Keep it cheap.
void on_tick(std::uint64_t frame,Builder& b);
// Reset internal state; used by the host test harness.
void reset();

}}  // namespace oui::app
