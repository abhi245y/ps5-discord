// ps5-discord - The app, hosted by the kit's shell as its only design.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Owns the screen stack and routes input to the visible screen. Its tour()
// walks every screen so `make host-snapshots` photographs the whole app for
// comparison with the design framework's comps.

#include <memory>

#include "app/concept.hpp"
#include "screens/draw.hpp"
#include "screens/home.hpp"
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
};

class DiscordApp final : public app::Concept
{
  public:
    explicit DiscordApp(app::Context &context)
        : context_(context), ink_{context.fonts, context.art}, home_(ink_)
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
        home_.update(context_.snapshot, input, dt, feedback);
    }

    void draw(app::Frame &frame) const override
    {
        frame.backdrop.mode = hui::gfx::BackdropMode::none;
        home_.draw(context_.snapshot, frame.scene);
    }

    std::span<const app::TourStep> tour() const override
    {
        return kTour;
    }

  private:
    app::Context &context_;
    Ink ink_;
    Home home_;
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
