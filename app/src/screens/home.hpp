// ps5-discord - Home: who is around, what is live, what was said.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <vector>

#include "app/concept.hpp"
#include "core/tween.hpp"
#include "screens/draw.hpp"

namespace screens
{

enum class HomeTab : int
{
    all,
    friends,
    servers,
    mentions,
    count,
};

class Home
{
  public:
    explicit Home(const Ink &ink) : ink_(ink) {}

    void enter();
    void update(const discord::Snapshot &snapshot, const hui::InputFrame &input, float dt,
                hui::ui::Feedback &feedback);
    void draw(const discord::Snapshot &snapshot, DrawList &list) const;

    HomeTab tab() const
    {
        return tab_;
    }

  private:
    enum Zone : int
    {
        kChips,
        kFriends,
        kRooms,
        kRecent,
        kZones,
    };

    struct Glide // a rectangle that springs toward its target
    {
        hui::tween::Spring x, y, w, h;
        void to(const Rect &r, bool snap);
        void update(float dt);
        Rect rect() const
        {
            return {x.value, y.value, w.value, h.value};
        }
    };

    int row_count(const discord::Snapshot &snapshot, int zone) const;
    Rect item_rect(const discord::Snapshot &snapshot, int zone, int index) const;
    void move(const discord::Snapshot &snapshot, int dx, int dy, hui::ui::Feedback &feedback);

    void draw_header(const discord::Snapshot &snapshot, DrawList &list) const;
    void draw_friends(const discord::Snapshot &snapshot, DrawList &list) const;
    void draw_rooms(const discord::Snapshot &snapshot, DrawList &list) const;
    void draw_recent(const discord::Snapshot &snapshot, DrawList &list) const;

    // Friends, Servers and Mentions tabs (home_tabs.cpp): a page of items in
    // page coordinates, focus moving to the nearest item in the pressed direction.
    struct TabItem
    {
        Rect rect; // before vertical scroll
        int ref;   // index into the snapshot's friends, guilds or mentions
    };
    std::vector<TabItem> layout_tab(const discord::Snapshot &snapshot) const;
    void update_tab(const discord::Snapshot &snapshot, const hui::InputFrame &input, float dt,
                    hui::ui::Feedback &feedback);
    void draw_tab(const discord::Snapshot &snapshot, DrawList &list) const;
    Rect tab_focus_rect(const discord::Snapshot &snapshot) const;

    const Ink &ink_;
    HomeTab tab_ = HomeTab::all;
    int zone_ = kRooms; // the live rooms row is the screen's dominant element
    std::array<int, kZones> index_{};
    std::array<hui::tween::Spring, kZones> scroll_{}; // horizontal offset per row
    Glide focus_;
    bool placed_ = false;
    std::array<int, static_cast<int>(HomeTab::count)> tab_focus_{};
    std::array<hui::tween::Spring, static_cast<int>(HomeTab::count)> tab_scroll_{};
};

} // namespace screens
