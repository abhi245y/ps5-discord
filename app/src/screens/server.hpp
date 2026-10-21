// ps5-discord - A server's own page: its banner, voice rooms and text channels.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "core/tween.hpp"
#include "screens/draw.hpp"

namespace screens
{

class Server
{
  public:
    explicit Server(const Ink &ink) : ink_(ink) {}

    void open(const std::string &guild_id);
    const std::string &guild() const
    {
        return guild_id_;
    }
    // Returns false when the screen asks to be left (Circle).
    bool update(const discord::Snapshot &snapshot, const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void draw(const discord::Snapshot &snapshot, DrawList &list) const;
    // The channel Cross picked during the last update, once.
    std::optional<std::string> take_open()
    {
        std::optional<std::string> out = std::move(open_);
        open_.reset();
        return out;
    }
    // The voice room Cross picked during the last update, once (the app joins it).
    std::optional<std::string> take_room()
    {
        std::optional<std::string> out = std::move(room_);
        room_.reset();
        return out;
    }

  private:
    enum Zone : int
    {
        kRooms,
        kChannels,
        kZones,
    };
    const discord::Guild *find(const discord::Snapshot &snapshot) const;
    // Text channels by latest activity: where people are talking comes first.
    std::vector<const discord::Channel *> channels(const discord::Guild &guild) const;
    // Voice rooms with people in them first.
    std::vector<const discord::Room *> rooms(const discord::Guild &guild) const;
    int count(const discord::Guild &guild, int zone) const;
    Rect item_rect(int zone, int index) const; // on screen, scroll applied

    const Ink &ink_;
    std::string guild_id_;
    int zone_ = kChannels;
    std::array<int, kZones> index_{};
    std::array<hui::tween::Spring, kZones> scroll_{};
    std::optional<std::string> open_;
    std::optional<std::string> room_;
};

} // namespace screens
