// ps5-discord - The update dialog: an offer, its notes, progress, "Restart to finish" and what
// went wrong. State comes from the core (Snapshot::update); this holds only what the user did.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

#include "core/input.hpp"
#include "discord/request.hpp"
#include "screens/draw.hpp"

namespace screens
{

class UpdateDialog
{
  public:
    explicit UpdateDialog(const Ink &ink) : ink_(ink) {}

    // Settings → About: bring the dialog up again, whatever Later said.
    void open() { open_ = true; later_ = ready_later_ = notes_ = false; }
    // True while a card is up and has the input. idle: the main screen, no call ringing.
    bool update(const discord::Snapshot &s, const hui::InputFrame &input, hui::ui::Feedback &feedback,
                std::vector<discord::Request> &requests, bool idle);
    void draw(const discord::Snapshot &s, DrawList &list) const;

  private:
    enum class Card { none, offer, notes, progress, ready, failed, rolled_back };
    Card card(const discord::Snapshot &s, bool idle) const;

    const Ink &ink_;
    bool open_ = false;        // asked for from Settings: the progress shows too
    bool idle_ = false;        // update()'s last idle, for draw
    bool later_ = false;       // Later on the offer: not again this launch
    bool ready_later_ = false; // Later on "Restart to finish"
    bool notes_ = false;       // the notes are showing
    bool started_ = false;     // Update was pressed: progress until the core says otherwise
    bool rolled_back_seen_ = false;
    std::string failed_seen_;  // the failure text the user has read
};

} // namespace screens
