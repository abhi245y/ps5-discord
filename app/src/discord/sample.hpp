// ps5-discord - Synthetic account for host snapshots and the tour.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "discord/model.hpp"
#include "discord/request.hpp"

namespace discord
{

Snapshot sample_snapshot();

// What the client would do with a request, done to the sample account: a sent
// message appears at once (as the gateway would echo it).
void apply_sample(Snapshot &snapshot, const Request &request);

} // namespace discord
