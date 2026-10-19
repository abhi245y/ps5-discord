// ps5-discord - A channel or DM: read history, act on a message, reply.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

#include "core/tween.hpp"
#include "discord/request.hpp"
#include "screens/draw.hpp"
#include "screens/keyboard.hpp"

namespace screens
{

class Chat
{
  public:
    explicit Chat(const Ink &ink) : ink_(ink), keys_(ink) {}

    // guild_id empty: a DM channel.
    void open(const discord::Snapshot &snapshot, const std::string &guild_id, const std::string &channel_id,
              std::vector<discord::Request> &requests);
    // Returns false when the screen asks to be left (Circle with nothing open).
    bool update(const discord::Snapshot &snapshot, const hui::InputFrame &input, float dt,
                hui::ui::Feedback &feedback, std::vector<discord::Request> &requests);
    void draw(const discord::Snapshot &snapshot, DrawList &list) const;

  private:
    struct Row // one laid-out message, page coordinates measured up from the composer
    {
        int index;    // into the channel's messages
        float bottom; // distance from the list's bottom edge to the row's bottom
        float height;
        bool grouped; // continues the previous author's run: no avatar or name
    };
    enum class Mode
    {
        browse,  // moving between messages
        actions, // the action menu of the focused message is open
        compose, // the composer with quick replies is open
        typing,  // the system keyboard is up over the app
        keys,    // the app's own keyboard is up
    };

    const std::vector<discord::Message> &messages(const discord::Snapshot &snapshot) const;
    std::vector<Row> layout(const discord::Snapshot &snapshot) const;
    float row_height(const discord::Snapshot &snapshot, int index, bool grouped) const;
    Rect row_rect(const Row &row) const; // on screen, scroll applied
    bool can_send(const discord::Snapshot &s) const;
    bool start_typing(const discord::Snapshot &s, hui::ui::Feedback &feedback);
    const discord::User *dm_person(const discord::Snapshot &s) const;
    bool dm() const
    {
        return guild_id_.empty();
    }
    std::vector<const discord::Channel *> chips(const discord::Snapshot &snapshot) const;

    void draw_header(const discord::Snapshot &snapshot, DrawList &list) const;
    void draw_message(const discord::Snapshot &snapshot, DrawList &list, const Row &row, const Rect &r,
                      bool focused) const;
    void draw_composer(const discord::Snapshot &snapshot, DrawList &list) const;
    void draw_actions(const discord::Snapshot &snapshot, DrawList &list, const Rect &anchor) const;

    const Ink &ink_;
    Keyboard keys_; // the app's own keyboard, when the system one is not there
    std::string guild_id_;
    std::string channel_id_;
    Mode mode_ = Mode::browse;
    int focus_ = -1;  // focused message index; -1 is the composer
    int action_ = 0;  // focused entry in the action menu (actions, then reactions)
    int quick_ = 0;   // focused quick reply in compose mode
    std::string reply_to_; // message id the composer answers
    hui::tween::Spring scroll_; // pixels the list is pushed down (history above revealed)
    hui::tween::Spring menu_show_;
};

} // namespace screens
