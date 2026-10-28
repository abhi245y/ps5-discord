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
    // Settings (frame 11): Options from Home, Voice & audio, the pairing PIN, Appearance.
    {0.4f, action_bit(Action::menu)},
    {0.8f, 0, Direction::none, "settings"},
    {0.4f, 0, Direction::down},
    {0.6f, 0, Direction::none, "settings-voice"},
    {0.4f, 0, Direction::right},
    {0.4f, action_bit(Action::confirm)},
    {0.6f, 0, Direction::none, "settings-pair-users"},
    {0.4f, action_bit(Action::confirm)},
    {1.8f, 0, Direction::none, "settings-pair"},
    {0.4f, action_bit(Action::confirm)},
    {0.4f, 0, Direction::down},
    {0.4f, action_bit(Action::confirm)},
    {1.8f, 0, Direction::none, "settings-pair-second"},
    {0.4f, action_bit(Action::confirm)},
    {0.4f, action_bit(Action::confirm)},
    {0.6f, 0, Direction::none, "settings-pair-menu"},
    {0.4f, action_bit(Action::back)},
    {0.6f, 0, Direction::none, "settings-pair-users-paired"},
    {0.4f, action_bit(Action::back)},
    {0.6f, 0, Direction::none, "settings-paired"},
    {0.4f, action_bit(Action::back)},
    {0.4f, 0, Direction::down},
    {0.4f, 0, Direction::down},
    {0.4f, 0, Direction::right},
    {0.6f, 0, Direction::none, "settings-appearance"},
    {0.4f, action_bit(Action::back)},
    {0.4f, action_bit(Action::back)},
    {0.6f, 0, Direction::none, "home-after-settings"},
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
    {0.4f, 0, Direction::up},
    {0.4f, 0, Direction::up},
    {0.4f, 0, Direction::up},
    {0.7f, 0, Direction::none, "chat-focus"},
    {0.4f, action_bit(Action::confirm)},
    {0.7f, 0, Direction::none, "chat-actions"},
    {0.4f, action_bit(Action::confirm)},
    {0.6f, 0, Direction::right},
    {0.7f, 0, Direction::none, "chat-compose"},
    {0.4f, action_bit(Action::confirm)},
    {0.8f, 0, Direction::none, "chat-sent"},
    {0.5f, action_bit(Action::page_next)},
    {0.8f, 0, Direction::none, "chat-next-channel"},
    {0.5f, action_bit(Action::back)},
    // The DM with Theo.
    {0.5f, 0, Direction::right},
    {0.3f, 0, Direction::right},
    {0.5f, action_bit(Action::confirm)},
    {0.9f, 0, Direction::none, "dm"},
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
        if (context_.snapshot.connection != discord::Connection::ready)
        {
            const auto &s = context_.snapshot;
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
                open_voice(ring->channel_id);
                feedback.play(hui::audio::Cue::open);
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
                route_ = voice_from_;
                fade_.snap(0.0f);
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
                    open_voice(channel);
                    feedback.play(hui::audio::Cue::open);
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
        // Square on the home and server pages: back to the voice room you are in.
        if (input.is_pressed(Action::west) && context_.snapshot.voice.active() &&
            (route_ == Route::home || route_ == Route::server) &&
            context_.snapshot.connection == discord::Connection::ready)
        {
            open_voice(context_.snapshot.voice.channel_id);
            feedback.play(hui::audio::Cue::open);
        }
        // A new live DM or mention: a toast and a chime, unless it is in the open chat.
        const discord::Notice &n = context_.snapshot.notice;
        if (n.seq != notice_seen_)
        {
            const bool here = route_ == Route::chat && chat_.channel() == n.channel_id;
            if (!here && context_.snapshot.connection == discord::Connection::ready)
            {
                toast_ = n;
                toast_time_ = 0.0f;
                feedback.play(hui::audio::Cue::notify);
            }
            notice_seen_ = n.seq;
        }
        toast_time_ += dt;
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
        if (context_.snapshot.connection == discord::Connection::signing_in)
            draw_sign_in(frame.scene, ink_, context_.snapshot, time_);
        else if (context_.snapshot.connection != discord::Connection::ready)
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
        if (const discord::Call *ring = discord::ringing_call(context_.snapshot);
            ring && ring->channel_id != dismissed_ring_)
            draw_incoming(frame.scene, ink_, context_.snapshot, *ring);
        if (toast_.seq != 0 && toast_time_ < kToastSeconds)
            draw_toast(frame.scene);
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
    float browser_cooldown_ = 0.0f;
    std::string waiting_dm_; // a friend whose new DM is being made, opened when it lands
    static constexpr float kToastSeconds = 5.0f;
    std::uint64_t notice_seen_ = 0; // notices only come from live messages, so any new one is news
    discord::Notice toast_;
    float toast_time_ = 0.0f;

    // Top right, sliding in and out: who wrote, and the start of what.
    void draw_toast(DrawList &list) const
    {
        const float in = std::min(1.0f, toast_time_ * 5.0f);
        const float out = std::min(1.0f, (kToastSeconds - toast_time_) * 4.0f);
        const float t = std::clamp(std::min(in, out), 0.0f, 1.0f);
        const Rect r{1920.0f - style::kMargin - 620.0f, 24.0f - (1.0f - t) * 140.0f, 620.0f, 108.0f};
        list.push_opacity(t);
        list.shadow(r, 22.0f, 40.0f, Color::rgb(0, 0.5f));
        list.rounded_rect(r, 22.0f, style::kRaisedHigh);
        list.rounded_rect({r.x, r.y + 18.0f, 4.0f, r.h - 36.0f}, 2.0f, style::kCoral);
        avatar(list, ink_, toast_.author, r.x + 64.0f, r.cy(), 36.0f, false);
        hui::ui::text(list, ink_.fonts.semibold, fit(ink_.fonts.semibold, toast_.title, 25.0f, r.w - 140.0f),
                      r.x + 120.0f, r.y + 46.0f, 25.0f, style::kText);
        hui::ui::text(list, ink_.fonts.regular, fit(ink_.fonts.regular, toast_.text, 23.0f, r.w - 140.0f),
                      r.x + 120.0f, r.y + 82.0f, 23.0f, style::kDim);
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
        open_voice(channel);
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
