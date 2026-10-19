// ps5-discord - What screens ask the client to do. The app passes these on to
// the core client (console) or applies them to the sample account (host).
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace discord
{

struct Request
{
    enum class Kind
    {
        open_channel, // load history and subscribe to the guild
        load_older,   // the next page of history above what is loaded
        send,         // post content to channel_id
    };
    Kind kind = Kind::open_channel;
    std::string guild_id; // empty for a DM
    std::string channel_id;
    std::string content;
    std::string reply_to; // message id this answers, or empty
};

} // namespace discord
