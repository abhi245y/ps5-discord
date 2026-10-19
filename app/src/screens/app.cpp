// ps5-discord - The app, hosted by the kit's shell as its only design.
// SPDX-License-Identifier: GPL-3.0-or-later
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
#include "screens/signin.hpp"
#include "screens/style.hpp"

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
    {0.5f, action_bit(Action::back)},
    {0.6f, 0, Direction::none, "home-return"},
};

class DiscordApp final : public app::Concept
{
  public:
    explicit DiscordApp(app::Context &context)
        : context_(context), ink_{context.fonts, context.art}, home_(ink_), chat_(ink_)
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
        std::vector<discord::Request> requests;
        if (context_.snapshot.connection != discord::Connection::ready)
        {
            // Sign-in and waiting screens take no input beyond the system's.
        }
        else if (route_ == Route::home)
        {
            home_.update(context_.snapshot, input, dt, feedback);
            std::optional<HomeOpen> open = home_.take_open();
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
            if (open)
            {
                chat_.open(context_.snapshot, open->guild_id, open->channel_id, requests);
                route_ = Route::chat;
                fade_.snap(0.0f);
            }
        }
        else if (!chat_.update(context_.snapshot, input, dt, feedback, requests))
        {
            route_ = Route::home;
            fade_.snap(0.0f);
        }
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
        else
            chat_.draw(context_.snapshot, frame.scene);
        frame.scene.pop_opacity();
    }

    std::span<const app::TourStep> tour() const override
    {
        return kTour;
    }

  private:
    enum class Route
    {
        home,
        chat,
    };
    app::Context &context_;
    Ink ink_;
    Home home_;
    Chat chat_;
    Route route_ = Route::home;
    std::string waiting_dm_; // a friend whose new DM is being made, opened when it lands
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
