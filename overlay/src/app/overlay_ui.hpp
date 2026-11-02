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
// Every rendered frame after build (UI thread). Keep it cheap.
void on_tick(std::uint64_t frame);
// Reset internal state; used by the host test harness.
void reset();

}}  // namespace oui::app
