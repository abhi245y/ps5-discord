// ps5-discord - Settings (framework frame 11): sections on the left, the chosen
// section's rows on the right, and the Remote Play pairing PIN for Go Live.
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
    // The pairing PIN: eight digits, -1 while empty; the digit being entered.
    bool pin_open_ = false, pin_sent_ = false;
    int pin_[8] = {};
    int pin_at_ = 0;

    void draw_pin(const discord::Snapshot &snapshot, DrawList &list) const;
    bool update_pin(const discord::Snapshot &snapshot, const hui::InputFrame &input, hui::ui::Feedback &feedback,
                    std::vector<discord::Request> &requests);
};

} // namespace screens
