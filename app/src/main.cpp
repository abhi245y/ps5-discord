// ps5-homebrew-ui - Application entry point.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Opens the display, controller and audio, then runs the shell every frame:
// read input, update, play the sounds it asked for, draw, present. Lifecycle
// markers and frame pacing are logged for hardware runs.

#include "app/shell.hpp"
#include "app/tour.hpp"
#include "audio/cues.hpp"
#include "audio/mixer.hpp"
#include "audio/music.hpp"
#include "core/frame_stats.hpp"
#include "core/input.hpp"
#include "core/save_file.hpp"
#include "core/settings.hpp"
#include "core/version.hpp"
#include "app/art.hpp"
#include "app/text_engine.hpp"
#include "discord/client.hpp"
#include "discord/sample.hpp"
#include "gfx/canvas.hpp"
#include "gfx/renderer.hpp"
#include "platform/ps5/audio_out.hpp"
#include "platform/ps5/display_egl.hpp"
#include "platform/ps5/ime.hpp"
#include "platform/ps5/usb_keyboard.hpp"
#include "platform/ps5/voice_audio.hpp"
#include "platform/ps5/pad.hpp"
#include "platform/ps5/system.hpp"
#include "ui/fonts.hpp"

#include <cstring>
#include <GL/glcorearb.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" void hui_heap_stats(std::size_t *live_bytes, std::size_t *peak_bytes,
                               std::size_t *blocks, std::size_t *failures);

namespace
{

constexpr const char *kAssets = "/app0/assets";
// Settings live in the title's own save area.
constexpr const char *kDataRoot = "/download0/hui";
// Development output (the tour's pictures and report, the log). A title's
// sandbox has no /data, so it lives in the title's own storage. While the
// title runs, a PC can *read* the folder over FTP as
// /mnt/sandbox/<TITLE_ID>_000/download0/hui/dev, but cannot write there.
constexpr const char *kDevRoot = "/download0/hui/dev";
// Requests therefore come in through the install folder, which a PC can
// write: /data/homebrew/<TITLE_ID>/dev/request.txt is /app0/dev/request.txt
// here. One line: "tour <design id|all> <token>", "quit - <token>",
// "shot <name> <token>" (the live frame to kDevRoot/<name>.bmp) or
// "press <key,key,...> <token>" (up down left right cross circle triangle
// square l1 r1 l2 r2 options, one every kPressSeconds; "key~N" holds it N frames). A request is honoured
// once per token (the last one is remembered in kDevRoot).
constexpr const char *kRequestPath = "/app0/dev/request.txt";
// How long a finished tour stays up so the PC can copy its evidence (the
// storage is unmounted when the title closes) before the app closes itself.
constexpr double kCollectSeconds = 240.0;
// Quarter of a quarter: the title's storage is small and a tour takes
// hundreds of pictures. Enough to compare layout and colour with the PC's.
constexpr int kCaptureWidth = 480;
constexpr int kCaptureHeight = 270;
// Live screenshots ("shot") are half size: enough to read the text.
constexpr int kShotWidth = 960;
constexpr int kShotHeight = 540;
constexpr double kPressSeconds = 0.35;

extern "C" int sceSystemServiceParamGetInt(int id, int *value);

// SceSystemServiceStatus: whether the app is the one on screen. On the home screen (the
// app minimised) it runs on in the background; under the PS menu it is overlaid.
struct SystemStatus
{
    std::int32_t event_count;
    bool ui_overlaid;
    bool in_background;
    bool cpu_mode7_normal;
    bool streaming;
    bool out_of_vr_area;
    std::uint8_t reserved[125];
};
extern "C" int sceSystemServiceGetStatus(SystemStatus *status);

// The console's offset from UTC (Settings > Date and Time), in minutes:
// system parameter 4 is the zone's offset, 5 is summer time (as spot5).
int console_utc_minutes()
{
    int zone = 0;
    int summer = 0;
    const int zone_rc = sceSystemServiceParamGetInt(4, &zone);
    const int summer_rc = sceSystemServiceParamGetInt(5, &summer);
    hui::sys::log("[HUI] time zone offset=%d summer=%d", zone, summer);
    return (zone_rc == 0 ? zone : 0) + (summer_rc == 0 && summer ? 60 : 0);
}

// One remote key ("press" request) as the pad would have produced it.
hui::InputFrame remote_key(const std::string &key)
{
    using namespace hui;
    static const struct
    {
        const char *name;
        Action action;
        Direction nav;
    } keys[] = {
        {"up", Action::up, Direction::up},          {"down", Action::down, Direction::down},
        {"left", Action::left, Direction::left},    {"right", Action::right, Direction::right},
        {"cross", Action::confirm, Direction::none}, {"circle", Action::back, Direction::none},
        {"triangle", Action::north, Direction::none}, {"square", Action::west, Direction::none},
        {"l1", Action::page_prev, Direction::none}, {"r1", Action::page_next, Direction::none},
        {"l2", Action::jump_prev, Direction::none}, {"r2", Action::jump_next, Direction::none},
        {"options", Action::menu, Direction::none},
        {"l3", Action::l3, Direction::none},         {"r3", Action::r3, Direction::none},
    };
    InputFrame input;
    input.connected = true;
    for (const auto &k : keys)
        if (key == k.name)
        {
            input.pressed = action_bit(k.action);
            input.held = input.pressed;
            input.nav = k.nav;
        }
    return input;
}

void log_heap(std::uint64_t frames)
{
    std::size_t live = 0;
    std::size_t peak = 0;
    std::size_t blocks = 0;
    std::size_t failures = 0;
    hui_heap_stats(&live, &peak, &blocks, &failures);
    hui::sys::log("[HUI] heap frames=%llu live=%zu peak=%zu blocks=%zu failures=%zu",
                  static_cast<unsigned long long>(frames), live, peak, blocks, failures);
}

// Opens at *resolution (an index into Settings::kResolutions), falling back
// to 1080p; *resolution reports the mode that opened.
bool open_display(hui::ps5::Display &display, int *resolution)
{
    using hui::Settings;
    const Settings::Resolution &mode = Settings::kResolutions[*resolution];
    if (display.open(mode.width, mode.height))
    {
        hui::sys::log("[HUI] display mode %s %dx%d", mode.label, display.width(), display.height());
        return true;
    }
    if (*resolution == 0)
        return false;
    hui::sys::log("[HUI] display mode %s failed, using 1080p", mode.label);
    *resolution = 0;
    return display.open(Settings::kResolutions[0].width, Settings::kResolutions[0].height);
}

bool load_font(hui::gfx::Renderer &renderer, const char *name, hui::gfx::Font *font,
               hui::ui::FontRef *ref)
{
    std::string data;
    const std::string path = std::string(kAssets) + "/fonts/" + name;
    if (!hui::save::read_file(path, &data) || !font->load(data))
    {
        hui::sys::log("[HUI] font %s failed: %s", name, font->error().c_str());
        return false;
    }
    ref->font = font;
    ref->texture = renderer.batch().create_font_texture(*font);
    return true;
}

// The frame as drawn, bottom row first, as a 24-bit BMP (tour pictures).
bool save_picture(const std::string &path, int width, int height)
{
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    const std::uint32_t row = (static_cast<std::uint32_t>(width) * 3 + 3) & ~3u;
    const std::uint32_t size = 54 + row * static_cast<std::uint32_t>(height);
    unsigned char header[54] = {'B', 'M'};
    const auto put = [&](int at, std::uint32_t value)
    {
        for (int i = 0; i < 4; ++i)
            header[at + i] = static_cast<unsigned char>(value >> (8 * i));
    };
    put(2, size);
    put(10, 54);
    put(14, 40);
    put(18, static_cast<std::uint32_t>(width));
    put(22, static_cast<std::uint32_t>(height));
    header[26] = 1;
    header[28] = 24;
    put(34, size - 54);
    std::FILE *file = std::fopen(path.c_str(), "wb");
    if (file == nullptr)
        return false;
    bool ok = std::fwrite(header, 1, sizeof(header), file) == sizeof(header);
    std::vector<unsigned char> line(row);
    for (int y = 0; ok && y < height; ++y)
    {
        const unsigned char *in = pixels.data() + static_cast<std::size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x)
        {
            line[static_cast<std::size_t>(x) * 3 + 0] = in[x * 4 + 2];
            line[static_cast<std::size_t>(x) * 3 + 1] = in[x * 4 + 1];
            line[static_cast<std::size_t>(x) * 3 + 2] = in[x * 4 + 0];
        }
        ok = std::fwrite(line.data(), 1, line.size(), file) == line.size();
    }
    return std::fclose(file) == 0 && ok;
}

} // namespace

int main()
{
    using namespace hui;
    sys::log("[HUI] entry");
    sys::log("[HUI] storage dir=%d", save::ensure_directory(kDataRoot) ? 1 : 0);

    // The display opens at the saved resolution (1080p if that fails).
    const Settings saved = app::Shell::load_settings(kDataRoot);
    int resolution = ps5::Display::supports_display_modes() ? saved.resolution : 0;
    ps5::Display display;
    if (!open_display(display, &resolution))
    {
        sys::log("[HUI] fatal: display open failed");
        sys::park();
    }

    gfx::Renderer renderer;
    gfx::Font regular;
    gfx::Font semibold;
    gfx::Font display_font;
    gfx::Font mono;
    gfx::Font pixel;
    gfx::Font hand;
    ui::Fonts fonts;
    if (!renderer.init() ||
        !load_font(renderer, "nunito-sans-regular.huifont", &regular, &fonts.regular) ||
        !load_font(renderer, "nunito-sans-bold.huifont", &semibold, &fonts.semibold) ||
        !load_font(renderer, "nunito-sans-extrabold.huifont", &display_font, &fonts.display) ||
        !load_font(renderer, "dejavu-sans-mono.huifont", &mono, &fonts.mono) ||
        !load_font(renderer, "press-start-2p.huifont", &pixel, &fonts.pixel) ||
        !load_font(renderer, "patrick-hand.huifont", &hand, &fonts.hand))
    {
        sys::log("[HUI] fatal: renderer init failed");
        sys::park();
    }
    gfx::Shaper shaper;
    app::attach_text_engine(shaper, renderer, std::string(kAssets) + "/fonts",
                            {&regular, &semibold, &display_font, &mono});
    app::ArtSet art = app::load_art(renderer, std::string(kAssets) + "/art");
    // The account comes from the core payload over loopback (discord::Client);
    // a sample account stands in only for the scripted tour run.
    discord::Snapshot snapshot;
    discord::Client client;
    const int utc_offset = console_utc_minutes();
    // Discord images: asked for while drawing, downloaded by the client, uploaded here.
    // ponytail: small pictures are kept for the session; big ones are forgotten when their
    // screen closes (art.forget). Evict by use if the small ones ever add up.
    std::unordered_map<std::string, app::ArtSet::Remote> remote_textures; // texture 0: on its way or failed
    art.remote = [&](const std::string &url) -> app::ArtSet::Remote {
        const auto found = remote_textures.find(url);
        if (found != remote_textures.end())
            return found->second;
        remote_textures.emplace(url, app::ArtSet::Remote{});
        client.want_image(url);
        return {};
    };
    art.forget = [&](const std::string &url) {
        const auto found = remote_textures.find(url);
        if (found == remote_textures.end())
            return;
        renderer.batch().destroy_texture(found->second.texture);
        remote_textures.erase(found); // one still on its way is dropped when it lands
    };

    ps5::Pad pad;
    pad.open();
    InputTracker tracker;
    ps5::Ime ime;
    art.keyboard_open = [&](const std::string &title) { return ime.open(pad.user(), title, ""); };
    art.keyboard_poll = [&](std::string &out) {
        switch (ime.poll())
        {
        case ps5::Ime::State::done:
            out = ime.text();
            return 1;
        case ps5::Ime::State::cancelled:
            return 2;
        default:
            return 0;
        }
    };
    audio::Mixer mixer;
    // The music stream attaches to the mixer before the audio thread starts.
    audio::MusicPlayer music;
    const int tracks = music.init(mixer, std::string(kAssets) + "/audio/music",
                                  static_cast<std::uint64_t>(sys::monotonic_us()));
    sys::log("[HUI] music songs=%d", tracks);
    // Voice audio: the mic and the core's audio socket. Its playback is a
    // voice ring on the mixer, attached before the audio thread starts.
    ps5::VoiceAudio voice_audio;
    voice_audio.start(pad.user());
    mixer.attach_voice(&voice_audio.playback());
    ps5::AudioOut audio_out;
    audio_out.start(mixer);
    audio::SoundBank sounds;
    const auto bank = sounds.load(std::string(kAssets) + "/audio/sfx");
    sys::log("[HUI] sounds files=%d rejected=%d", bank.files, bank.rejected);
    for (const std::string &error : bank.errors)
        sys::log("[HUI] sound rejected %s", error.c_str());

    const std::function<void(const discord::Request &)> submit = [&client](const discord::Request &r) {
        client.submit(r);
    };
    app::Shell shell(fonts, snapshot, art, submit, kDataRoot, renderer.glass_texture());
    client.start();
    const std::string version = read_content_version("/app0/sce_sys/param.json");
    shell.set_version(version);
    sys::log("[HUI] version %s designs=%zu", version.empty() ? "unknown" : version.c_str(),
             shell.concept_count());

    // A tour request turns this launch into an unattended validation run:
    // every design is shown with scripted input, pictures and a report are
    // written to kDevRoot, and the app closes itself a while later.
    std::unique_ptr<app::Tour> tour;
    save::ensure_directory(kDevRoot);
    const std::string handled_path = std::string(kDevRoot) + "/handled.txt";
    const std::string report_path = std::string(kDevRoot) + "/tour/report.txt";
    bool quit_requested = false;
    std::string shot_name;               // a live screenshot is due
    std::deque<std::string> remote_keys; // "press" keys still to play
    double remote_wait = 0.0;
    int remote_hold = 0;         // frames the held remote key stays down
    bool remote_fast = false;    // "press fast,...": 0.1 s apart (bursts, for rate-limit tests)
    std::string remote_hold_key;
    std::string remote_typed; // "type" text, as a hardware keyboard would give it
    const auto poll_request = [&]()
    {
        std::string request;
        if (!save::read_file(kRequestPath, &request, 1024))
            return;
        char verb[16] = {};
        char argument[768] = {};
        char token[64] = {};
        if (std::sscanf(request.c_str(), "%15s %767s %63s", verb, argument, token) != 3)
            return;
        std::string handled;
        save::read_file(handled_path, &handled, 128);
        if (handled == token)
            return; // this request was honoured by an earlier launch (or frame)
        save::write_atomic(handled_path, token);
        if (std::strcmp(verb, "quit") == 0)
        {
            quit_requested = true;
        }
        else if (std::strcmp(verb, "notice") == 0)
        {
            std::string text = argument;
            std::replace(text.begin(), text.end(), '_', ' ');
            client.test_notice(text);
        }
        else if (std::strcmp(verb, "type") == 0)
        {
            // _ space, | Enter, < Backspace: the request line has no spaces.
            for (const char c : std::string(argument))
                remote_typed += c == '_' ? ' ' : c == '|' ? '\n' : c == '<' ? '\b' : c;
        }
        else if (std::strcmp(verb, "shot") == 0)
        {
            shot_name = argument;
        }
        else if (std::strcmp(verb, "press") == 0)
        {
            std::string keys = argument;
            for (std::size_t at = 0; at <= keys.size();)
            {
                const std::size_t comma = std::min(keys.find(',', at), keys.size());
                remote_keys.push_back(keys.substr(at, comma - at));
                at = comma + 1;
            }
            remote_fast = !remote_keys.empty() && remote_keys.front() == "fast";
            if (remote_fast)
                remote_keys.pop_front();
            sys::log("[HUI] press %s", argument);
        }
        else if (std::strcmp(verb, "tour") == 0 && !tour)
        {
            save::ensure_directory(std::string(kDevRoot) + "/tour");
            std::remove(report_path.c_str()); // a new report must not be taken for an old one
            const bool all = std::strcmp(argument, "all") == 0;
            tour = std::make_unique<app::Tour>(shell, all ? std::string() : std::string(argument));
            snapshot = discord::sample_snapshot();
            sys::log("[HUI] tour requested: %s token=%s", argument, token);
        }
    };
    poll_request();
    double collect_wait = -1.0; // seconds a finished tour has waited to be collected

    std::int64_t previous = sys::monotonic_us();
    std::uint64_t frames = 0;
    FrameStats stats;
    // The FPS readout averages over half a second so the number is readable.
    double fps_seconds = 0.0;
    int fps_frames = 0;
    PadSample samples[64];
    gfx::Canvas capture;
    std::int64_t last_frame_start = sys::monotonic_us();
    for (;;)
    {
        const std::int64_t now = sys::monotonic_us();
        // Animation time is start-to-start (one full frame), not the gap
        // between the previous swap returning and this frame beginning.
        float dt = frames == 0 ? 1.0f / 60.0f : static_cast<float>(now - last_frame_start) / 1e6f;
        last_frame_start = now;
        if (dt > 0.05f)
            dt = 0.05f; // a hitch must not teleport the animations
        const std::size_t count = pad.read(samples);
        InputFrame input = tracker.update(std::span<const PadSample>(samples, count),
                                          static_cast<std::uint64_t>(now));
        // A keyboard plugged into the console types and navigates (read by the core).
        if (!tour)
            for (const discord::KeyPress &k : client.take_keys())
                ps5::apply_key(input, {k.usage, k.mods, k.led});
        if (!remote_typed.empty())
        {
            // One key a frame, as typing would arrive.
            input.typed += remote_typed.front();
            if (remote_typed.front() == '\n')
                input.pressed |= action_bit(Action::confirm);
            remote_typed.erase(remote_typed.begin());
        }
        if (tour)
            input = tour->step(dt);
        else if (remote_hold > 0)
        {
            // "key~N": the key stays down for N frames after its press.
            input = remote_key(remote_hold_key);
            input.pressed = 0;
            input.nav = Direction::none;
            --remote_hold;
        }
        else if (!remote_keys.empty() && (remote_wait += dt) >= (remote_fast ? 0.1 : kPressSeconds))
        {
            std::string key = remote_keys.front();
            remote_keys.pop_front();
            if (const std::size_t tilde = key.find('~'); tilde != std::string::npos)
            {
                remote_hold = std::atoi(key.c_str() + tilde + 1);
                key.resize(tilde);
                remote_hold_key = key;
            }
            input = remote_key(key);
            remote_wait = 0.0;
        }

        if (!tour) // the tour runs on the sample account
        {
            // In front or not: the core shows the PS5's alert instead of our banner when not.
            if (frames % 30 == 0)
            {
                SystemStatus status{};
                if (sceSystemServiceGetStatus(&status) == 0)
                {
                    const bool front = !status.in_background && !status.ui_overlaid;
                    if (front != client.front())
                        sys::log("[HUI] in front %d (background %d, overlaid %d)", front, status.in_background,
                                 status.ui_overlaid);
                    client.set_front(front);
                }
            }
            client.take(snapshot);
            // The mic is open while connected and neither muted nor deafened (deafen mutes too,
            // as in Discord); playback while not deafened.
            const bool linked = snapshot.voice.state == discord::VoiceState::connected;
            const bool hearing = !snapshot.voice.deafened;
            voice_audio.set_wanted(linked, linked && hearing && !snapshot.voice.muted, linked && hearing);
            snapshot.voice.mic_level = voice_audio.mic_level();
        }
        snapshot.utc_offset_minutes = utc_offset;
        for (const discord::Image &image : client.take_images())
            if (const auto wanted = remote_textures.find(image.url); wanted != remote_textures.end() && image.w > 0)
                wanted->second = {renderer.batch().create_texture(image.w, image.h, image.rgba.data()), image.w,
                                  image.h, image.frames, image.columns, image.fps};
        shell.update(input, dt);
        if (shell.take_settings_changed())
        {
            const Settings &settings = shell.settings();
            mixer.set_bus_gain(audio::Bus::music, Settings::gain(settings.music_volume));
            mixer.set_bus_gain(audio::Bus::sfx, Settings::gain(settings.sfx_volume));
            mixer.set_bus_gain(audio::Bus::ui, Settings::gain(settings.ui_volume));
            InputSettings input_settings = tracker.settings();
            input_settings.swap_confirm = settings.swap_confirm;
            tracker.set_settings(input_settings);
        }
        const app::Feedback &feedback = shell.feedback();
        for (const audio::CueEvent &event : feedback.cues)
        {
            sounds.play(mixer, event.set == audio::SoundSet::count ? shell.sound_set() : event.set,
                        event);
            if (event.cue == audio::Cue::complete || event.cue == audio::Cue::welcome)
                music.duck();
        }
        if (shell.settings().haptics && feedback.rumble_strength > 0.0f)
            pad.rumble(feedback.rumble_strength, feedback.rumble_seconds);
        pad.tick(dt);
        if (shell.settings().light_bar)
        {
            const gfx::Color accent = shell.accent();
            pad.set_light_bar(static_cast<std::uint8_t>(accent.r * 255.0f),
                              static_cast<std::uint8_t>(accent.g * 255.0f),
                              static_cast<std::uint8_t>(accent.b * 255.0f));
        }
        music.pump(dt);

        shell.compose(renderer);
        shaper.upload(renderer.batch());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        renderer.present(0, display.width(), display.height());
        if (tour && !tour->capture().empty())
        {
            // The picture is the same frame drawn once more into a small
            // off-screen target: reading the display surface back is slow, and
            // a quarter-size picture is enough to compare with the PC render.
            if (capture.texture() == 0)
                capture.create(kCaptureWidth, kCaptureHeight, 1);
            capture.bind();
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            renderer.present(capture.framebuffer(), kCaptureWidth, kCaptureHeight);
            glBindFramebuffer(GL_FRAMEBUFFER, capture.framebuffer());
            const std::string path = std::string(kDevRoot) + "/tour/" + tour->capture() + ".bmp";
            const bool ok = save_picture(path, kCaptureWidth, kCaptureHeight);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            sys::log("[HUI] tour picture %s ok=%d", tour->capture().c_str(), ok ? 1 : 0);
            tour->capture_done();
            last_frame_start = sys::monotonic_us(); // saving is slow; the frame was not
        }
        if (!shot_name.empty())
        {
            gfx::Canvas shot;
            shot.create(kShotWidth, kShotHeight, 1);
            shot.bind();
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            renderer.present(shot.framebuffer(), kShotWidth, kShotHeight);
            glBindFramebuffer(GL_FRAMEBUFFER, shot.framebuffer());
            const bool ok = save_picture(std::string(kDevRoot) + "/" + shot_name + ".bmp", kShotWidth, kShotHeight);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            sys::log("[HUI] shot %s ok=%d", shot_name.c_str(), ok ? 1 : 0);
            shot_name.clear();
            last_frame_start = sys::monotonic_us();
        }
        if (!display.swap())
        {
            sys::log("[HUI] fatal: swap failed frame=%llu error=%s",
                     static_cast<unsigned long long>(frames),
                     ps5::egl_error_name(display.last_error()));
            sys::park();
        }
        ++frames;
        const std::int64_t presented = sys::monotonic_us();
        const float frame_ms = static_cast<float>(presented - previous) / 1000.0f;
        app::Telemetry &telemetry = shell.telemetry();
        if (frames == 1)
        {
            sys::log("[HUI] first-swap ok shapes=%zu draws=%zu", renderer.last_instances(),
                     renderer.last_draw_calls());
            const bool hidden = sys::hide_splash_screen();
            sys::log("[HUI] ready splash_hidden=%d", hidden ? 1 : 0);
            log_heap(frames);
        }
        else
        {
            stats.add(static_cast<double>(frame_ms));
            telemetry.push(frame_ms);
            if (tour)
                tour->record_frame(frame_ms, renderer.last_draw_calls(), renderer.last_instances());
        }
        previous = presented;
        telemetry.draw_calls = renderer.last_draw_calls();
        telemetry.instances = renderer.last_instances();
        telemetry.voices = mixer.active_voices();
        fps_seconds += static_cast<double>(frame_ms) / 1000.0;
        ++fps_frames;
        if (fps_seconds >= 0.5)
        {
            telemetry.fps = static_cast<float>(fps_frames / fps_seconds);
            telemetry.average_ms = static_cast<float>(fps_seconds * 1000.0 / fps_frames);
            fps_seconds = 0.0;
            fps_frames = 0;
        }

        if (tour && tour->finished())
        {
            std::string report;
            for (const std::string &line : tour->report())
            {
                sys::log("[HUI] %s", line.c_str());
                report += line + "\n";
            }
            save::write_atomic(report_path, report);
            sys::log("[HUI] tour finished designs=%zu", tour->report().size());
            // The storage is unmounted when the title closes, so the app stays
            // up for a while: the PC copies the evidence, then the app closes.
            tour.reset();
            collect_wait = 0.0;
        }
        if (collect_wait >= 0.0)
            collect_wait += static_cast<double>(frame_ms) / 1000.0;
        if (frames % 20 == 0)
        {
            poll_request();
            if (quit_requested || client.quit_wanted() || collect_wait > kCollectSeconds)
            {
                sys::log("[HUI] closing: %s",
                         quit_requested || client.quit_wanted() ? "quit requested" : "tour evidence window over");
                voice_audio.stop();
                pad.close();
                sys::quit();
            }
        }
        if (stats.count() == 600)
        {
            char summary[160];
            stats.format(summary, sizeof(summary));
            sys::log("[HUI] %s draws=%zu shapes=%zu", summary, renderer.last_draw_calls(),
                     renderer.last_instances());
            sys::log("[HUI] audio grains=%llu errors=%llu voices=%d music=%s underruns=%llu voice_underruns=%llu",
                     static_cast<unsigned long long>(audio_out.grains()),
                     static_cast<unsigned long long>(audio_out.errors()), mixer.active_voices(),
                     music.current().c_str(),
                     static_cast<unsigned long long>(mixer.stream_underruns()),
                     static_cast<unsigned long long>(mixer.voice_underruns()));
            stats.reset();
            if (frames % 3600 < 600)
                log_heap(frames);
        }
    }
}
