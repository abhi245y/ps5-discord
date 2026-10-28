// ps5-discord - Settings (framework frame 11): sections on the left, the chosen
// section's rows on the right, and the Remote Play pairing dialog for Go Live.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

#include "core/settings.hpp"
#include "discord/request.hpp"
#include "screens/draw.hpp"

namespace screens
{

class SettingsScreen
{
  public:
    explicit SettingsScreen(const Ink &ink);

    void open();
    // Returns false when the screen is left (Circle on the sections). app_settings are the
    // app's saved settings (volumes, motion, buttons); changed is set when one changes.
    bool update(const discord::Snapshot &snapshot, const hui::InputFrame &input, float dt,
                hui::ui::Feedback &feedback, std::vector<discord::Request> &requests,
                hui::Settings &app_settings, bool &changed);
    void draw(const discord::Snapshot &snapshot, const hui::Settings &app_settings, DrawList &list) const;

  private:
    const Ink &ink_;
    std::string version_;
    int section_ = 0;
    int row_ = 0;
    bool in_rows_ = false; // focus is on the rows, not the sections
    // The pairing dialog: a menu while paired (pair again, remove), the wait while the
    // core pairs, and the PIN boxes when the console wants its Link Device PIN typed.
    enum class Pair
    {
        none,
        menu,
        wait,
        pin,
    };
    Pair pair_ = Pair::none;
    int menu_at_ = 0;
    float sent_time_ = 9.0f; // since the last pairing request
    bool pin_sent_ = false;
    int pin_[8] = {}; // -1 while empty
    int pin_at_ = 0;

    void draw_pair(const discord::Snapshot &snapshot, DrawList &list) const;
    void update_pair(const discord::Snapshot &snapshot, const hui::InputFrame &input, hui::ui::Feedback &feedback,
                     std::vector<discord::Request> &requests);
    void start_pin();
};

} // namespace screens
