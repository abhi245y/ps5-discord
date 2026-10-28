// ps5-discord - The emoji picker, as Discord's but for a controller: a rail of
// categories on the left (recent, each server's own emoji, Unicode's groups), the grid
// with a heading per category, and the focused emoji named at the bottom.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

#include "core/input.hpp"
#include "discord/model.hpp"
#include "screens/draw.hpp"
#include "ui/feedback.hpp"

namespace screens
{

class EmojiPicker
{
  public:
    explicit EmojiPicker(const Ink &ink);

    // Where recently picked emoji are kept between runs (unset: this run only).
    static void set_store(const std::string &path);

    // guild_id: the server the chat is in ("" in a DM); without Nitro only its own
    // emoji can be sent there. title: "React" or "Add to message".
    void open(const discord::Snapshot &snapshot, const std::string &guild_id, const std::string &title);

    enum class Result
    {
        none,
        picked,
        cancel,
    };
    Result update(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void draw(DrawList &list) const;

    // The emoji picked: as message text (itself, or <:name:id>) and as a reaction
    // (itself, or name:id).
    std::string markup() const;
    std::string reaction() const;

  private:
    struct Item
    {
        std::string text; // a Unicode emoji; empty for a server's
        std::string id, name;
        bool animated = false;
        bool usable = true;
        const char *why = ""; // when not usable
    };
    struct Section
    {
        std::string title;
        Item icon; // the rail's picture
        std::vector<Item> items;
    };

    const Ink &ink_;
    std::string title_;
    std::vector<Section> sections_;
    int section_ = 0, index_ = 0;
    float scroll_ = 0.0f, scroll_target_ = 0.0f;
    Item picked_;

    float section_top(int section) const; // in the grid, scroll not applied
    const Item &focused() const;
    void remember(const Item &item);
};

} // namespace screens
