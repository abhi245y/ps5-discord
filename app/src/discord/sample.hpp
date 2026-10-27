// ps5-discord - Synthetic account for host snapshots and the tour.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "discord/model.hpp"
#include "discord/request.hpp"

#include <string>

namespace discord
{

Snapshot sample_snapshot();

// What the client would do with a request, done to the sample account: a sent
// message appears at once (as the gateway would echo it).
void apply_sample(Snapshot &snapshot, const Request &request);

// Host pictures that need a state the tour cannot reach by input (an incoming
// ring, a failed connection): sets it on the sample account for one picture.
void sample_picture(Snapshot &snapshot, const std::string &picture);

} // namespace discord
