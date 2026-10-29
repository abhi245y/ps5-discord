// ps5-discord - The app, hosted by the kit's shell as its only design.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Owns the screen stack and routes input to the visible screen. Its tour()
// walks every screen so `make host-snapshots` photographs the whole app for
// comparison with the design framework's comps.

#include <algorithm>
#include <memory>
#include <optional>
#include <vector>

#include "app/concept.hpp"
#include "screens/draw.hpp"
#include "screens/chat.hpp"
#include "screens/home.hpp"
#include "screens/server.hpp"
#include "screens/signin.hpp"
#include "screens/style.hpp"
#include "screens/settings.hpp"
#include "screens/voice.hpp"

namespace screens
{

namespace
{

using hui::Action;
using hui::Direction;
using hui::action_bit;
namespace app = hui::app;

constexpr const char *kTechniques[] = {
    "TV Media Rows: rows you glide through, one coral focus",
    "Springs for focus and scrolling",
};

const app::ConceptInfo kInfo{
    "ps5-discord",
    "ps5-discord",
    "Discord on the TV",
    "src/screens/app.cpp",
    hui::audio::SoundSet::glass,
    style::kCoral,
    kTechniques,
};

// Screen names in captures follow the framework's frame numbers.
constexpr app::TourStep kTour[] = {
    {0.9f, 0, Direction::none, "home"},
    // Settings (frame 11): Options from Home, sign out, Voice & audio, pairing, quality,
    // notifications, Appearance.
    {0.4f, action_bit(Action::menu)},
    {0.8f, 0, Direction::none, "settings"},
    {0.4f, 0, Direction::right},
    {0.4f, action_bit(Action::confirm)},
    {0.6f, 0, Direction::none, "settings-signout"},
    {0.4f, action_bit(Action::back)},
    {0.4f, action_bit(Action::back)},
    {0.4f, 0, Direction::down},
    {0.4f, 0, Direction::right},
    {0.4f, 0, Direction::right},
    {0.6f, 0, Direction::none, "settings-voice"},
    {0.4f, 0, Direction::down},
    {0.4f, 0, Direction::down},
    {0.4f, action_bit(Action::confirm)},
    {0.6f, 0, Direction::none, "settings-pair-users"},
    {0.4f, action_bit(Action::confirm)},
    {1.8f, 0, Direction::none, "settings-pair"},
    {0.4f, action_bit(Action::confirm)},
    {0.4f, 0, Direction::down},
    {0.6f, 0, Direction::none, "settings-pair-other"},
    {0.4f, 0, Direction::up},
    {0.4f, action_bit(Action::confirm)},
    {0.6f, 0, Direction::none, "settings-pair-menu"},
    {0.4f, action_bit(Action::back)},
    {0.6f, 0, Direction::none, "settings-pair-users-paired"},
    {0.4f, action_bit(Action::back)},
    {0.4f, 0, Direction::down},
    {0.4f, 0, Direction::right},
    {0.6f, 0, Direction::none, "settings-quality"},
    {0.4f, action_bit(Action::confirm)},
    {0.6f, 0, Direction::none, "settings-paired"},
    {0.4f, action_bit(Action::back)},
    {0.4f, 0, Direction::down},
    {0.4f, 0, Direction::right},
    {0.4f, 0, Direction::down},
    {0.4f, action_bit(Action::confirm)},
    {0.6f, 0, Direction::none, "settings-notifications"},
    {0.4f, action_bit(Action::back)},
    {0.4f, 0, Direction::down},
    {0.4f, 0, Direction::right},
    {0.6f, 0, Direction::none, "settings-appearance"},
    {0.4f, action_bit(Action::back)},
    {0.4f, 0, Direction::down},
    {0.6f, 0, Direction::none, "settings-privacy"},
    {0.4f, 0, Direction::down},
    {0.6f, 0, Direction::none, "settings-about"},
    {0.4f, action_bit(Action::back)},
    {0.6f, 0, Direction::none, "home-after-settings"},
    {0.4f, action_bit(Action::back)},
    {0.6f, 0, Direction::none, "home-quit"},
    {0.4f, action_bit(Action::back)},
    {0.5f, 0, Direction::right},
    {0.6f, 0, Direction::right, "home-rooms-scroll"},
    {0.5f, 0, Direction::down},
    {0.6f, 0, Direction::none, "home-recent"},
    {0.5f, 0, Direction::up},
    {0.4f, 0, Direction::up},
    {0.6f, 0, Direction::none, "home-friends"},
    {0.4f, action_bit(Action::page_next)},
    {0.8f, 0, Direction::none, "tab-friends"},
    {0.4f, 0, Direction::down},
    {0.6f, 0, Direction::none, "tab-friends-scrolled"},
    {0.4f, action_bit(Action::page_next)},
    {0.8f, 0, Direction::none, "tab-servers"},
    // Night Shift (focused on the servers tab): joined to its raid room, the room, then back out.
    {0.4f, action_bit(Action::confirm)},
    {0.9f, 0, Direction::none, "voice-server"},
    {0.4f, 0, Direction::up},
    {0.6f, 0, Direction::none, "voice-server-room"},
    {0.4f, action_bit(Action::confirm)},
    {0.9f, 0, Direction::none, "voice-room"},
    {0.4f, action_bit(Action::back)},
    {0.4f, action_bit(Action::back)},
    {0.6f, 0, Direction::none, "tab-servers-back"},
    {0.4f, action_bit(Action::page_next)},
    {0.8f, 0, Direction::none, "tab-mentions"},
    {0.4f, action_bit(Action::page_next)},
    {0.6f, 0, Direction::none, "home-again"},
    // Into #general from Recent conversations.
    {0.4f, 0, Direction::down},
    {0.3f, 0, Direction::left},
    {0.3f, 0, Direction::left},
    {0.3f, 0, Direction::left},
    {0.5f, action_bit(Action::confirm)},
    {0.9f, 0, Direction::none, "chat"},
    // Ana's screenshot: its menu has View image first, which opens it full screen.
    {0.4f, 0, Direction::up},
    {0.4f, 0, Direction::up},
    {0.4f, action_bit(Action::confirm)},
    {0.6f, 0, Direction::none, "chat-image-actions"},
    {0.4f, action_bit(Action::confirm)},
    {0.8f, 0, Direction::none, "chat-image"},
    {0.4f, action_bit(Action::back)},
    {0.4f, 0, Direction::up},
    {0.7f, 0, Direction::none, "chat-focus"},
    {0.4f, action_bit(Action::confirm)},
    {0.7f, 0, Direction::none, "chat-actions"},
    // React opens the picker; R1 jumps a category; Circle comes back to the menu.
    {0.4f, 0, Direction::down},
    {0.4f, action_bit(Action::confirm)},
    {0.7f, 0, Direction::none, "chat-picker"},
    {0.4f, action_bit(Action::page_next)},
    {0.4f, 0, Direction::down},
    {0.7f, 0, Direction::none, "chat-picker-next"},
    {0.4f, action_bit(Action::back)},
    // L3: emoji into the message bar, two of them; Circle is done, R2 sends.
    {0.4f, action_bit(Action::back)},
    {0.4f, action_bit(Action::l3)},
    {0.4f, action_bit(Action::confirm)},
    {0.3f, 0, Direction::right},
    {0.4f, action_bit(Action::confirm)},
    {0.7f, 0, Direction::none, "chat-emoji"},
    {0.4f, action_bit(Action::back)},
    {0.6f, 0, Direction::none, "chat-draft"},
    {0.4f, action_bit(Action::jump_next)},
    {0.8f, 0, Direction::none, "chat-sent"},
    // The app's keyboard: R1 opens the picker, Square its stickers.
    {0.4f, action_bit(Action::north)},
    {0.4f, action_bit(Action::page_next)},
    {0.7f, 0, Direction::none, "chat-keys-emoji"},
    {0.4f, action_bit(Action::west)},
    {0.7f, 0, Direction::none, "chat-keys-stickers"},
    {0.4f, action_bit(Action::back)},
    {0.4f, action_bit(Action::back)},
    {0.5f, action_bit(Action::page_next)},
    {0.8f, 0, Direction::none, "chat-next-channel"},
    // Options: the channel list (the raid room's dock under it), then closed again.
    {0.4f, action_bit(Action::menu)},
    {0.7f, 0, Direction::none, "chat-channels"},
    {0.3f, 0, Direction::down},
    {0.3f, 0, Direction::down},
    {0.3f, 0, Direction::down},
    {0.6f, 0, Direction::none, "chat-channels-dock"},
    {0.4f, action_bit(Action::menu)},
    {0.5f, action_bit(Action::back)},
    // The DM with Theo.
    {0.5f, 0, Direction::right},
    {0.3f, 0, Direction::right},
    {0.5f, action_bit(Action::confirm)},
    {0.9f, 0, Direction::none, "dm"},
    // The reply with five reactions: its menu lists them to toggle.
    {0.4f, 0, Direction::up},
    {0.4f, 0, Direction::up},
    {0.4f, 0, Direction::up},
    {0.4f, action_bit(Action::confirm)},
    {0.7f, 0, Direction::none, "dm-actions"},
    {0.4f, action_bit(Action::back)},
    // Square calls Theo: the room rings them, then Circle goes back to the DM.
    {0.4f, action_bit(Action::west)},
    {0.9f, 0, Direction::none, "voice-dm-calling"},
    {0.4f, action_bit(Action::back)},
    {0.8f, 0, Direction::none, "voice-chat-dock"},
    {0.5f, action_bit(Action::back)},
    {0.6f, 0, Direction::none, "home-return"},
    // Theo rings in; Circle declines it.
    {0.9f, 0, Direction::none, "voice-incoming"},
    {0.4f, action_bit(Action::back)},
    {0.6f, 0, Direction::none, "home-declined"},
    // The connection fails: the pill on home, then the room, with the failure.
    {0.9f, 0, Direction::none, "voice-error"},
    {0.4f, action_bit(Action::west)},
    {0.9f, 0, Direction::none, "voice-error-room"},
    {0.4f, action_bit(Action::back)},
};

class DiscordApp final : public app::Concept
{
  public:
    explicit DiscordApp(app::Context &context)
        : context_(context), ink_{context.fonts, context.art}, home_(ink_), server_(ink_), chat_(ink_), voice_(ink_),
          settings_(ink_)
    {
    }

    const app::ConceptInfo &info() const override
    {
        return kInfo;
    }

    void enter() override
    {
        home_.enter();
    }

    void update(const hui::InputFrame &input, float dt, app::Feedback &feedback) override
    {
        time_ += dt;
        browser_cooldown_ = std::max(0.0f, browser_cooldown_ - dt);
        std::vector<discord::Request> requests;
        const discord::Call *ring = discord::ringing_call(context_.snapshot);
        if (!ring) // the ring is over: the next one, even for the same DM, is news again
        {
            dismissed_ring_.clear();
            cued_ring_.clear();
        }
        const bool reconnecting = context_.snapshot.connection == discord::Connection::reconnecting;
        if (reconnecting && route_ != Route::settings)
        {
            // The Reconnecting card: Left and Right pick Retry now or Settings.
            if ((input.nav == Direction::left && reconnect_focus_ == 1) ||
                (input.nav == Direction::right && reconnect_focus_ == 0))
            {
                reconnect_focus_ ^= 1;
                feedback.play(hui::audio::Cue::focus);
            }
            if (input.is_pressed(Action::confirm) && reconnect_focus_ == 1)
                open_settings(feedback);
            else if (input.is_pressed(Action::confirm) && browser_cooldown_ == 0.0f)
            {
                discord::Request r;
                r.kind = discord::Request::Kind::reconnect;
                requests.push_back(r);
                browser_cooldown_ = 3.0f;
                feedback.play(hui::audio::Cue::select);
            }
        }
        else if (context_.snapshot.connection != discord::Connection::ready && !reconnecting)
        {
            const auto &s = context_.snapshot;
            if (s.connection == discord::Connection::signing_in)
                route_ = Route::home; // signed out: the next account starts at Home
            // Cross starts browser login, or reopens the browser during one; Square cancels it.
            const bool open = input.is_pressed(Action::confirm) && !s.login_scanned;
            const bool cancel = input.is_pressed(Action::west) && s.login_browser;
            if (s.connection == discord::Connection::signing_in && (open || cancel) && browser_cooldown_ == 0.0f)
            {
                discord::Request request;
                request.kind = cancel ? discord::Request::Kind::browser_cancel : discord::Request::Kind::browser_login;
                requests.push_back(request);
                browser_cooldown_ = 3.0f;
                feedback.play(hui::audio::Cue::select);
            }
        }
        else if (update_voice_wait(input, dt, feedback, requests))
        {
        }
        else if (ring && ring->channel_id != dismissed_ring_)
        {
            // An incoming call takes the input: Cross answers, Circle declines.
            if (cued_ring_ != ring->channel_id)
            {
                cued_ring_ = ring->channel_id;
                feedback.play(hui::audio::Cue::notify);
            }
            if (input.is_pressed(Action::confirm))
            {
                discord::Request r;
                r.kind = discord::Request::Kind::voice_answer;
                r.channel_id = ring->channel_id;
                requests.push_back(r);
                dismissed_ring_ = ring->channel_id;
                await_voice(context_.snapshot, ring->channel_id);
                feedback.play(hui::audio::Cue::select);
            }
            else if (input.is_pressed(Action::back))
            {
                discord::Request r;
                r.kind = discord::Request::Kind::voice_decline;
                r.channel_id = ring->channel_id;
                requests.push_back(r);
                dismissed_ring_ = ring->channel_id;
                feedback.play(hui::audio::Cue::back);
            }
        }
        else if (route_ == Route::settings)
        {
            bool changed = false;
            if (!settings_.update(context_.snapshot, input, dt, feedback, requests, context_.settings, changed))
            {
                route_ = settings_from_;
                fade_.snap(0.0f);
            }
            context_.settings_changed |= changed;
        }
        else if (quitting_)
        {
            if (input.is_pressed(Action::confirm))
            {
                discord::Request r;
                r.kind = discord::Request::Kind::quit;
                requests.push_back(r);
                feedback.play(hui::audio::Cue::select);
                quitting_ = false;
            }
            else if (input.is_pressed(Action::back))
            {
                quitting_ = false;
                feedback.play(hui::audio::Cue::modal_close);
            }
        }
        else if (route_ == Route::home && input.is_pressed(Action::back)) // Circle on Home: quit?
        {
            quitting_ = true;
            feedback.play(hui::audio::Cue::modal_open);
        }
        else if (route_ == Route::home && input.is_pressed(Action::menu)) // Options: settings
            open_settings(feedback);
        else if (route_ == Route::home)
        {
            home_.update(context_.snapshot, input, dt, feedback);
            std::optional<HomeOpen> open = home_.take_open();
            if (open && open->join)
            {
                join_voice(context_.snapshot, open->guild_id, open->channel_id, requests);
                open.reset();
            }
            if (open && open->channel_id.empty() && !open->user_id.empty())
            {
                // No DM with them yet: ask for one and open it when it arrives.
                discord::Request r;
                r.kind = discord::Request::Kind::open_dm;
                r.user_id = open->user_id;
                requests.push_back(r);
                waiting_dm_ = open->user_id;
                open.reset();
            }
            if (!open && !waiting_dm_.empty())
                if (const auto dm = context_.snapshot.dm_by_user.find(waiting_dm_);
                    dm != context_.snapshot.dm_by_user.end())
                {
                    open = HomeOpen{"", dm->second, waiting_dm_};
                    waiting_dm_.clear();
                }
            if (open && open->channel_id.empty())
            {
                server_.open(open->guild_id);
                route_ = Route::server;
                fade_.snap(0.0f);
            }
            else if (open)
            {
                chat_.open(context_.snapshot, open->guild_id, open->channel_id, requests);
                route_ = Route::chat;
                from_server_ = false;
                fade_.snap(0.0f);
            }
        }
        else if (route_ == Route::server)
        {
            if (!server_.update(context_.snapshot, input, dt, feedback))
            {
                route_ = Route::home;
                fade_.snap(0.0f);
            }
            else if (const std::optional<std::string> channel = server_.take_open())
            {
                chat_.open(context_.snapshot, server_.guild(), *channel, requests);
                route_ = Route::chat;
                from_server_ = true;
                fade_.snap(0.0f);
            }
            else if (const std::optional<std::string> room = server_.take_room())
            {
                join_voice(context_.snapshot, server_.guild(), *room, requests);
            }
        }
        else if (route_ == Route::voice)
        {
            if (!voice_.update(context_.snapshot, input, dt, feedback, requests))
            {
                const bool left = std::any_of(requests.begin(), requests.end(), [](const discord::Request &r) {
                    return r.kind == discord::Request::Kind::voice_leave;
                });
                if (left) // stay until the call is over
                {
                    leaving_ = true;
                    voice_wait_ = 0.0f;
                }
                else
                {
                    route_ = voice_from_;
                    fade_.snap(0.0f);
                }
            }
            else if (voice_.take_settings()) // Go Live needs the pairing first
                open_settings(feedback);
        }
        else
        {
            const bool stay = chat_.update(context_.snapshot, input, dt, feedback, requests);
            if (chat_.take_voice()) // Square: call the DM, or open the room in use
            {
                const std::string channel = chat_.channel();
                const discord::Voice &v = context_.snapshot.voice;
                if (chat_.dm())
                {
                    if (!(v.active() && v.channel_id == channel))
                    {
                        discord::Request r;
                        r.kind = discord::Request::Kind::voice_call;
                        r.channel_id = channel;
                        requests.push_back(r);
                    }
                    await_voice(context_.snapshot, channel);
                    feedback.play(hui::audio::Cue::select);
                }
                else if (v.active())
                {
                    open_voice(v.channel_id);
                    feedback.play(hui::audio::Cue::open);
                }
            }
            else if (!stay)
            {
                route_ = from_server_ ? Route::server : Route::home; // Back returns where the chat was opened from
                fade_.snap(0.0f);
            }
        }
        // Leave from the chat's call dock too: the spinner until the call is over.
        if (joining_.empty() && !leaving_ &&
            std::any_of(requests.begin(), requests.end(),
                        [](const discord::Request &r) { return r.kind == discord::Request::Kind::voice_leave; }))
        {
            leaving_ = true;
            voice_wait_ = 0.0f;
        }
        const bool waiting = !joining_.empty() || leaving_;
        // Square on the home and server pages: back to the voice room you are in.
        if (!waiting && input.is_pressed(Action::west) && context_.snapshot.voice.active() &&
            (route_ == Route::home || route_ == Route::server) &&
            context_.snapshot.connection == discord::Connection::ready)
        {
            open_voice(context_.snapshot.voice.channel_id);
            feedback.play(hui::audio::Cue::open);
        }
        // Touchpad: the newest banner's chat.
        if (!waiting && input.is_pressed(Action::touch) && !banners_.empty() &&
            context_.snapshot.connection == discord::Connection::ready)
        {
            const discord::Notice n = banners_.front().notice;
            banners_.erase(banners_.begin());
            chat_.open(context_.snapshot, n.guild_id, n.channel_id, requests);
            route_ = Route::chat;
            from_server_ = false;
            fade_.snap(0.0f);
            feedback.play(hui::audio::Cue::open);
        }
        update_banners(dt, feedback);
        fade_.target = 1.0f;
        fade_.update(dt, 16.0f);
        for (const discord::Request &request : requests)
            context_.submit(request);
    }

    void draw(app::Frame &frame) const override
    {
        frame.backdrop.mode = hui::gfx::BackdropMode::none;
        // A short fade in on every screen change.
        frame.scene.rounded_rect({0, 0, 1920, 1080}, 0.0f, style::kGround);
        frame.scene.push_opacity(std::clamp(fade_.value, 0.0f, 1.0f));
        const bool reconnecting = context_.snapshot.connection == discord::Connection::reconnecting;
        if (context_.snapshot.connection == discord::Connection::signing_in)
            draw_sign_in(frame.scene, ink_, context_.snapshot, time_);
        else if (context_.snapshot.connection != discord::Connection::ready && !reconnecting)
            draw_waiting(frame.scene, ink_, context_.snapshot, time_);
        else if (route_ == Route::home)
            home_.draw(context_.snapshot, frame.scene);
        else if (route_ == Route::server)
            server_.draw(context_.snapshot, frame.scene);
        else if (route_ == Route::voice)
            voice_.draw(context_.snapshot, frame.scene);
        else if (route_ == Route::settings)
            settings_.draw(context_.snapshot, context_.settings, frame.scene);
        else
            chat_.draw(context_.snapshot, frame.scene);
        frame.scene.pop_opacity();
        if (reconnecting && route_ != Route::settings) // over the screen that was up, dimmed
            draw_reconnecting(frame.scene, ink_, context_.snapshot, time_, reconnect_focus_);
        if (const discord::Call *ring = discord::ringing_call(context_.snapshot);
            ring && ring->channel_id != dismissed_ring_)
            draw_incoming(frame.scene, ink_, context_.snapshot, *ring);
        if (quitting_)
            confirm_card(frame.scene, ink_, "Quit Discord?", kQuitText, "Quit");
        if (context_.snapshot.connection == discord::Connection::ready && (leaving_ || !joining_.empty()))
        {
            const std::string room = room_title(context_.snapshot, joining_);
            if (leaving_)
                busy_card(frame.scene, ink_, "Leaving the call", "", nullptr, time_);
            else if (!join_error_.empty())
                busy_card(frame.scene, ink_, "Couldn't join " + room, join_error_, "Close", time_);
            else
                busy_card(frame.scene, ink_, "Joining " + room, "", "Cancel", time_);
        }
        for (int i = static_cast<int>(banners_.size()) - 1; i >= 0; --i)
            draw_banner(frame.scene, banners_[i]);
    }

    std::span<const app::TourStep> tour() const override
    {
        return kTour;
    }

  private:
    enum class Route
    {
        home,
        server,
        chat,
        voice,
        settings,
    };
    app::Context &context_;
    Ink ink_;
    Home home_;
    Server server_;
    Chat chat_;
    VoiceRoom voice_;
    SettingsScreen settings_;
    Route route_ = Route::home;
    Route settings_from_ = Route::home; // the screen Circle returns to from settings
    Route voice_from_ = Route::home; // the screen Circle returns to from the voice room
    std::string dismissed_ring_;     // a ring answered or declined: hidden until it ends
    std::string cued_ring_;          // the ring whose chime has played
    bool from_server_ = false; // the open chat came from a server's page
    bool quitting_ = false;    // the Quit question is up (Circle on Home)
    float browser_cooldown_ = 0.0f;
    std::string waiting_dm_; // a friend whose new DM is being made, opened when it lands
    // A call being joined or left: a spinner until the core says it is done, so the voice room
    // opens only once connected and Leave goes back only once out of the call.
    std::string joining_;          // the room or DM call being joined
    std::string join_error_;       // why it failed, shown until Circle
    bool leaving_ = false;
    bool join_seen_ = false;       // the core showed this join under way (joining, connecting, securing)
    int reconnect_focus_ = 0;      // the Reconnecting card: 0 Retry now, 1 Settings
    float voice_wait_ = 0.0f;      // seconds since the join or leave was asked for
    // Banners at the top right, newest on top: DMs and mentions, and every server message
    // when that is on (Settings › Notifications). A message from a channel already shown
    // joins its banner (the count on the avatar) instead of stacking another.
    static constexpr float kBannerSeconds = 5.0f;
    static constexpr std::size_t kBannerMax = 4;
    struct Banner
    {
        discord::Notice notice;
        float time = 0.0f;
        float slot = -1.0f; // where it is drawn, easing to its place in the stack
        int count = 1;
    };
    std::vector<Banner> banners_;   // newest first
    std::uint64_t notice_seen_ = 0; // notices only come from live messages, so any new one is news

    void update_banners(float dt, app::Feedback &feedback)
    {
        for (const discord::Notice &n : context_.snapshot.notices)
        {
            if (n.seq <= notice_seen_)
                continue;
            notice_seen_ = n.seq;
            const bool here = route_ == Route::chat && chat_.channel() == n.channel_id;
            if (here || context_.snapshot.connection != discord::Connection::ready)
                continue;
            Banner b;
            b.notice = n;
            for (auto it = banners_.begin(); it != banners_.end(); ++it)
                if (it->notice.channel_id == n.channel_id)
                {
                    b.slot = it->slot;
                    b.count = it->count + 1;
                    b.notice.for_me = n.for_me || it->notice.for_me;
                    banners_.erase(it);
                    break;
                }
            banners_.insert(banners_.begin(), b);
            if (banners_.size() > kBannerMax)
                banners_.pop_back();
            // Each kind its own chime: a DM, a mention a little higher, server chatter low and
            // soft. None in the quiet hours.
            // ponytail: one chime per message; a busy server with every message on is noisy,
            // rate-limit the soft chime if that bites.
            if (!n.quiet)
                feedback.play(hui::audio::Cue::notify, n.kind == 3 ? 1.0f : n.kind == 2 ? 1.2f : 0.85f, 0.0f,
                              n.kind == 1 ? 0.35f : 1.0f);
        }
        const float ease = context_.settings.reduced_motion ? 1.0f : std::min(1.0f, dt * 14.0f);
        for (std::size_t i = 0; i < banners_.size(); ++i)
        {
            banners_[i].time += dt;
            banners_[i].slot += (static_cast<float>(i) - banners_[i].slot) * ease;
        }
        std::erase_if(banners_, [](const Banner &b) { return b.time >= kBannerSeconds; });
    }

    // Who wrote and where on the first line, the start of what on the second.
    void draw_banner(DrawList &list, const Banner &b) const
    {
        constexpr float kW = 660.0f, kH = 108.0f, kGap = 12.0f;
        const discord::Notice &n = b.notice;
        const float in = std::min(1.0f, b.time * 5.0f);
        const float out = std::min(1.0f, (kBannerSeconds - b.time) * 4.0f);
        const float t = std::clamp(std::min(in, out), 0.0f, 1.0f);
        const Rect r{1920.0f - style::kMargin - kW, 24.0f + b.slot * (kH + kGap), kW, kH};
        list.push_opacity(t);
        list.shadow(r, 22.0f, 40.0f, Color::rgb(0, 0.5f));
        list.rounded_rect(r, 22.0f, style::kRaisedHigh);
        list.rounded_rect({r.x, r.y + 18.0f, 4.0f, r.h - 36.0f}, 2.0f, n.for_me ? style::kCoral : style::kLine);
        avatar(list, ink_, n.author, r.x + 64.0f, r.cy(), 36.0f, false);
        if (b.count > 1)
            count_badge(list, ink_, b.count, r.x + 104.0f, r.cy() + 26.0f, 30.0f);
        const bool newest = &b == &banners_.front(); // the touchpad opens it
        const float text_x = r.x + 120.0f, text_w = r.w - 148.0f;
        const std::string title = fit(ink_.fonts.semibold, n.title, 25.0f, text_w * 0.5f);
        hui::ui::text(list, ink_.fonts.semibold, title, text_x, r.y + 46.0f, 25.0f, style::kText);
        const float title_w = ink_.fonts.semibold.measure(title, 25.0f) + 20.0f;
        const std::string place = n.place.empty() ? "Direct message" : n.place;
        hui::ui::text(list, ink_.fonts.regular, fit(ink_.fonts.regular, place, 21.0f, text_w - title_w),
                      r.x + r.w - 28.0f, r.y + 45.0f, 21.0f, style::kFaint, hui::gfx::Align::right);
        hui::ui::text(list, ink_.fonts.regular, fit(ink_.fonts.regular, n.text, 23.0f, text_w - (newest ? 110.0f : 0.0f)),
                      text_x, r.y + 82.0f, 23.0f, style::kDim);
        if (newest)
        {
            const hui::ui::Hint open[] = {{hui::ui::Button::touchpad, "Open"}};
            hui::ui::HintLayout layout;
            layout.size = 26.0f;
            layout.text_size = 19.0f;
            layout.cy = r.y + 75.0f;
            hui::ui::draw_hints(list, ink_.fonts, hint_glyphs(), open, 1, r.x + r.w - 28.0f, true, layout);
        }
        list.pop_opacity();
    }
    // Settings, reached from the current screen (Circle returns there).
    void open_settings(app::Feedback &feedback)
    {
        settings_.open();
        if (route_ != Route::settings)
            settings_from_ = route_;
        route_ = Route::settings;
        fade_.snap(0.0f);
        feedback.play(hui::audio::Cue::open);
    }
    // The voice room for channel, reached from the current screen (Circle returns there).
    void open_voice(const std::string &channel)
    {
        voice_.open(channel);
        if (route_ != Route::voice)
            voice_from_ = route_;
        route_ = Route::voice;
        fade_.snap(0.0f);
    }
    // The voice room for channel once we are connected to it; a spinner until then.
    void await_voice(const discord::Snapshot &s, const std::string &channel)
    {
        if (s.voice.state == discord::VoiceState::connected && s.voice.channel_id == channel)
        {
            open_voice(channel);
            return;
        }
        joining_ = channel;
        join_error_.clear();
        join_seen_ = false;
        leaving_ = false;
        voice_wait_ = 0.0f;
    }
    // Joins a server's voice room unless it is the one we are in, then opens its screen.
    void join_voice(const discord::Snapshot &s, const std::string &guild, const std::string &channel,
                    std::vector<discord::Request> &requests)
    {
        if (!(s.voice.active() && s.voice.channel_id == channel))
        {
            discord::Request r;
            r.kind = discord::Request::Kind::voice_join;
            r.guild_id = guild;
            r.channel_id = channel;
            requests.push_back(r);
        }
        await_voice(s, channel);
    }
    // While a join or leave is under way it has the input. Returns false when nothing is.
    bool update_voice_wait(const hui::InputFrame &input, float dt, app::Feedback &feedback,
                           std::vector<discord::Request> &requests)
    {
        if (joining_.empty() && !leaving_)
            return false;
        const discord::Voice &v = context_.snapshot.voice;
        voice_wait_ += dt;
        // The core's answer lands on a later voice poll, which waits behind whatever the client
        // is fetching (a DM's history): until the core shows this join under way, the state on
        // screen is the old one and says nothing about it.
        const bool under_way = v.state == discord::VoiceState::joining ||
                               v.state == discord::VoiceState::connecting || v.state == discord::VoiceState::securing;
        join_seen_ |= under_way;
        const bool settled = voice_wait_ > 1.5f;
        if (!joining_.empty() && v.state == discord::VoiceState::connected && v.channel_id == joining_)
        {
            open_voice(joining_); // connected, even after a failure was shown: the call is up
            joining_.clear();
            join_error_.clear();
            feedback.play(hui::audio::Cue::open);
        }
        else if (!join_error_.empty())
        {
            if (input.is_pressed(Action::back) || input.is_pressed(Action::confirm))
            {
                joining_.clear();
                join_error_.clear();
                feedback.play(hui::audio::Cue::modal_close);
            }
        }
        else if (!joining_.empty())
        {
            if (join_seen_ && v.state == discord::VoiceState::connected && v.channel_id != joining_)
            {
                // A switch Discord refused: the core stayed in the call we were in.
                join_error_ = "Discord did not confirm that channel. You are still in " +
                              room_title(context_.snapshot, v.channel_id) + ".";
                feedback.play(hui::audio::Cue::error);
            }
            else if ((v.state == discord::VoiceState::error && (join_seen_ || voice_wait_ > 8.0f)) ||
                     (v.state == discord::VoiceState::idle && join_seen_) || voice_wait_ > 30.0f)
            {
                join_error_ = v.state == discord::VoiceState::error && !v.error.empty()
                                  ? v.error
                                  : "The call did not connect. Try again in a moment.";
                if (v.active() && v.channel_id == joining_) // timed out half-way: do not leave it hanging
                {
                    discord::Request r;
                    r.kind = discord::Request::Kind::voice_leave;
                    requests.push_back(r);
                }
                feedback.play(hui::audio::Cue::error);
            }
            else if (input.is_pressed(Action::back)) // changed our mind: leave what was started
            {
                discord::Request r;
                r.kind = discord::Request::Kind::voice_leave;
                requests.push_back(r);
                joining_.clear();
                leaving_ = true;
                voice_wait_ = 0.0f;
                feedback.play(hui::audio::Cue::back);
            }
        }
        else if ((settled && !v.active()) || voice_wait_ > 10.0f)
        {
            leaving_ = false;
            if (route_ == Route::voice)
            {
                route_ = voice_from_;
                fade_.snap(0.0f);
            }
        }
        return true;
    }
    hui::tween::Spring fade_;
    float time_ = 0.0f;
};

} // namespace

} // namespace screens

namespace hui::app
{

namespace
{
std::unique_ptr<Concept> make_app(Context &context)
{
    return std::make_unique<screens::DiscordApp>(context);
}
constexpr ConceptFactory kRegistry[] = {make_app};
} // namespace

std::span<const ConceptFactory> concept_registry()
{
    return kRegistry;
}

} // namespace hui::app
