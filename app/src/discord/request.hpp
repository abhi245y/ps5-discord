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
        browser_login, // open Discord and import its session on-console
        browser_cancel,
        open_channel, // load history and subscribe to the guild
        load_older,   // the next page of history above what is loaded
        send,         // post content to channel_id
        react,        // add emoji to message_id in channel_id
        open_dm,      // find or create the DM with user_id (Snapshot::dm_by_user)
        prefetch,     // load history ahead of an open, without marking it read
        edit,         // replace message_id's content (the account's own message)
        remove,       // delete message_id (the account's own message)
    };
    Kind kind = Kind::open_channel;
    std::string guild_id; // empty for a DM
    std::string channel_id;
    std::string content;
    std::string reply_to; // message id this answers, or empty
    std::string message_id;
    std::string emoji;    // a Unicode emoji, for react
    std::string user_id;  // for open_dm
    std::string nonce;    // set by the client for send: matches the gateway's echo
};

} // namespace discord
