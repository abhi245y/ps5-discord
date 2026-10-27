// ps5-discord - The voice room (a server room or a DM call) and the incoming call.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

#include "discord/request.hpp"
#include "screens/draw.hpp"

namespace screens
{

// A voice room: who is in it, how the connection is, and the controls. Mute,
// Deafen and Leave act on the core; Share is drawn but not yet available.
class VoiceRoom
{
  public:
    explicit VoiceRoom(const Ink &ink) : ink_(ink) {}

    // The room to show: a server room or DM call, joined or being joined.
    void open(const std::string &channel_id);
    const std::string &channel() const
    {
        return channel_;
    }
    // Returns false when the screen is left (Circle, or Leave).
    bool update(const discord::Snapshot &snapshot, const hui::InputFrame &input, float dt,
                hui::ui::Feedback &feedback, std::vector<discord::Request> &requests);
    void draw(const discord::Snapshot &snapshot, DrawList &list) const;

  private:
    const Ink &ink_;
    std::string channel_;
    int focus_ = 0; // index into the controls: Mute, Deafen, Share (skipped), Leave
};

// The incoming call over whatever is on screen: the caller, Cross to answer, Circle to decline.
void draw_incoming(DrawList &list, const Ink &ink, const discord::Snapshot &snapshot, const discord::Call &call);

// The name of a room or DM call, for titles.
std::string room_title(const discord::Snapshot &snapshot, const std::string &channel_id);

} // namespace screens
