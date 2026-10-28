// ps5-discord - The emoji (and sticker) picker, as Discord's but for a controller: a
// rail of categories on the left (recent, each server's own, Unicode's groups or
// Discord's sticker packs), the grid with a heading per category, and the focused one
// named at the bottom.
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
    // emoji and stickers can be sent there. title: "React" or "Add to message".
    // stickers: Square switches to the stickers (when writing a message). adds: emoji go
    // into text and the picker stays open for more (Circle is Done).
    void open(const discord::Snapshot &snapshot, const std::string &guild_id, const std::string &title,
              bool stickers, bool adds);
    // Frees the sticker previews it loaded (the picker has closed).
    void close();

    enum class Result
    {
        none,
        picked,  // an emoji: markup() / reaction()
        sticker, // a sticker: sticker()
        cancel,
    };
    // The snapshot: Discord's sticker packs arrive after the picker opens.
    Result update(const discord::Snapshot &snapshot, const hui::InputFrame &input, float dt,
                  hui::ui::Feedback &feedback);
    void draw(DrawList &list) const;

    // The emoji picked: as message text (itself, or <:name:id>) and as a reaction
    // (itself, or name:id).
    std::string markup() const;
    std::string reaction() const;
    const discord::Sticker &sticker() const
    {
        return picked_sticker_;
    }

  private:
    struct Item
    {
        std::string text; // a Unicode emoji; empty for a server's emoji or a sticker
        std::string id, name;
        bool animated = false;
        int format = 0; // a sticker's (discord::Sticker), 0 for an emoji
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
    std::string title_, guild_id_;
    bool nitro_ = false;
    bool stickers_allowed_ = false;
    bool adds_ = false;
    bool on_stickers_ = false;
    std::size_t packs_seen_ = 0;
    std::vector<Section> sections_;
    int section_ = 0, index_ = 0;
    float scroll_ = 0.0f, scroll_target_ = 0.0f;
    Item picked_;
    discord::Sticker picked_sticker_;
    mutable std::vector<std::string> previews_; // sticker preview pictures asked for

    void build(const discord::Snapshot &snapshot);
    int columns() const;
    float cell() const;
    float section_top(int section) const; // in the grid, scroll not applied
    const Item &focused() const;
    void remember(const Item &item);
    void draw_item(DrawList &list, const Item &item, const Rect &r) const;
};

} // namespace screens
