# The overlay UI — edit here

This directory is the **UI layer** of the PS5 overlay. If your task is "change
what the overlay looks like or does", you only need these two files:

- `overlay_ui.hpp` — the API: `oui::Builder` (panels, labels, colours, text,
  visibility, screen size), `oui::Gesture`, and the four app callbacks.
- `overlay_app.cpp` — the current UI: a demo bar that L3+R3 toggles.

Everything is plain UI code. There is no console, memory, process or runtime
plumbing in these files — the engine behind the API handles all of it.

## The app contract

The engine calls these on the console's UI thread at the right moments:

```cpp
namespace oui { namespace app {
bool build(Builder& b);                        // once, when the scene is ready
void on_gesture(Gesture g, Builder& b);        // L3+R3 chord edge
void on_text(const char* line, Builder& b);    // new content line (deduped)
void on_chat(const char* feed, Builder& b);    // new chat feed (chat.bin, deduped, up to 16 KB)
void on_tick(std::uint64_t frame, Builder& b); // every rendered frame
}}
```

Keep a widget handle (`void*`) from `add_panel`/`add_label` to change it
later. Coordinates are in a 1920x1080 logical space regardless of output
resolution; `screen_width()`/`screen_height()` report the live values. A label
added to a panel is placed relative to that panel; pass `nullptr` to use
screen coordinates. Colours are sRGB; the engine converts them for PUI.

Example — the whole demo bar:

```cpp
bool build(Builder& b){
    float x=b.screen_width()-736;
    bar=b.add_panel({x,27,700,64},{0.95f,0.05f,0.75f,0.92f});
    label=b.add_label(bar,{x+12,36,676,46},"hello");
    return bar&&label;
}
void on_gesture(Gesture,Builder& b){ shown=!shown; b.set_visible(nullptr,shown); }
```

## Content channel

`on_text` receives each new line written to `text.bin` in the overlay data
folder (today the file channel; later the core's local API). Write a line,
the label changes on screen within a few frames — no rebuild needed.

## Build, test, verify

From the repo root (on the build PC):

```
make test CXX=clang++ CC=clang     # host suites incl. the fake-runtime tests
make all                           # all payloads incl. overlay-widget.elf
```

The host tests run the app callbacks against a fake runtime, so layout and
lifecycle logic can be checked on a PC before any console run. Deployment is
the standard payload flow used elsewhere in this workspace.

## Known limits (engine-side, deliberate)

- Labels use the system default font/colour; custom font/colour support is
  the next engine feature (13.x recipe is already identified).
- 16 widgets per overlay build (ask if you need more).
- Gestures: the L3+R3 chord only, for now.
