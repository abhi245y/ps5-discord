// ps5-discord - A channel or DM: read history, act on a message, reply.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "core/tween.hpp"
#include "discord/request.hpp"
#include "screens/draw.hpp"
#include "screens/emoji_picker.hpp"
#include "screens/keyboard.hpp"

namespace screens
{

class Chat
{
  public:
    explicit Chat(const Ink &ink);

    // guild_id empty: a DM channel.
    const std::string &channel() const
    {
        return channel_id_;
    }
    bool dm() const // a DM channel
    {
        return guild_id_.empty();
    }
    void open(const discord::Snapshot &snapshot, const std::string &guild_id, const std::string &channel_id,
              std::vector<discord::Request> &requests);
    // Returns false when the screen asks to be left (Circle with nothing open).
    bool update(const discord::Snapshot &snapshot, const hui::InputFrame &input, float dt,
                hui::ui::Feedback &feedback, std::vector<discord::Request> &requests);
    void draw(const discord::Snapshot &snapshot, DrawList &list) const;
    // Square (or Cross on the DM's profile) asked for voice: once, then false.
    bool take_voice()
    {
        const bool out = voice_;
        voice_ = false;
        return out;
    }

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
        typing,  // the system keyboard is up over the app
        keys,    // the app's own keyboard is up
        channels, // the server's channel list is open (Options)
        profile,  // the DM person's card is open (Options in a DM)
        image,    // the focused message's pictures, full screen
        emoji,    // the emoji picker: to react, or into the keyboard's text
    };

    const std::vector<discord::Message> &messages(const discord::Snapshot &snapshot) const;
    std::vector<Row> layout(const discord::Snapshot &snapshot) const;
    float row_height(const discord::Snapshot &snapshot, int index, bool grouped) const;
    Rect row_rect(const Row &row) const; // on screen, scroll applied
    std::pair<float, float> focus_range(const Row &row) const; // scroll values that keep the row in view
    int reaction_lines(const discord::Message &m, float width) const;
    // A message's text wrapped to width, room left on each line for the mention pill when
    // it opens with one; drawing and measuring use the same lines.
    std::vector<std::string> text_lines(const discord::Snapshot &s, const discord::Message &m, float width) const;
    float pill_extra(const discord::Snapshot &s) const; // the pill's width beyond the plain @name
    bool edit_tag_wraps(const discord::Snapshot &s, const discord::Message &m, float width) const;
    // A DM bubble's inside width: its widest line, reply quote, reactions row or card.
    float bubble_fit(const discord::Snapshot &s, const discord::Message &m, float width) const;
    float reaction_width(const discord::Reaction &r) const;
    // One wrapped line of a message, server emoji as pictures; emoji counts them across lines.
    float draw_line(DrawList &list, const discord::Message &m, const std::string &line, float x, float baseline,
                    std::size_t &emoji) const;
    bool can_send(const discord::Snapshot &s) const;
    bool start_typing(const discord::Snapshot &s, hui::ui::Feedback &feedback);
    const discord::User *dm_person(const discord::Snapshot &s) const;
    std::vector<const discord::Channel *> chips(const discord::Snapshot &snapshot) const;

    void draw_header(const discord::Snapshot &snapshot, DrawList &list) const;
    void draw_message(const discord::Snapshot &snapshot, DrawList &list, const Row &row, const Rect &r,
                      bool focused) const;
    void draw_composer(const discord::Snapshot &snapshot, DrawList &list) const;
    void draw_actions(const discord::Snapshot &snapshot, DrawList &list, const Rect &anchor) const;
    void draw_image(const discord::Snapshot &snapshot, DrawList &list) const;

    const Ink &ink_;
    Keyboard keys_; // the app's own keyboard, when the system one is not there
    EmojiPicker picker_;
    // What a pick does: react to the focused message, or go into the text (the app
    // keyboard's, or the message bar's from L3).
    enum class PickFor
    {
        react,
        keys,
        draft,
    };
    PickFor pick_for_ = PickFor::react;
    void open_picker(const discord::Snapshot &s, PickFor pick_for, std::vector<discord::Request> &requests);

    int list_index_ = 0;        // focused row of the channel list
    hui::tween::Spring list_scroll_;
    void draw_channel_list(const discord::Snapshot &s, DrawList &list) const;
    float left() const; // the chat's left edge: past the channel list in a server
    void follow_list(const discord::Snapshot &s, float dt);
    std::string guild_id_;
    std::string channel_id_;
    Mode mode_ = Mode::browse;
    int focus_ = -1;  // focused message index; -1 is the composer
    std::string front_id_;        // oldest loaded message last frame: an older page shifts focus
    std::string editing_id_;      // the own message the keyboard is rewriting
    std::string draft_before_edit_;
    bool delete_armed_ = false;   // Delete pressed once: the next press deletes
    float size_note_ = 0.0f;      // seconds the text-size note stays up
    std::string older_asked_for_; // the oldest id an older page was requested above
    int action_ = 0;  // focused entry in the action menu (actions, then reactions)
    int image_at_ = 0; // the picture shown full screen, among the message's
    std::string reply_to_; // message id the composer answers
    hui::tween::Spring scroll_; // pixels the list is pushed down (history above revealed)
    hui::tween::Spring menu_show_;
    bool voice_ = false; // see take_voice
};

} // namespace screens
